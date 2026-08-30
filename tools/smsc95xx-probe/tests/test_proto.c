/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for the pure protocol layer.
 *
 * Every expected value here was read off real hardware and is present in
 * reference/mach-init-sequence.txt or reference/evb-init-sequence.txt. Do not
 * replace these with hand-derived numbers.
 */
#include "../smsc95xx_proto.h"
#include "../smsc95xx_regs.h"
#include "test_harness.h"

static void test_mii_addr_word(void)
{
    /* From the captured traces: reading PHY0's PHYID1 writes MII_ADDR=0x81,
     * PHYID2 writes 0xC1, BMSR writes 0x41, and PHY1's PHYID1 writes 0x881. */
    CHECK_EQ_U32(smsc95xx_mii_addr_word(0, SMSC95XX_MII_PHYID1, false), 0x81,
                 "PHY0 PHYID1 read");
    CHECK_EQ_U32(smsc95xx_mii_addr_word(0, SMSC95XX_MII_PHYID2, false), 0xC1,
                 "PHY0 PHYID2 read");
    CHECK_EQ_U32(smsc95xx_mii_addr_word(0, SMSC95XX_MII_BMSR, false), 0x41,
                 "PHY0 BMSR read");
    CHECK_EQ_U32(smsc95xx_mii_addr_word(0, SMSC95XX_MII_BMCR, false), 0x01,
                 "PHY0 BMCR read");
    CHECK_EQ_U32(smsc95xx_mii_addr_word(1, SMSC95XX_MII_PHYID1, false), 0x881,
                 "PHY1 PHYID1 read");

    /* A write sets MII_WRITE in addition to MII_BUSY. Not present in the
     * captures (Linux never writes a PHY register on this hardware), so this
     * asserts the field layout only. */
    CHECK_EQ_U32(smsc95xx_mii_addr_word(0, SMSC95XX_MII_BMCR, true), 0x03,
                 "PHY0 BMCR write");
}

static void test_e2p_read_cmd(void)
{
    /* The captured EEPROM MAC read issues E2P_CMD 0x80000001..0x80000006. */
    CHECK_EQ_U32(smsc95xx_e2p_read_cmd(1), 0x80000001, "E2P read offset 1");
    CHECK_EQ_U32(smsc95xx_e2p_read_cmd(6), 0x80000006, "E2P read offset 6");
    /* Offset is masked to 9 bits. */
    CHECK_EQ_U32(smsc95xx_e2p_read_cmd(0x1FF), 0x800001FF, "E2P read max offset");
}

static void test_id_rev_split(void)
{
    uint16_t chip = 0, rev = 0;
    /* Both dongles read ID_REV = 0x9E000002. */
    smsc95xx_id_rev_split(0x9E000002u, &chip, &rev);
    CHECK_EQ_U32(chip, 0x9E00, "ID_REV chip id");
    CHECK_EQ_U32(rev, 0x0002, "ID_REV revision");
}

static void test_mac_packing(void)
{
    /* MACH dongle: EEPROM MAC 4a:f8:f8:c2:c2:f2 was written to the chip as
     * ADDRL=0xC2F8F84A, ADDRH=0x0000F2C2. */
    const uint8_t mach_mac[SMSC95XX_MAC_LEN] = {
        0x4A, 0xF8, 0xF8, 0xC2, 0xC2, 0xF2
    };
    uint32_t addrl = 0, addrh = 0;
    smsc95xx_mac_to_regs(mach_mac, &addrl, &addrh);
    CHECK_EQ_U32(addrl, 0xC2F8F84Au, "MACH ADDRL");
    CHECK_EQ_U32(addrh, 0x0000F2C2u, "MACH ADDRH");

    /* EVB dongle: MAC 9c:95:6e:b5:9b:62. */
    const uint8_t evb_mac[SMSC95XX_MAC_LEN] = {
        0x9C, 0x95, 0x6E, 0xB5, 0x9B, 0x62
    };
    smsc95xx_mac_to_regs(evb_mac, &addrl, &addrh);
    CHECK_EQ_U32(addrl, 0xB56E959Cu, "EVB ADDRL");
    CHECK_EQ_U32(addrh, 0x0000629Bu, "EVB ADDRH");

    /* Round-trip must be lossless. */
    uint8_t out[SMSC95XX_MAC_LEN] = {0};
    smsc95xx_regs_to_mac(0xC2F8F84Au, 0x0000F2C2u, out);
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++)
        CHECK_EQ_U32(out[i], mach_mac[i], "MACH MAC round-trip byte");
}

static void test_bmsr_decode(void)
{
    /* Both dongles read BMSR = 0x0805: link up (bit 2), 10 Mb/s half-duplex
     * capable (bit 11), extended capability (bit 0). Bit 3 is clear, meaning
     * the PHY cannot autonegotiate. */
    CHECK_TRUE(smsc95xx_bmsr_link_up(0x0805), "BMSR 0x0805 link up");
    CHECK_FALSE(smsc95xx_bmsr_autoneg_capable(0x0805), "BMSR 0x0805 no autoneg");

    /* BMSR with link bit clear (0x0801): link down. */
    CHECK_FALSE(smsc95xx_bmsr_link_up(0x0801), "BMSR without link bit");

    /* A conventional autoneg-capable PHY sets bit 3. This is the signature the
     * dext will use to refuse unsupported 10/100 adapters. */
    CHECK_TRUE(smsc95xx_bmsr_autoneg_capable(0x782D), "autoneg-capable PHY");
}

static void test_eeprom_signature(void)
{
    /* Observed on the MACH unit: 0xA5 at offset 0 when the power-on auto-load
     * succeeded and the MAC read fc:61:79:90:04:56 identically 10/10 times;
     * 0x4A when it failed and the same offsets read 4a:f8:f8:c2:c2:f2 -- a
     * systematically mis-clocked read that is stable and passes every pattern
     * check, so only this signature distinguishes the two. */
    CHECK_TRUE(smsc95xx_eeprom_sig_valid(0xA5), "0xA5 is the valid signature");
    CHECK_FALSE(smsc95xx_eeprom_sig_valid(0x4A),
                "0x4A (mis-clocked 0xA5) must be rejected");
    CHECK_FALSE(smsc95xx_eeprom_sig_valid(0x00), "0x00 must be rejected");
    CHECK_FALSE(smsc95xx_eeprom_sig_valid(0xFF), "0xFF must be rejected");

    /* The real MAC packs to these registers. The mis-clocked address packs to
     * 0xC2F8F84A/0x0000F2C2, which is what the captured Linux trace wrote --
     * both are exercised here so the distinction stays visible. */
    const uint8_t real_mac[SMSC95XX_MAC_LEN] = {
        0xFC, 0x61, 0x79, 0x90, 0x04, 0x56
    };
    uint32_t addrl = 0, addrh = 0;
    smsc95xx_mac_to_regs(real_mac, &addrl, &addrh);
    CHECK_EQ_U32(addrl, 0x907961FCu, "real MACH ADDRL");
    CHECK_EQ_U32(addrh, 0x00005604u, "real MACH ADDRH");
}

int main(void)
{
    test_mii_addr_word();
    test_e2p_read_cmd();
    test_id_rev_split();
    test_mac_packing();
    test_bmsr_decode();
    test_eeprom_signature();
    TEST_REPORT();
}
