#include <CoreFoundation/CoreFoundation.h> // CFDictionary, CFNumber, CFRunLoop
#include <IOKit/hid/IOHIDKeys.h> // kIOHIDVendorIDKey, kIOHIDProductIDKey, ...
#include <IOKit/hid/IOHIDManager.h> // manager, matching, callbacks
#include <assert.h>
#include <stdio.h>

/// Callback that runs every time we get an input report from the attached
/// device. Used as a probe to check what happens when you press buttons etc.
///
/// When we get an input report, prints:
/// - The interface ID of the sender device
/// - The raw bytes sent by the device.
static void on_report(void *context, IOReturn result, void *sender,
                      IOHIDReportType type, uint32_t reportID, uint8_t *report,
                      CFIndex reportLength) {
  (void)context;
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
  int iface2int = -1;
  if (!CFNumberGetValue(iface, kCFNumberIntType, &iface2int)) {
    printf("Couldn't convert interface number to an int; giving up\n");
    return;
  }
  // We know this is recognized as a mouse, but let's document it.
  assert(iface2int == 1 && "expected a mouse?!");
  printf("Interface number: %d, hex: ", iface2int);

  // Now, print out the raw bytes that were sent by the device.
  for (CFIndex i = 0; i < reportLength; ++i) {
    printf("%02x ", report[i]);
  }
  printf("\n");
}

int main(void) {
  IOHIDManagerRef mgr =
      IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  if (!mgr) {
    printf("error: Couldn't create IOHIDManager; bailing.");
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

  // Register a probe callback.
  IOHIDManagerRegisterInputReportCallback(mgr, on_report, /*context =*/NULL);

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
