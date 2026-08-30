/* SPDX-License-Identifier: GPL-2.0 */
#include "usb_backend.h"

#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>

struct usb_device {
    IOUSBDeviceInterface942 **intf;
};

static IOUSBDeviceInterface942 **query_device(io_service_t svc)
{
    IOCFPlugInInterface **plugin = NULL;
    IOUSBDeviceInterface942 **intf = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin,
                                          &score) != KERN_SUCCESS || !plugin)
        return NULL;

    (*plugin)->QueryInterface(plugin,
                              CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
                              (LPVOID *)&intf);
    IODestroyPlugInInterface(plugin);
    return intf;
}

usb_device *usb_open_id(uint16_t vid, uint16_t pid, int *kr_out)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) {
        if (kr_out)
            *kr_out = kIOReturnNoDevice;
        return NULL;
    }

    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt16Type, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt16Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), p);
    CFRelease(v);
    CFRelease(p);

    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter)
        != KERN_SUCCESS) {
        if (kr_out)
            *kr_out = kIOReturnNoDevice;
        return NULL;
    }

    usb_device *dev = NULL;
    int last_kr = kIOReturnNoDevice;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        IOUSBDeviceInterface942 **intf = query_device(svc);
        IOObjectRelease(svc);
        if (!intf)
            continue;

        int open_kr = (*intf)->USBDeviceOpen(intf);
        if (open_kr == kIOReturnSuccess) {
            /* The MAC/PHY register block at offsets >= 0x100 is only reachable
             * on a CONFIGURED device; reads of it STALL otherwise, while
             * registers below 0x100 work either way. macOS configures this
             * device in some states but not others -- it does not when the
             * EEPROM auto-load succeeds and the device presents
             * bDeviceProtocol 1, which is the normal case for the EVB and for
             * the MACH unit at 0x9905. So configure it ourselves when needed.
             *
             * Conditional on purpose: re-issuing SET_CONFIGURATION resets
             * endpoint state, so do not do it when macOS already has. */
            UInt8 cfg = 0;
            if ((*intf)->GetConfiguration(intf, &cfg) == kIOReturnSuccess &&
                cfg == 0 &&
                (*intf)->SetConfiguration(intf, 1) != kIOReturnSuccess) {
                last_kr = kIOReturnNotReady;
                (*intf)->USBDeviceClose(intf);
                (*intf)->Release(intf);
                continue;
            }

            dev = calloc(1, sizeof(*dev));
            if (dev) {
                dev->intf = intf;
                if (kr_out)
                    *kr_out = kIOReturnSuccess;
                IOObjectRelease(iter);
                return dev;
            }
            (*intf)->USBDeviceClose(intf);
        } else {
            last_kr = open_kr;
        }
        (*intf)->Release(intf);
    }
    IOObjectRelease(iter);
    if (kr_out)
        *kr_out = last_kr;
    return NULL;
}

usb_device *usb_open_first(uint16_t *vid_out, uint16_t *pid_out, int *kr_out)
{
    static const uint16_t ids[][2] = {
        { SMSC95XX_VID_MACH, SMSC95XX_PID_MACH_EE },
        { SMSC95XX_VID_MACH, SMSC95XX_PID_MACH    },
        { SMSC95XX_VID_EVB,  SMSC95XX_PID_EVB     },
    };

    int last_kr = kIOReturnNoDevice;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int attempt_kr = kIOReturnNoDevice;
        usb_device *dev = usb_open_id(ids[i][0], ids[i][1], &attempt_kr);
        if (dev) {
            if (vid_out) *vid_out = ids[i][0];
            if (pid_out) *pid_out = ids[i][1];
            if (kr_out) *kr_out = kIOReturnSuccess;
            return dev;
        }
        if (attempt_kr != kIOReturnNoDevice)
            last_kr = attempt_kr;
    }
    if (kr_out)
        *kr_out = last_kr;
    return NULL;
}

void usb_close(usb_device *d)
{
    if (!d)
        return;
    if (d->intf) {
        (*d->intf)->USBDeviceClose(d->intf);
        (*d->intf)->Release(d->intf);
    }
    free(d);
}

static int control(usb_device *d, uint8_t direction, uint8_t request,
                   uint16_t value, uint16_t index, void *buf, uint16_t len)
{
    if (!d || !d->intf)
        return kIOReturnBadArgument;

    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(direction, kUSBVendor, kUSBDevice),
        .bRequest      = request,
        .wValue        = value,
        .wIndex        = index,
        .wLength       = len,
        .pData         = buf,
        .wLenDone      = 0,
    };
    int kr = (*d->intf)->DeviceRequest(d->intf, &req);
    if (kr == kIOReturnSuccess && req.wLenDone != len)
        return kIOReturnUnderrun;
    return kr;
}

int usb_control_in(usb_device *d, uint8_t request, uint16_t value,
                   uint16_t index, void *buf, uint16_t len)
{
    return control(d, kUSBIn, request, value, index, buf, len);
}

int usb_control_out(usb_device *d, uint8_t request, uint16_t value,
                    uint16_t index, const void *buf, uint16_t len)
{
    /* DeviceRequest takes a non-const pointer even for OUT transfers. */
    return control(d, kUSBOut, request, value, index, (void *)buf, len);
}

const char *usb_strerror(int kr)
{
    switch (kr) {
    case kIOReturnSuccess:      return "success";
    case kIOReturnNoDevice:     return "no such device";
    case kIOReturnNotOpen:      return "device not open";
    case kIOReturnExclusiveAccess: return "device claimed by another driver";
    case kIOReturnNotPermitted: return "not permitted";
    case kIOReturnTimeout:      return "timed out";
    case kIOReturnBadArgument:  return "bad argument";
    case kIOReturnNotFound:     return "not found";
    case kIOUSBPipeStalled:     return "pipe stalled";
    case kIOUSBTransactionTimeout: return "transaction timeout";
    case kIOReturnUnderrun:     return "short transfer";
    case kIOReturnAborted:      return "operation aborted";
    default:                    return "unknown error";
    }
}
