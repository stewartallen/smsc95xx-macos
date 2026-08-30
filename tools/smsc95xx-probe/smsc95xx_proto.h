/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure encode/decode helpers for the smsc95xx register protocol.
 *
 * Nothing in this header performs I/O. Over integer arguments, all functions are
 * total. Out-parameter functions require their pointer arguments to be non-NULL.
 * No I/O or allocation, so testable on the host without hardware and liftable
 * unchanged into the DriverKit extension later.
 */
#ifndef SMSC95XX_PROTO_H
#define SMSC95XX_PROTO_H

#include <stdbool.h>
#include <stdint.h>

/* Build the MII_ADDR register value that starts a PHY register access.
 * Always sets MII_BUSY; sets MII_WRITE when `write` is true. */
uint32_t smsc95xx_mii_addr_word(uint8_t phy, uint8_t reg, bool write);

/* Build the E2P_CMD value that starts a read of one EEPROM byte. */
uint32_t smsc95xx_e2p_read_cmd(uint16_t offset);

/* Split ID_REV into its chip-id and revision halves. chip and rev must be non-NULL. */
void smsc95xx_id_rev_split(uint32_t id_rev, uint16_t *chip, uint16_t *rev);

/* Pack a six-byte MAC into the ADDRL/ADDRH register pair, and unpack it.
 * addrl, addrh, and mac must be non-NULL. */
void smsc95xx_mac_to_regs(const uint8_t mac[6], uint32_t *addrl, uint32_t *addrh);
void smsc95xx_regs_to_mac(uint32_t addrl, uint32_t addrh, uint8_t mac[6]);

/* True only for the EEPROM signature byte that marks a trustworthy read.
 *
 * A read that does not carry this byte at offset 0 must be discarded however
 * plausible the rest looks: on marginal hardware a failed read is systematically
 * shifted rather than random, so it is stable across re-reads and passes ordinary
 * pattern checks. See the README's Known findings. */
bool smsc95xx_eeprom_sig_valid(uint8_t sig);

/* Decode BMSR. */
bool smsc95xx_bmsr_link_up(uint16_t bmsr);
bool smsc95xx_bmsr_autoneg_capable(uint16_t bmsr);

#endif /* SMSC95XX_PROTO_H */
