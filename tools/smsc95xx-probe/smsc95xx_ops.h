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

/* Program the station address into ADDRL/ADDRH. */
int smsc95xx_set_mac(usb_device *d, const uint8_t mac[6]);

/* Run the full initialization sequence and enable TX and RX.
 *
 * The sequence itself lives in common/smsc95xx_init.c as smsc95xx_init_seq(), shared
 * verbatim with the DriverKit extension so the two cannot drift; this is a thin
 * adapter over it. Behaviour, including the IOReturn values, is unchanged.
 *
 * Follows the captured Linux bring-up, with two deliberate omissions (documented
 * in common/smsc95xx_init.h): checksum offload (COE_CR) is left off, because it is
 * out of scope and would add two bytes to every received frame; and the interrupt
 * endpoint (INT_EP_CTL) is left disabled, because link state is polled instead.
 *
 * Configures the MAC for 10 Mb/s half duplex -- RCVOWN set, FDPX clear, and the
 * low nibble of AFC_CFG set -- which is what this hardware runs at; it has no
 * autonegotiation. When `promiscuous` is true, PRMS is set so the probe can see
 * frames not addressed to it, which is what makes RX testing practical. */
int smsc95xx_init(usb_device *d, const uint8_t mac[6], bool promiscuous);

/* Read BMSR bit 2 and report it.
 *
 * NOTE: on this hardware that bit is NOT a reliable indication of link state.
 * With the 10BASE-T1S cable physically unplugged it still reads set, verified
 * over twelve consecutive direct reads. T1S is a multidrop bus with no
 * continuous idle signalling, so a clause-22 PHY has nothing to detect while
 * the medium is quiet; genuine link and PLCA status live in clause-45 MMD
 * registers. Treat this as a raw register readout, not as link truth, and do
 * not gate anything on it. */
int smsc95xx_link_up(usb_device *d, bool *up);

#endif /* SMSC95XX_OPS_H */
