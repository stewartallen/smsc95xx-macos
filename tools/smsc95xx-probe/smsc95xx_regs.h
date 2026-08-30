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

/* EEPROM layout: the MAC address occupies six bytes starting at offset 1. */
#define SMSC95XX_EEPROM_MAC_OFFSET 0x01
#define SMSC95XX_MAC_LEN           6

/* MAC address bit: locally administered vs. globally unique. */
#define SMSC95XX_MAC_LOCALLY_ADMINISTERED 0x02u

#endif /* SMSC95XX_REGS_H */
