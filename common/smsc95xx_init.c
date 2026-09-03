/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Stewart Allen
 * Derived from the Linux smsc95xx driver, Copyright (C) 2007-2008 SMSC. See NOTICE.
 */
#include "smsc95xx_init.h"

#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"

/* Write a register and stop at the first failure, to keep the sequence readable. */
#define WR(off, val)                                     \
    do {                                                 \
        int wkr = io->write(io->ctx, (off), (val));       \
        if (wkr != 0)                                    \
            return wkr;                                  \
    } while (0)

int smsc95xx_init_seq(const smsc95xx_io *io, const uint8_t mac[6], bool promiscuous)
{
    uint32_t v = 0;
    int kr;

    /* A null callback here would be a null call in the middle of a partially
     * initialised chip, so check rather than assume. */
    if (io == NULL || io->read == NULL || io->write == NULL || mac == NULL)
        return SMSC95XX_INIT_ERR_BAD_ARG;

    /* Lite reset, then read-poll until the chip clears the bit itself -- no sleeping,
     * which keeps this layer free of any platform timing API. */
    WR(SMSC95XX_REG_HW_CFG, SMSC95XX_HW_CFG_LRST);
    for (int i = 0; ; i++) {
        kr = io->read(io->ctx, SMSC95XX_REG_HW_CFG, &v);
        if (kr != 0)
            return kr;
        if (!(v & SMSC95XX_HW_CFG_LRST))
            break;
        if (i + 1 >= SMSC95XX_INIT_RESET_POLLS)
            return SMSC95XX_INIT_ERR_RESET_TIMEOUT;
    }

    /* The station address. */
    {
        uint32_t addrl = 0, addrh = 0;
        smsc95xx_mac_to_regs(mac, &addrl, &addrh);
        WR(SMSC95XX_REG_ADDRL, addrl);
        WR(SMSC95XX_REG_ADDRH, addrh);
    }

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
    /* The captured bring-up enables the transmitter in MAC_CR BEFORE turning on the TX
     * datapath in TX_CFG, then enables the receiver afterwards. The ordering encodes an
     * undocumented hardware requirement; do not collapse these writes. */
    WR(SMSC95XX_REG_MAC_CR, SMSC95XX_MAC_CR_TXEN);
    WR(SMSC95XX_REG_TX_CFG, SMSC95XX_TX_CFG_ON);
    WR(SMSC95XX_REG_MAC_CR, mac_cr);

    /* AFC_CFG's low nibble is set on the half-duplex path. */
    WR(SMSC95XX_REG_AFC_CFG,
       SMSC95XX_AFC_CFG_INIT | SMSC95XX_AFC_CFG_HALF_DUPLEX_BITS);

    /* PHY reset is read-modify-write: the surrounding bits differ between devices and
     * must be preserved. The captured bring-up does not poll for PHY_RST completion --
     * it proceeds straight to MII traffic -- so neither does this. */
    kr = io->read(io->ctx, SMSC95XX_REG_PM_CTRL, &v);
    if (kr != 0)
        return kr;
    WR(SMSC95XX_REG_PM_CTRL, v | SMSC95XX_PM_CTRL_PHY_RST);

    return 0;
}

#undef WR

const char *smsc95xx_reg_name(uint16_t offset)
{
    switch (offset) {
    case SMSC95XX_REG_ID_REV:       return "ID_REV";
    case SMSC95XX_REG_INT_STS:      return "INT_STS";
    case SMSC95XX_REG_RX_CFG:       return "RX_CFG";
    case SMSC95XX_REG_TX_CFG:       return "TX_CFG";
    case SMSC95XX_REG_HW_CFG:       return "HW_CFG";
    case SMSC95XX_REG_PM_CTRL:      return "PM_CTRL";
    case SMSC95XX_REG_LED_GPIO_CFG: return "LED_GPIO_CFG";
    case SMSC95XX_REG_AFC_CFG:      return "AFC_CFG";
    case SMSC95XX_REG_E2P_CMD:      return "E2P_CMD";
    case SMSC95XX_REG_E2P_DATA:     return "E2P_DATA";
    case SMSC95XX_REG_BURST_CAP:    return "BURST_CAP";
    case SMSC95XX_REG_INT_EP_CTL:   return "INT_EP_CTL";
    case SMSC95XX_REG_BULK_IN_DLY:  return "BULK_IN_DLY";
    case SMSC95XX_REG_MAC_CR:       return "MAC_CR";
    case SMSC95XX_REG_ADDRH:        return "ADDRH";
    case SMSC95XX_REG_ADDRL:        return "ADDRL";
    case SMSC95XX_REG_HASHH:        return "HASHH";
    case SMSC95XX_REG_HASHL:        return "HASHL";
    case SMSC95XX_REG_MII_ADDR:     return "MII_ADDR";
    case SMSC95XX_REG_MII_DATA:     return "MII_DATA";
    case SMSC95XX_REG_FLOW:         return "FLOW";
    case SMSC95XX_REG_VLAN1:        return "VLAN1";
    case SMSC95XX_REG_COE_CR:       return "COE_CR";
    default:                        return "?";
    }
}
