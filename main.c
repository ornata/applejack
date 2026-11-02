#include <CoreFoundation/CoreFoundation.h> // CFDictionary, CFNumber, CFRunLoop
#include <IOKit/hid/IOHIDKeys.h> // kIOHIDVendorIDKey, kIOHIDProductIDKey, ...
#include <IOKit/hid/IOHIDManager.h> // manager, matching, callbacks
#include <assert.h>
#include <dispatch/dispatch.h>
#include <libusb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/// USB side of the iMate. Vendor identity, interfaces, etc.
enum usb_protocol_imate {
  IMATE_VENDOR_ID = 0x077d, // Griffin Technology
  IMATE_PRODUCT_ID = 0x0405,
  IMATE_INTERFACE_KEYBOARD = 0, // USB interface numbers
  IMATE_REQUEST_TYPE = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE |
                       LIBUSB_ENDPOINT_OUT,
  IMATE_REQUEST_ADB_COMMAND = 0, // bRequest: send an ADB command
  IMATE_TIMEOUT_MS = 1000,

  IMATE_REPLY_SIZE = 8,
  IMATE_REPLY_COMMAND_OFFSET = 0,
  IMATE_REPLY_PACKET_OFFSET = 2,
  IMATE_REPLY_FIRST_PACKET = 0x99, // byte 2: first packet of a reply
  IMATE_REPLY_LENGTH_OFFSET = 3,
  IMATE_REPLY_DATA_OFFSET = 4,

  IMATE_REQUEST_CONTROL = 1,   // bRequest: iMate settings
  IMATE_CONTROL_AUTOPOLL = 4,  // wValue: iMate polling for the bus
  IMATE_AUTOPOLL_OFF = 0x00ff, // wIndex
  IMATE_AUTOPOLL_ON = 0x000a,  // wIndex: poll interval
};

// polling interval in seconds
#define IMATE_POLL_INTERVAL_S 0.010

/// Generic ADB protocol values.
enum adb_protocol {
  ADB_COMMAND_LISTEN = 0x08, // command bits: write register on device
  ADB_COMMAND_TALK = 0x0C,   // command bits: read register
  ADB_REGISTER_0 = 0,        // event data: button, motion reports
  ADB_REGISTER_3 = 3,        // holds a device's address and handler ID
  ADB_REGISTER_SIZE = 2,     // bytes in register 3
  ADB_ADDRESS_SHIFT = 4,     // address is the high nibble of the command byte
  ADB_R3_SRQ_ENABLE = 0x20,  // register 3, high byte: keep service requests on
};

/// Pippin controller-specific ADB protocol values.
enum adb_protocol_pippin_controller {
  PAD_ADB_ADDRESS = 3,
  // two buttons + motion (mouse)
  PAD_HANDLER_DEFAULT = 0x01,
  // adds additional buttons; seems to be the only
  // ID > 1 the pad accepts
  PAD_HANDLER_EXTENDED = 0x46,
  // full ADB command byte to register 0
  PAD_COMMAND_TALK_R0 = (PAD_ADB_ADDRESS << ADB_ADDRESS_SHIFT) |
                        ADB_COMMAND_TALK | ADB_REGISTER_0,
  PAD_REGISTER_0_SIZE = 4,

  // trackball
  PAD_MOTION_Y_BYTE = 0,  // y delta
  PAD_MOTION_X_BYTE = 1,  // x delta
  PAD_MOTION_MASK = 0x7f, // low 7: signed delta, 7: button (unused?)
  PAD_MOTION_SIGN = 0x40, // bit 6: sign
  PAD_MOTION_RANGE = 0x80 // 2^7, subtracted to sext
};

/// Represents the gamepad state as collected from the adapter.
typedef struct gamepad_state {
  uint16_t buttons; // BTN_*
  int8_t dx;        // trackball delta x; 7 bit signed value
  int8_t dy;        // trackball delta y; 7 bit signed value
} gamepad_state;

/// Represents the mapping of report values to buttons on the gamepad.
typedef struct gamepad_button_mapping {
  uint16_t button; // BTN bit set in the gamepad_state.
  uint8_t byte;    // Which byte of the report.
  uint8_t mask;    // Which bit in the report byte
} gamepad_button_mapping;

/// Helper macro to define buttons on the pad.
// clang-format off
#define GAMEPAD_BUTTONS(X)                            \
  /*  id             name              byte  mask */  \
  X(LEFT_TRIGGER,  "left_trigger",     0,    0x80)    \
  X(RIGHT_TRIGGER, "right_trigger",    1,    0x80)    \
  X(YELLOW,        "yellow",           2,    0x02)    \
  X(BLUE,          "blue",             2,    0x01)    \
  X(RED,           "red",              2,    0x40)    \
  X(DPAD_UP,       "dpad_up",          2,    0x04)    \
  X(DPAD_DOWN,     "dpad_down",        2,    0x20)    \
  X(DPAD_LEFT,     "dpad_left",        2,    0x08)    \
  X(DPAD_RIGHT,    "dpad_right",       2,    0x10)    \
  X(GREEN,         "green",            2,    0x80)    \
  X(SQUARE,        "square",           3,    0x01)    \
  X(CIRCLE,        "circle",           3,    0x02)    \
  X(DIAMOND,       "diamond",          3,    0x04)
// clang-format on

#define X(id, name, byte, mask) BTN_IDX_##id,
enum gamepad_button_index { GAMEPAD_BUTTONS(X) BTN_IDX_COUNT };
#undef X
_Static_assert(BTN_IDX_COUNT <= 16, "buttons is a uint16_t");

/// Static LUT that contains all button mappings for the controller.
#define X(id, name, byte, mask) {1U << BTN_IDX_##id, byte, mask},
static const gamepad_button_mapping button_map[] = {GAMEPAD_BUTTONS(X)};
#undef X

/// Pretty printable names for buttons stored in a LUT.
#define X(id, name, byte, mask) [BTN_IDX_##id] = (name),
static const char *const button_names[BTN_IDX_COUNT] = {GAMEPAD_BUTTONS(X)};
#undef X

/// Maximum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MAX 192
/// Minimum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MIN 7
_Static_assert(BTN_PRETTY_PRINT_MIN >= sizeof("(none)"),
               "need to fit '(none)' at the minimum");

/// Turns a byte from the pad into a raw movement amount.
static int8_t motion_delta(uint8_t byte) {
  // bit:   7        6 5 4 3 2 1 0      < movement in [-64, -1]
  //        flag     7-bit signed delta < mouse trigger
  // Don't need the mouse trigger, so kill it.
  int v = byte & PAD_MOTION_MASK;
  // manually instead of bit twiddling to avoid impl defined behaviour
  if (v & PAD_MOTION_SIGN) // check if negative
    v -= PAD_MOTION_RANGE; // sext
  // cast is fine, we're in -64..63
  return (int8_t)v;
}

/// Stores the accumulated position of the controller trackball.
typedef struct trackball_position {
  int64_t x;
  int64_t y;
} trackball_position;

/// Returns the limit value when adding v to a 64-bit value that overflows.
static int64_t overflow_limit_64_8(int8_t v) {
  return v > 0 ? INT64_MAX : INT64_MIN;
}

/// Add dx and dy to the trackball position.
/// Saturates on overflow.
static void trackball_accumulate(trackball_position *pos, int8_t dx,
                                 int8_t dy) {
  if (__builtin_add_overflow(pos->x, dx, &pos->x))
    pos->x = overflow_limit_64_8(dx);
  if (__builtin_add_overflow(pos->y, dy, &pos->y))
    pos->y = overflow_limit_64_8(dy);
}

typedef struct app_context {
  libusb_device_handle *lusb_handle;
  gamepad_state state;
  trackball_position trackball_pos;
} app_context;

/// Signals to handle, so we don't end up with a stuck pad.
static const int caught_signals[] = {SIGINT, SIGTERM, SIGHUP};
#define CAUGHT_SIGNAL_COUNT (sizeof(caught_signals) / sizeof(caught_signals[0]))

/// Stops the run loop so main can restore the pad.
static void on_signal(void *context) {
  (void)context;
  CFRunLoopStop(CFRunLoopGetMain());
}

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

static const uint8_t *raw_reply_data(const uint8_t *report, CFIndex len) {
  if (len != IMATE_REPLY_SIZE)
    return NULL;
  if (report[IMATE_REPLY_COMMAND_OFFSET] != PAD_COMMAND_TALK_R0)
    return NULL;
  if (report[IMATE_REPLY_PACKET_OFFSET] != IMATE_REPLY_FIRST_PACKET)
    return NULL;
  if (report[IMATE_REPLY_LENGTH_OFFSET] != PAD_REGISTER_0_SIZE) // single packet
    return NULL;
  return report + IMATE_REPLY_DATA_OFFSET;
}

/// Decode button state from the pad and output it in out_state.
static void pad_decode(const uint8_t data[PAD_REGISTER_0_SIZE],
                       gamepad_state *out_state) {
  uint16_t buttons = 0;
  size_t len = sizeof(button_map) / sizeof(button_map[0]);
  for (size_t i = 0; i < len; ++i) {
    const gamepad_button_mapping *m = &button_map[i];
    if (!(data[m->byte] & m->mask))
      buttons |= m->button;
  }
  out_state->buttons = buttons;
  out_state->dx = motion_delta(data[PAD_MOTION_X_BYTE]);
  out_state->dy = motion_delta(data[PAD_MOTION_Y_BYTE]);
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
  if (iface2int != IMATE_INTERFACE_KEYBOARD)
    return;
  // Print out whatever we got from the device.
  printf("Interface number: %d, hex: ", iface2int);
  for (CFIndex i = 0; i < report_length; ++i)
    printf("%02x ", report[i]);
  printf("\n");
  app_context *app_ctx = context;
  // Decode the buttons in the report and print the updated state.
  const uint8_t *data = raw_reply_data(report, report_length);
  if (!data)
    return;
  pad_decode(data, &app_ctx->state);
  char names[BTN_PRETTY_PRINT_MAX];
  if (buttons_to_string(names, sizeof(names), app_ctx->state.buttons))
    printf("gamepad_buttons: buttons = %s\n", names);
  else
    fprintf(stderr, "error: Couldn't compute button name\n");
  int8_t dx = app_ctx->state.dx;
  int8_t dy = app_ctx->state.dy;
  if (dx || dy) {
    trackball_accumulate(&app_ctx->trackball_pos, dx, dy);
    printf("gamepad_trackball: dx = %d, dy = %d, pos = (%lld, %lld)\n", dx, dy,
           app_ctx->trackball_pos.x, app_ctx->trackball_pos.y);
  }
}

/// Send an ADB command through the iMate that tells the device at the given
/// address to switch to the given handler ID.
///
/// Blocks.
///
/// Returns true if the full message was sent.
static bool set_pad_handler(app_context *app_ctx, uint8_t adb_address,
                            uint8_t handler_id) {
  // Listen to register 3.
  uint8_t register_contents[ADB_REGISTER_SIZE] = {
      ADB_R3_SRQ_ENABLE | adb_address, handler_id};
  uint8_t adb_command_byte =
      (adb_address << ADB_ADDRESS_SHIFT) | ADB_COMMAND_LISTEN | ADB_REGISTER_3;
  return libusb_control_transfer(app_ctx->lusb_handle, IMATE_REQUEST_TYPE,
                                 IMATE_REQUEST_ADB_COMMAND, adb_command_byte,
                                 /*unused*/ 0, register_contents,
                                 sizeof(register_contents),
                                 IMATE_TIMEOUT_MS) == sizeof(register_contents);
}

/// Toggles autopolling for the pad.
static bool set_autopoll(app_context *app_ctx, bool enabled) {
  return libusb_control_transfer(app_ctx->lusb_handle, IMATE_REQUEST_TYPE,
                                 IMATE_REQUEST_CONTROL, IMATE_CONTROL_AUTOPOLL,
                                 enabled ? IMATE_AUTOPOLL_ON
                                         : IMATE_AUTOPOLL_OFF,
                                 /* data = */ NULL,
                                 /* wLength = */ 0, IMATE_TIMEOUT_MS) == 0;
}

static void poll_pad(CFRunLoopTimerRef timer, void *info) {
  (void)timer;
  app_context *app_ctx = info;
  int rc = libusb_control_transfer(
      app_ctx->lusb_handle, IMATE_REQUEST_TYPE, IMATE_REQUEST_ADB_COMMAND,
      PAD_COMMAND_TALK_R0, 0, NULL, 0, IMATE_TIMEOUT_MS);
  if (rc) {
    fprintf(stderr, "error: Couldn't send talk r0 to the pad\n");
    // Send failure to cleanup.
    CFRunLoopStop(CFRunLoopGetCurrent());
  }
}

int main(void) {
  int vid = IMATE_VENDOR_ID;
  int pid = IMATE_PRODUCT_ID;

  libusb_context *lusb_ctx = NULL;
  app_context app_ctx = {0};
  bool handler_switched = false;
  bool autopoll_off = false;
  CFRunLoopTimerRef timer = NULL;
  dispatch_source_t signal_sources[CAUGHT_SIGNAL_COUNT] = {0};
  int status = 1;

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
  // Initialize reporting callback and attach it to the current thread.
  IOHIDManagerRegisterInputReportCallback(mgr, on_report,
                                          /*context =*/&app_ctx);
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
  app_ctx.lusb_handle = libusb_open_device_with_vid_pid(lusb_ctx, vid, pid);
  if (!app_ctx.lusb_handle) {
    fprintf(stderr, "error: Failed to create libusb device handle\n");
    goto cleanup;
  }
  if (!set_pad_handler(&app_ctx, PAD_ADB_ADDRESS, PAD_HANDLER_EXTENDED)) {
    fprintf(stderr, "error: Could not set pad handler\n");
    goto cleanup;
  }
  handler_switched = true;
  autopoll_off = set_autopoll(&app_ctx, /* enabled = */ false);
  if (!autopoll_off) {
    fprintf(stderr, "error: Could not disable autopoll\n");
    goto cleanup;
  }
  CFRunLoopTimerContext run_timer_ctx = {
      0, &app_ctx, NULL, NULL,
      NULL}; // version, info, retain, release, description
  timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
                               CFAbsoluteTimeGetCurrent() +
                                   IMATE_POLL_INTERVAL_S, // first fire
                               IMATE_POLL_INTERVAL_S,     // repeat interval
                               0, 0,                      // flags, order
                               poll_pad, &run_timer_ctx);
  if (!timer) {
    fprintf(stderr, "error: Could not create run loop timer\n");
    goto cleanup;
  }
  CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
  for (size_t i = 0; i < CAUGHT_SIGNAL_COUNT; ++i) {
    // Make sure default action doesn't kill us.
    signal(caught_signals[i], SIG_IGN);
    signal_sources[i] =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, caught_signals[i],
                               0, dispatch_get_main_queue());
    if (!signal_sources[i]) {
      fprintf(stderr, "error: Could not create dispatch source for signal\n");
      goto cleanup;
    }
    dispatch_source_set_event_handler_f(signal_sources[i], on_signal);
    dispatch_resume(signal_sources[i]);
  }
  CFRunLoopRun();
  status = 0; // success
cleanup:
  for (size_t i = CAUGHT_SIGNAL_COUNT; i-- > 0;) {
    if (!signal_sources[i])
      continue;
    dispatch_source_cancel(signal_sources[i]);
    dispatch_release(signal_sources[i]);
  }
  if (timer) {
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
  }
  if (autopoll_off && !set_autopoll(&app_ctx, /* enabled = */ true))
    fprintf(stderr, "warning: Could not re-enable autopoll on pad\n");
  if (handler_switched &&
      !set_pad_handler(&app_ctx, PAD_ADB_ADDRESS, PAD_HANDLER_DEFAULT))
    fprintf(stderr,
            "warning: Pad may still be in extended mode; replug the iMate\n");
  if (app_ctx.lusb_handle)
    libusb_close(app_ctx.lusb_handle);
  if (lusb_ctx)
    libusb_exit(lusb_ctx);
  CFRelease(mgr);
  return status;
}
