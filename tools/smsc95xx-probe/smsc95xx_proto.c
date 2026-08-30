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

/* Store a u32 little-endian, so the layout is explicit rather than relying on
 * host byte order. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

size_t smsc95xx_tx_prepend(uint8_t *buf, size_t buflen, size_t frame_len)
{
    if (buflen < SMSC95XX_TX_HEADER_LEN)
        return 0;
    if (frame_len < SMSC95XX_FRAME_MIN || frame_len > SMSC95XX_FRAME_MAX)
        return 0;

    uint32_t len = (uint32_t)frame_len;
    uint32_t cmd_a = (len & SMSC95XX_TX_CMD_A_LEN_MASK) |
                     SMSC95XX_TX_CMD_A_FIRST_SEG |
                     SMSC95XX_TX_CMD_A_LAST_SEG;
    uint32_t cmd_b = len & SMSC95XX_TX_CMD_B_LEN_MASK;

    put_le32(buf, cmd_a);
    put_le32(buf + 4, cmd_b);
    return SMSC95XX_TX_HEADER_LEN;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool smsc95xx_rx_next(const uint8_t *buf, size_t len, size_t *offset,
                      const uint8_t **frame, size_t *frame_len,
                      uint32_t *status)
{
    size_t at = *offset;

    /* Need a whole status word to make any progress. */
    if (at + SMSC95XX_RX_HEADER_LEN > len)
        return false;

    uint32_t sts = get_le32(buf + at);
    size_t flen = (size_t)((sts & SMSC95XX_RX_STS_LEN_MASK) >>
                           SMSC95XX_RX_STS_LEN_SHIFT);

    /* The frame length must include the 4-byte CRC appended by the hardware.
     * A length below that minimum is not decodable. Also reject zero and any
     * frame that runs past the buffer. */
    if (flen < SMSC95XX_RX_CRC_LEN || flen == 0)
        return false;
    if (at + SMSC95XX_RX_HEADER_LEN + flen > len)
        return false;

    *frame = buf + at + SMSC95XX_RX_HEADER_LEN;
    *frame_len = flen;
    *status = sts;

    /* Advance past this record, then to the next 4-byte boundary. */
    size_t used = SMSC95XX_RX_HEADER_LEN + flen;
    *offset = at + used + ((4 - (used % 4)) % 4);
    return true;
}
