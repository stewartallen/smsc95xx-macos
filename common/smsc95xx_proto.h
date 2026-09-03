/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Stewart Allen
 * Derived from the Linux smsc95xx driver, Copyright (C) 2007-2008 SMSC. See NOTICE.
 *
 * Pure encode/decode helpers for the smsc95xx register protocol.
 *
 * Nothing here performs I/O or allocates, so everything is testable on the host
 * without hardware. Over integer arguments, all functions are total. Out-parameter
 * functions require their pointer arguments to be non-NULL.
 */
#ifndef SMSC95XX_PROTO_H
#define SMSC95XX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* True only for the EEPROM signature byte that marks a trustworthy read. A read that
 * does not carry this byte at offset 0 must be discarded however plausible the rest
 * looks: on marginal hardware a failed read is systematically shifted rather than
 * random, so it is stable across re-reads and passes ordinary pattern checks (see the
 * README's Known findings). */
bool smsc95xx_eeprom_sig_valid(uint8_t sig);

/* Decode BMSR. */
bool smsc95xx_bmsr_link_up(uint16_t bmsr);
bool smsc95xx_bmsr_autoneg_capable(uint16_t bmsr);

/* Write the 8-byte TX command header for a frame of `frame_len` bytes into
 * `buf`, which must have room for at least SMSC95XX_TX_HEADER_LEN bytes.
 *
 * Returns the number of header bytes written (always SMSC95XX_TX_HEADER_LEN) on
 * success, or 0 if `buflen` is too small or `frame_len` is outside
 * [SMSC95XX_FRAME_MIN, SMSC95XX_FRAME_MAX]. Returning 0 rather than truncating
 * keeps a bad length from becoming a malformed transfer. */
size_t smsc95xx_tx_prepend(uint8_t *buf, size_t buflen, size_t frame_len);

/* Iterate the frames in one bulk IN transfer.
 *
 * A single transfer can carry several frames back to back, because the driver
 * enables multiple-Ethernet-frames mode (HW_CFG.MEF). Each record is a 4-byte
 * little-endian status word followed by frame data, padded so the next record
 * starts 4-byte aligned.
 *
 * Call with *offset == 0 and call repeatedly until it returns false. On success
 * *frame points into `buf` (no copy is made), *frame_len is the frame length
 * INCLUDING the trailing 4-byte Ethernet CRC that the hardware leaves in place,
 * and *status is the raw status word so the caller can test error bits.
 *
 * Returns false at the end of the buffer, and also on a malformed record --
 * a truncated status word, or a length that would run past the end of the
 * buffer. Malformed and end-of-buffer are deliberately not distinguished:
 * either way there is nothing more to decode. */
bool smsc95xx_rx_next(const uint8_t *buf, size_t len, size_t *offset,
                      const uint8_t **frame, size_t *frame_len,
                      uint32_t *status);

/* Parse "vid:pid" (hex, optional 0x prefixes) into two 16-bit values.
 * Returns false and leaves output arguments untouched on any malformed input. */
bool smsc95xx_parse_vid_pid(const char *s, uint16_t *vid, uint16_t *pid);

/* Pattern checks applied to a MAC after the EEPROM signature matched; they guard
 * against a signature byte that matched by accident. A locally-administered address is
 * deliberately NOT rejected: the MAC this hardware produces when its EEPROM mis-clocks
 * (4a:f8:f8:c2:c2:f2) is locally administered but unicast, so it passes every pattern
 * check -- which is why provenance, not plausibility, is the real gate. */
typedef enum {
    SMSC95XX_MAC_PLAUSIBLE = 0,
    SMSC95XX_MAC_ALL_ZEROS,
    SMSC95XX_MAC_ALL_ONES,
    SMSC95XX_MAC_MULTICAST
} smsc95xx_mac_check;

smsc95xx_mac_check smsc95xx_mac_plausible(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* SMSC95XX_PROTO_H */
