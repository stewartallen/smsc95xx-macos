/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LAN9500A register map. Offsets and bit masks follow the Linux smsc95xx
 * driver's definitions; values observed on real hardware are recorded in
 * reference/mach-init-sequence.txt or reference/evb-init-sequence.txt.
 */
#ifndef SMSC95XX_REGS_H
#define SMSC95XX_REGS_H

/* Vendor control requests. */
#define SMSC95XX_REQ_WRITE_REGISTER 0xA0
#define SMSC95XX_REQ_READ_REGISTER  0xA1

/* Register offsets, passed in wIndex. */
#define SMSC95XX_REG_ID_REV       0x000
#define SMSC95XX_REG_INT_STS      0x008
#define SMSC95XX_REG_RX_CFG       0x00C
#define SMSC95XX_REG_TX_CFG       0x010
#define SMSC95XX_REG_HW_CFG       0x014
#define SMSC95XX_REG_PM_CTRL      0x020
#define SMSC95XX_REG_LED_GPIO_CFG 0x024
#define SMSC95XX_REG_AFC_CFG      0x02C
#define SMSC95XX_REG_E2P_CMD      0x030
#define SMSC95XX_REG_E2P_DATA     0x034
#define SMSC95XX_REG_BURST_CAP    0x038
#define SMSC95XX_REG_INT_EP_CTL   0x068
#define SMSC95XX_REG_BULK_IN_DLY  0x06C
#define SMSC95XX_REG_MAC_CR       0x100
#define SMSC95XX_REG_ADDRH        0x104
#define SMSC95XX_REG_ADDRL        0x108
#define SMSC95XX_REG_HASHH        0x10C
#define SMSC95XX_REG_HASHL        0x110
#define SMSC95XX_REG_MII_ADDR     0x114
#define SMSC95XX_REG_MII_DATA     0x118
#define SMSC95XX_REG_FLOW         0x11C
#define SMSC95XX_REG_VLAN1        0x120
#define SMSC95XX_REG_COE_CR       0x130

/* MII_ADDR fields. */
#define SMSC95XX_MII_BUSY         0x00000001u
#define SMSC95XX_MII_WRITE        0x00000002u
#define SMSC95XX_MII_PHY_SHIFT    11
#define SMSC95XX_MII_REG_SHIFT    6
#define SMSC95XX_MII_PHY_MASK     0x1Fu
#define SMSC95XX_MII_REG_MASK     0x1Fu

/* E2P_CMD fields. */
#define SMSC95XX_E2P_BUSY         0x80000000u
#define SMSC95XX_E2P_TIMEOUT      0x00000400u
/* LOADED is set when the chip's power-on EEPROM auto-load succeeded. When it is
 * clear, EEPROM reads on marginal hardware can return systematically shifted
 * data that still looks plausible -- see the README's Known findings. */
#define SMSC95XX_E2P_LOADED       0x00000200u
#define SMSC95XX_E2P_ADDR_MASK    0x000001FFu

/* HW_CFG bits. */
#define SMSC95XX_HW_CFG_SRST      0x00000001u
#define SMSC95XX_HW_CFG_BCE       0x00000002u
#define SMSC95XX_HW_CFG_PSEL      0x00000004u
#define SMSC95XX_HW_CFG_LRST      0x00000008u
#define SMSC95XX_HW_CFG_MEF       0x00000020u
#define SMSC95XX_HW_CFG_BIR       0x00001000u

/* MAC_CR bits. */
#define SMSC95XX_MAC_CR_RXEN      0x00000004u
#define SMSC95XX_MAC_CR_TXEN      0x00000008u
#define SMSC95XX_MAC_CR_HPFILT    0x00002000u
#define SMSC95XX_MAC_CR_PRMS      0x00040000u
#define SMSC95XX_MAC_CR_FDPX      0x00100000u
#define SMSC95XX_MAC_CR_RCVOWN    0x00800000u

/* IEEE 802.3 clause 22 PHY registers. */
#define SMSC95XX_MII_BMCR         0x00
#define SMSC95XX_MII_BMSR         0x01
#define SMSC95XX_MII_PHYID1       0x02
#define SMSC95XX_MII_PHYID2       0x03
#define SMSC95XX_MII_ANAR         0x04
#define SMSC95XX_MII_ANLPAR       0x05

/* BMSR bits. */
#define SMSC95XX_BMSR_LINK_UP     0x0004u
#define SMSC95XX_BMSR_AUTONEG_CAP 0x0008u

/* The only MII address this driver uses. See the note in the project README:
 * the two known boards disagree about address decoding and PHY ID, and 0 is
 * the only value that works on both.
 */
#define SMSC95XX_PHY_ADDR         0

/* EEPROM layout: offset 0 holds a fixed signature byte, and the MAC address
 * occupies six bytes starting at offset 1. A read that does not return the
 * signature at offset 0 must not be trusted, however plausible the rest looks. */
#define SMSC95XX_EEPROM_SIG_OFFSET 0x00
#define SMSC95XX_EEPROM_SIGNATURE  0xA5
#define SMSC95XX_EEPROM_MAC_OFFSET 0x01
#define SMSC95XX_MAC_LEN           6

/* MAC address bit: locally administered vs. globally unique. */
#define SMSC95XX_MAC_LOCALLY_ADMINISTERED 0x02u

/* TX framing. Each frame sent on the bulk OUT endpoint is preceded by two
 * little-endian 32-bit command words.
 *
 * Verified against ten frames in reference/mach-bringup.pcap: every one had
 * TX_CMD_A == (FIRST_SEG | LAST_SEG | len) and TX_CMD_B == len, with the bulk
 * transfer being exactly 8 + len bytes and no padding. */
#define SMSC95XX_TX_HEADER_LEN     8
#define SMSC95XX_TX_CMD_A_LEN_MASK 0x000007FFu
#define SMSC95XX_TX_CMD_A_LAST_SEG 0x00001000u
#define SMSC95XX_TX_CMD_A_FIRST_SEG 0x00002000u
#define SMSC95XX_TX_CMD_B_LEN_MASK 0x000007FFu

/* Ethernet frame size bounds, excluding the TX header. Linux is observed
 * transmitting unpadded frames as short as 42 bytes on this hardware, which
 * the chip successfully pads before appending its 4-byte CRC. */
#define SMSC95XX_FRAME_MIN         42
#define SMSC95XX_FRAME_MAX         1514

/* RX framing. Each frame in a bulk IN transfer is preceded by one
 * little-endian 32-bit status word, and each record is padded so the next
 * status word starts on a 4-byte boundary.
 *
 * VERIFIED against measured hardware:
 *   4-byte header and LEN_MASK/LEN_SHIFT -- confirmed on three transfers whose
 *     4 + len exactly equalled the observed size (68/70/108 for len 64/66/104).
 *   length includes the Ethernet CRC.
 *   BROADCAST, MULTICAST, FRAME_TYPE -- confirmed across all three address
 *     classes, which separates the bits from one another rather than just
 *     showing them set:
 *       broadcast (ff:ff:ff:ff:ff:ff)      low bits 0x2420  BC+MC+FT
 *       multicast (33:33:.. / 01:00:5E:..) low bits 0x0420  MC+FT, BC clear
 *       unicast                            low bits 0x0020  FT only
 *     Multicast is the discriminating case: a defect treating BROADCAST and
 *     MULTICAST as one bit would pass the broadcast and unicast samples but
 *     fail here.
 *   FILTER_FAIL and ERROR_SUM were clear on every known-good frame observed,
 *     which is consistent but is not a positive confirmation.
 *
 * NOT VERIFIED (transcribed from Linux smsc95xx, never provoked):
 * LENGTH_ERROR, RUNT, TOO_LONG, COLLISION, WATCHDOG, MII_ERROR, DRIBBLING,
 * CRC_ERROR. These are error conditions awaiting real hardware observations.
 * Also not verified: the multi-frame record padding and 4-byte alignment rule.
 * Only single-record transfers have been received on this hardware; multi-frame
 * transfers are reachable via HW_CFG_INIT_2's MEF enable but have never been
 * observed. The padding calculation (4 - (used % 4)) % 4 comes from the Linux
 * driver and is mirrored in the test builder, so the tests cannot distinguish a
 * correct rule from an incorrect one that both sides agree on. */
#define SMSC95XX_RX_HEADER_LEN     4
#define SMSC95XX_RX_STS_FILTER_FAIL  0x40000000u
#define SMSC95XX_RX_STS_LEN_MASK     0x3FFF0000u
#define SMSC95XX_RX_STS_LEN_SHIFT    16
#define SMSC95XX_RX_STS_ERROR_SUM    0x00008000u
#define SMSC95XX_RX_STS_BROADCAST    0x00002000u
#define SMSC95XX_RX_STS_LENGTH_ERROR 0x00001000u
#define SMSC95XX_RX_STS_RUNT         0x00000800u
#define SMSC95XX_RX_STS_MULTICAST    0x00000400u
#define SMSC95XX_RX_STS_TOO_LONG     0x00000080u
#define SMSC95XX_RX_STS_COLLISION    0x00000040u
#define SMSC95XX_RX_STS_FRAME_TYPE   0x00000020u
#define SMSC95XX_RX_STS_WATCHDOG     0x00000010u
#define SMSC95XX_RX_STS_MII_ERROR    0x00000008u
#define SMSC95XX_RX_STS_DRIBBLING    0x00000004u
#define SMSC95XX_RX_STS_CRC_ERROR    0x00000002u

/* Frame length reported in the status word includes the 4-byte Ethernet CRC,
 * which the hardware does not strip. */
#define SMSC95XX_RX_CRC_LEN        4

/* Values used by the init sequence, taken from the captured Linux bring-up in
 * reference/mach-init-sequence.txt. Both supported devices produced an
 * identical sequence; see the project README. */
#define SMSC95XX_HW_CFG_INIT_1     0x00001004u   /* BIR | PSEL            */
#define SMSC95XX_HW_CFG_INIT_2     0x00001026u   /* BIR | MEF | PSEL | BCE */
#define SMSC95XX_BURST_CAP_INIT    0x00000005u
#define SMSC95XX_BULK_IN_DLY_INIT  0x00002000u
#define SMSC95XX_LED_GPIO_CFG_INIT 0x81110007u
#define SMSC95XX_AFC_CFG_INIT      0x00F830A1u
#define SMSC95XX_AFC_CFG_HALF_DUPLEX_BITS 0x0000000Fu
#define SMSC95XX_VLAN1_INIT        0x00008100u   /* ETH_P_8021Q */
#define SMSC95XX_INT_STS_CLEAR_ALL 0xFFFFFFFFu
#define SMSC95XX_TX_CFG_ON         0x00000004u
#define SMSC95XX_PM_CTRL_PHY_RST   0x00000010u

#endif /* SMSC95XX_REGS_H */
