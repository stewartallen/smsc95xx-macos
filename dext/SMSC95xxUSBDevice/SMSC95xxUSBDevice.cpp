/* SPDX-License-Identifier: GPL-2.0 */

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
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    ret = CopyFirstInterface(ivars->device, &probe);
    if (ret == kIOReturnSuccess && probe != nullptr) {
        Log("device already configured");
        OSSafeReleaseNULL(probe);
    } else {
        /* matchInterfaces MUST be true here: the interface nodes this creates are
         * exactly what SMSC95xxDriver's personality matches against. M4 passed false
         * because one driver owned everything; in the split that would leave the
         * interface driver with nothing to match. */
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

    /* The session is deliberately held for this driver's lifetime, NOT released after
     * configuring. Two reasons, one measured here:
     *
     * Holding it open does not hide the interfaces. Measured on hardware: CopyInterface
     * reports one interface both before and after Close, and the interface node is
     * published in the IOService plane either way. An earlier round of this work closed
     * the device on the theory that exclusive access suppressed interface publication;
     * that theory was wrong, and it came from reading the IOUSB plane, which does not
     * show interface nodes as device children.
     *
     * And it keeps other openers out. This device attracts them: with the session held,
     * the registry shows Google Chrome and cef_server both parked on it as
     * AppleUSBHostDeviceUserClient children, and the log shows their open attempts being
     * refused because the provider is already opened for exclusive access. Releasing the
     * session would let an arbitrary process claim the device between configuration and
     * our own interface driver starting. */
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
