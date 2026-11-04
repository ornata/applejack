/*
 * main.c - Use the AppleJack controller on a modern MacBook
 * This program allows you to use an AppleJack controller on a modern MacBook,
 * provided you have
 *
 * - A wired AppleJack controller with a P-ADB -> ADB adapter
 * - An iMate adapter
 * - An adapter to get the iMate to USB-C
 *
 * If you have all of this, then this program will allow you to map the
 * controller's buttons to keyboard keys, and use the trackball as a mouse.
 *
 */
#include <ApplicationServices/ApplicationServices.h> // CGEvent*, AXIsProcessTrusted
#include <Carbon/Carbon.h> // kVK_* constants only; nothing to link
#include <CoreFoundation/CoreFoundation.h> // CFDictionary, CFNumber, CFRunLoop
#include <IOKit/hid/IOHIDKeys.h> // kIOHIDVendorIDKey, kIOHIDProductIDKey, ...
#include <IOKit/hid/IOHIDManager.h> // manager, matching, callbacks
#include <assert.h>
#include <dispatch/dispatch.h>
#include <libusb.h>
#include <math.h>
#include <os/log.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LOG_SUBSYSTEM "com.barrelshifter.applejack"
#define LOG_CATEGORIES(X)                                                      \
  X(REPORTS, "reports")                                                        \
  X(STATE, "state")                                                            \
  X(KEYS, "keys")                                                              \
  X(MOUSE, "mouse")

#define X(id, name) LOG_##id,
enum log_category { LOG_CATEGORIES(X) LOG_COUNT };
#undef X

#define X(id, name) [LOG_##id] = (name),
static const char *const log_names[LOG_COUNT] = {LOG_CATEGORIES(X)};
#undef X

static os_log_t logs[LOG_COUNT]; // filled once at startup
static void log_init(void) {
  for (size_t i = 0; i < LOG_COUNT; ++i)
    logs[i] = os_log_create(LOG_SUBSYSTEM, log_names[i]);
}
#define LOG(cat, ...) os_log_info(logs[cat], __VA_ARGS__)

/// USB side of the iMate. Vendor identity, interfaces, etc.
///
/// Commands (host -> iMate) are vendor control transfers, bmRequestType 0x41:
///
///   bRequest 0  ADB command    wValue = ADB command byte
///                              data   = register bytes (Listen only)
///   bRequest 1  iMate setting  wValue = setting (4 = autopoll)
///                              wIndex = value (0x00ff off, 0x000a on)
///
/// Replies (iMate -> host) are 8-byte HID input reports on interface 0:
///
///   byte    0         1      2            3      4 .. 7
///        +---------+------+------------+------+-----------------+
///        | command | 0x00 | 0x99 first | data | register data   |
///        | echo    |      | 0x98 next  | len  | (4 per packet)  |
///        +---------+------+------------+------+-----------------+
///
///   e.g. Talk R3 reply  3f 00 99 02 6a 01 00 00   (2 bytes: handler 0x01)
///        Talk R0 reply  3c 00 99 04 80 80 ff ff   (4 bytes: extended, idle)
///
/// macOS sees interface 0 as a keyboard and parses these as keyboard
/// reports, so we seize it to keep the system from acting on them.
enum imate_usb_protocol {
  IMATE_VENDOR_ID = 0x077d, // Griffin Technology
  IMATE_PRODUCT_ID = 0x0405,
  IMATE_INTERFACE_KEYBOARD = 0, // USB interface numbers
  IMATE_REQUEST_TYPE = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE |
                       LIBUSB_ENDPOINT_OUT,
  IMATE_REQUEST_ADB_COMMAND = 0, // bRequest: send an ADB command

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

/// Tuning for the imate, separate from wire format.
enum imate_tuning {
  IMATE_TIMEOUT_MS = 1000, // libusb control transfer timeout

  // hack: add polling gaps to add minimum waits before sending commands
  // to the iMate. The iMate tends to drop commands that arrive within
  // a few ms of each other, which can prevent startup from working.
  IMATE_POLL_GAP_MS = 5,     // hack: locally measured min with autopoll off
  IMATE_COMMAND_GAP_MS = 30, // hack: from the vendor firmware
};

/// Generic ADB protocol values.
///
/// ADB command byte (host -> device):
///
///   bit    7 6 5 4   3 2   1 0
///        +---------+-----+-----+
///        | address | cmd | reg |
///        +---------+-----+-----+
///
///   cmd: 00 reset/flush, 10 Listen (write register), 11 Talk (read register)
///   e.g. Talk R0 to address 3 = 0x3c, Listen R3 to address 3 = 0x3b
///
/// Register 3 (2 bytes) holds the device's address and handler ID:
///
///   high byte                    low byte
///   bit  7 6 5 4  3 2 1 0        7 6 5 4 3 2 1 0
///       +-+-+-+-+---------+     +-----------------+
///       |r|e|S|r| address |     |   handler ID    |
///       +-+-+-+-+---------+     +-----------------+
///   r = reserved, e = exceptional event, S = SRQ enable (0x20 in the byte)
///
/// On a read, the address nibble varied between replies (0x6a, 0x6b), so we
/// never compare it; only the handler ID is meaningful.
enum adb_protocol {
  ADB_COMMAND_LISTEN = 0x08, // command bits: write register on device
  ADB_COMMAND_TALK = 0x0C,   // command bits: read register
  ADB_REGISTER_0 = 0,        // event data: button, motion reports
  ADB_REGISTER_3 = 3,        // holds a device's address and handler ID
  ADB_REGISTER_SIZE = 2,     // bytes in register 3
  ADB_ADDRESS_SHIFT = 4,     // address is the high nibble of the command byte
  ADB_R3_SRQ_ENABLE = 0x20,  // register 3, high byte: keep service requests on
};

/// Values specific to the AppleJack controller.
///
/// Register 0 in extended mode (handler 0x46) is 4 bytes. Buttons are active
/// low: a 0 bit means pressed. Idle is 80 80 ff ff.
///
///   bit        7       6       5       4       3       2       1       0
///          +-------+-------+-------+-------+-------+-------+-------+-------+
///   byte 0 | LT    | Y delta, 7-bit signed, + is down                      |
///          +-------+-------+-------+-------+-------+-------+-------+-------+
///   byte 1 | RT    | X delta, 7-bit signed, + is right                     |
///          +-------+-------+-------+-------+-------+-------+-------+-------+
///   byte 2 | GREEN | RED   | DOWN  | RIGHT | LEFT  | UP    | YELLOW| BLUE  |
///          +-------+-------+-------+-------+-------+-------+-------+-------+
///   byte 3 | -     | -     | -     | -     | -     |DIAMOND|CIRCLE |SQUARE |
///          +-------+-------+-------+-------+-------+-------+-------+-------+
///
/// Default mode (handler 0x01) is only bytes 0 and 1: the usual ADB mouse
/// format, a button in bit 7 and a signed delta in the low 7 bits.
enum applejack_protocol {
  APPLEJACK_ADB_ADDRESS = 3,
  // two buttons + motion (mouse)
  APPLEJACK_HANDLER_DEFAULT = 0x01,
  // adds additional buttons; seems to be the only
  // ID > 1 the AppleJack accepts
  APPLEJACK_HANDLER_EXTENDED = 0x46,
  // full ADB command byte to register 0
  APPLEJACK_COMMAND_TALK_R0 = (APPLEJACK_ADB_ADDRESS << ADB_ADDRESS_SHIFT) |
                              ADB_COMMAND_TALK | ADB_REGISTER_0,
  APPLEJACK_REGISTER_0_SIZE = 4,

  // trackball
  APPLEJACK_MOTION_Y_BYTE = 0,  // y delta
  APPLEJACK_MOTION_X_BYTE = 1,  // x delta
  APPLEJACK_MOTION_MASK = 0x7f, // low 7: signed delta, 7: button (unused?)
  APPLEJACK_MOTION_SIGN = 0x40, // bit 6: sign
  APPLEJACK_MOTION_RANGE = 0x80 // 2^7, subtracted to sext
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

/// Represents which buttons changed between two snapshots.
typedef struct gamepad_edges {
  uint16_t pressed;  // bits that went up -> down
  uint16_t released; // bits that went down -> up
} gamepad_edges;

/// Splits the change from prev to now into newly pressed and newly released
/// buttons.
static gamepad_edges gamepad_diff(uint16_t prev, uint16_t now) {
  uint16_t changed = prev ^ now;
  return (gamepad_edges){.pressed = changed & now, .released = changed & prev};
}

/// Turns a byte from the pad into a raw movement amount.
static int8_t applejack_motion_delta(uint8_t byte) {
  // bit:   7        6 5 4 3 2 1 0
  //        trigger  7-bit signed delta, two's complement, -64..63
  //
  // e.g. 0x05 = +5, 0x7b = -5, 0x40 = -64, 0x3f = +63, 0x80 = 0 (trigger only)
  // Bit 7 is the trigger's state (active low), not part of the delta, so
  // kill it.
  int v = byte & APPLEJACK_MOTION_MASK;
  // manually instead of bit twiddling to avoid impl defined behaviour
  if (v & APPLEJACK_MOTION_SIGN) // check if negative
    v -= APPLEJACK_MOTION_RANGE; // sext
  // cast is fine, we're in -64..63
  return (int8_t)v;
}

/// Which key each button posts.
// clang-format off
#define KEYMAP(X)                          \
  /*  id             key               */  \
  X(DPAD_UP,         kVK_UpArrow)          \
  X(DPAD_DOWN,       kVK_DownArrow)        \
  X(DPAD_LEFT,       kVK_LeftArrow)        \
  X(DPAD_RIGHT,      kVK_RightArrow)       \
  X(RED,             kVK_ANSI_X)           \
  X(GREEN,           kVK_ANSI_Z)           \
  X(BLUE,            kVK_ANSI_S)           \
  X(YELLOW,          kVK_ANSI_A)           \
  X(LEFT_TRIGGER,    kVK_ANSI_D)           \
  X(RIGHT_TRIGGER,   kVK_ANSI_C)           \
  X(CIRCLE,          kVK_Return)           \
  X(SQUARE,          kVK_ANSI_Quote)       \
  X(DIAMOND,         kVK_Tab)
// clang-format on

#define X(id, key) [BTN_IDX_##id] = (key),
static const CGKeyCode button_keycode[BTN_IDX_COUNT] = {KEYMAP(X)};
#undef X

// Counting fragment: expands to `0 +1 +1 ...`, so it can't be parenthesized.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define X(id, key) +1
_Static_assert(0 KEYMAP(X) == BTN_IDX_COUNT, "keymap must list every button");
#undef X

typedef struct keyboard_output {
  uint16_t held; // buttons currently posted as down
} keyboard_output;

/// Sends a keyboard message saying that the given key was down or up.
static void key_post(CGKeyCode key, bool down) {
  // Tidy matches our names against CoreGraphics' virtualKey/keyDown.
  // NOLINTNEXTLINE(readability-suspicious-call-argument)
  CGEventRef ev = CGEventCreateKeyboardEvent(NULL, key, down);
  if (!ev)
    return;
  LOG(LOG_KEYS, "%{public}s %u", down ? "down" : "up", key);
  CGEventPost(kCGHIDEventTap, ev);
  CFRelease(ev);
}

/// Determines which keys to press or release based off the passed buttons.
static void keyboard_apply(keyboard_output *out, uint16_t buttons) {
  // Compare what we last told the OS was held with the new snapshot from the
  // pad.
  gamepad_edges e = gamepad_diff(out->held, buttons);
  // Find the lowest bit set in the pressed and released sets and turn it into a
  // keycode. Then, post that keycode. We'll do this for both presses and
  // releases.
  for (uint16_t m = e.released; m; m &= m - 1)
    key_post(button_keycode[__builtin_ctz(m)], /*down = */ false);
  for (uint16_t m = e.pressed; m; m &= m - 1)
    key_post(button_keycode[__builtin_ctz(m)], /*down = */ true);
  // Store the snapshot so the next call compares against it.
  out->held = buttons;
}

/// Represents the position of the mouse.
typedef struct mouse_point {
  double x;
  double y;
} mouse_point;

/// Represents the space in which the mouse may be present.
typedef struct mouse_rect {
  double x_min;
  double x_max;
  double y_min;
  double y_max;
} mouse_rect;

#define MOUSE_GAIN 1

/// Return the bounds for the display.
/// Note: Assumes one display.
static mouse_rect mouse_bounds(void) {
  CGRect r = CGDisplayBounds(CGMainDisplayID());
  return (mouse_rect){
      .x_min = CGRectGetMinX(r),
      .x_max = CGRectGetMaxX(r) - 1, // MaxX is the edge past the last pixel
      .y_min = CGRectGetMinY(r),
      .y_max = CGRectGetMaxY(r) - 1,
  };
}

/// Get the mouse position on the display.
static mouse_point mouse_position(void) {
  CGEventRef ev = CGEventCreate(NULL);
  CGPoint p = ev ? CGEventGetLocation(ev) : CGPointZero;
  if (ev)
    CFRelease(ev);
  return (mouse_point){.x = p.x, .y = p.y};
}

/// Clamp v to [lo, hi].
static double clamp(double v, double lo, double hi) {
  return fmin(fmax(v, lo), hi);
}

/// Update the position p to the next mouse position, clamped to the given
/// bounds.
static mouse_point mouse_step(mouse_point p, int sdx, int sdy,
                              mouse_rect bounds) {
  p.x = clamp(p.x + (double)sdx, bounds.x_min, bounds.x_max);
  p.y = clamp(p.y + (double)sdy, bounds.y_min, bounds.y_max);
  return p;
}

/// Post a mouse event to the OS, so we can move the mouse.
static void mouse_post(mouse_point p, int sdx, int sdy) {
  LOG(LOG_MOUSE, "mouse_point: (%f, %f), scaled dx = %d, scaled dy = %d", p.x,
      p.y, sdx, sdy);
  CGEventRef ev = CGEventCreateMouseEvent(
      NULL, kCGEventMouseMoved, (CGPoint){p.x, p.y}, kCGMouseButtonLeft);
  if (!ev)
    return;
  CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, sdx);
  CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, sdy);
  CGEventPost(kCGHIDEventTap, ev);
  CFRelease(ev);
}

/// Holds runtime state read by callbacks as the program is running.
typedef struct app_context {
  libusb_device_handle *lusb_handle;
  gamepad_state state;
  uint64_t last_command_ns;
  bool stop_requested;      // e.g. ctrl-c
  keyboard_output keyboard; // keys we've told the OS we're holding
} app_context;

/// Signals to handle, so we don't end up with a stuck pad.
static const int caught_signals[] = {SIGINT, SIGTERM, SIGHUP};
#define CAUGHT_SIGNAL_COUNT (sizeof(caught_signals) / sizeof(caught_signals[0]))

/// Holds owned handles that need to be cleaned up.
typedef struct app_resources {
  IOHIDManagerRef hid;
  libusb_context *lusb_ctx;
  dispatch_source_t signal_sources[CAUGHT_SIGNAL_COUNT];
  CFRunLoopTimerRef timer;
  // True if we tried to update the applejack handler.
  bool applejack_handler_touched;
  // True if we tried to update the iMate's autopoll.
  bool imate_autopoll_touched;
  // Number of setup steps that ran, up to failure. Teardown will undo these
  // steps.
  size_t setup_steps_attempted;
} app_resources;

/// Stops the run loop so main can restore the pad.
static void on_signal(void *context) {
  ((app_context *)context)->stop_requested = true;
  CFRunLoopStop(CFRunLoopGetMain());
}

/// Maximum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MAX 192
/// Minimum length of pretty printing string for buttons.
#define BTN_PRETTY_PRINT_MIN 7
_Static_assert(BTN_PRETTY_PRINT_MIN >= sizeof("(none)"),
               "need to fit '(none)' at the minimum");

/// Change a set of buttons into a human-readable string.
static bool gamepad_buttons_to_string(char *out_buf, size_t size,
                                      uint16_t buttons) {
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

static const uint8_t *applejack_reply_data(const uint8_t *report, CFIndex len) {
  if (len != IMATE_REPLY_SIZE)
    return NULL;
  if (report[IMATE_REPLY_COMMAND_OFFSET] != APPLEJACK_COMMAND_TALK_R0)
    return NULL;
  if (report[IMATE_REPLY_PACKET_OFFSET] != IMATE_REPLY_FIRST_PACKET)
    return NULL;
  if (report[IMATE_REPLY_LENGTH_OFFSET] !=
      APPLEJACK_REGISTER_0_SIZE) // single packet
    return NULL;
  return report + IMATE_REPLY_DATA_OFFSET;
}

/// Decode button state from the pad and output it in out_state.
static void applejack_decode(const uint8_t data[APPLEJACK_REGISTER_0_SIZE],
                             gamepad_state *out_state) {
  uint16_t buttons = 0;
  size_t len = sizeof(button_map) / sizeof(button_map[0]);
  for (size_t i = 0; i < len; ++i) {
    const gamepad_button_mapping *m = &button_map[i];
    if (!(data[m->byte] & m->mask))
      buttons |= m->button;
  }
  out_state->buttons = buttons;
  out_state->dx = applejack_motion_delta(data[APPLEJACK_MOTION_X_BYTE]);
  out_state->dy = applejack_motion_delta(data[APPLEJACK_MOTION_Y_BYTE]);
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
  LOG(LOG_REPORTS, "iface %d reply %{public}.*P", iface2int, (int)report_length,
      report);
  app_context *app_ctx = context;
  // Decode the buttons in the report and print the updated state.
  const uint8_t *data = applejack_reply_data(report, report_length);
  if (!data)
    return;
  applejack_decode(data, &app_ctx->state);
  keyboard_apply(&app_ctx->keyboard, app_ctx->state.buttons);
  if (os_log_info_enabled(logs[LOG_STATE])) {
    char names[BTN_PRETTY_PRINT_MAX];
    if (gamepad_buttons_to_string(names, sizeof(names), app_ctx->state.buttons))
      LOG(LOG_STATE, "gamepad_buttons: buttons = %{public}s", names);
    else
      fprintf(stderr, "gamepad_buttons: Couldn't compute button name\n");
  }
  int8_t dx = app_ctx->state.dx;
  int8_t dy = app_ctx->state.dy;
  if (dx || dy) {
    LOG(LOG_STATE, "gamepad_trackball: dx = %d, dy = %d", dx, dy);
    mouse_rect bounds = mouse_bounds(); // don't cache in case the display dies
    mouse_point curr_mouse_pos = mouse_position();
    int sdx = dx * MOUSE_GAIN;
    int sdy = dy * MOUSE_GAIN;
    mouse_point next_mouse_pos = mouse_step(curr_mouse_pos, sdx, sdy, bounds);
    mouse_post(next_mouse_pos, sdx, sdy);
  }
}

static uint64_t now_ns(void) {
  return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

/// True if at least gap_ms has passed since the previous command.
static bool imate_ready(const app_context *app_ctx, uint32_t gap_ms) {
  return now_ns() - app_ctx->last_command_ns >=
         (uint64_t)gap_ms * NSEC_PER_MSEC;
}

/// Send a command to the iMate.
/// Respects minimum spacing after the previous command.
/// Must never be called from on_report or a timer callback with a nonzero wait.
/// Returns 0 on success, or the libusb error code.
/// TODO: remove gap_ms hack
static int imate_send(app_context *app_ctx, uint32_t gap_ms, uint8_t request,
                      uint16_t value, uint16_t index, uint8_t *data,
                      uint16_t length) {
  // pump until the device is ready
  while (!imate_ready(app_ctx, gap_ms))
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
  int rc =
      libusb_control_transfer(app_ctx->lusb_handle, IMATE_REQUEST_TYPE, request,
                              value, index, data, length, IMATE_TIMEOUT_MS);
  app_ctx->last_command_ns = now_ns(); // stamp after transfer returns
  if (rc < 0) {
    fprintf(stderr,
            "error: iMate command (request %u, value 0x%04x) failed: %s\n",
            request, value, libusb_error_name(rc));
    return rc;
  }
  return rc == length ? 0 : LIBUSB_ERROR_IO; // short transfer is an error
}

/// Send an ADB command through the iMate that tells the device at the given
/// address to switch to the given handler ID.
///
/// Blocks.
///
/// Returns true if the full message was sent.
static bool adb_set_handler(app_context *app_ctx, uint8_t adb_address,
                            uint8_t handler_id) {
  // Listen to register 3.
  uint8_t register_contents[ADB_REGISTER_SIZE] = {
      ADB_R3_SRQ_ENABLE | adb_address, handler_id};
  uint8_t adb_command_byte =
      (adb_address << ADB_ADDRESS_SHIFT) | ADB_COMMAND_LISTEN | ADB_REGISTER_3;
  return imate_send(app_ctx, IMATE_COMMAND_GAP_MS, IMATE_REQUEST_ADB_COMMAND,
                    adb_command_byte, 0, register_contents,
                    sizeof(register_contents)) == 0;
}

/// Toggles autopolling for the pad.
static bool imate_set_autopoll(app_context *app_ctx, bool enabled) {
  return imate_send(app_ctx, IMATE_COMMAND_GAP_MS, IMATE_REQUEST_CONTROL,
                    IMATE_CONTROL_AUTOPOLL,
                    enabled ? IMATE_AUTOPOLL_ON : IMATE_AUTOPOLL_OFF, NULL,
                    0) == 0;
}

static void applejack_poll(CFRunLoopTimerRef timer, void *info) {
  (void)timer;
  app_context *app_ctx = info;
  if (!imate_ready(app_ctx, IMATE_POLL_GAP_MS))
    return;
  int rc = imate_send(app_ctx, IMATE_POLL_GAP_MS, IMATE_REQUEST_ADB_COMMAND,
                      APPLEJACK_COMMAND_TALK_R0, 0, NULL, 0);
  if (rc) {
    fprintf(stderr, "error: Couldn't send talk r0 to the pad\n");
    // Send failure to cleanup.
    CFRunLoopStop(CFRunLoopGetCurrent());
  }
}

/// Create the HID manager in app_rsrc for the iMate's vendor/product ID.
/// Requires root.
///
/// app_ctx is used for the reporting callback.
///
/// Returns true on success, false otherwise.
///
/// On failure, app_rsrc->hid may still be set. imate_hid_close releases it.
static bool imate_hid_open(app_resources *app_rsrc, app_context *app_ctx) {
  int vid = IMATE_VENDOR_ID;
  int pid = IMATE_PRODUCT_ID;
  // FIXME: can we avoid this?
  if (geteuid() != 0) {
    fprintf(stderr, "error: run with sudo; macOS gets in our way\n");
    return false;
  }
  app_rsrc->hid =
      IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!app_rsrc->hid) {
    fprintf(stderr, "error: Couldn't create IOHIDManager; bailing.\n");
    return false;
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
  IOHIDManagerSetDeviceMatching(app_rsrc->hid, match);
  // Matching dictionary is set up. Clear the stuff we don't need.
  CFRelease(vid_num);
  CFRelease(pid_num);
  CFRelease(match);
  // Initialize reporting callback and attach it to the current thread.
  IOHIDManagerRegisterInputReportCallback(app_rsrc->hid, on_report,
                                          /*context =*/app_ctx);
  IOHIDManagerScheduleWithRunLoop(app_rsrc->hid, CFRunLoopGetCurrent(),
                                  kCFRunLoopDefaultMode);
  // Open it and check the result.
  // Requires root, so the OS gets out of our way.
  IOReturn rc = IOHIDManagerOpen(app_rsrc->hid, kIOHIDOptionsTypeSeizeDevice);
  if (rc != kIOReturnSuccess) {
    fprintf(stderr, "error: IOHIDManagerOpen failed: 0x%x\n", rc);
    return false;
  }
  return true;
}

/// Initialize libusb and open the iMate by vendor/product ID.
///
/// Stores the context in app_rsrc->lusb_ctx and the device handle in
/// app_ctx->lusb_handle.
///
/// Returns true on success.
static bool imate_usb_open(app_resources *app_rsrc, app_context *app_ctx) {
  if (libusb_init(&app_rsrc->lusb_ctx)) {
    fprintf(stderr, "error: Failed to init libusb\n");
    return false;
  }
  app_ctx->lusb_handle = libusb_open_device_with_vid_pid(
      app_rsrc->lusb_ctx, IMATE_VENDOR_ID, IMATE_PRODUCT_ID);
  if (!app_ctx->lusb_handle) {
    fprintf(stderr, "error: Failed to create libusb device handle\n");
    return false;
  }
  return true;
}

/// Route SIGINT, SIGTERM and SIGHUP to on_signal so we can restore the pad
/// before exiting.
///
/// Returns true on success. On failure, sources created so far remain in
/// app_rsrc for teardown.
static bool install_signal_handlers(app_resources *app_rsrc,
                                    app_context *app_ctx) {
  for (size_t i = 0; i < CAUGHT_SIGNAL_COUNT; ++i) {
    // Make sure default action doesn't kill us.
    signal(caught_signals[i], SIG_IGN);
    app_rsrc->signal_sources[i] =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, caught_signals[i],
                               0, dispatch_get_main_queue());
    if (!app_rsrc->signal_sources[i]) {
      fprintf(stderr, "error: Could not create dispatch source for signal\n");
      return false;
    }
    dispatch_set_context(app_rsrc->signal_sources[i], app_ctx);
    dispatch_source_set_event_handler_f(app_rsrc->signal_sources[i], on_signal);
    dispatch_resume(app_rsrc->signal_sources[i]);
  }
  return true;
}

/// Try to set the AppleJack to extended mode.
/// Returns true on success, false otherwise.
/// Always sets app_rsrc->applejack_handler_touched.
static bool applejack_try_set_extended_mode(app_resources *app_rsrc,
                                            app_context *app_ctx) {
  // Even if we fail, we may have communicated with the pad.
  // Note that.
  app_rsrc->applejack_handler_touched = true;
  if (!adb_set_handler(app_ctx, APPLEJACK_ADB_ADDRESS,
                       APPLEJACK_HANDLER_EXTENDED)) {
    fprintf(stderr, "error: Could not set pad handler\n");
    return false;
  }
  return true;
}

/// Try to set up autopolling on the iMate.
/// Returns true on success, false otherwise.
/// Always sets app_rsrc->imate_autopoll_touched.
static bool imate_try_setup_autopoll(app_resources *app_rsrc,
                                     app_context *app_ctx) {
  app_rsrc->imate_autopoll_touched = true;
  if (!imate_set_autopoll(app_ctx, /* enabled = */ false)) {
    fprintf(stderr, "error: Could not disable autopoll\n");
    return false;
  }
  return true;
}

// polling interval in seconds
#define IMATE_POLL_INTERVAL_S 0.010

/// Create the timer that polls the pad and add it to the current run
/// loop.
///
/// Stores the timer in app_rsrc->timer.
///
/// app_ctx is the timer callback's context and must outlive it.
///
/// Returns true on success.
static bool setup_timer(app_resources *app_rsrc, app_context *app_ctx) {
  // version, info, retain, release, description
  CFRunLoopTimerContext run_timer_ctx = {0, app_ctx, NULL, NULL, NULL};
  app_rsrc->timer = CFRunLoopTimerCreate(
      kCFAllocatorDefault,
      CFAbsoluteTimeGetCurrent() + IMATE_POLL_INTERVAL_S, // first fire
      IMATE_POLL_INTERVAL_S,                              // repeat interval
      0, 0,                                               // flags, order
      applejack_poll, &run_timer_ctx);
  if (!app_rsrc->timer) {
    fprintf(stderr, "error: Could not create run loop timer\n");
    return false;
  }
  CFRunLoopAddTimer(CFRunLoopGetCurrent(), app_rsrc->timer,
                    kCFRunLoopDefaultMode);
  return true;
}

/// Release the HID manager, if one was created.
/// Also return the seized device to the OS.
static void imate_hid_close(app_resources *app_rsrc, app_context *app_ctx) {
  (void)app_ctx;
  if (app_rsrc->hid)
    CFRelease(app_rsrc->hid);
}

/// Close the iMate's libusb handle and then its context, if they were opened.
static void imate_usb_close(app_resources *app_rsrc, app_context *app_ctx) {
  if (app_ctx->lusb_handle)
    libusb_close(app_ctx->lusb_handle);
  if (app_rsrc->lusb_ctx)
    libusb_exit(app_rsrc->lusb_ctx);
}

/// Cancel and release the signal sources that were created, newest first.
static void remove_signal_handlers(app_resources *app_rsrc,
                                   app_context *app_ctx) {
  (void)app_ctx;
  for (size_t i = CAUGHT_SIGNAL_COUNT; i-- > 0;) {
    if (!app_rsrc->signal_sources[i])
      continue;
    dispatch_source_cancel(app_rsrc->signal_sources[i]);
    dispatch_release(app_rsrc->signal_sources[i]);
  }
}

/// Put the AppleJack back in its default mode, if we touched it.
static void applejack_restore_default_mode(app_resources *app_rsrc,
                                           app_context *app_ctx) {
  if (app_rsrc->applejack_handler_touched &&
      !adb_set_handler(app_ctx, APPLEJACK_ADB_ADDRESS,
                       APPLEJACK_HANDLER_DEFAULT))
    fprintf(stderr,
            "warning: Pad may still be in extended mode; replug the iMate\n");
}

/// Re-enable autopolling on the iMate, if we touched it.
static void imate_restore_autopoll(app_resources *app_rsrc,
                                   app_context *app_ctx) {
  if (app_rsrc->imate_autopoll_touched &&
      !imate_set_autopoll(app_ctx, /* enabled = */ true))
    fprintf(stderr, "warning: Could not re-enable autopoll on iMate\n");
}

/// Stop and release the polling timer, if there is one, then release any keys
/// we were holding so nothing stays pressed.
static void teardown_timer(app_resources *app_rsrc, app_context *app_ctx) {
  if (app_rsrc->timer) {
    CFRunLoopTimerInvalidate(app_rsrc->timer);
    CFRelease(app_rsrc->timer);
  }
  keyboard_apply(&app_ctx->keyboard, /*buttons =*/0);
}

/// Defines a step in the app setup/teardown.
///
/// Each step is something that needs to happen to set up the device/app, along
/// with its buddy teardown step.
typedef struct setup_step {
  /// Function used to set up a resource that must be torn down.
  bool (*up)(app_resources *, app_context *);

  /// Buddy teardown. Must be safe to call after a partial up.
  void (*down)(app_resources *, app_context *);
} setup_step;

/// Setup steps and buddy teardown steps, in order.
/// Teardown happens in opposite order.
static const setup_step setup_steps[] = {
    {imate_hid_open, imate_hid_close},
    {imate_usb_open, imate_usb_close},
    {install_signal_handlers, remove_signal_handlers},
    {applejack_try_set_extended_mode, applejack_restore_default_mode},
    {imate_try_setup_autopoll, imate_restore_autopoll},
    {setup_timer, teardown_timer},
};
#define SETUP_STEP_COUNT (sizeof(setup_steps) / sizeof(setup_steps[0]))

/// Run each setup step in order, stopping at the first failure.
///
/// Records in app_rsrc->setup_steps_attempted how far it got, counting the
/// step that failed, since it may have partly succeeded.
///
/// Returns true if every step succeeded.
static bool app_setup(app_resources *app_rsrc, app_context *app_ctx) {
  for (size_t i = 0; i < SETUP_STEP_COUNT; ++i) {
    app_rsrc->setup_steps_attempted = i + 1;
    if (!setup_steps[i].up(app_rsrc, app_ctx))
      return false;
  }
  return true;
}

/// Undo every setup step that app_setup attempted, newest first.
static void app_teardown(app_resources *app_rsrc, app_context *app_ctx) {
  for (size_t i = app_rsrc->setup_steps_attempted; i-- > 0;)
    setup_steps[i].down(app_rsrc, app_ctx);
}

int main(void) {
  log_init();
  app_context app_ctx = {0};
  app_resources app_rsrc = {0};
  int status = 1;

  // Set up all the devices and install the polling timer.
  // If anything fails, tear down whatever was attempted.
  if (app_setup(&app_rsrc, &app_ctx)) {
    if (!app_ctx.stop_requested)
      CFRunLoopRun();
    status = 0;
  }
  app_teardown(&app_rsrc, &app_ctx);
  return status;
}
