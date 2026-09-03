/* SPDX-License-Identifier: GPL-2.0 */

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>

#include "SMSC95xxUSBDevice.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xxUSBDevice: " fmt, ##__VA_ARGS__)

struct SMSC95xxUSBDevice_IVars {
    IOUSBHostDevice *device;   /* borrowed from the provider: Close, never release */
};

bool
SMSC95xxUSBDevice::init(void)
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(SMSC95xxUSBDevice_IVars, 1);
    return ivars != nullptr;
}

void
SMSC95xxUSBDevice::free(void)
{
    IOSafeDeleteNULL(ivars, SMSC95xxUSBDevice_IVars, 1);
    super::free();
}

/* Probe for a child interface to decide whether the device is already configured.
 * DriverKit's IOUSBHostDevice has no GetConfiguration() -- that is IOUSBLib's API.
 * Child interfaces exist only once a configuration is selected, so their presence
 * answers the same question. Re-issuing SET_CONFIGURATION terminates existing child
 * interfaces, so only do it when none were found. */
static kern_return_t
CopyFirstInterface(IOUSBHostDevice *device, IOUSBHostInterface **out)
{
    uintptr_t      iter = 0;
    kern_return_t  ret;

    *out = nullptr;

    ret = device->CreateInterfaceIterator(&iter);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = device->CopyInterface(iter, out);
    device->DestroyInterfaceIterator(iter);
    return ret;
}

kern_return_t
IMPL(SMSC95xxUSBDevice, Start)
{
    kern_return_t       ret;
    IOUSBHostInterface *probe = nullptr;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("Start(super) failed: 0x%x", ret);
        return ret;
    }

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Log("provider is not an IOUSBHostDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    /* IOUSBHostDevice::Open takes uintptr_t for arg; IOUSBHostInterface::Open takes
     * uint8_t *. They are not interchangeable. */
    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed: 0x%x", ret);
        /* Null before Stop: ivars->device non-null is what tells Stop to Close, and
         * closing a device that was never opened unbalances the opener accounting. */
        ivars->device = nullptr;
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    ret = CopyFirstInterface(ivars->device, &probe);
    if (ret == kIOReturnSuccess && probe != nullptr) {
        Log("device already configured");
        OSSafeReleaseNULL(probe);
    } else {
        /* matchInterfaces MUST be true: the interface nodes this creates are what
         * SMSC95xxDriver's personality matches against. */
        ret = ivars->device->SetConfiguration(1, true);
        if (ret != kIOReturnSuccess) {
            Log("SetConfiguration(1) failed: 0x%x", ret);
            ivars->device->Close(this, 0);
            ivars->device = nullptr;
            Stop(provider, SUPERDISPATCH);
            return ret;
        }
        Log("device arrived unconfigured; selected configuration 1 with interface matching");
    }

    /* The session is held for this driver's lifetime, NOT released after configuring:
     * holding it open keeps other processes from claiming the device between
     * configuration and the interface driver starting, and it does not hide the
     * interface nodes (they are published in the IOService plane either way). */
    Log("Start: done, holding the device open");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(SMSC95xxUSBDevice, Stop)
{
    if (ivars != nullptr && ivars->device != nullptr) {
        ivars->device->Close(this, 0);
        ivars->device = nullptr;   /* borrowed from the provider: never release */
    }
    Log("Stop");
    return Stop(provider, SUPERDISPATCH);
}
