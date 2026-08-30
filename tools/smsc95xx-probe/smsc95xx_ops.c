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
