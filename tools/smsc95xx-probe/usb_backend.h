/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal userspace USB transport over IOUSBLib.
 *
 * Only device-level (endpoint 0) control transfers are supported, which is all
 * the smsc95xx register protocol needs. Because we never claim the interface,
 * this does not conflict with any driver and needs no entitlements.
 */
#ifndef USB_BACKEND_H
#define USB_BACKEND_H

#include <stdint.h>

typedef struct usb_device usb_device;

/* Known LAN9500A-based dongles, tried in order by usb_open_first(). */
#define SMSC95XX_VID_MACH 0x0424
#define SMSC95XX_PID_MACH 0x9E00
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

#endif /* USB_BACKEND_H */
