# smsc95xx-probe

Reads identity, PHY state, and the EEPROM MAC address from a LAN9500A-based
USB Ethernet dongle, using only userspace USB control transfers.

Its purpose is to validate the smsc95xx register protocol independently of
DriverKit. It needs no entitlements, no SIP changes, and no driver loading,
because register access happens over endpoint 0 and the USB interface is never
claimed.

## Build and run

```sh
make            # builds smsc95xx-probe
make test       # runs unit tests for the pure protocol layer (no hardware)
./smsc95xx-probe all
```

**Note:** On the MACH SYSTEMS dongle (`0424:9e00`), the `./smsc95xx-probe eeprom` and
`./smsc95xx-probe all` commands exit with code 1, because the EEPROM read returns invalid
data (all zeros). This is a hardware finding, not a tool bug. Anyone scripting this tool
should expect that exit code.

## Subcommands

| Command | What it reads |
|---|---|
| `id` | `ID_REV` — chip id and revision |
| `phy` | `BMCR`, `BMSR`, `PHYID1/2`, `ANAR`, `ANLPAR` at MII address 0, plus decoded link and autoneg state |
| `eeprom` | The six-byte MAC at EEPROM offset 1, and the `ADDRL`/`ADDRH` values it packs into |
| `all` | All of the above |

## Supported devices

Tried in order: MACH SYSTEMS `0424:9e00`, then Microchip EVB-LAN8670-USB
`184f:0051`.

## Expected output

Values should match those in `reference/*-init-sequence.txt`, which were
captured from these same devices under the Linux `smsc95xx` driver. A
mismatch means either this tool is wrong or the hardware differs from what was
characterized — investigate rather than adjusting expectations.

## Layout

| File | Responsibility |
|---|---|
| `smsc95xx_regs.h` | Register offsets and bit masks. Constants only. |
| `smsc95xx_proto.c` | Pure integer encode/decode. No I/O, fully unit-tested, destined to be lifted into the dext. |
| `usb_backend.c` | IOUSBLib control transfers. |
| `smsc95xx_ops.c` | Register, MII, and EEPROM operations. |
| `main.c` | CLI. |

## Known findings

On the MACH SYSTEMS test unit (`0424:9e00`):

- **EEPROM reads as all zeros:** offsets 0–8 all return `0x00`, including offset 0 which should
  hold the `0xA5` signature on a programmed EEPROM. `ADDRL`/`ADDRH` read their power-on defaults
  (`0xFFFFFFFF`/`0x0000FFFF`). A `HW_CFG` LRST does not change this.

- **E2P_CMD readback echoes the requested address:** When the tool writes `E2P_CMD = 0x80000001`
  for an EEPROM read at offset 1, the readback is `0x00000001` (BUSY bit cleared, address bits
  preserved). The same pattern holds for offsets 2–6: `0x80000002` → `0x00000002`, etc. This
  proves the write and readback are landing correctly in the hardware. The EEPROM operation
  completes (BUSY bit clears), but no data is returned — the issue is at the EEPROM level, not
  the register protocol.

- **Discrepancy with reference trace:** `reference/mach-init-sequence.txt` records Linux reading
  `4A F8 F8 C2 C2 F2` from EEPROM offsets 1–6 on this same dongle. That decode has been
  re-verified against the raw pcap frames.

- **Everything else matches the reference exactly:** `ID_REV 0x9E000002`, `BMCR 0x0000`,
  `BMSR 0x0805`, PHY ID `0x0007:0x4165`, autoneg not supported.

**Cause: not yet established.** One untested hypothesis worth checking: in the Linux capture the
EEPROM read completed at ~325.6 s while the dongle's STM32 companion enumerated at ~326.4 s
(i.e., the EEPROM read happened *before* the STM32 came up). If the STM32 shares or arbitrates
the EEPROM bus, the access window may only be open right after a fresh plug-in, before the STM32
boots. This remains speculation without further test data.

The Microchip EVB (`184f:0051`) has never been tested against live hardware. Its support is by
source code inspection, and the test vectors in `tests/test_proto.c` come from a capture file
but have not been validated in a real run.
