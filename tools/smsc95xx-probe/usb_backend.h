/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal userspace USB transport over IOUSBLib.
 *
 * Control transfers (endpoint 0) are supported for register access and need no
 * interface claim or entitlements — they coexist with any bound driver. Bulk
 * transfers (on claimed interface) require claiming the interface, which may
 * conflict with an existing driver.
 */
#ifndef USB_BACKEND_H
#define USB_BACKEND_H

#include <stdint.h>

typedef struct usb_device usb_device;

/* Known LAN9500A-based dongles, tried in order by usb_open_first().
 *
 * Note that on this chip family the USB product ID can come FROM the EEPROM: the
 * MACH unit presents 0x9E00 (the chip's hardwired default) when the power-on
 * EEPROM auto-load fails, and 0x9905 (the programmed ID) when it succeeds. Both
 * are the same physical device, so both must be matched. */
#define SMSC95XX_VID_MACH 0x0424
#define SMSC95XX_PID_MACH 0x9E00   /* auto-load failed: chip default  */
#define SMSC95XX_PID_MACH_EE 0x9905 /* auto-load succeeded: from EEPROM */
#define SMSC95XX_VID_EVB  0x184F
#define SMSC95XX_PID_EVB  0x0051

/* Open a specific device, or the first known dongle found. Both return NULL if
 * no matching device is present or it could not be opened. On success
 * usb_open_first() reports which IDs matched. If kr_out is non-NULL, it receives
 * the last significant IOReturn: kIOReturnNoDevice if nothing matched, or the
 * USBDeviceOpen failure code if a device was found but could not be opened. */
usb_device *usb_open_id(uint16_t vid, uint16_t pid, int *kr_out);
usb_device *usb_open_first(uint16_t *vid_out, uint16_t *pid_out, int *kr_out);

void usb_close(usb_device *d);

/* Vendor control transfers. Return an IOReturn; 0 means success. */
int usb_control_in(usb_device *d, uint8_t request, uint16_t value,
                   uint16_t index, void *buf, uint16_t len);
int usb_control_out(usb_device *d, uint8_t request, uint16_t value,
                    uint16_t index, const void *buf, uint16_t len);

/* Human-readable form of an IOReturn from this module. */
const char *usb_strerror(int kr);

/* Claim interface 0 and locate its bulk pipes.
 *
 * Required before any bulk transfer; the control-transfer functions above do
 * NOT need it. Idempotent: calling it twice is harmless. usb_close() releases
 * the interface, so callers never do.
 *
 * Returns kIOReturnSuccess, or an IOReturn describing the failure. In
 * particular kIOReturnExclusiveAccess means another driver has claimed the
 * interface -- which is expected once a dext is installed, and is why the
 * control path deliberately avoids claiming. */
int usb_claim_interface(usb_device *d);

/* Bulk transfer on the claimed interface. usb_claim_interface() must have
 * succeeded first; without it these return kIOReturnNotOpen.
 *
 * Both use a 1000 ms timeout. usb_bulk_in() is given the buffer size in *len
 * and writes the number of bytes actually received back to it; a timeout with
 * no data is reported as kIOReturnTimeout, which for RX simply means no frame
 * arrived and is not necessarily an error. */
int usb_bulk_out(usb_device *d, const void *buf, uint32_t len);
int usb_bulk_in(usb_device *d, void *buf, uint32_t *len);

#endif /* USB_BACKEND_H */
