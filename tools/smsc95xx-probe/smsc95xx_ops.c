/* SPDX-License-Identifier: GPL-2.0 */
#include "smsc95xx_ops.h"

#include <stdbool.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOReturn.h>

#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"

/* Both the MII and EEPROM engines are polled for completion. The captured
 * traces show these clearing within a handful of reads, so a small bound is
 * plenty; it exists to avoid hanging on broken hardware. */
#define POLL_ATTEMPTS 100

int smsc95xx_read_reg(usb_device *d, uint16_t offset, uint32_t *value)
{
    uint32_t raw = 0;
    int kr = usb_control_in(d, SMSC95XX_REQ_READ_REGISTER, 0, offset,
                            &raw, sizeof(raw));
    if (kr == kIOReturnSuccess)
        *value = OSSwapLittleToHostInt32(raw);
    return kr;
}

int smsc95xx_write_reg(usb_device *d, uint16_t offset, uint32_t value)
{
    uint32_t raw = OSSwapHostToLittleInt32(value);
    return usb_control_out(d, SMSC95XX_REQ_WRITE_REGISTER, 0, offset,
                           &raw, sizeof(raw));
}

/* Poll a register until the given mask clears. */
static int wait_clear(usb_device *d, uint16_t offset, uint32_t mask)
{
    for (int i = 0; i < POLL_ATTEMPTS; i++) {
        uint32_t v = 0;
        int kr = smsc95xx_read_reg(d, offset, &v);
        if (kr != kIOReturnSuccess)
            return kr;
        if (!(v & mask))
            return kIOReturnSuccess;
    }
    return kIOReturnTimeout;
}

int smsc95xx_mii_read(usb_device *d, uint8_t phy, uint8_t reg, uint16_t *value)
{
    int kr = wait_clear(d, SMSC95XX_REG_MII_ADDR, SMSC95XX_MII_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    kr = smsc95xx_write_reg(d, SMSC95XX_REG_MII_ADDR,
                            smsc95xx_mii_addr_word(phy, reg, false));
    if (kr != kIOReturnSuccess)
        return kr;

    kr = wait_clear(d, SMSC95XX_REG_MII_ADDR, SMSC95XX_MII_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    uint32_t data = 0;
    kr = smsc95xx_read_reg(d, SMSC95XX_REG_MII_DATA, &data);
    if (kr == kIOReturnSuccess)
        *value = (uint16_t)(data & 0xFFFFu);
    return kr;
}

int smsc95xx_eeprom_read(usb_device *d, uint16_t offset, uint8_t *buf, size_t len)
{
    /* EEPROM addresses are 9 bits; reject out-of-range requests. */
    if (offset + len > 0x200)
        return kIOReturnBadArgument;

    int kr = wait_clear(d, SMSC95XX_REG_E2P_CMD, SMSC95XX_E2P_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    for (size_t i = 0; i < len; i++) {
        kr = smsc95xx_write_reg(d, SMSC95XX_REG_E2P_CMD,
                                smsc95xx_e2p_read_cmd((uint16_t)(offset + i)));
        if (kr != kIOReturnSuccess)
            return kr;

        /* Read back E2P_CMD to confirm the operation retired and did not time
         * out. The captured trace does exactly this between each byte. */
        uint32_t cmd = 0;
        bool retired = false;
        for (int attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
            kr = smsc95xx_read_reg(d, SMSC95XX_REG_E2P_CMD, &cmd);
            if (kr != kIOReturnSuccess)
                return kr;
            if (!(cmd & SMSC95XX_E2P_BUSY)) {
                retired = true;
                break;
            }
        }
        if (!retired)
            return kIOReturnTimeout;
        if (cmd & SMSC95XX_E2P_TIMEOUT)
            return kIOReturnNotFound;   /* no EEPROM responding */

        uint32_t data = 0;
        kr = smsc95xx_read_reg(d, SMSC95XX_REG_E2P_DATA, &data);
        if (kr != kIOReturnSuccess)
            return kr;
        buf[i] = (uint8_t)(data & 0xFFu);
    }
    return kIOReturnSuccess;
}

int smsc95xx_read_mac(usb_device *d, uint8_t mac[6])
{
    return smsc95xx_eeprom_read(d, SMSC95XX_EEPROM_MAC_OFFSET, mac,
                                SMSC95XX_MAC_LEN);
}

int smsc95xx_eeprom_loaded(usb_device *d, bool *loaded)
{
    uint32_t cmd = 0;
    int kr = smsc95xx_read_reg(d, SMSC95XX_REG_E2P_CMD, &cmd);
    if (kr == kIOReturnSuccess && loaded)
        *loaded = (cmd & SMSC95XX_E2P_LOADED) != 0;
    return kr;
}

int smsc95xx_read_mac_verified(usb_device *d, uint8_t mac[6], uint8_t *sig_out)
{
    uint8_t sig = 0;
    int kr = smsc95xx_eeprom_read(d, SMSC95XX_EEPROM_SIG_OFFSET, &sig, 1);
    if (kr != kIOReturnSuccess)
        return kr;
    if (sig_out)
        *sig_out = sig;
    if (!smsc95xx_eeprom_sig_valid(sig))
        return kIOReturnNotReadable;

    return smsc95xx_read_mac(d, mac);
}

int smsc95xx_set_mac(usb_device *d, const uint8_t mac[6])
{
    uint32_t addrl = 0, addrh = 0;
    smsc95xx_mac_to_regs(mac, &addrl, &addrh);

    int kr = smsc95xx_write_reg(d, SMSC95XX_REG_ADDRL, addrl);
    if (kr != kIOReturnSuccess)
        return kr;
    return smsc95xx_write_reg(d, SMSC95XX_REG_ADDRH, addrh);
}

/* Read BMSR bit 2 and report it.
 *
 * NOTE: on this hardware that bit is NOT a reliable indication of link state.
 * With the 10BASE-T1S cable physically unplugged it still reads set, verified
 * over twelve consecutive direct reads. T1S is a multidrop bus with no
 * continuous idle signalling, so a clause-22 PHY has nothing to detect while
 * the medium is quiet; genuine link and PLCA status live in clause-45 MMD
 * registers. Treat this as a raw register readout, not as link truth, and do
 * not gate anything on it. */
int smsc95xx_link_up(usb_device *d, bool *up)
{
    uint16_t bmsr = 0;
    int kr = smsc95xx_mii_read(d, SMSC95XX_PHY_ADDR, SMSC95XX_MII_BMSR, &bmsr);
    if (kr == kIOReturnSuccess && up)
        *up = smsc95xx_bmsr_link_up(bmsr);
    return kr;
}

/* Write a register and stop at the first failure, to keep init readable. */
#define WR(off, val)                                     \
    do {                                                 \
        int wkr = smsc95xx_write_reg(d, (off), (val));   \
        if (wkr != kIOReturnSuccess)                     \
            return wkr;                                  \
    } while (0)

int smsc95xx_init(usb_device *d, const uint8_t mac[6], bool promiscuous)
{
    uint32_t v = 0;
    int kr;

    /* Lite reset, then wait for the chip to clear the bit itself. */
    WR(SMSC95XX_REG_HW_CFG, SMSC95XX_HW_CFG_LRST);
    for (int i = 0; ; i++) {
        kr = smsc95xx_read_reg(d, SMSC95XX_REG_HW_CFG, &v);
        if (kr != kIOReturnSuccess)
            return kr;
        if (!(v & SMSC95XX_HW_CFG_LRST))
            break;
        if (i >= 100)
            return kIOReturnTimeout;
    }

    kr = smsc95xx_set_mac(d, mac);
    if (kr != kIOReturnSuccess)
        return kr;

    WR(SMSC95XX_REG_HW_CFG,      SMSC95XX_HW_CFG_INIT_1);
    WR(SMSC95XX_REG_BURST_CAP,   SMSC95XX_BURST_CAP_INIT);
    WR(SMSC95XX_REG_BULK_IN_DLY, SMSC95XX_BULK_IN_DLY_INIT);
    WR(SMSC95XX_REG_HW_CFG,      SMSC95XX_HW_CFG_INIT_2);
    WR(SMSC95XX_REG_INT_STS,     SMSC95XX_INT_STS_CLEAR_ALL);
    WR(SMSC95XX_REG_LED_GPIO_CFG, SMSC95XX_LED_GPIO_CFG_INIT);
    WR(SMSC95XX_REG_FLOW,        0);
    WR(SMSC95XX_REG_AFC_CFG,     SMSC95XX_AFC_CFG_INIT);
    WR(SMSC95XX_REG_VLAN1,       SMSC95XX_VLAN1_INIT);
    WR(SMSC95XX_REG_HASHH,       0);
    WR(SMSC95XX_REG_HASHL,       0);

    /* COE_CR and INT_EP_CTL are deliberately not written -- see the header. */

    /* Half-duplex 10 Mb/s: RCVOWN set, FDPX clear. */
    uint32_t mac_cr = SMSC95XX_MAC_CR_TXEN | SMSC95XX_MAC_CR_RXEN |
                      SMSC95XX_MAC_CR_RCVOWN;
    if (promiscuous)
        mac_cr |= SMSC95XX_MAC_CR_PRMS;

    WR(SMSC95XX_REG_MAC_CR, 0);
    /* The captured sequence enables the transmitter in MAC_CR BEFORE turning on
     * the TX datapath in TX_CFG, then enables the receiver afterwards. Both
     * captures agree on this ordering; preserve it rather than collapsing the
     * writes, since these orderings encode undocumented hardware requirements. */
    WR(SMSC95XX_REG_MAC_CR, SMSC95XX_MAC_CR_TXEN);
    WR(SMSC95XX_REG_TX_CFG, SMSC95XX_TX_CFG_ON);
    WR(SMSC95XX_REG_MAC_CR, mac_cr);

    /* AFC_CFG's low nibble is set on the half-duplex path. */
    WR(SMSC95XX_REG_AFC_CFG,
       SMSC95XX_AFC_CFG_INIT | SMSC95XX_AFC_CFG_HALF_DUPLEX_BITS);

    /* PHY reset is read-modify-write: the surrounding bits differ between
     * devices and must be preserved. */
    kr = smsc95xx_read_reg(d, SMSC95XX_REG_PM_CTRL, &v);
    if (kr != kIOReturnSuccess)
        return kr;
    WR(SMSC95XX_REG_PM_CTRL, v | SMSC95XX_PM_CTRL_PHY_RST);

    return kIOReturnSuccess;
}

#undef WR

