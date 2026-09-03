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
 *
 * The init-sequence tests at the end of this file are new with the move of the
 * sequence into common/. Their expected register writes are transcribed from
 * reference/mach-init-sequence.txt with the frame numbers alongside each entry, and
 * the three places where this driver deliberately differs from that capture are
 * pinned explicitly in test_init_seq_matches_capture() rather than left implicit.
 */
#include "smsc95xx_init.h"
#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"
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

    /* THE ACCEPTED BOUNDARIES, and the two lengths the dext's transmit path was
     * measured handling. The driver applies its own guard first (reject 0 or
     * anything over 1518, a VLAN-tagged maximum frame) and then hands the length
     * straight to this function, so exactly where accept turns into refuse is the
     * seam between "the driver refused it" and "the protocol refused it". The
     * cases below pin the accepting side of that seam; the refusing side is
     * checked immediately after.
     *
     * cmd_a is frame_len | FIRST_SEG (0x2000) | LAST_SEG (0x1000), cmd_b is
     * frame_len, so these are computed by hand rather than copied from a capture:
     *   42   -> 0x0000302A / 0x0000002A   the shortest frame the stack was seen to send
     *   1372 -> 0x0000355C / 0x0000055C   the longest frame the stack was seen to send
     *   1514 -> 0x000035EA / 0x000005EA   SMSC95XX_FRAME_MAX, the last accepted length
     */
    static const struct {
        size_t   frame_len;
        uint32_t cmd_a;
        uint32_t cmd_b;
    } bounds[] = {
        { SMSC95XX_FRAME_MIN, 0x0000302Au, 0x0000002Au },
        { 1372,               0x0000355Cu, 0x0000055Cu },
        { SMSC95XX_FRAME_MAX, 0x000035EAu, 0x000005EAu },
    };

    for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
        /* The dext's real buffer size, header plus a VLAN-tagged maximum frame, so
         * the buflen argument under test is the one the driver actually passes. */
        uint8_t buf[SMSC95XX_TX_HEADER_LEN + 1518] = {0};
        size_t n = smsc95xx_tx_prepend(buf, sizeof(buf), bounds[i].frame_len);
        CHECK_EQ_U32(n, SMSC95XX_TX_HEADER_LEN, "tx_prepend accepts a boundary length");
        CHECK_EQ_U32(le32_at(buf),     bounds[i].cmd_a, "TX_CMD_A at a boundary length");
        CHECK_EQ_U32(le32_at(buf + 4), bounds[i].cmd_b, "TX_CMD_B at a boundary length");
        /* Only the header is written: the caller copies the frame after it. */
        CHECK_EQ_U32(buf[SMSC95XX_TX_HEADER_LEN], 0,
                     "tx_prepend writes only the header");
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
    /* The dext's own guard admits up to 1518 (a VLAN-tagged maximum frame) before
     * calling this, so 1515..1518 reaches here and must be refused. If this ever
     * starts succeeding, the driver would frame a length the chip does not accept. */
    CHECK_EQ_U32(smsc95xx_tx_prepend(hdr, sizeof(hdr), 1518), 0,
                 "reject a VLAN-tagged maximum frame (the driver's guard band)");
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

/* The two cases below are what the driver actually meets on a real link, as opposed to
 * the single-fault cases above. Both test already-correct code, so they are regression
 * tests rather than TDD -- their job is to pin behaviour the receive loop depends on and
 * that nothing else here states. */

static void test_rx_walk_error_in_middle_does_not_stop_the_walk(void)
{
    /* Three records, the middle one carrying ERROR_SUM. All three must be returned:
     * filtering is the driver's decision, not the decoder's. If an error record ended
     * the walk instead, every frame after a single CRC error in a transfer would be
     * silently lost -- a 4096-byte transfer can hold sixty of them -- and the symptom
     * would be occasional missing packets under load, which is close to undiagnosable
     * from outside. The distinct fillers are what prove the third record was reached. */
    uint8_t buf[256] = {0};
    size_t n = 0;
    n += build_rx_record(buf + n, 64, 0x11, 0);
    n += build_rx_record(buf + n, 64, 0x22, SMSC95XX_RX_STS_ERROR_SUM);
    n += build_rx_record(buf + n, 64, 0x33, 0);

    const uint8_t want_fill[3] = { 0x11, 0x22, 0x33 };
    size_t offset = 0;
    int seen = 0;
    int flagged = 0;

    for (int i = 0; i < 3; i++) {
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        uint32_t status = 0;
        CHECK_TRUE(smsc95xx_rx_next(buf, n, &offset, &frame, &frame_len, &status),
                   "an error record does not end the walk");
        CHECK_EQ_U32(frame_len, 64, "length of each record either side of the error");
        CHECK_EQ_U32(frame[0], want_fill[i], "the right record, in order");
        seen++;
        if ((status & SMSC95XX_RX_STS_ERROR_SUM) != 0)
            flagged++;
    }
    CHECK_EQ_U32(seen, 3, "all three records yielded");
    CHECK_EQ_U32(flagged, 1, "exactly one carried the error bit");

    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;
    CHECK_FALSE(smsc95xx_rx_next(buf, n, &offset, &frame, &frame_len, &status),
                "and then the walk ends");
}

static void test_rx_walk_truncated_trailing_record(void)
{
    /* One complete record followed by two stray bytes -- too few even for a status
     * word. The walk must yield exactly one frame and then stop rather than read past
     * the end. This differs from "reject truncated status word" above, which starts at
     * offset 0: here the offset has already been advanced by a successful decode, so it
     * exercises the bounds check on the second iteration, which is the one the driver
     * relies on for every transfer whose last record is cut short by the host's chosen
     * transfer length. */
    uint8_t buf[128] = {0};
    size_t used = build_rx_record(buf, 64, 0x5A, 0);

    size_t offset = 0;
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    uint32_t status = 0;

    CHECK_TRUE(smsc95xx_rx_next(buf, used + 2, &offset, &frame, &frame_len, &status),
               "the complete record decodes");
    CHECK_EQ_U32(frame_len, 64, "its length");
    CHECK_EQ_U32(frame[0], 0x5A, "its data");
    CHECK_FALSE(smsc95xx_rx_next(buf, used + 2, &offset, &frame, &frame_len, &status),
                "two trailing bytes are not a second record");

    /* Every truncation length short of a whole status word behaves the same way. Three
     * spare bytes is the interesting boundary: it is one short of a status word, and an
     * off-by-one in the check would accept it. */
    for (size_t spare = 1; spare < SMSC95XX_RX_HEADER_LEN; spare++) {
        offset = 0;
        CHECK_TRUE(smsc95xx_rx_next(buf, used + spare, &offset, &frame, &frame_len,
                                    &status),
                   "the complete record still decodes with a short tail");
        CHECK_FALSE(smsc95xx_rx_next(buf, used + spare, &offset, &frame, &frame_len,
                                     &status),
                    "a partial status word is never a record");
    }
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
     * Broadcast sets BROADCAST and MULTICAST together, unicast clears both, and
     * multicast is the case that separates them: MULTICAST set while BROADCAST
     * stays clear. Without it, a defect treating the two bits as one would pass
     * the broadcast and unicast vectors.
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

static void test_parse_vid_pid(void)
{
    /* --device vid:pid parsing */
    {
        uint16_t vid = 0, pid = 0;
        CHECK(smsc95xx_parse_vid_pid("0424:9905", &vid, &pid) && vid == 0x0424 && pid == 0x9905,
              "parse lowercase hex pair");
        CHECK(smsc95xx_parse_vid_pid("184F:0051", &vid, &pid) && vid == 0x184F && pid == 0x0051,
              "parse uppercase hex pair");
        CHECK(smsc95xx_parse_vid_pid("0x0424:0x9905", &vid, &pid) && vid == 0x0424 && pid == 0x9905,
              "0x prefixes accepted");
        CHECK(!smsc95xx_parse_vid_pid("0424", &vid, &pid),         "reject missing colon");
        CHECK(!smsc95xx_parse_vid_pid("0424:", &vid, &pid),        "reject empty pid");
        CHECK(!smsc95xx_parse_vid_pid(":9905", &vid, &pid),        "reject empty vid");
        CHECK(!smsc95xx_parse_vid_pid("10000:9905", &vid, &pid),   "reject vid over 16 bits");
        CHECK(!smsc95xx_parse_vid_pid("0424:9905:1", &vid, &pid),  "reject trailing garbage");
        CHECK(!smsc95xx_parse_vid_pid("g424:9905", &vid, &pid),    "reject non-hex");
        CHECK(!smsc95xx_parse_vid_pid(NULL, &vid, &pid),           "reject NULL");
    }
}

static void test_mac_plausible(void)
{
    /* The address this hardware actually carries. */
    const uint8_t good[6]      = { 0xFC, 0x61, 0x79, 0x90, 0x04, 0x56 };
    const uint8_t zeros[6]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t ones[6]      = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t multicast[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0x01 };
    /* Locally administered but unicast, so plausible. */
    const uint8_t local[6]     = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    /* The MAC this unit reports when its EEPROM mis-clocks. It is unicast and
     * passes every pattern check, which is why only the 0xA5 signature and the
     * E2P_CMD LOADED bit can reject it. Asserting PLAUSIBLE here is deliberate:
     * it pins the fact that plausibility does not and cannot catch this case. */
    const uint8_t misclocked[6] = { 0x4A, 0xF8, 0xF8, 0xC2, 0xC2, 0xF2 };

    CHECK(smsc95xx_mac_plausible(good) == SMSC95XX_MAC_PLAUSIBLE,
          "real EEPROM MAC is plausible");
    CHECK(smsc95xx_mac_plausible(zeros) == SMSC95XX_MAC_ALL_ZEROS,
          "all-zero MAC rejected");
    CHECK(smsc95xx_mac_plausible(ones) == SMSC95XX_MAC_ALL_ONES,
          "all-ones MAC rejected");
    CHECK(smsc95xx_mac_plausible(multicast) == SMSC95XX_MAC_MULTICAST,
          "multicast MAC rejected (group bit set)");
    CHECK(smsc95xx_mac_plausible(local) == SMSC95XX_MAC_PLAUSIBLE,
          "locally administered unicast MAC is plausible");
    CHECK(smsc95xx_mac_plausible(misclocked) == SMSC95XX_MAC_PLAUSIBLE,
          "mis-clocked MAC passes plausibility -- only provenance rejects it");
}

/* ===========================================================================
 * The initialisation sequence.
 *
 * Testable at all only because common/smsc95xx_init.c takes its register access
 * as callbacks: the mock below records every (offset, value) write, so the exact
 * ordered sequence -- the thing that actually switches RX and TX on in hardware --
 * can be asserted with no device present.
 *
 * The expected values are cross-checked against reference/mach-init-sequence.txt,
 * the decoded trace of the real Linux driver bringing this same hardware up. The
 * frame numbers in test_init_seq_matches_capture() are that file's, so any
 * expectation here can be re-checked against the capture line by line.
 * ========================================================================= */

#define MOCK_MAX_WRITES 32

/* An arbitrary non-zero callback error. Any non-zero value must abort the sequence
 * and come back to the caller unchanged; this one is recognisable in a failure
 * message and small enough to be a valid int. */
#define MOCK_ERR 0x0BADF00D

typedef struct {
    uint16_t offset;
    uint32_t value;
} reg_write;

typedef struct {
    reg_write writes[MOCK_MAX_WRITES];
    size_t    nwrites;      /* attempted writes, which can exceed MOCK_MAX_WRITES */
    size_t    nreads;
    bool      overflow;     /* set if the sequence wrote more than we can record  */

    uint32_t  hw_cfg_read;  /* what a HW_CFG read returns                         */
    uint32_t  pm_ctrl_read; /* what a PM_CTRL read returns                        */
    bool      hw_cfg_stuck; /* report LRST as never clearing, for the timeout path */

    int       fail_write_at; /* -1 = never, else the 0-based write index to fail   */
    int       fail_read_at;  /* -1 = never, else the 0-based read index to fail    */
} mock_io;

static void mock_reset(mock_io *m)
{
    for (size_t i = 0; i < MOCK_MAX_WRITES; i++) {
        m->writes[i].offset = 0;
        m->writes[i].value  = 0;
    }
    m->nwrites  = 0;
    m->nreads   = 0;
    m->overflow = false;
    /* Both defaults are measured, not invented: HW_CFG reads back 0x00000004 (PSEL)
     * once the chip has cleared LRST (capture frame 152), and PM_CTRL reads
     * 0x000001C0 before the PHY reset bit is set (capture frame 216). Using the
     * measured PM_CTRL value is what makes the read-modify-write assertion below
     * comparable with the capture's 0x000001D0 (frame 218). */
    m->hw_cfg_read  = SMSC95XX_HW_CFG_PSEL;
    m->pm_ctrl_read = 0x000001C0u;
    m->hw_cfg_stuck = false;
    m->fail_write_at = -1;
    m->fail_read_at  = -1;
}

static int mock_read(void *ctx, uint16_t offset, uint32_t *value)
{
    mock_io *m = (mock_io *)ctx;
    int index = (int)m->nreads;
    m->nreads++;

    if (m->fail_read_at >= 0 && index == m->fail_read_at)
        return MOCK_ERR;    /* deliberately leaves *value untouched */

    switch (offset) {
    case SMSC95XX_REG_HW_CFG:
        *value = m->hw_cfg_stuck ? (m->hw_cfg_read | SMSC95XX_HW_CFG_LRST)
                                 : m->hw_cfg_read;
        break;
    case SMSC95XX_REG_PM_CTRL:
        *value = m->pm_ctrl_read;
        break;
    default:
        *value = 0;
        break;
    }
    return 0;
}

static int mock_write(void *ctx, uint16_t offset, uint32_t value)
{
    mock_io *m = (mock_io *)ctx;
    int index = (int)m->nwrites;

    if (m->nwrites < MOCK_MAX_WRITES) {
        m->writes[m->nwrites].offset = offset;
        m->writes[m->nwrites].value  = value;
    } else {
        m->overflow = true;
    }
    m->nwrites++;

    if (m->fail_write_at >= 0 && index == m->fail_write_at)
        return MOCK_ERR;
    return 0;
}

static void mock_bind(mock_io *m, smsc95xx_io *io)
{
    io->ctx   = m;
    io->read  = mock_read;
    io->write = mock_write;
}

/* The MAC used for every ordering assertion below: the real address this unit
 * carries, which test_eeprom_signature already pins to ADDRL 0x907961FC /
 * ADDRH 0x00005604. Reusing it means the ADDRL/ADDRH entries in the expected table
 * are cross-checked by an independent test rather than only by this one. */
static const uint8_t init_test_mac[SMSC95XX_MAC_LEN] = {
    0xFC, 0x61, 0x79, 0x90, 0x04, 0x56
};

/* The ordered sequence, promiscuous=false. Twenty writes, in this order, with these
 * values. Transcribed from the sequence proven on hardware at M2; the `capture`
 * column is the corresponding line of reference/mach-init-sequence.txt, and where
 * the two differ it is called out in test_init_seq_matches_capture(). */
static const reg_write init_expected[] = {
    { SMSC95XX_REG_HW_CFG,       0x00000008u },  /* LRST                 frame 150 */
    { SMSC95XX_REG_ADDRL,        0x907961FCu },  /* station address      frame 154 */
    { SMSC95XX_REG_ADDRH,        0x00005604u },  /*                      frame 156 */
    { SMSC95XX_REG_HW_CFG,       0x00001004u },  /* BIR|PSEL             frame 160 */
    { SMSC95XX_REG_BURST_CAP,    0x00000005u },  /*                      frame 164 */
    { SMSC95XX_REG_BULK_IN_DLY,  0x00002000u },  /*                      frame 168 */
    { SMSC95XX_REG_HW_CFG,       0x00001026u },  /* BIR|MEF|PSEL|BCE     frame 174 */
    { SMSC95XX_REG_INT_STS,      0xFFFFFFFFu },  /* clear all            frame 178 */
    { SMSC95XX_REG_LED_GPIO_CFG, 0x81110007u },  /*                      frame 184 */
    { SMSC95XX_REG_FLOW,         0x00000000u },  /*                      frame 186 */
    { SMSC95XX_REG_AFC_CFG,      0x00F830A1u },  /*                      frame 188 */
    { SMSC95XX_REG_VLAN1,        0x00008100u },  /* ETH_P_8021Q          frame 192 */
    { SMSC95XX_REG_HASHH,        0x00000000u },  /*                      frame 198 */
    { SMSC95XX_REG_HASHL,        0x00000000u },  /*                      frame 199 */
    { SMSC95XX_REG_MAC_CR,       0x00000000u },  /*                      frame 200 */
    { SMSC95XX_REG_MAC_CR,       0x00000008u },  /* TXEN                 frame 208 */
    { SMSC95XX_REG_TX_CFG,       0x00000004u },  /* TX_ON                frame 210 */
    { SMSC95XX_REG_MAC_CR,       0x0080000Cu },  /* RCVOWN|TXEN|RXEN  cf. frame 212 */
    { SMSC95XX_REG_AFC_CFG,      0x00F830AFu },  /* half duplex       cf. frame 918 */
    { SMSC95XX_REG_PM_CTRL,      0x000001D0u },  /* PHY_RST              frame 218 */
};

#define INIT_EXPECTED_N (sizeof(init_expected) / sizeof(init_expected[0]))

/* Compare a recorded write log against an expected table, entry by entry. */
static void check_write_log(const mock_io *m, const reg_write *want, size_t n,
                            const char *what)
{
    CHECK_FALSE(m->overflow, "write log did not overflow");
    CHECK_EQ_U32(m->nwrites, n, what);
    for (size_t i = 0; i < n && i < m->nwrites && i < MOCK_MAX_WRITES; i++) {
        CHECK_EQ_U32(m->writes[i].offset, want[i].offset, what);
        CHECK_EQ_U32(m->writes[i].value,  want[i].value,  what);
    }
}

static void test_init_seq_order(void)
{
    mock_io m;
    smsc95xx_io io;

    mock_reset(&m);
    mock_bind(&m, &io);

    CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false), 0,
                 "init sequence succeeds when every access succeeds");
    check_write_log(&m, init_expected, INIT_EXPECTED_N,
                    "init sequence: exact ordered register writes");

    /* The advertised write count is part of the contract -- callers log against it. */
    CHECK_EQ_U32(SMSC95XX_INIT_WRITE_COUNT, INIT_EXPECTED_N,
                 "SMSC95XX_INIT_WRITE_COUNT matches the sequence");

    /* Two reads, and only two: the LRST poll and the PM_CTRL read-modify-write.
     * Nothing else in the sequence reads, which is what lets it be free of any
     * sleeping or MII access. */
    CHECK_EQ_U32(m.nreads, 2u, "init sequence performs exactly two reads");
}

static void test_init_seq_enables_rx_and_tx(void)
{
    mock_io m;
    smsc95xx_io io;

    mock_reset(&m);
    mock_bind(&m, &io);
    CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false), 0, "init for MAC_CR check");

    /* Find the LAST MAC_CR write: that is the one the chip is left with, and the
     * whole point of running the sequence at all. A driver that omits it presents a
     * network interface over a chip that will neither receive nor transmit -- which
     * is exactly the defect this sequence was moved into common/ to fix. */
    int last = -1;
    for (size_t i = 0; i < m.nwrites && i < MOCK_MAX_WRITES; i++) {
        if (m.writes[i].offset == SMSC95XX_REG_MAC_CR)
            last = (int)i;
    }
    CHECK_TRUE(last >= 0, "MAC_CR is written at all");
    if (last < 0)
        return;

    uint32_t final_mac_cr = m.writes[last].value;
    CHECK_TRUE((final_mac_cr & SMSC95XX_MAC_CR_RXEN) != 0,
               "final MAC_CR write sets RXEN");
    CHECK_TRUE((final_mac_cr & SMSC95XX_MAC_CR_TXEN) != 0,
               "final MAC_CR write sets TXEN");
    /* Half duplex 10 Mb/s: RCVOWN set, FDPX clear. */
    CHECK_TRUE((final_mac_cr & SMSC95XX_MAC_CR_RCVOWN) != 0,
               "final MAC_CR write sets RCVOWN (half duplex)");
    CHECK_FALSE(final_mac_cr & SMSC95XX_MAC_CR_FDPX,
                "final MAC_CR write leaves FDPX clear");

    /* MAC_CR three times, and TX_CFG between the second and third: the transmitter is
     * enabled in MAC_CR BEFORE the TX datapath is turned on in TX_CFG, and the
     * receiver only afterwards. Both captures agree, so the ordering is asserted
     * rather than left as a comment. */
    int mac_cr_count = 0, tx_cfg_index = -1;
    int mac_cr_index[8];
    for (size_t i = 0; i < m.nwrites && i < MOCK_MAX_WRITES; i++) {
        if (m.writes[i].offset == SMSC95XX_REG_MAC_CR && mac_cr_count < 8)
            mac_cr_index[mac_cr_count++] = (int)i;
        if (m.writes[i].offset == SMSC95XX_REG_TX_CFG)
            tx_cfg_index = (int)i;
    }
    CHECK_EQ_U32(mac_cr_count, 3, "MAC_CR is written exactly three times");
    CHECK_TRUE(tx_cfg_index > 0, "TX_CFG is written");
    if (mac_cr_count == 3 && tx_cfg_index > 0) {
        CHECK_TRUE(mac_cr_index[1] < tx_cfg_index,
                   "MAC_CR.TXEN is set before TX_CFG turns the datapath on");
        CHECK_TRUE(tx_cfg_index < mac_cr_index[2],
                   "TX_CFG comes before the MAC_CR write that enables RXEN");
        CHECK_EQ_U32(m.writes[mac_cr_index[1]].value, SMSC95XX_MAC_CR_TXEN,
                     "second MAC_CR write is TXEN alone");
    }

    /* HW_CFG.MEF must end up set: the RX decoder walks several frames per bulk IN
     * transfer and that only happens in multiple-Ethernet-frames mode. */
    int last_hw_cfg = -1;
    for (size_t i = 0; i < m.nwrites && i < MOCK_MAX_WRITES; i++) {
        if (m.writes[i].offset == SMSC95XX_REG_HW_CFG)
            last_hw_cfg = (int)i;
    }
    CHECK_TRUE(last_hw_cfg >= 0, "HW_CFG is written");
    if (last_hw_cfg >= 0) {
        CHECK_TRUE((m.writes[last_hw_cfg].value & SMSC95XX_HW_CFG_MEF) != 0,
                   "final HW_CFG write sets MEF");
    }
}

static void test_init_seq_promiscuous(void)
{
    mock_io plain, promisc;
    smsc95xx_io io;

    mock_reset(&plain);
    mock_bind(&plain, &io);
    CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false), 0, "init promiscuous=false");

    mock_reset(&promisc);
    mock_bind(&promisc, &io);
    CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, true), 0, "init promiscuous=true");

    CHECK_EQ_U32(promisc.nwrites, plain.nwrites,
                 "promiscuous mode changes no write COUNT");

    /* Exactly one write differs, and it is the final MAC_CR. Asserting the whole log
     * rather than just that one entry is what catches a PRMS bit that leaked into
     * some other register. */
    size_t differing = 0;
    size_t diff_index = 0;
    for (size_t i = 0; i < plain.nwrites && i < MOCK_MAX_WRITES; i++) {
        CHECK_EQ_U32(promisc.writes[i].offset, plain.writes[i].offset,
                     "promiscuous mode changes no register ORDER");
        if (promisc.writes[i].value != plain.writes[i].value) {
            differing++;
            diff_index = i;
        }
    }
    CHECK_EQ_U32(differing, 1u, "promiscuous mode changes exactly one written value");
    if (differing == 1) {
        CHECK_EQ_U32(plain.writes[diff_index].offset, SMSC95XX_REG_MAC_CR,
                     "the differing write is MAC_CR");
        CHECK_TRUE((promisc.writes[diff_index].value & SMSC95XX_MAC_CR_PRMS) != 0,
                   "promiscuous=true sets MAC_CR.PRMS");
        CHECK_FALSE(plain.writes[diff_index].value & SMSC95XX_MAC_CR_PRMS,
                    "promiscuous=false leaves MAC_CR.PRMS clear");
        /* PRMS is the ONLY difference: RXEN, TXEN and RCVOWN are unaffected. */
        CHECK_EQ_U32(promisc.writes[diff_index].value & ~SMSC95XX_MAC_CR_PRMS,
                     plain.writes[diff_index].value,
                     "PRMS is the only bit promiscuous mode adds");
    }
}

static void test_init_seq_aborts_on_error(void)
{
    /* A callback failure must stop the sequence dead and be returned unchanged. If it
     * carried on, a chip that rejected HW_CFG would still get MAC_CR.RXEN and the
     * driver would believe it had initialised. Every write index is exercised, so
     * there is no position at which a missing error check could hide. */
    for (size_t at = 0; at < INIT_EXPECTED_N; at++) {
        mock_io m;
        smsc95xx_io io;

        mock_reset(&m);
        m.fail_write_at = (int)at;
        mock_bind(&m, &io);

        CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false), MOCK_ERR,
                     "write failure is propagated unchanged");
        /* The failing write was attempted, and nothing after it was. */
        CHECK_EQ_U32(m.nwrites, at + 1, "sequence stops at the failing write");
    }

    /* Both reads too: the LRST poll (read 0) and the PM_CTRL read-modify-write
     * (read 1). A failed PM_CTRL read must not turn into a PHY reset written on top
     * of a garbage value. */
    for (int at = 0; at < 2; at++) {
        mock_io m;
        smsc95xx_io io;

        mock_reset(&m);
        m.fail_read_at = at;
        mock_bind(&m, &io);

        CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false), MOCK_ERR,
                     "read failure is propagated unchanged");
        if (at == 0) {
            CHECK_EQ_U32(m.nwrites, 1u, "a failed LRST poll stops after the LRST write");
        } else {
            /* Everything but the final PM_CTRL write. */
            CHECK_EQ_U32(m.nwrites, INIT_EXPECTED_N - 1,
                         "a failed PM_CTRL read stops before writing PM_CTRL");
        }
    }
}

static void test_init_seq_reset_timeout(void)
{
    mock_io m;
    smsc95xx_io io;

    mock_reset(&m);
    m.hw_cfg_stuck = true;      /* the chip never clears LRST */
    mock_bind(&m, &io);

    CHECK_EQ_U32(smsc95xx_init_seq(&io, init_test_mac, false),
                 (uint32_t)SMSC95XX_INIT_ERR_RESET_TIMEOUT,
                 "a chip that never clears LRST reports a reset timeout");
    CHECK_EQ_U32(m.nwrites, 1u, "nothing is written after a failed reset");
    CHECK_TRUE(m.nreads > 1, "the LRST wait polls rather than reading once");
    CHECK_TRUE(m.nreads <= (size_t)SMSC95XX_INIT_RESET_POLLS + 1,
               "the LRST wait is bounded");
}

static void test_init_seq_bad_args(void)
{
    mock_io m;
    smsc95xx_io io;

    mock_reset(&m);
    mock_bind(&m, &io);

    CHECK_EQ_U32(smsc95xx_init_seq(NULL, init_test_mac, false),
                 (uint32_t)SMSC95XX_INIT_ERR_BAD_ARG, "NULL io rejected");
    CHECK_EQ_U32(smsc95xx_init_seq(&io, NULL, false),
                 (uint32_t)SMSC95XX_INIT_ERR_BAD_ARG, "NULL mac rejected");

    smsc95xx_io no_read = io;
    no_read.read = NULL;
    CHECK_EQ_U32(smsc95xx_init_seq(&no_read, init_test_mac, false),
                 (uint32_t)SMSC95XX_INIT_ERR_BAD_ARG, "NULL read callback rejected");

    smsc95xx_io no_write = io;
    no_write.write = NULL;
    CHECK_EQ_U32(smsc95xx_init_seq(&no_write, init_test_mac, false),
                 (uint32_t)SMSC95XX_INIT_ERR_BAD_ARG, "NULL write callback rejected");

    CHECK_EQ_U32(m.nwrites, 0u, "a rejected call touches no register");
}

static void test_init_seq_matches_capture(void)
{
    /* Cross-check against reference/mach-init-sequence.txt directly, using the MAC
     * that capture was taken with (4a:f8:f8:c2:c2:f2, the mis-clocked EEPROM read
     * Linux happened to program) so ADDRL/ADDRH are comparable too.
     *
     * The first seventeen writes match the capture EXACTLY, register for register and
     * value for value, with two of the capture's writes omitted on purpose:
     *   COE_CR    0x00010001  (frame 196) -- checksum offload, out of scope, and it
     *                                        would add two bytes to every RX frame
     *   INT_EP_CTL 0x00008000 (frame 206) -- the interrupt endpoint; link state is
     *                                        polled instead
     * Both omissions are documented in common/smsc95xx_init.h.
     *
     * The last three are checked separately below, because two of them are places
     * where this sequence deliberately does in one pass what Linux does in two. */
    static const reg_write capture_prefix[] = {
        { SMSC95XX_REG_HW_CFG,       0x00000008u },  /* frame 150 */
        { SMSC95XX_REG_ADDRL,        0xC2F8F84Au },  /* frame 154 */
        { SMSC95XX_REG_ADDRH,        0x0000F2C2u },  /* frame 156 */
        { SMSC95XX_REG_HW_CFG,       0x00001004u },  /* frame 160 */
        { SMSC95XX_REG_BURST_CAP,    0x00000005u },  /* frame 164 */
        { SMSC95XX_REG_BULK_IN_DLY,  0x00002000u },  /* frame 168 */
        { SMSC95XX_REG_HW_CFG,       0x00001026u },  /* frame 174 */
        { SMSC95XX_REG_INT_STS,      0xFFFFFFFFu },  /* frame 178 */
        { SMSC95XX_REG_LED_GPIO_CFG, 0x81110007u },  /* frame 184 */
        { SMSC95XX_REG_FLOW,         0x00000000u },  /* frame 186 */
        { SMSC95XX_REG_AFC_CFG,      0x00F830A1u },  /* frame 188 */
        { SMSC95XX_REG_VLAN1,        0x00008100u },  /* frame 192 */
        { SMSC95XX_REG_HASHH,        0x00000000u },  /* frame 198 */
        { SMSC95XX_REG_HASHL,        0x00000000u },  /* frame 199 */
        { SMSC95XX_REG_MAC_CR,       0x00000000u },  /* frame 200 */
        { SMSC95XX_REG_MAC_CR,       0x00000008u },  /* frame 208 */
        { SMSC95XX_REG_TX_CFG,       0x00000004u },  /* frame 210 */
    };
    const size_t prefix_n = sizeof(capture_prefix) / sizeof(capture_prefix[0]);

    const uint8_t capture_mac[SMSC95XX_MAC_LEN] = {
        0x4A, 0xF8, 0xF8, 0xC2, 0xC2, 0xF2
    };

    mock_io m;
    smsc95xx_io io;
    mock_reset(&m);
    mock_bind(&m, &io);

    CHECK_EQ_U32(smsc95xx_init_seq(&io, capture_mac, false), 0,
                 "init with the capture's MAC");
    CHECK_EQ_U32(m.nwrites, prefix_n + 3,
                 "capture prefix plus three writes this sequence folds in");

    for (size_t i = 0; i < prefix_n; i++) {
        CHECK_EQ_U32(m.writes[i].offset, capture_prefix[i].offset,
                     "capture cross-check: register");
        CHECK_EQ_U32(m.writes[i].value, capture_prefix[i].value,
                     "capture cross-check: value");
    }

    /* KNOWN, DELIBERATE DIVERGENCES from the capture, pinned here so they cannot
     * change silently and so a reader is not left to discover them:
     *
     * 1. The third MAC_CR write. The capture writes 0x0000000C (TXEN|RXEN) at frame
     *    212 and only adds RCVOWN much later, at frame 907, from Linux's link-up
     *    path. This sequence sets RCVOWN in the same write, because this hardware is
     *    always 10 Mb/s half duplex -- it has no autonegotiation -- so there is no
     *    later moment at which the duplex becomes known. Same end state, one write
     *    instead of two. (The capture also carries HPFILT there on its second run,
     *    which is multicast filtering this driver does not program.)
     * 2. AFC_CFG's half-duplex low nibble. The capture writes 0x00F830AF at frame
     *    918, again from the link-up path; this sequence writes it immediately after
     *    MAC_CR for the same reason.
     * Both were validated together on hardware at M2. */
    CHECK_EQ_U32(m.writes[prefix_n + 0].offset, SMSC95XX_REG_MAC_CR,
                 "divergence 1 is a MAC_CR write");
    CHECK_EQ_U32(m.writes[prefix_n + 0].value, 0x0080000Cu,
                 "divergence 1: capture frame 212 writes 0x0000000C, we fold in RCVOWN");
    CHECK_EQ_U32(m.writes[prefix_n + 0].value & ~SMSC95XX_MAC_CR_RCVOWN, 0x0000000Cu,
                 "divergence 1: RCVOWN is the only addition to the capture's value");

    CHECK_EQ_U32(m.writes[prefix_n + 1].offset, SMSC95XX_REG_AFC_CFG,
                 "divergence 2 is an AFC_CFG write");
    CHECK_EQ_U32(m.writes[prefix_n + 1].value, 0x00F830AFu,
                 "divergence 2: matches capture frame 918, written earlier here");

    /* The PM_CTRL read-modify-write matches the capture exactly: it read 0x000001C0
     * at frame 216 and wrote 0x000001D0 at frame 218. */
    CHECK_EQ_U32(m.writes[prefix_n + 2].offset, SMSC95XX_REG_PM_CTRL,
                 "PM_CTRL is the last write");
    CHECK_EQ_U32(m.writes[prefix_n + 2].value, 0x000001D0u,
                 "PM_CTRL read-modify-write matches capture frames 216/218");

    /* Read-modify-write, not a blind store: a different starting value must be
     * preserved under the PHY_RST bit. */
    mock_reset(&m);
    m.pm_ctrl_read = 0x0000A5A0u;
    mock_bind(&m, &io);
    CHECK_EQ_U32(smsc95xx_init_seq(&io, capture_mac, false), 0, "init for PM_CTRL rmw");
    CHECK_EQ_U32(m.writes[m.nwrites - 1].value, 0x0000A5B0u,
                 "PM_CTRL preserves the bits it read");
}

static void test_reg_name(void)
{
    /* Names are for log lines, so the only thing that matters is that they are the
     * right ones and that an unknown offset cannot produce a NULL a logger would
     * print as garbage. */
    CHECK_TRUE(smsc95xx_reg_name(SMSC95XX_REG_MAC_CR)[0] == 'M', "MAC_CR names itself");
    CHECK_TRUE(smsc95xx_reg_name(SMSC95XX_REG_HW_CFG)[0] == 'H', "HW_CFG names itself");
    CHECK_TRUE(smsc95xx_reg_name(0x7FF) != NULL, "unknown offset is not NULL");
    CHECK_TRUE(smsc95xx_reg_name(0x7FF)[0] == '?', "unknown offset reads as ?");
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
    test_rx_walk_error_in_middle_does_not_stop_the_walk();
    test_rx_walk_truncated_trailing_record();
    test_rx_measured_vector();
    test_rx_status_unicast_vs_broadcast();
    test_rx_status_multicast();
    test_parse_vid_pid();
    test_mac_plausible();
    test_init_seq_order();
    test_init_seq_enables_rx_and_tx();
    test_init_seq_promiscuous();
    test_init_seq_aborts_on_error();
    test_init_seq_reset_timeout();
    test_init_seq_bad_args();
    test_init_seq_matches_capture();
    test_reg_name();
    TEST_REPORT();
}
