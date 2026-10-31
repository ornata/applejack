#include <CoreFoundation/CoreFoundation.h> // CFDictionary, CFNumber, CFRunLoop
#include <IOKit/hid/IOHIDKeys.h> // kIOHIDVendorIDKey, kIOHIDProductIDKey, ...
#include <IOKit/hid/IOHIDManager.h> // manager, matching, callbacks
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/// Represents the gamepad state as collected from the adapter.
typedef struct gamepad_state {
  int32_t x, y;    // accumulated trackball motion
  uint32_t other;  // count of "unknown button" events
  uint8_t buttons; // bit0 = left trigger, bit1 = right trigger, bit2 = unused
} gamepad_state;

/// Returns true if a and b are the same gamepad state, false otherwise.
static bool equal_state(gamepad_state a, gamepad_state b) {
  return a.x == b.x && a.y == b.y && a.other == b.other &&
         a.buttons == b.buttons;
}

/// Returns the overflow limit that should be applied to a 32-bit value when
/// adding an 8-bit signed value v. Assumes that v is nonzero.
static int32_t overflow_limit_8_32(int8_t v) {
  assert(v && "v shouldn't ever be 0?!");
  return v > 0 ? INT32_MAX : INT32_MIN;
}

/// Decodes what happened with the gamepad when a button was pressed, the
/// trackball was moved, etc. The updated state is returned in \p outState.
/// Returns true on success, false otherwise.
static bool pad_decode(gamepad_state prevState, const uint8_t *report,
                       CFIndex reportLength, gamepad_state *outState) {
  // Only accept a mouse-sized report.
  if (reportLength != 3)
    return false;
  // Mask out which buttons came through via 0b00..0111
  uint8_t buttons = report[0] & 0x07;
  int8_t dx = (int8_t)report[1]; // change in x
  int8_t dy = (int8_t)report[2]; // change in y
  outState->buttons = buttons;
  outState->other = prevState.other; // pick up where we left off for other
  // Saturate on overflow when updating trackball position.
  // Probably never happens, but let's avoid UB.
  if (__builtin_add_overflow(prevState.x, dx, &outState->x))
    outState->x = overflow_limit_8_32(dx);
  if (__builtin_add_overflow(prevState.y, dy, &outState->y))
    outState->y = overflow_limit_8_32(dy);
  // If we have no buttons and no movement, then either we pressed something we
  // don't handle, or we released something. If previously no trigger was held
  // before, we assume the previous state was a release. Therefore, in that
  // case, this must be an unknown/other press.
  if (buttons == 0 && dx == 0 && dy == 0 && prevState.buttons == 0)
    outState->other += 1;
  return true;
}

/// Callback that runs every time we get an input report from the attached
/// device. Used as a probe to check what happens when you press buttons etc.
///
/// When we get an input report, prints:
/// - The interface ID of the sender device
/// - The raw bytes sent by the device.
static void on_report(void *context, IOReturn result, void *sender,
                      IOHIDReportType type, uint32_t reportID, uint8_t *report,
                      CFIndex reportLength) {
  (void)type;
  (void)reportID;

  // When we enter this function, it means we got an IO operation.
  // If there was any sort of error, then output it and bail.
  if (result != kIOReturnSuccess) {
    printf("report error: 0x%x\n", result);
    return;
  }
  assert(sender && "sender was null?!");
  assert(report && "report was null?!");

  // OK. We have some result from some attached sender device.
  // Check if we can get the sender's device interface.
  IOHIDDeviceRef device = (IOHIDDeviceRef)sender;
  CFTypeRef iface = IOHIDDeviceGetProperty(device, CFSTR("bInterfaceNumber"));
  if (!iface) {
    printf("Interface property missing; giving up\n");
    return;
  }

  // OK, we have an interface. Now let's make it printable by turning it into an
  // int. This lets us figure out how the OS interprets the device.
  //
  // If it's 0, we have a keyboard. If it's 1, we have a mouse.
  // We expect a mouse.
  int iface2int = -1;
  if (!CFNumberGetValue(iface, kCFNumberIntType, &iface2int)) {
    printf("Couldn't convert interface number to an int; giving up\n");
    return;
  }

  if (iface2int != 1) {
    printf("Expected a mouse; giving up\n");
    return;
  }

  printf("Interface number: %d, hex: ", iface2int);

  // Now, print out the raw bytes that were sent by the device.
  for (CFIndex i = 0; i < reportLength; ++i) {
    printf("%02x ", report[i]);
  }
  printf("\n");

  // Check for updated gamepad state and report it if anything changed.
  gamepad_state *state = context;
  assert(state && "expected a gamepad state in the context?!");
  gamepad_state old = *state;
  if (!pad_decode(old, report, reportLength, state))
    return;

  if (equal_state(old, *state))
    return;

  // We updated the gamepad state. Print the updated state.
  printf("gamepad state: x = %d, y = %d, other = %u, buttons = %d\n", state->x,
         state->y, state->other, state->buttons);
}

int main(void) {
  IOHIDManagerRef mgr =
      IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!mgr) {
    printf("error: Couldn't create IOHIDManager; bailing.\n");
    return 1;
  }

  // Build the matching dictionary. This lets us say that we only care about
  // some specific attached device. Otherwise, we match against all devices.
  int vid = 0x077d; // Griffin Technology
  int pid = 0x0405; // iMate

  CFNumberRef vidNum =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
  assert(vidNum &&
         "couldn't create vidNum?!"); // throwaway tool; assert is fine.
  CFNumberRef pidNum =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
  assert(pidNum && "couldn't create pidNum?!");

  CFMutableDictionaryRef match = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vidNum);
  CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pidNum);
  IOHIDManagerSetDeviceMatching(mgr, match);

  // We've set up the matching dictionary, so we don't need this stuff anymore.
  CFRelease(vidNum);
  CFRelease(pidNum);
  CFRelease(match);

  // Initialize the gamepad state and pass it along in the context, so it's
  // registered for every callback.
  gamepad_state state = {0};

  // Register a probe callback.
  IOHIDManagerRegisterInputReportCallback(mgr, on_report, /*context =*/&state);

  // Attach it to this thread's run loop.
  IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(),
                                  kCFRunLoopDefaultMode);

  // Open it and check the result.
  IOReturn rc = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
  if (rc != kIOReturnSuccess) {
    printf("error: \n");
    CFRelease(mgr);
    return 1;
  }

  // Block here and fire off callbacks from inside this call.
  CFRunLoopRun();
  CFRelease(mgr);
  return 0;
}
