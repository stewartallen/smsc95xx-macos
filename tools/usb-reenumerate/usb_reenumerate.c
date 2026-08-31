/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Force a USB device to re-enumerate, so IOKit re-runs driver matching against it.
 *
 * Newly installed or replaced dext personalities are only considered when a device
 * re-enumerates. Without this, every iteration of a driver change needs someone to
 * physically unplug and replug the dongle -- which during M3 meant a dozen manual
 * round-trips. USBDeviceReEnumerate() produces the same effect in software.
 *
 *   usb-reenumerate 0424 9905
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <vid-hex> <pid-hex>    e.g. %s 0424 9905\n", argv[0], argv[0]);
        return 2;
    }
    long vid = strtol(argv[1], NULL, 16), pid = strtol(argv[2], NULL, 16);

    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    if (!m) { fprintf(stderr, "IOServiceMatching failed\n"); return 1; }
    SInt32 v = (SInt32)vid, p = (SInt32)pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR("idVendor"), nv);
    CFDictionarySetValue(m, CFSTR("idProduct"), np);
    CFRelease(nv); CFRelease(np);

    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed\n"); return 1;
    }
    io_service_t svc = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!svc) { fprintf(stderr, "no device %04lx:%04lx\n", vid, pid); return 1; }

    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
            kIOCFPlugInInterfaceID, &plug, &score) != KERN_SUCCESS || !plug) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService failed\n"); IOObjectRelease(svc); return 1;
    }
    IOObjectRelease(svc);

    IOUSBDeviceInterface942 **dev = NULL;
    if ((*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
                                (LPVOID *)&dev) != S_OK || !dev) {
        fprintf(stderr, "QueryInterface failed\n"); IODestroyPlugInInterface(plug); return 1;
    }
    /* Release the plug-in, do not IODestroyPlugInInterface it: Destroy calls the
     * plug-in's Stop, and the device interface obtained above is derived from it
     * and is still needed below. */
    (*plug)->Release(plug);

    /* USBDeviceReEnumerate requires an open device, so a failed open is fatal
     * rather than something to continue past. kIOReturnExclusiveAccess here means
     * a driver already owns the device -- expected once our own dext has attached. */
    IOReturn kr = (*dev)->USBDeviceOpen(dev);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "USBDeviceOpen failed: 0x%x%s\n", kr,
                kr == kIOReturnExclusiveAccess ? " (device already claimed by a driver)" : "");
        (*dev)->Release(dev);
        return 1;
    }
    kr = (*dev)->USBDeviceReEnumerate(dev, 0);
    printf("USBDeviceReEnumerate -> 0x%x %s\n", kr, kr == kIOReturnSuccess ? "OK" : "FAILED");
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return kr == kIOReturnSuccess ? 0 : 1;
}
