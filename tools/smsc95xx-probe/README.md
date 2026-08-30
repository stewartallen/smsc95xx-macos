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

### EEPROM reads on the MACH unit are intermittently corrupt

**Measured 2026-08-30.** This supersedes two earlier descriptions in this file: first that the
EEPROM was unreadable, then that it was readable only inside a closing window. Both were
artifacts of under-sampling. The actual behaviour is *intermittent corruption*.

The MAC is at EEPROM offsets 1–6 and reads `4a:f8:f8:c2:c2:f2` — exactly what
`reference/mach-init-sequence.txt` shows Linux reading. The register protocol was always
correct.

Reading the full MAC 25 times back-to-back per attach, across three plug-in cycles (75 reads):

| Value returned | Count | What it is |
|---|---|---|
| `4a:f8:f8:c2:c2:f2` | 62 | correct |
| `00:00:00:00:00:00` | 8 | dead read |
| `4a:f8:1f:c2:c2:f2` | 2 | one byte corrupted |
| `4a:f8:f8:c2:58:00` | 1 | trailing bytes corrupted |
| `00:00:f8:c2:c2:f2` | 1 | leading bytes corrupted |
| `00:00:00:00:00:f2` | 1 | mostly dead |

Per-read correctness by cycle (`C` = correct, `x` = anything else), in order:

```
cycle 1  xCCCCCCCCxxxxxxxxxxCCCCCC
cycle 2  CCCCCCCCCCCCCCCCCCCCCCCCC
cycle 3  CxCCCCCCCCxCCCCCCCCCCCCCC
```

Three properties matter for anything consuming this:

1. **Failures come in bursts as well as singles.** Cycle 1 shows ten consecutive bad reads
   spanning roughly 370 ms, then full recovery. So a single retry is not enough, and a failed
   read does not mean the EEPROM is permanently gone.
2. **Every consecutive run of failures was all-zeros** — which a pattern check catches. The
   non-zero corruptions were always isolated single reads.
3. **A wholly plausible wrong address is possible.** A separate run returned
   `fc:61:79:90:04:56`: not zeros, not all-ones, not multicast, and with the
   locally-administered bit clear, so it looks like a legitimately assigned address. Pattern
   validation alone would have accepted it. It was never seen twice in a row.

Timing was also measured (time from enumeration to the first sustained failure): 1.4 s, 2.7 s,
5.0 s, 7.8 s, 9.9 s across five attaches. Highly variable, and — given the recovery seen in
cycle 1 — not a hard deadline. Read early anyway; there is no benefit to waiting.

**Cause not confirmed.** The dongle's STM32 companion (`0483:5740`) enumerates at about +0.06 s
while the EEPROM keeps reading correctly for seconds afterwards, so enumeration is not the
trigger. Contention for a shared EEPROM/config bus is the natural explanation for corruption
that is intermittent, bursty, and byte-granular, but that has not been proven.

### Failed register reads return stale data, not errors

`ADDRL` (`0x108`) and `ADDRH` (`0x104`) were observed returning `0x000000F2` — the value of the
*last EEPROM byte read* — and `0x00000503`, rather than their power-on defaults or an error.
Meanwhile `ID_REV` kept returning the correct `0x9E000002` and `E2P_CMD` readback kept correctly
echoing the requested address. The device does not fail cleanly, so `kIOReturnSuccess` is not
evidence that the data is real.

### What a driver must therefore do

- Read the MAC **early**, in `Start()`.
- **Require several consecutive identical reads** — three is a reasonable choice, given that no
  non-zero corruption was ever observed twice in a row across ~100 reads. Two would probably
  suffice; three costs microseconds.
- **Also validate the pattern:** reject all-`0x00`, all-`0xFF`, and the multicast bit. Necessary
  but not sufficient on its own, per property 3 above.
- **Retry through bursts.** Allow at least ~500 ms of attempts before giving up, since a 370 ms
  dead burst was observed followed by full recovery.
- Fail `Start()` rather than invent an address if no stable value emerges.

### Everything else matches the reference exactly

`ID_REV 0x9E000002`, `BMCR 0x0000`, `BMSR 0x0805`, PHY ID `0x0007:0x4165`, autoneg not supported.

The Microchip EVB (`184f:0051`) has never been tested against live hardware. Its support is by
source code inspection, and the test vectors in `tests/test_proto.c` come from a capture file
but have not been validated in a real run.
