/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smsc95xx-probe: read identity, PHY state, and EEPROM MAC from a LAN9500A
 * over USB control transfers, without loading any driver.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <IOKit/IOReturn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "smsc95xx_ops.h"
#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"
#include "usb_backend.h"

static int cmd_id(usb_device *d)
{
    uint32_t id_rev = 0;
    int kr = smsc95xx_read_reg(d, SMSC95XX_REG_ID_REV, &id_rev);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "ID_REV read failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    uint16_t chip = 0, rev = 0;
    smsc95xx_id_rev_split(id_rev, &chip, &rev);
    printf("ID_REV   0x%08X  chip 0x%04X  rev 0x%04X\n", id_rev, chip, rev);
    return 0;
}

static int cmd_phy(usb_device *d)
{
    static const struct { uint8_t reg; const char *name; } regs[] = {
        { SMSC95XX_MII_BMCR,   "BMCR"   },
        { SMSC95XX_MII_BMSR,   "BMSR"   },
        { SMSC95XX_MII_PHYID1, "PHYID1" },
        { SMSC95XX_MII_PHYID2, "PHYID2" },
        { SMSC95XX_MII_ANAR,   "ANAR"   },
        { SMSC95XX_MII_ANLPAR, "ANLPAR" },
    };

    uint16_t phyid1 = 0, phyid2 = 0, bmsr = 0;
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint16_t v = 0;
        int kr = smsc95xx_mii_read(d, SMSC95XX_PHY_ADDR, regs[i].reg, &v);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "MII read of %s failed: %s (0x%08x)\n",
                    regs[i].name, usb_strerror(kr), kr);
            return 1;
        }
        printf("PHY%u.%-7s 0x%04X\n", (unsigned)SMSC95XX_PHY_ADDR, regs[i].name, v);

        if (regs[i].reg == SMSC95XX_MII_PHYID1) phyid1 = v;
        if (regs[i].reg == SMSC95XX_MII_PHYID2) phyid2 = v;
        if (regs[i].reg == SMSC95XX_MII_BMSR)   bmsr   = v;
    }

    printf("\nPHY ID   0x%04X:0x%04X\n", phyid1, phyid2);
    printf("link     %s\n", smsc95xx_bmsr_link_up(bmsr) ? "up" : "down");
    printf("autoneg  %s\n",
           smsc95xx_bmsr_autoneg_capable(bmsr) ? "capable" : "not supported");
    return 0;
}

static int cmd_eeprom(usb_device *d)
{
    /* Report whether the chip's power-on EEPROM auto-load succeeded. When it did
     * not, reads can come back systematically shifted but still plausible. */
    bool loaded = false;
    int kr = smsc95xx_eeprom_loaded(d, &loaded);
    if (kr == kIOReturnSuccess)
        printf("E2P_CMD  auto-load %s\n", loaded ? "LOADED" : "NOT loaded");

    uint8_t mac[SMSC95XX_MAC_LEN] = {0};
    uint8_t sig = 0;
    kr = smsc95xx_read_mac_verified(d, mac, &sig);

    if (kr == kIOReturnNotFound) {
        fprintf(stderr, "no EEPROM responding (E2P_CMD reported TIMEOUT)\n");
        return 1;
    }
    if (kr == kIOReturnNotReadable) {
        fprintf(stderr,
                "EEPROM signature mismatch: offset 0 read 0x%02X, expected 0x%02X.\n"
                "Refusing to report a MAC. On this hardware a failed read returns\n"
                "plausible-looking but systematically shifted data, so the address\n"
                "would be wrong in a way no re-read or pattern check would catch.\n",
                sig, SMSC95XX_EEPROM_SIGNATURE);
        return 1;
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "EEPROM read failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    /* Signature matched, so the read is trustworthy. Pattern checks below are a
     * belt-and-braces guard against a signature byte that matched by accident. */
    bool all_zeros = true, all_ones = true;
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
        if (mac[i] != 0x00)
            all_zeros = false;
        if (mac[i] != 0xFF)
            all_ones = false;
    }
    if (all_zeros || all_ones) {
        fprintf(stderr, "EEPROM signature matched but MAC is invalid: "
                "%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 1;
    }

    printf("EEPROM   signature 0x%02X ok\n", sig);
    printf("MAC      %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("         %s administered\n",
           (mac[0] & SMSC95XX_MAC_LOCALLY_ADMINISTERED) ? "locally" : "globally (OUI)");

    uint32_t addrl = 0, addrh = 0;
    smsc95xx_mac_to_regs(mac, &addrl, &addrh);
    printf("ADDRL    0x%08X\nADDRH    0x%08X\n", addrl, addrh);
    return 0;
}

/* Parse aa:bb:cc:dd:ee:ff into six bytes. Returns true on success. */
static bool parse_mac(const char *s, uint8_t mac[SMSC95XX_MAC_LEN])
{
    unsigned v[SMSC95XX_MAC_LEN];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != SMSC95XX_MAC_LEN)
        return false;
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
        if (v[i] > 0xFF)
            return false;
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

/* Resolve the station address: an explicit override if given, otherwise the
 * signature-verified EEPROM address. There is no fallback to an invented
 * address -- on the MACH unit an unverified read yields a plausible but wrong
 * MAC, so guessing would be worse than failing. */
static int resolve_mac(usb_device *d, const char *override_mac,
                       uint8_t mac[SMSC95XX_MAC_LEN])
{
    if (override_mac) {
        if (!parse_mac(override_mac, mac)) {
            fprintf(stderr, "bad --mac value: %s\n", override_mac);
            return 1;
        }
        printf("MAC      %02x:%02x:%02x:%02x:%02x:%02x (from --mac)\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 0;
    }

    /* Report whether the chip's power-on EEPROM auto-load succeeded. */
    bool loaded = false;
    int kr = smsc95xx_eeprom_loaded(d, &loaded);
    if (kr == kIOReturnSuccess)
        printf("E2P_CMD  auto-load %s\n", loaded ? "LOADED" : "NOT loaded");

    uint8_t sig = 0;
    kr = smsc95xx_read_mac_verified(d, mac, &sig);

    if (kr == kIOReturnNotFound) {
        fprintf(stderr, "no EEPROM responding (E2P_CMD reported TIMEOUT)\n");
        return 1;
    }
    if (kr == kIOReturnNotReadable) {
        fprintf(stderr,
                "EEPROM signature mismatch: offset 0 read 0x%02X, expected 0x%02X.\n"
                "Refusing to guess a MAC. Pass --mac aa:bb:cc:dd:ee:ff to proceed.\n",
                sig, SMSC95XX_EEPROM_SIGNATURE);
        return 1;
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "MAC read failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    /* Signature matched, so the read is trustworthy. Pattern checks below are a
     * belt-and-braces guard against a signature byte that matched by accident. */
    bool all_zeros = true, all_ones = true;
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
        if (mac[i] != 0x00)
            all_zeros = false;
        if (mac[i] != 0xFF)
            all_ones = false;
    }
    if (all_zeros || all_ones) {
        fprintf(stderr, "EEPROM signature matched but MAC is invalid: "
                "%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 1;
    }

    /* Reject multicast source addresses (bit 0 of first octet). */
    if (mac[0] & 0x01) {
        fprintf(stderr, "EEPROM signature matched but MAC is multicast: "
                "%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 1;
    }

    printf("MAC      %02x:%02x:%02x:%02x:%02x:%02x (from EEPROM)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int cmd_init(usb_device *d, const char *override_mac)
{
    uint8_t mac[SMSC95XX_MAC_LEN] = {0};
    if (resolve_mac(d, override_mac, mac) != 0)
        return 1;

    /* Check if the PHY supports autonegotiation. This tool configures a fixed
     * 10 Mb/s half-duplex mode, which is what this hardware runs at, but a
     * conventional autoneg-capable adapter would be misconfigured and broken. */
    uint16_t bmsr = 0;
    int kr = smsc95xx_mii_read(d, SMSC95XX_PHY_ADDR, SMSC95XX_MII_BMSR, &bmsr);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "BMSR read failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }
    if (smsc95xx_bmsr_autoneg_capable(bmsr)) {
        fprintf(stderr,
                "This device appears to be an autonegotiating 10/100 adapter.\n"
                "This tool does not implement autonegotiation and would misconfigure\n"
                "the MAC. Refusing to initialize.\n");
        return 1;
    }

    kr = usb_claim_interface(d);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "claim interface failed: %s (0x%08x)\n",
                usb_strerror(kr), kr);
        return 1;
    }

    kr = smsc95xx_init(d, mac, true);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "init failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }
    printf("init     ok (10 Mb/s half duplex, promiscuous)\n");

    /* Read back the registers init just wrote, so success is demonstrated
     * rather than assumed. */
    uint32_t mac_cr = 0, tx_cfg = 0;
    bool up = false;
    int kr_mac = smsc95xx_read_reg(d, SMSC95XX_REG_MAC_CR, &mac_cr);
    int kr_tx = smsc95xx_read_reg(d, SMSC95XX_REG_TX_CFG, &tx_cfg);
    int rc = 0;

    if (kr_mac != kIOReturnSuccess) {
        fprintf(stderr, "MAC_CR readback failed: %s (0x%08x)\n",
                usb_strerror(kr_mac), kr_mac);
        rc = 1;
    } else {
        printf("MAC_CR   0x%08X  (TXEN=%d RXEN=%d RCVOWN=%d PRMS=%d FDPX=%d)\n",
               mac_cr,
               (mac_cr & SMSC95XX_MAC_CR_TXEN)   ? 1 : 0,
               (mac_cr & SMSC95XX_MAC_CR_RXEN)   ? 1 : 0,
               (mac_cr & SMSC95XX_MAC_CR_RCVOWN) ? 1 : 0,
               (mac_cr & SMSC95XX_MAC_CR_PRMS)   ? 1 : 0,
               (mac_cr & SMSC95XX_MAC_CR_FDPX)   ? 1 : 0);
    }

    if (kr_tx != kIOReturnSuccess) {
        fprintf(stderr, "TX_CFG readback failed: %s (0x%08x)\n",
                usb_strerror(kr_tx), kr_tx);
        rc = 1;
    } else {
        printf("TX_CFG   0x%08X\n", tx_cfg);
    }

    int kr_link = smsc95xx_link_up(d, &up);
    if (kr_link != kIOReturnSuccess) {
        fprintf(stderr, "link state read failed: %s (0x%08x)\n",
                usb_strerror(kr_link), kr_link);
    } else {
        printf("link     %s\n", up ? "up" : "down");
    }

    return rc;
}

static int cmd_tx(usb_device *d, const char *override_mac)
{
    uint8_t mac[SMSC95XX_MAC_LEN] = {0};
    if (resolve_mac(d, override_mac, mac) != 0)
        return 1;

    int kr = usb_claim_interface(d);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "claim interface failed: %s (0x%08x)\n",
                usb_strerror(kr), kr);
        return 1;
    }
    kr = smsc95xx_init(d, mac, true);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "init failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    /* A broadcast frame with an unassigned EtherType (0x88B5, reserved for
     * experimental use) carrying a recognisable payload, so it is easy to spot
     * in a capture on the far end and cannot be confused with real traffic. */
    uint8_t frame[SMSC95XX_FRAME_MIN] = {0};
    memset(frame, 0xFF, 6);                    /* destination: broadcast */
    memcpy(frame + 6, mac, SMSC95XX_MAC_LEN);  /* source: our address    */
    frame[12] = 0x88;
    frame[13] = 0xB5;
    static const char tag[] = "SMSC95XX-PROBE-M2";
    memcpy(frame + 14, tag, sizeof(tag) - 1);

    uint8_t xfer[SMSC95XX_TX_HEADER_LEN + sizeof(frame)];
    size_t hdr = smsc95xx_tx_prepend(xfer, sizeof(xfer), sizeof(frame));
    if (hdr == 0) {
        fprintf(stderr, "tx_prepend rejected frame length %zu\n", sizeof(frame));
        return 1;
    }
    memcpy(xfer + hdr, frame, sizeof(frame));

    kr = usb_bulk_out(d, xfer, (uint32_t)(hdr + sizeof(frame)));
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "bulk OUT failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    printf("tx       sent %zu bytes (%zu-byte frame + %zu-byte header)\n",
           hdr + sizeof(frame), sizeof(frame), hdr);
    printf("         dst ff:ff:ff:ff:ff:ff  ethertype 0x88B5  tag \"%s\"\n", tag);
    return 0;
}

static int cmd_rx(usb_device *d, const char *override_mac)
{
    uint8_t mac[SMSC95XX_MAC_LEN] = {0};
    if (resolve_mac(d, override_mac, mac) != 0)
        return 1;

    int kr = usb_claim_interface(d);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "claim interface failed: %s (0x%08x)\n",
                usb_strerror(kr), kr);
        return 1;
    }
    kr = smsc95xx_init(d, mac, true);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "init failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    /* BURST_CAP is set to 5 during init, in units of the 512-byte high-speed
     * max packet size, so a transfer can be up to 2560 bytes. */
    uint8_t buf[4096] = {0};
    printf("rx       waiting for a frame (10 attempts, 1s timeout each)...\n");

    for (int attempt = 0; attempt < 10; attempt++) {
        uint32_t got = (uint32_t)sizeof(buf);
        kr = usb_bulk_in(d, buf, &got);
        if (kr == kIOReturnTimeout || kr == kIOUSBTransactionTimeout) {
            printf("         attempt %d: timeout, no data\n", attempt);
            continue;
        }
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "bulk IN failed: %s (0x%08x)\n", usb_strerror(kr), kr);
            return 1;
        }
        if (got == 0) {
            printf("         attempt %d: zero-length transfer\n", attempt);
            continue;
        }

        printf("         got %u-byte transfer\n", got);

        /* Print the raw leading bytes. This is the ground truth for the RX
         * status-word layout, which was derived from the Linux source and never
         * checked against hardware -- so show it plainly rather than only
         * showing what the parser made of it. */
        printf("         raw[0..15]:");
        for (uint32_t i = 0; i < got && i < 16; i++)
            printf(" %02X", buf[i]);
        printf("\n");

        size_t offset = 0;
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        uint32_t status = 0;
        int n = 0;
        while (smsc95xx_rx_next(buf, got, &offset, &frame, &frame_len, &status)) {
            n++;
            printf("         frame %d: status=0x%08X len=%zu%s%s%s\n",
                   n, status, frame_len,
                   (status & SMSC95XX_RX_STS_ERROR_SUM) ? " ERROR_SUM" : "",
                   (status & SMSC95XX_RX_STS_CRC_ERROR) ? " CRC_ERROR" : "",
                   (status & SMSC95XX_RX_STS_FILTER_FAIL) ? " FILTER_FAIL" : "");
            if (frame_len >= 14) {
                printf("           dst %02x:%02x:%02x:%02x:%02x:%02x"
                       "  src %02x:%02x:%02x:%02x:%02x:%02x  type 0x%02X%02X\n",
                       frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                       frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                       frame[12], frame[13]);
            } else {
                printf("           frame too short (%zu bytes) to decode Ethernet header\n", frame_len);
            }
        }
        if (n == 0) {
            fprintf(stderr,
                    "         transfer decoded to 0 frames -- the RX status-word\n"
                    "         layout in smsc95xx_regs.h is probably wrong. Compare\n"
                    "         the raw bytes above against it before changing code.\n");
            return 1;
        }
        printf("rx       ok, %d frame(s)\n", n);
        return 0;
    }

    fprintf(stderr, "no frame received in 10 attempts\n");
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <id|phy|eeprom|init|tx|rx|all> [--mac aa:bb:cc:dd:ee:ff]\n"
        "\n"
        "Reads and drives a LAN9500A over USB. Tries the MACH SYSTEMS dongle\n"
        "(%04x:%04x and %04x:%04x) then the Microchip EVB (%04x:%04x).\n"
        "\n"
        "  --mac  Override the station address. Needed only when the EEPROM\n"
        "         signature does not verify; without it those commands refuse\n"
        "         to run rather than use a possibly-wrong address.\n",
        argv0,
        SMSC95XX_VID_MACH, SMSC95XX_PID_MACH_EE,
        SMSC95XX_VID_MACH, SMSC95XX_PID_MACH,
        SMSC95XX_VID_EVB,  SMSC95XX_PID_EVB);
}

int main(int argc, char **argv)
{
    /* Line-buffer stdout so stderr errors appear in order when redirected. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2 || argc > 4) {
        usage(argv[0]);
        return 2;
    }

    /* Parse optional --mac argument. */
    const char *override_mac = NULL;
    if (argc == 4) {
        if (strcmp(argv[2], "--mac") != 0) {
            usage(argv[0]);
            return 2;
        }
        override_mac = argv[3];
    } else if (argc == 3) {
        usage(argv[0]);
        return 2;
    }

    /* Validate subcommand before opening the device. */
    if (strcmp(argv[1], "id") != 0 &&
        strcmp(argv[1], "phy") != 0 &&
        strcmp(argv[1], "eeprom") != 0 &&
        strcmp(argv[1], "init") != 0 &&
        strcmp(argv[1], "tx") != 0 &&
        strcmp(argv[1], "rx") != 0 &&
        strcmp(argv[1], "all") != 0) {
        usage(argv[0]);
        return 2;
    }

    uint16_t vid = 0, pid = 0;
    int kr = kIOReturnNoDevice;
    usb_device *d = usb_open_first(&vid, &pid, &kr);
    if (!d) {
        fprintf(stderr, "no supported device found: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }
    printf("device   %04x:%04x\n", vid, pid);

    int rc;
    if (!strcmp(argv[1], "id")) {
        rc = cmd_id(d);
    } else if (!strcmp(argv[1], "phy")) {
        rc = cmd_phy(d);
    } else if (!strcmp(argv[1], "eeprom")) {
        rc = cmd_eeprom(d);
    } else if (!strcmp(argv[1], "init")) {
        rc = cmd_init(d, override_mac);
    } else if (!strcmp(argv[1], "tx")) {
        rc = cmd_tx(d, override_mac);
    } else if (!strcmp(argv[1], "rx")) {
        rc = cmd_rx(d, override_mac);
    } else { /* "all" */
        /* Run every subcommand even if an earlier one fails. A diagnostic tool
         * should not withhold the EEPROM state just because the MII path
         * stalled -- that is exactly when you want the rest of the picture.
         * Report failure if any of them failed. */
        rc = cmd_id(d);
        printf("\n");
        rc |= cmd_phy(d);
        printf("\n");
        rc |= cmd_eeprom(d);
    }

    usb_close(d);
    return rc;
}
