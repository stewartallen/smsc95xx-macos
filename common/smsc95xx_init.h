/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The LAN9500A power-on initialisation sequence, parameterised by register I/O.
 *
 * This is the one thing that MUST be identical in the userspace probe and in the
 * DriverKit extension, so it lives in common/ alongside the framing helpers rather
 * than being written twice. Everything here is pure C with no platform headers, no
 * allocation and no sleeping: the two reset waits are read-polls, and the caller
 * supplies register access through a pair of callbacks. That is what makes the
 * sequence testable on the host with a mock that simply records every write.
 *
 * Provenance: the register values and their ORDER are transcribed from the decoded
 * capture of the real Linux driver bringing this hardware up
 * (reference/mach-init-sequence.txt) and were proven end to end at milestone M2,
 * where the probe used exactly this sequence to transmit and receive Ethernet
 * frames over 10BASE-T1S. The ordering is load-bearing -- see the comments at the
 * MAC_CR/TX_CFG writes -- so it must not be tidied or reordered.
 */
#ifndef SMSC95XX_INIT_H
#define SMSC95XX_INIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register access, supplied by the caller.
 *
 * Both callbacks return 0 on success and any non-zero value on failure; that value
 * is propagated to the caller of smsc95xx_init_seq unchanged, so a caller whose
 * transport speaks IOReturn gets its own IOReturn back. `ctx` is passed through
 * untouched and is never dereferenced here.
 *
 * `read` must leave *value untouched on failure. `write` must not retry: the
 * sequence stops at the first failure on purpose, because a chip that rejected one
 * register write is not in a state where the remaining writes mean anything. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint16_t offset, uint32_t *value);
    int (*write)(void *ctx, uint16_t offset, uint32_t value);
} smsc95xx_io;

/* Failures that originate here rather than in a callback.
 *
 * Deliberately NOT IOReturn values: common/ carries no platform headers, so it
 * cannot name kIOReturnTimeout. They are small negatives, which no IOReturn ever
 * is (IOReturn failure codes are 0xe00002xx), so a caller can tell the two apart
 * and map these onto its own error space -- both callers do. */
#define SMSC95XX_INIT_ERR_BAD_ARG       (-1)
#define SMSC95XX_INIT_ERR_RESET_TIMEOUT (-2)

/* Read-polls of HW_CFG allowed while waiting for the chip to clear LRST itself.
 * The captured traces show it clear on the first read; the bound exists only so
 * broken hardware cannot hang the caller. */
#define SMSC95XX_INIT_RESET_POLLS       100

/* Number of register WRITES the sequence performs when every callback succeeds.
 * Exported so a caller can log progress meaningfully and so the unit tests can
 * assert the count rather than only the contents. */
#define SMSC95XX_INIT_WRITE_COUNT       20

/* Run the full initialisation sequence and leave the chip receiving and
 * transmitting.
 *
 * Writes 20 registers in a fixed order, including MAC_CR three times -- the last
 * of which carries RXEN and TXEN, i.e. this is the call that switches the datapath
 * on -- plus the station address in ADDRL/ADDRH, TX_CFG, HW_CFG (with the
 * multiple-Ethernet-frames bit the RX decoder depends on), BURST_CAP, BULK_IN_DLY,
 * AFC_CFG, FLOW, HASHH/HASHL, VLAN1, LED_GPIO_CFG, PM_CTRL and INT_STS.
 *
 * Two registers the captured Linux bring-up writes are deliberately NOT written:
 * checksum offload (COE_CR), which is out of scope and would add two bytes to every
 * received frame; and the interrupt endpoint (INT_EP_CTL), because link state is
 * polled instead.
 *
 * Configures the MAC for 10 Mb/s half duplex -- RCVOWN set, FDPX clear, and the low
 * nibble of AFC_CFG set -- which is what this hardware runs at; it has no
 * autonegotiation. When `promiscuous` is true, MAC_CR.PRMS is set as well.
 *
 * Returns 0 on success, a callback's own non-zero error otherwise, or one of the
 * SMSC95XX_INIT_ERR_* codes above. On failure the chip is left partially
 * initialised: the caller must not present a working interface. */
int smsc95xx_init_seq(const smsc95xx_io *io, const uint8_t mac[6], bool promiscuous);

/* Name of a register offset, for logging. Returns a static string, never NULL;
 * unknown offsets come back as "?". Pure lookup -- no I/O, no allocation -- so it
 * is usable from a completion handler or a dext log line. */
const char *smsc95xx_reg_name(uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* SMSC95XX_INIT_H */
