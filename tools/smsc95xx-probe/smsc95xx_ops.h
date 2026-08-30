/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smsc95xx register, MII, and EEPROM operations over a usb_device.
 *
 * All functions return an IOReturn; 0 means success.
 */
#ifndef SMSC95XX_OPS_H
#define SMSC95XX_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "usb_backend.h"

int smsc95xx_read_reg(usb_device *d, uint16_t offset, uint32_t *value);
int smsc95xx_write_reg(usb_device *d, uint16_t offset, uint32_t value);

/* Read one PHY register through the MII_ADDR/MII_DATA pair. */
int smsc95xx_mii_read(usb_device *d, uint8_t phy, uint8_t reg, uint16_t *value);

/* Read `len` bytes from the EEPROM starting at `offset`.
 * Returns kIOReturnBadArgument if offset + len > 0x200 (EEPROM size). */
int smsc95xx_eeprom_read(usb_device *d, uint16_t offset, uint8_t *buf, size_t len);

/* Read the six-byte MAC address from its fixed EEPROM location. */
int smsc95xx_read_mac(usb_device *d, uint8_t mac[6]);

#endif /* SMSC95XX_OPS_H */
