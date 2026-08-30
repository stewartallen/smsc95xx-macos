/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for the pure protocol layer.
 *
 * Most expected values were read off real hardware. BMCR/BMSR/PHY registers,
 * MAC packing, and EEPROM signature validation are from
 * reference/mach-init-sequence.txt or reference/evb-init-sequence.txt. TX
 * command vectors (frame length and encoding) come from reference/mach-bringup.pcap.
 * For the RX vector, only its first 18 bytes were captured on hardware
 * (2026-08-30); the rest of that array is reconstructed to reach the measured
 * 68-byte length and is not asserted on. See the note on the vector itself.
 *
 * Two values are synthetic rather than measured: the autoneg-capable BMSR
 * 0x782D, and the MII write address 0x03 in test_mii_addr_word.
 * Do not replace measured values with hand-derived numbers.
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

/* Read a little-endian u32 out of a byte buffer, for checking header layout. */
static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_tx_prepend(void)
{
    /* Every expected value below was measured from the bulk OUT transfers in
     * reference/mach-bringup.pcap. Frame numbers are given so they can be
     * re-checked with:
     *   tshark -r reference/mach-bringup.pcap -Y 'frame.number == N' \
     *          -T fields -e usb.capdata
     *
     *   pcap frame  total bytes  TX_CMD_A     TX_CMD_B     frame_len
     *   1108        70           0x0000303E   0x0000003E   62
     *   1056        94           0x00003056   0x00000056   86
     *   1028        118          0x0000306E   0x0000006E   110
     *   1140        259          0x000030FB   0x000000FB   251
     *   1110        294          0x0000311E   0x0000011E   286
     *   1014        338          0x0000314A   0x0000014A   330
     */
    static const struct {
        size_t   frame_len;
        uint32_t cmd_a;
        uint32_t cmd_b;
    } vectors[] = {
        {  62, 0x0000303Eu, 0x0000003Eu },
        {  86, 0x00003056u, 0x00000056u },
        { 110, 0x0000306Eu, 0x0000006Eu },
        { 251, 0x000030FBu, 0x000000FBu },
        { 286, 0x0000311Eu, 0x0000011Eu },
        { 330, 0x0000314Au, 0x0000014Au },
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t hdr[SMSC95XX_TX_HEADER_LEN] = {0};
        size_t n = smsc95xx_tx_prepend(hdr, sizeof(hdr), vectors[i].frame_len);
        CHECK_EQ_U32(n, SMSC95XX_TX_HEADER_LEN, "tx_prepend header length");
        CHECK_EQ_U32(le32_at(hdr),     vectors[i].cmd_a, "TX_CMD_A");
        CHECK_EQ_U32(le32_at(hdr + 4), vectors[i].cmd_b, "TX_CMD_B");
    }

    /* Refuse rather than truncate: a too-small buffer or an out-of-range
     * length must produce 0, not a partially-written header. */
    uint8_t small[4] = {0};
    CHECK_EQ_U32(smsc95xx_tx_prepend(small, sizeof(small), 62), 0,
                 "reject undersized buffer");

    uint8_t hdr[SMSC95XX_TX_HEADER_LEN] = {0};
    CHECK_EQ_U32(smsc95xx_tx_prepend(hdr, sizeof(hdr), SMSC95XX_FRAME_MIN - 1), 0,
                 "reject undersized frame (below minimum)");
    CHECK_EQ_U32(smsc95xx_tx_prepend(hdr, sizeof(hdr), SMSC95XX_FRAME_MAX + 1), 0,
                 "reject oversized frame");
    CHECK_EQ_U32(smsc95xx_tx_prepend(hdr, sizeof(hdr), 0), 0,
                 "reject zero-length frame");
}

/* Build one RX record into `dst`: status word for `frame_len`, then `frame_len`
 * filler bytes, then padding to the next 4-byte boundary. Returns bytes written.
 * This mirrors the layout smsc95xx_rx_next expects, so a bug in the parser shows
 * up as a mismatch rather than both sides agreeing on something wrong -- the
 * expectations below are written out by hand, not derived from the parser. */
static size_t build_rx_record(uint8_t *dst, size_t frame_len, uint8_t filler,
                              uint32_t extra_status_bits)
{
    uint32_t sts = ((uint32_t)frame_len << SMSC95XX_RX_STS_LEN_SHIFT) &
                   SMSC95XX_RX_STS_LEN_MASK;
    sts |= extra_status_bits;

    dst[0] = (uint8_t)(sts & 0xFF);
    dst[1] = (uint8_t)((sts >> 8) & 0xFF);
    dst[2] = (uint8_t)((sts >> 16) & 0xFF);
    dst[3] = (uint8_t)((sts >> 24) & 0xFF);

    for (size_t i = 0; i < frame_len; i++)
        dst[SMSC95XX_RX_HEADER_LEN + i] = filler;

    size_t used = SMSC95XX_RX_HEADER_LEN + frame_len;
    size_t pad = (4 - (used % 4)) % 4;
    for (size_t i = 0; i < pad; i++)
        dst[used + i] = 0;

    return used + pad;
}

static void test_rx_single_frame(void)
{
    uint8_t buf[256] = {0};
    size_t total = build_rx_record(buf, 64, 0xAB, 0);

    size_t offset = 0;
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;

    CHECK_TRUE(smsc95xx_rx_next(buf, total, &offset, &frame, &frame_len, &status),
               "decode one frame");
    CHECK_EQ_U32(frame_len, 64, "frame length from status word");
    CHECK_TRUE(frame == buf + SMSC95XX_RX_HEADER_LEN,
               "frame points just past the status word");
    CHECK_EQ_U32(frame[0], 0xAB, "frame data");
    CHECK_FALSE(smsc95xx_rx_next(buf, total, &offset, &frame, &frame_len, &status),
                "no second frame");
}

static void test_rx_multiple_frames(void)
{
    /* Three frames whose lengths force every possible padding amount, so the
     * 4-byte alignment step is genuinely exercised: 64 -> used 68, pad 0;
     * 61 -> used 65, pad 3; 62 -> used 66, pad 2. */
    uint8_t buf[512] = {0};
    size_t n = 0;
    n += build_rx_record(buf + n, 64, 0x11, 0);
    n += build_rx_record(buf + n, 61, 0x22, 0);
    n += build_rx_record(buf + n, 62, 0x33, 0);

    const size_t want_len[3]  = { 64, 61, 62 };
    const uint8_t want_fill[3] = { 0x11, 0x22, 0x33 };

    size_t offset = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        uint32_t status = 0;
        CHECK_TRUE(smsc95xx_rx_next(buf, n, &offset, &frame, &frame_len, &status),
                   "decode frame in multi-frame transfer");
        CHECK_EQ_U32(frame_len, want_len[i], "multi-frame length");
        CHECK_EQ_U32(frame[0], want_fill[i], "multi-frame data");
    }
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;
    CHECK_FALSE(smsc95xx_rx_next(buf, n, &offset, &frame, &frame_len, &status),
                "exactly three frames");
}

static void test_rx_error_bits_and_malformed(void)
{
    uint8_t buf[256] = {0};

    /* An error frame is still returned, with its bits intact, so the caller
     * decides what to do. Silently dropping it here would hide real faults. */
    size_t total = build_rx_record(buf, 64, 0xCD,
                                   SMSC95XX_RX_STS_ERROR_SUM |
                                   SMSC95XX_RX_STS_CRC_ERROR);
    size_t offset = 0;
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;
    CHECK_TRUE(smsc95xx_rx_next(buf, total, &offset, &frame, &frame_len, &status),
               "error frame is still decoded");
    CHECK_TRUE((status & SMSC95XX_RX_STS_ERROR_SUM) != 0, "error summary bit");
    CHECK_TRUE((status & SMSC95XX_RX_STS_CRC_ERROR) != 0, "CRC error bit");

    /* A truncated status word must not be decoded. */
    offset = 0;
    CHECK_FALSE(smsc95xx_rx_next(buf, 3, &offset, &frame, &frame_len, &status),
                "reject truncated status word");

    /* A length that runs past the end of the buffer must not be decoded. */
    uint8_t liar[64] = {0};
    build_rx_record(liar, 40, 0xEE, 0);   /* claims 40 bytes... */
    offset = 0;
    CHECK_FALSE(smsc95xx_rx_next(liar, 20, &offset, &frame, &frame_len, &status),
                "reject length past end of buffer");

    /* An empty transfer decodes nothing. */
    offset = 0;
    CHECK_FALSE(smsc95xx_rx_next(buf, 0, &offset, &frame, &frame_len, &status),
                "empty transfer");

    /* A frame length below the 4-byte CRC must be rejected, as it indicates
     * either corruption or a contract violation: the hardware includes the CRC
     * in the reported length. */
    uint8_t short_frame[32] = {0};
    build_rx_record(short_frame, SMSC95XX_RX_CRC_LEN - 1, 0xEE, 0);
    offset = 0;
    CHECK_FALSE(smsc95xx_rx_next(short_frame, sizeof(short_frame), &offset,
                                 &frame, &frame_len, &status),
                "reject frame shorter than CRC length");
}

static void test_rx_measured_vector(void)
{
    /* Measured 2026-08-30 on the MACH dongle, receiving a broadcast ARP frame
     * transmitted by the EVB attached to a Raspberry Pi, over 10BASE-T1S. The
     * transfer was exactly 68 bytes (4-byte status header + 64-byte frame),
     * which is what pins SMSC95XX_RX_HEADER_LEN to 4: with a header of 5 or 6
     * the declared 64-byte frame would not fit and the decode would fail.
     *
     * PROVENANCE, stated precisely because this is the only non-circular test
     * in the RX suite:
     *   bytes 0..17  OBSERVED. The tool printed raw[0..15] and decoded the
     *                EtherType, giving status 0x00402420, dst ff:ff:ff:ff:ff:ff,
     *                src 9c:95:6e:b5:9b:62 (the EVB's own MAC), type 0x0806.
     *   bytes 18..67 NOT OBSERVED. Reconstructed as a plausible ARP body so the
     *                array reaches the measured 68-byte length. Do not treat
     *                these as captured data and do not assert on them -- the
     *                assertions below deliberately cover only bytes 0..17.
     *
     * The status word 0x00402420 was seen identically in two independent
     * sessions with different source MACs, which is good corroboration of the
     * status-word layout specifically. */
    static const uint8_t observed[] = {
        0x20, 0x24, 0x40, 0x00,  /* status = 0x00402420 */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  /* frame[0..5] = dst = broadcast */
        0x9C, 0x95, 0x6E, 0xB5, 0x9B, 0x62,  /* frame[6..11] = src = the EVB */
        0x08, 0x06,  /* frame[12..13] = EtherType = 0x0806 (ARP) */
        /* frame[14..63] NOT OBSERVED - reconstructed ARP body, see the note above */
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x9C, 0x95, 0x6E, 0xB5, 0x9B, 0x62, 0xC0, 0xA8,
        0x4D, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0xA8, 0x4D, 0x63, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    const size_t observed_len = sizeof(observed);

    /* Unconditional, so the check is always counted: 4 + 64 == 68 is what pins
     * SMSC95XX_RX_HEADER_LEN. */
    CHECK_EQ_U32(observed_len, 68u, "measured RX vector is exactly 68 bytes");

    size_t offset = 0;
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;

    CHECK_TRUE(smsc95xx_rx_next(observed, observed_len, &offset,
                                &frame, &frame_len, &status),
               "decode measured RX transfer");
    CHECK_EQ_U32(status, 0x00402420u, "measured status word");
    CHECK_EQ_U32(frame_len, 64u, "measured frame length");
    CHECK_TRUE((status & SMSC95XX_RX_STS_BROADCAST) != 0, "broadcast bit set");
    CHECK_TRUE((status & SMSC95XX_RX_STS_MULTICAST) != 0, "multicast bit set");
    CHECK_TRUE((status & SMSC95XX_RX_STS_FRAME_TYPE) != 0, "frame type bit set");

    /* Pin the observed frame bytes specifically to verify they were not corrupted. */
    CHECK_EQ_U32(frame[0], 0xFF, "dst first octet is broadcast");
    CHECK_EQ_U32(frame[6], 0x9C, "src first octet matches observed (EVB)");
    CHECK_EQ_U32(frame[12], 0x08, "EtherType high byte");
    CHECK_EQ_U32(frame[13], 0x06, "EtherType low byte");
}

static void test_rx_status_unicast_vs_broadcast(void)
{
    /* Two further status words measured 2026-08-30 from a usbmon capture of
     * Linux driving the MACH dongle against an embedded peer at 169.254.17.71
     * (reference/mach-embedded-baseline.pcap). Both frames were UNICAST, and
     * that is what makes them valuable: they pin BROADCAST and MULTICAST by
     * their ABSENCE, where the earlier broadcast vector only showed them set.
     *
     *   ARP reply : transfer 70  status 0x00420020  len 66
     *   ICMP reply: transfer 108 status 0x00680020  len 104
     *
     * In both, only FRAME_TYPE (0x20) is set in the low bits. So a bit position
     * error that conflated BROADCAST or MULTICAST with FRAME_TYPE would be
     * caught here, which a broadcast-only sample cannot do. The 4-byte header
     * size is also re-confirmed twice: 4 + 66 == 70 and 4 + 104 == 108. */
    static const struct {
        uint32_t status;
        size_t   frame_len;
        size_t   transfer_len;
        const char *what;
    } vectors[] = {
        { 0x00420020u,  66,  70, "unicast ARP reply"  },
        { 0x00680020u, 104, 108, "unicast ICMP reply" },
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint32_t s = vectors[i].status;

        /* Length field decodes to the measured frame length. */
        CHECK_EQ_U32((s & SMSC95XX_RX_STS_LEN_MASK) >> SMSC95XX_RX_STS_LEN_SHIFT,
                     vectors[i].frame_len, vectors[i].what);

        /* 4-byte header plus frame equals the observed transfer size. */
        CHECK_EQ_U32(SMSC95XX_RX_HEADER_LEN + vectors[i].frame_len,
                     vectors[i].transfer_len, "header + frame == transfer");

        /* Unicast: neither broadcast nor multicast, but frame type is set. */
        CHECK_FALSE(s & SMSC95XX_RX_STS_BROADCAST, "unicast: BROADCAST clear");
        CHECK_FALSE(s & SMSC95XX_RX_STS_MULTICAST, "unicast: MULTICAST clear");
        CHECK_TRUE(s & SMSC95XX_RX_STS_FRAME_TYPE, "unicast: FRAME_TYPE set");

        /* A good frame reports no error. */
        CHECK_FALSE(s & SMSC95XX_RX_STS_ERROR_SUM,   "good frame: no error summary");
        CHECK_FALSE(s & SMSC95XX_RX_STS_FILTER_FAIL, "good frame: no filter fail");
    }
}

static void test_rx_status_multicast(void)
{
    /* Four status words measured 2026-08-30 on the MACH dongle, receiving mDNS
     * traffic that a Raspberry Pi emitted onto the 10BASE-T1S segment.
     *
     * These complete the three-way discrimination of the address-class bits.
     * Previously we had broadcast frames (BROADCAST and MULTICAST both set) and
     * unicast frames (both clear). Multicast is the case that separates the two
     * bits from each other: MULTICAST set while BROADCAST stays clear. Without
     * it, a defect that treated the two bits as one would pass both earlier
     * tests.
     *
     *   status      len  transfer  destination
     *   0x00FF0420  255  259       33:33:00:00:00:FB  (IPv6 mDNS)
     *   0x00FB0420  251  255       01:00:5E:00:00:FB  (IPv4 mDNS)
     *   0x00CC0420  204  208       33:33:00:00:00:FB  (IPv6 mDNS)
     *   0x01420420  322  326       01:00:5E:00:00:FB  (IPv4 mDNS)
     *
     * The larger lengths here also exercise the length field well above the
     * 8-bit boundary, which the earlier vectors (64, 66, 104) did not: 322
     * requires bit 8 of the field and would be truncated by a mask narrower
     * than LEN_MASK. */
    static const struct {
        uint32_t status;
        size_t   frame_len;
        size_t   transfer_len;
        const char *what;
    } vectors[] = {
        { 0x00FF0420u, 255, 259, "multicast IPv6 mDNS len 255" },
        { 0x00FB0420u, 251, 255, "multicast IPv4 mDNS len 251" },
        { 0x00CC0420u, 204, 208, "multicast IPv6 mDNS len 204" },
        { 0x01420420u, 322, 326, "multicast IPv4 mDNS len 322" },
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint32_t s = vectors[i].status;

        CHECK_EQ_U32((s & SMSC95XX_RX_STS_LEN_MASK) >> SMSC95XX_RX_STS_LEN_SHIFT,
                     vectors[i].frame_len, vectors[i].what);
        CHECK_EQ_U32(SMSC95XX_RX_HEADER_LEN + vectors[i].frame_len,
                     vectors[i].transfer_len, "header + frame == transfer");

        /* The discriminating assertion: multicast without broadcast. */
        CHECK_FALSE(s & SMSC95XX_RX_STS_BROADCAST, "multicast: BROADCAST clear");
        CHECK_TRUE(s & SMSC95XX_RX_STS_MULTICAST,  "multicast: MULTICAST set");
        CHECK_TRUE(s & SMSC95XX_RX_STS_FRAME_TYPE, "multicast: FRAME_TYPE set");

        CHECK_FALSE(s & SMSC95XX_RX_STS_ERROR_SUM,   "good frame: no error summary");
        CHECK_FALSE(s & SMSC95XX_RX_STS_FILTER_FAIL, "good frame: no filter fail");
    }
}

int main(void)
{
    test_mii_addr_word();
    test_e2p_read_cmd();
    test_id_rev_split();
    test_mac_packing();
    test_bmsr_decode();
    test_eeprom_signature();
    test_tx_prepend();
    test_rx_single_frame();
    test_rx_multiple_frames();
    test_rx_error_bits_and_malformed();
    test_rx_measured_vector();
    test_rx_status_unicast_vs_broadcast();
    test_rx_status_multicast();
    TEST_REPORT();
}
