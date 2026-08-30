/* SPDX-License-Identifier: GPL-2.0 */
#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"

uint32_t smsc95xx_mii_addr_word(uint8_t phy, uint8_t reg, bool write)
{
    uint32_t word = SMSC95XX_MII_BUSY;

    word |= ((uint32_t)(phy & SMSC95XX_MII_PHY_MASK)) << SMSC95XX_MII_PHY_SHIFT;
    word |= ((uint32_t)(reg & SMSC95XX_MII_REG_MASK)) << SMSC95XX_MII_REG_SHIFT;
    if (write)
        word |= SMSC95XX_MII_WRITE;

    return word;
}

uint32_t smsc95xx_e2p_read_cmd(uint16_t offset)
{
    /* Opcode READ is zero, so the command is just BUSY plus the address. */
    return SMSC95XX_E2P_BUSY | ((uint32_t)offset & SMSC95XX_E2P_ADDR_MASK);
}

void smsc95xx_id_rev_split(uint32_t id_rev, uint16_t *chip, uint16_t *rev)
{
    *chip = (uint16_t)(id_rev >> 16);
    *rev  = (uint16_t)(id_rev & 0xFFFFu);
}

void smsc95xx_mac_to_regs(const uint8_t mac[6], uint32_t *addrl, uint32_t *addrh)
{
    *addrl = (uint32_t)mac[0]        | ((uint32_t)mac[1] << 8) |
             ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    *addrh = (uint32_t)mac[4]        | ((uint32_t)mac[5] << 8);
}

void smsc95xx_regs_to_mac(uint32_t addrl, uint32_t addrh, uint8_t mac[6])
{
    mac[0] = (uint8_t)(addrl & 0xFF);
    mac[1] = (uint8_t)((addrl >> 8) & 0xFF);
    mac[2] = (uint8_t)((addrl >> 16) & 0xFF);
    mac[3] = (uint8_t)((addrl >> 24) & 0xFF);
    mac[4] = (uint8_t)(addrh & 0xFF);
    mac[5] = (uint8_t)((addrh >> 8) & 0xFF);
}

bool smsc95xx_eeprom_sig_valid(uint8_t sig)
{
    return sig == SMSC95XX_EEPROM_SIGNATURE;
}

bool smsc95xx_bmsr_link_up(uint16_t bmsr)
{
    return (bmsr & SMSC95XX_BMSR_LINK_UP) != 0;
}

bool smsc95xx_bmsr_autoneg_capable(uint16_t bmsr)
{
    return (bmsr & SMSC95XX_BMSR_AUTONEG_CAP) != 0;
}
