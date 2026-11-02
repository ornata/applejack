#include <CoreFoundation/CoreFoundation.h> // CFDictionary, CFNumber, CFRunLoop
#include <IOKit/hid/IOHIDKeys.h> // kIOHIDVendorIDKey, kIOHIDProductIDKey, ...
#include <IOKit/hid/IOHIDManager.h> // manager, matching, callbacks
#include <assert.h>
#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/// USB side of the iMate. Vendor identity, interfaces, etc.
enum usb_protocol_imate {
  IMATE_VENDOR_ID = 0x077d, // Griffin Technology
  IMATE_PRODUCT_ID = 0x0405,
  IMATE_INTERFACE_KEYBOARD = 0, // USB interface numbers
  IMATE_INTERFACE_MOUSE = 1,
  IMATE_MOUSE_REPORT_LENGTH = 3, // bytes in the mouse interface's HID report
  IMATE_REQUEST_TYPE = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE |
                       LIBUSB_ENDPOINT_OUT,
  IMATE_REQUEST_ADB_COMMAND = 0, // bRequest: send an ADB command
  IMATE_TIMEOUT_MS = 1000,
};

/// Generic ADB protocol values.
enum adb_protocol {
  ADB_COMMAND_LISTEN = 0x08, // command bits of the ADB command byte
  ADB_REGISTER_3 = 3,        // holds a device's address and handler ID
  ADB_REGISTER_SIZE = 2,     // bytes in register 3
  ADB_ADDRESS_SHIFT = 4,     // address is the high nibble of the command byte
  ADB_R3_SRQ_ENABLE = 0x20,  // register 3, high byte: keep service requests on
};

/// Pippin controller-specific ADB protocol values.
enum adb_protocol_pippin_controller {
  PAD_ADB_ADDRESS = 3,
  PAD_HANDLER_DEFAULT = 0x01,  // classic mouse: two buttons plus motion
  PAD_HANDLER_EXTENDED = 0x46, // also reports the other buttons
};

/// Represents the gamepad state as collected from the adapter.
typedef struct gamepad_state {
  uint16_t buttons; // BTN_*
} gamepad_state;

/// Represents the mapping of report values to buttons on the gamepad.
typedef struct gamepad_button_mapping {
  uint16_t button; // BTN bit set in the gamepad_state.
  uint8_t byte;    // Which byte of the report.
  uint8_t mask;    // Which bit in the report byte
  uint8_t invert;  // XORed into report byte first. Matches mask == active low.
} gamepad_button_mapping;

/// Helper macro to define buttons on the pad.
// clang-format off
#define GAMEPAD_BUTTONS(X)                                   \
  /*  id             name              byte  mask  invert  */\
  X(LEFT_TRIGGER,  "left_trigger",     0,    0x01,  0x00)    \
  X(RIGHT_TRIGGER, "right_trigger",    0,    0x02,  0x00)    \
  X(YELLOW,        "yellow",           1,    0x08,  0x08)    \
  X(BLUE,          "blue",             1,    0x04,  0x04)    \
  X(RED,           "red",              2,    0x10,  0x10)
// clang-format on

#define X(id, name, byte, mask, invert) BTN_IDX_##id,
enum gamepad_button_index { GAMEPAD_BUTTONS(X) BTN_IDX_COUNT };
#undef X
_Static_assert(BTN_IDX_COUNT <= 16, "buttons is a uint16_t");

/// Static LUT that contains all button mappings for the controller.
#define X(id, name, byte, mask, invert) {1U << BTN_IDX_##id, byte, mask, invert},
static const gamepad_button_mapping button_map[] = {GAMEPAD_BUTTONS(X)};
#undef X

/// Pretty printable names for buttons stored in a LUT.
#define X(id, name, byte, mask, invert) [BTN_IDX_##id] = (name),
static const char *const button_names[BTN_IDX_COUNT] = {GAMEPAD_BUTTONS(X)};
#undef X

/// Maximum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MAX 64
/// Minimum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MIN 7
_Static_assert(BTN_PRETTY_PRINT_MIN >= sizeof("(none)"),
               "need to fit '(none)' at the minimum");

/// Change a set of buttons into a human-readable string.
static bool buttons_to_string(char *out_buf, size_t size, uint16_t buttons) {
  // make sure nobody is handing us garbage
  if (size < BTN_PRETTY_PRINT_MIN || size > BTN_PRETTY_PRINT_MAX || !out_buf)
    return false;
  size_t remaining = size;
  bool button_pressed = false;
  for (size_t i = 0; i < BTN_IDX_COUNT; ++i) {
    // if we only have one byte left, we need to write '\0'
    if (remaining <= 1)
      break;
    if (!(buttons & (1U << i)))
      continue;
    button_pressed = true;
    size_t used = size - remaining;
    const char *name = button_names[i];
    const char *sep = used > 0 ? ", " : "";
    int n = name ? snprintf(out_buf + used, remaining, "%s%s", sep, name)
                 : snprintf(out_buf + used, remaining, "%s(bit %zu)", sep, i);
    // Formatting error?
    if (n < 0)
      break;
    // Need to truncate?
    if ((size_t)n >= remaining) {
      remaining = 1;
      break;
    }
    remaining -= (size_t)n;
  }
  // Was anything pressed at all?
  if (!button_pressed) {
    // Nothing was pressed, so just output "(none)" to signify that.
    int n = snprintf(out_buf, size, "(none)");
    remaining -= (size_t)n;
  }
  out_buf[size - remaining] = '\0';
  return true;
}

/// Decode the button state of a 3 byte or empty report.
/// Returns false if the report is the wrong size.
static bool pad_decode(const uint8_t *report, CFIndex report_length,
                       gamepad_state *out_state) {
  // Only accept a mouse-sized report.
  if (report_length != IMATE_MOUSE_REPORT_LENGTH)
    return false;
  // Walk over the button set. Mask in the bits for each button.
  uint16_t buttons = 0;
  size_t len = sizeof(button_map) / sizeof(button_map[0]);
  for (size_t i = 0; i < len; ++i) {
    const gamepad_button_mapping *m = &button_map[i];
    if ((report[m->byte] ^ m->invert) & m->mask)
      buttons |= m->button;
  }
  out_state->buttons = buttons;
  return true;
}

/// Callback that runs every time we get an input report from the attached
/// device. Used as a probe to check what happens when you press buttons etc.
///
/// When we get an input report, prints:
/// - The interface ID of the sender device
/// - The raw bytes sent by the device.
static void on_report(void *context, IOReturn result, void *sender,
                      IOHIDReportType type, uint32_t report_id, uint8_t *report,
                      CFIndex report_length) {
  (void)type;
  (void)report_id;
  // When we enter this function, it means we got an IO operation.
  // If there was any sort of error, then output it and bail.
  if (result != kIOReturnSuccess) {
    fprintf(stderr, "error: input report failed: 0x%x\n", result);
    return;
  }
  assert(sender && "sender was null?!");
  assert(report && "report was null?!");
  // OK. We have some result from some attached sender device.
  // Check if we can get the sender's device interface.
  IOHIDDeviceRef device = (IOHIDDeviceRef)sender;
  CFTypeRef iface = IOHIDDeviceGetProperty(device, CFSTR("bInterfaceNumber"));
  if (!iface) {
    fprintf(stderr, "error: Interface property missing; giving up\n");
    return;
  }
  // Get the integer ID for the interface so we can print it.
  int iface2int = -1;
  if (!CFNumberGetValue(iface, kCFNumberIntType, &iface2int)) {
    fprintf(stderr,
            "error: Couldn't convert interface number to an int; giving up\n");
    return;
  }
  // The pad's reports come in on the mouse interface.
  if (iface2int != IMATE_INTERFACE_MOUSE) {
    fprintf(stderr, "error: Expected a mouse; giving up\n");
    return;
  }
  // Print out whatever we got from the device.
  printf("Interface number: %d, hex: ", iface2int);
  for (CFIndex i = 0; i < report_length; ++i)
    printf("%02x ", report[i]);
  printf("\n");
  gamepad_state *state = context;
  assert(state && "expected a gamepad state in the context?!");
  // Decode the buttons in the report and print the updated state.
  if (!pad_decode(report, report_length, state))
    return;
  char names[BTN_PRETTY_PRINT_MAX];
  if (buttons_to_string(names, sizeof(names), state->buttons))
    printf("gamepad state: buttons = %s\n", names);
  else
    fprintf(stderr, "error: Couldn't compute button name\n");
}

/// Send an ADB command through the iMate that tells the device at the given
/// address to switch to the given handler ID.
///
/// Blocks.
///
/// Returns true if the full message was sent.
static bool set_pad_handler(libusb_device_handle *handle, uint8_t adb_address,
                            uint8_t handler_id) {
  // Listen to register 3.
  uint8_t register_contents[ADB_REGISTER_SIZE] = {
      ADB_R3_SRQ_ENABLE | adb_address, handler_id};
  uint8_t adb_command_byte =
      (adb_address << ADB_ADDRESS_SHIFT) | ADB_COMMAND_LISTEN | ADB_REGISTER_3;
  return libusb_control_transfer(handle, IMATE_REQUEST_TYPE,
                                 IMATE_REQUEST_ADB_COMMAND, adb_command_byte,
                                 /*unused*/ 0, register_contents,
                                 sizeof(register_contents),
                                 IMATE_TIMEOUT_MS) == sizeof(register_contents);
}

int main(void) {
  int vid = IMATE_VENDOR_ID;
  int pid = IMATE_PRODUCT_ID;

  libusb_context *lusb_ctx = NULL;
  libusb_device_handle *lusb_handle = NULL;
  int status = 1;
  gamepad_state state = {0};

  IOHIDManagerRef mgr =
      IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!mgr) {
    fprintf(stderr, "error: Couldn't create IOHIDManager; bailing.\n");
    return status;
  }
  // Build the matching dictionary. This lets us say that we only care about
  // some specific attached device. Otherwise, we match against all devices.
  CFNumberRef vid_num =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
  assert(vid_num &&
         "couldn't create vid_num?!"); // throwaway tool; assert is fine.
  CFNumberRef pid_num =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
  assert(pid_num && "couldn't create pid_num?!");
  CFMutableDictionaryRef match = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vid_num);
  CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pid_num);
  IOHIDManagerSetDeviceMatching(mgr, match);
  // Matching dictionary is set up. Clear the stuff we don't need.
  CFRelease(vid_num);
  CFRelease(pid_num);
  CFRelease(match);
  // Initialize a callback that uses the gamepad state in its context, then
  // attach it to the current thread.
  IOHIDManagerRegisterInputReportCallback(mgr, on_report, /*context =*/&state);
  IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(),
                                  kCFRunLoopDefaultMode);
  // Open it and check the result.
  IOReturn rc = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
  if (rc != kIOReturnSuccess) {
    fprintf(stderr, "error: IOHIDManagerOpen failed: 0x%x\n", rc);
    CFRelease(mgr);
    return status;
  }
  // Open libusb.
  if (libusb_init(&lusb_ctx)) {
    fprintf(stderr, "error: Failed to init libusb\n");
    goto cleanup;
  }
  lusb_handle = libusb_open_device_with_vid_pid(lusb_ctx, vid, pid);
  if (!lusb_handle) {
    fprintf(stderr, "error: Failed to create libusb device handle\n");
    goto cleanup;
  }
  if (!set_pad_handler(lusb_handle, PAD_ADB_ADDRESS, PAD_HANDLER_EXTENDED)) {
    fprintf(stderr, "error: Could not set pad handler\n");
    goto cleanup;
  }
  CFRunLoopRun();
  if (!set_pad_handler(lusb_handle, PAD_ADB_ADDRESS, PAD_HANDLER_DEFAULT)) {
    // Failed to restore the state. This is probably out of our control, so
    // we'll let it return 0.
    fprintf(stderr,
            "warning: Pad may still be in extended mode; replug the iMate\n");
  }
  status = 0; // success
cleanup:
  if (lusb_handle)
    libusb_close(lusb_handle);
  if (lusb_ctx)
    libusb_exit(lusb_ctx);
  CFRelease(mgr);
  return status;
}
