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

### The MACH unit has two distinct EEPROM states, and only one is trustworthy

**Diagnosed 2026-08-30.** This supersedes three earlier accounts in this file — "EEPROM
unreadable", then "readable only in a closing window", then "intermittently corrupt". All three
were wrong. Here is what is actually happening.

The chip performs a power-on EEPROM auto-load. On this unit it sometimes succeeds and sometimes
fails, and the device presents differently in each case:

| | auto-load **succeeded** | auto-load **failed** |
|---|---|---|
| `E2P_CMD` bit 9 (`LOADED`) | set | clear |
| USB product ID | **`0x9905`** (from EEPROM) | **`0x9E00`** (chip default) |
| `bcdDevice` | `0x0200` | `0x0300` |
| String descriptors | present (`10BASE-T1S-USB-IF`) | absent |
| `bDeviceProtocol` | `1` | `255` |
| EEPROM offset 0 | `0xA5` — valid signature | `0x4A` |
| MAC at offsets 1–6 | `fc:61:79:90:04:56` | `4a:f8:f8:c2:c2:f2` |
| Read stability | 10/10 byte-identical | unstable under load |

**The real MAC is `fc:61:79:90:04:56`.** It sits behind a valid `0xA5` signature, is
globally-administered, and reads identically every time.

### The failed-state read is a systematic mis-clock, not noise

The two readings are related exactly:

```
bad[k] == (real[k >> 1] << 1) & 0xFF        for every k
```

| offset | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| real | A5 | FC | 61 | 79 | 90 | 04 | 56 |
| bad  | 4A | 4A | F8 | F8 | C2 | C2 | F2 |

`A5<<1 = 4A`, `FC<<1 = F8`, `61<<1 = C2`, `79<<1 = F2`, each appearing at two consecutive
addresses. So in the failed state the EEPROM interface reads with **one extra clock (data shifted
left a bit) and a halved address**. That is a hardware timing fault on the EEPROM interface of
this unit, not random corruption.

### Why re-reading and pattern validation cannot save you

This is the important part, and it is counter-intuitive.

The bad read is **stable and repeatable**. It also passes every pattern check: `4a:f8:f8:c2:c2:f2`
is not all-zeros, not all-ones, not multicast. So:

- Reading twice and comparing — **fails**, both reads agree.
- Reading three times and requiring agreement — **fails**, all three agree.
- Rejecting all-`0x00` / all-`0xFF` / multicast — **fails**, it passes all of them.

Only checking provenance works:

1. **`E2P_CMD` bit 9 (`LOADED`)** — tells you the auto-load succeeded before you read anything.
2. **EEPROM offset 0 == `0xA5`** — the signature. In the bad state it reads `0x4A`, so this
   catches the mis-clock directly.

`smsc95xx_read_mac_verified()` implements the second check and refuses to return a MAC without
it. `smsc95xx_eeprom_loaded()` exposes the first.

### Stock Linux gets this wrong

`reference/mach-init-sequence.txt` shows Linux reading `4A F8 F8 C2 C2 F2` and configuring `eth1`
with it. `smsc95xx` validates only with `is_valid_ether_addr()` and never checks the signature, so
on this dongle it silently uses the mis-clocked address whenever auto-load has failed. The
captured trace also shows `E2P_CMD` readbacks of `0x00000001`–`0x00000006` with `LOADED` **clear**,
confirming the capture was taken in the failed state. By contrast the EVB capture shows
`0x0000020N` with `LOADED` **set**.

So the `4a:f8:f8:c2:c2:f2` value that appears throughout `reference/` and in this project's unit
tests is a corrupted read. The unit tests remain valid — they verify that `mac_to_regs()` packs
whatever bytes it is given the same way the hardware did, which is a property of the packing
function and independent of which address is genuine.

### The USB product ID comes from the EEPROM

`0x9E00` and `0x9905` are the *same physical device* in different auto-load states. Any matching
on `idProduct` is therefore matching EEPROM contents rather than silicon, and must accept both.
`usb_open_first()` tries `0x9905`, then `0x9E00`, then the EVB.

One partial auto-load was also observed, presenting `bNumConfigurations = 112` — impossible — which
left macOS unable to configure the device at all until it was power-cycled.

### Open: MII reads stall in the loaded state

With the device at `0x9905` (auto-load succeeded, `bDeviceProtocol = 1`), MII register reads fail
with `kIOUSBPipeStalled`. In the `0x9E00` state (`bDeviceProtocol = 255`) they work and return the
expected `BMCR 0x0000` / `BMSR 0x0805` / PHY ID `0x0007:0x4165`. Not yet investigated.

### What a driver should do

- Check `E2P_CMD.LOADED` and the `0xA5` signature. **Do not** rely on re-reads or pattern checks.
- Match both `0x9E00` and `0x9905`.
- Fail `Start()` rather than configure an interface with an unverified address.
- Keep EEPROM access light. Several hundred back-to-back byte reads destabilised this unit,
  and it did not recover within 8 s of rest; a normal driver reads a handful of bytes once.

### Everything else matches the reference exactly

In the `0x9E00` state: `ID_REV 0x9E000002`, `BMCR 0x0000`, `BMSR 0x0805`, PHY ID `0x0007:0x4165`,
autoneg not supported.

The Microchip EVB (`184f:0051`) has never been tested against live hardware. Its support is by
source code inspection, and the test vectors in `tests/test_proto.c` come from a capture file
but have not been validated in a real run.
