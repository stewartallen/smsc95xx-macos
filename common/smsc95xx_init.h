/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Stewart Allen
 * Derived from the Linux smsc95xx driver, Copyright (C) 2007-2008 SMSC. See NOTICE.
 *
 * LAN9500A power-on initialisation, parameterised by register I/O callbacks.
 *
 * Shared verbatim by the userspace probe and the DriverKit extension so the two cannot
 * drift. Pure C with no platform headers, no allocation and no sleeping: the reset wait
 * is a read-poll, and the caller supplies register access through callbacks, which is
 * what makes the sequence unit-testable on the host.
 *
 * The register values and their ORDER are transcribed from a capture of the Linux
 * smsc95xx driver bringing this hardware up (reference/mach-init-sequence.txt). The
 * ordering encodes undocumented hardware requirements -- see the MAC_CR/TX_CFG comments
 * in smsc95xx_init.c -- so it must not be reordered.
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
 * Both callbacks return 0 on success and any non-zero value on failure; that value is
 * propagated to the caller of smsc95xx_init_seq unchanged, so a caller whose transport
 * speaks IOReturn gets its own IOReturn back. `ctx` is passed through untouched and is
 * never dereferenced here.
 *
 * `read` must leave *value untouched on failure. `write` must not retry: the sequence
 * stops at the first failure, because a chip that rejected one register write is not in
 * a state where the remaining writes mean anything. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint16_t offset, uint32_t *value);
    int (*write)(void *ctx, uint16_t offset, uint32_t value);
} smsc95xx_io;

/* Failures that originate here rather than in a callback.
 *
 * Deliberately NOT IOReturn values: common/ carries no platform headers, so it cannot
 * name kIOReturnTimeout. They are small negatives, which no IOReturn failure ever is
 * (those are 0xe00002xx), so a caller can tell the two apart and map these onto its own
 * error space. */
#define SMSC95XX_INIT_ERR_BAD_ARG       (-1)
#define SMSC95XX_INIT_ERR_RESET_TIMEOUT (-2)
#define SMSC95XX_INIT_ERR_E2P_TIMEOUT   (-3)   /* E2P engine never went un-busy       */
#define SMSC95XX_INIT_ERR_E2P_NO_EEPROM (-4)   /* E2P_CMD.TIMEOUT: no EEPROM responded */

/* Maximum read-polls of HW_CFG while waiting for the chip to clear LRST itself. The
 * chip clears it almost immediately; the bound only keeps broken hardware from hanging
 * the caller. */
#define SMSC95XX_INIT_RESET_POLLS       100

/* Maximum read-polls of E2P_CMD per EEPROM byte; same rationale as the reset bound. */
#define SMSC95XX_INIT_E2P_POLLS         100

/* Number of register WRITES the sequence performs when every callback succeeds.
 * Exported so a caller can log progress and the unit tests can assert the count. */
#define SMSC95XX_INIT_WRITE_COUNT       20

/* Run the full initialisation sequence and leave the chip receiving and transmitting.
 *
 * Writes 20 registers in a fixed order, including the station address in ADDRL/ADDRH
 * and the final MAC_CR write that switches the datapath on. Configures the MAC for
 * 10 Mb/s half duplex -- RCVOWN set, FDPX clear, the low nibble of AFC_CFG set -- which
 * is what this hardware runs at; it has no autonegotiation. When `promiscuous` is true,
 * MAC_CR.PRMS is set as well.
 *
 * Two registers the captured Linux bring-up writes are deliberately NOT written:
 * checksum offload (COE_CR), which is out of scope and would add two bytes to every
 * received frame; and the interrupt endpoint (INT_EP_CTL), because link state is polled
 * instead.
 *
 * Returns 0 on success, a callback's own non-zero error otherwise, or one of the
 * SMSC95XX_INIT_ERR_* codes above. On failure the chip is left partially initialised:
 * the caller must not present a working interface. */
int smsc95xx_init_seq(const smsc95xx_io *io, const uint8_t mac[6], bool promiscuous);

/* Read `len` bytes of EEPROM starting at `offset`, one E2P_CMD/E2P_DATA cycle per byte,
 * polling E2P_CMD between bytes the way the captured Linux bring-up does.
 *
 * Bounds are checked so a huge `len` cannot wrap: the address space is
 * SMSC95XX_E2P_SIZE (512) bytes. Returns 0 on success, a callback's own non-zero error
 * unchanged, or one of the SMSC95XX_INIT_ERR_* codes: E2P_TIMEOUT when the engine never
 * goes un-busy, E2P_NO_EEPROM when the chip's own TIMEOUT bit reports that no EEPROM
 * answered.
 *
 * Reading is all this driver needs; there is deliberately no write: a bad EEPROM write
 * can brick the dongle's identity, and nothing in bring-up requires one. */
int smsc95xx_eeprom_read(const smsc95xx_io *io, uint16_t offset, uint8_t *buf,
                         size_t len);

/* Name of a register offset, for logging. Returns a static string, never NULL; unknown
 * offsets come back as "?". Pure lookup -- no I/O, no allocation. */
const char *smsc95xx_reg_name(uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* SMSC95XX_INIT_H */
