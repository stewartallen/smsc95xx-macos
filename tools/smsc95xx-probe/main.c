/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smsc95xx-probe: read identity, PHY state, and EEPROM MAC from a LAN9500A
 * over USB control transfers, without loading any driver.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <IOKit/IOReturn.h>

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
    uint8_t mac[SMSC95XX_MAC_LEN] = {0};
    int kr = smsc95xx_read_mac(d, mac);
    if (kr == kIOReturnNotFound) {
        fprintf(stderr, "no EEPROM responding (E2P_CMD reported TIMEOUT)\n");
        return 1;
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "EEPROM read failed: %s (0x%08x)\n", usb_strerror(kr), kr);
        return 1;
    }

    /* Check for invalid MAC patterns: all zeros or all ones indicate read failure,
     * not a valid address. */
    bool all_zeros = true, all_ones = true;
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
        if (mac[i] != 0x00)
            all_zeros = false;
        if (mac[i] != 0xFF)
            all_ones = false;
    }
    if (all_zeros || all_ones) {
        fprintf(stderr, "EEPROM read succeeded but returned invalid data: %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 1;
    }

    printf("MAC      %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("         %s administered\n",
           (mac[0] & SMSC95XX_MAC_LOCALLY_ADMINISTERED) ? "locally" : "globally (OUI)");

    uint32_t addrl = 0, addrh = 0;
    smsc95xx_mac_to_regs(mac, &addrl, &addrh);
    printf("ADDRL    0x%08X\nADDRH    0x%08X\n", addrl, addrh);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <id|phy|eeprom|all>\n"
        "\n"
        "Reads a LAN9500A over USB control transfers. Tries the MACH SYSTEMS\n"
        "dongle (%04x:%04x) then the Microchip EVB (%04x:%04x).\n",
        argv0,
        SMSC95XX_VID_MACH, SMSC95XX_PID_MACH,
        SMSC95XX_VID_EVB, SMSC95XX_PID_EVB);
}

int main(int argc, char **argv)
{
    /* Line-buffer stdout so stderr errors appear in order when redirected. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }

    /* Validate subcommand before opening the device. */
    if (strcmp(argv[1], "id") != 0 &&
        strcmp(argv[1], "phy") != 0 &&
        strcmp(argv[1], "eeprom") != 0 &&
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
    } else { /* "all" */
        rc = cmd_id(d);
        if (!rc) { printf("\n"); rc = cmd_phy(d); }
        if (!rc) { printf("\n"); rc = cmd_eeprom(d); }
    }

    usb_close(d);
    return rc;
}
