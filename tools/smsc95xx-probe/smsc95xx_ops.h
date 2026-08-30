/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smsc95xx register, MII, and EEPROM operations over a usb_device.
 *
 * All functions return an IOReturn; 0 means success.
 */
#ifndef SMSC95XX_OPS_H
#define SMSC95XX_OPS_H

#include <stdbool.h>
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

/* Read the six-byte MAC address from its fixed EEPROM location.
 *
 * This performs the raw read only. It does NOT verify the EEPROM signature, so
 * on marginal hardware it can return systematically corrupted data that still
 * looks like a valid address. Prefer smsc95xx_read_mac_verified(). */
int smsc95xx_read_mac(usb_device *d, uint8_t mac[6]);

/* Read the MAC, but only after confirming the EEPROM signature byte at offset 0.
 *
 * Returns kIOReturnSuccess only when the signature matched. On mismatch returns
 * kIOReturnNotReadable and reports the byte actually seen via *sig_out (which may
 * be NULL). This is the check that distinguishes a genuine read from a
 * mis-clocked one, which no amount of re-reading or pattern validation can. */
int smsc95xx_read_mac_verified(usb_device *d, uint8_t mac[6], uint8_t *sig_out);

/* Read E2P_CMD and report whether the power-on EEPROM auto-load succeeded. */
int smsc95xx_eeprom_loaded(usb_device *d, bool *loaded);

#endif /* SMSC95XX_OPS_H */
