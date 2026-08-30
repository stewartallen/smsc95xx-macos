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

| Command | What it does |
|---|---|
| `id` | Reads `ID_REV` — chip id and revision |
| `phy` | Reads `BMCR`, `BMSR`, `PHYID1/2`, `ANAR`, `ANLPAR` at MII address 0, plus decoded link and autoneg state |
| `eeprom` | Reads the six-byte MAC at EEPROM offset 1, and shows the `ADDRL`/`ADDRH` values it packs into |
| `init` | Initializes the chip for 10 Mb/s half-duplex fixed mode (no autoneg); reads BMSR and refuses if the PHY supports autonegotiation, to prevent misconfiguring a 10/100 adapter; requires `--mac` if EEPROM is unreadable |
| `tx` | Sends one broadcast test frame with EtherType `0x88B5` and payload "SMSC95XX-PROBE-M2"; requires `--mac` if EEPROM is unreadable |
| `rx` | Waits for inbound frames and prints raw bytes alongside parsed interpretation; requires `--mac` if EEPROM is unreadable |
| `all` | Reads `id`, `phy`, and `eeprom` (performs control-transfer I/O only, does not initialize or transmit/receive frames) |

## Supported devices

Tried in order: MACH SYSTEMS `0424:9905` (auto-load succeeded) and `0424:9e00`
(auto-load failed) — these are the same physical device in different EEPROM states —
then Microchip EVB-LAN8670-USB-D `184f:0051`.

## Expected output

Values should match those in `reference/*-init-sequence.txt`, which were
captured from these same devices under the Linux `smsc95xx` driver. A
mismatch means either this tool is wrong or the hardware differs from what was
characterized — investigate rather than adjusting expectations.

## RX framing

The RX status-word bit layout has been validated against hardware.

**Measured on EVB over 10BASE-T1S, 2026-08-30:** broadcast ARP frame from a
Raspberry Pi, 68 bytes total (4-byte status header + 64-byte frame including CRC).

```
Raw transfer (first 16 bytes):
  20 24 40 00  FF FF FF FF FF FF  9C 95 6E B5 9B 62  08 06
  ^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^  ^^^^^
  status      dst=broadcast       src=peer MAC        ARP
```

Status word `0x00402420` (little-endian) decodes as:
- **Length**: `(0x00402420 & 0x3FFF0000) >> 16 = 0x40 = 64 bytes` ✓
- **Broadcast bit**: `0x2000` set ✓
- **Multicast bit**: `0x0400` set ✓  
- **Frame Type bit**: `0x0020` set ✓

**Verification status:**
- ✓ **Confirmed against hardware**: 4-byte header size, `LEN_MASK`/`LEN_SHIFT`, length includes CRC,
  `BROADCAST`, `MULTICAST`, `FRAME_TYPE`. `FILTER_FAIL` and `ERROR_SUM` read zero on this clean frame (consistent but not a positive confirmation).
- ⊘ **Still unverified** (transcribed from Linux, never observed): `LENGTH_ERROR`, `RUNT`, `TOO_LONG`,
  `COLLISION`, `WATCHDOG`, `MII_ERROR`, `DRIBBLING`, `CRC_ERROR`. These are error conditions that
  would require deliberate provocation.

## Layout

| File | Responsibility |
|---|---|
| `smsc95xx_regs.h` | Register offsets and bit masks. Constants only. |
| `smsc95xx_proto.c` | Pure integer encode/decode. No I/O, fully unit-tested, destined to be lifted into the dext. |
| `usb_backend.c` | IOUSBLib control transfers (register access, no interface claim) and bulk transfers (with interface claim). |
| `smsc95xx_ops.c` | Register, MII, and EEPROM operations; full initialization sequence for TX/RX. |
| `main.c` | CLI. |

## Known findings

### The MACH unit has two distinct EEPROM states, and only one is trustworthy

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

### The MAC/PHY register block needs a CONFIGURED device

Registers at offsets **`>= 0x100`** — `MAC_CR`, `ADDRL/H`, `HASHH/L`, `MII_ADDR/DATA`, `FLOW`,
`VLAN1`, `COE_CR` — return `kIOUSBPipeStalled` unless the USB device has been configured. Registers
below `0x100` work regardless.

This is easy to miss because macOS configures the device automatically in some states but not
others. In the auto-load-failed state (`0x9E00`, `bDeviceProtocol 255`) it configures it, so
everything works. In the auto-load-succeeded state (`0x9905`, `bDeviceProtocol 1`) it does **not**:
`GetConfiguration` returns 0, and every MAC/PHY register stalls until `SetConfiguration(1)` is
issued.

`usb_open_id()` therefore calls `GetConfiguration` and only issues `SetConfiguration(1)` when the
device is unconfigured. The check is conditional on purpose — re-issuing `SET_CONFIGURATION` resets
endpoint state, so it should not be done when macOS has already configured the device.

### What a driver should do

- Check `E2P_CMD.LOADED` and the `0xA5` signature. **Do not** rely on re-reads or pattern checks.
- Match both `0x9E00` and `0x9905`.
- Fail `Start()` rather than configure an interface with an unverified address.
- Keep EEPROM access light. Several hundred back-to-back byte reads destabilised this unit,
  and it did not recover within 8 s of rest; a normal driver reads a handful of bytes once.

### `GET_STATS` vendor request stalls on this hardware

The `GET_STATS` vendor request (opcode `0xA2`) stalls with `kIOUSBPipeStalled`, making the chip's
internal TX/RX byte counters unavailable as diagnostics.
The cause is uninvestigated. This is a hardware issue on the MACH unit specifically, not the
LAN9500A family generally, because the EVB has not been tested for this.

### A direct dongle-to-dongle T1S link failed, then worked after rewiring

Transmitting from one LAN9500A dongle straight into another over T1S initially failed completely:
150 frames sent, zero received, while the reverse direction over the same link worked. After the
link was rewired and re-checked, the same test passed cleanly:

```
60 frames transmitted from the MACH  ->  Pi's EVB: rx_packets 0 -> 60, rx_bytes 2760
rx_errors 0   rx_dropped 0   rx_crc_errors 0   rx_frame_errors 0
frames confirmed ours by content: src fc:61:79:90:04:56, EtherType 0x88B5,
payload "SMSC95XX-PROBE-M2"
```

Exactly 60 received for 60 sent, no errors of any kind. So the original failure was in the
physical link. **The specific defect was not isolated** — the link was rewired and re-checked in
one step, so we cannot say what was wrong with it. A reversed pair on the differential wiring was
suspected but never confirmed, and would not obviously explain the asymmetry, since a polarity
swap would normally break both directions.

**Practical guidance:** a direct two-dongle link is a working configuration. If it fails, check
the wiring before suspecting software, and instrument the *receiving* end to separate "frames
never arrived" from "frames arrived and were discarded" — `usbmon` on the receiving host shows
whether the dongle handed anything up over its bulk IN endpoint at all, which the interface
counters alone cannot tell you.

### Everything else matches the reference exactly

In the `0x9E00` state: `ID_REV 0x9E000002`, `BMCR 0x0000`, `BMSR 0x0805`, PHY ID `0x0007:0x4165`,
autoneg not supported.

### The Microchip EVB reads cleanly — validated on macOS 2026-08-30

The EVB (`184f:0051`) has now been exercised on this Mac, and it is the clean baseline the MACH
unit is not:

```
ID_REV   0x9E000002  chip 0x9E00  rev 0x0002
PHY0.BMCR    0x0000     PHY0.BMSR    0x0805
PHY0.PHYID1  0x0007     PHY0.PHYID2  0xC165
link     up             autoneg  not supported
E2P_CMD  auto-load LOADED
EEPROM   signature 0xA5 ok
MAC      9c:95:6e:b5:9b:62  (globally administered)
ADDRL    0xB56E959C   ADDRH  0x0000629B
```

The MAC and register values match `reference/evb-init-sequence.txt` exactly, the signature is
valid, and reads are stable. **So the mis-clocked EEPROM read is a fault in the MACH unit, not
behaviour of the LAN9500A family.** The signature check is still worth keeping — it costs one byte
read and it is the only thing that distinguishes a good read from a bad one — but it guards against
a faulty part rather than a design quirk.

Per-board differences observed, all benign:

| | MACH | EVB |
|---|---|---|
| PHY ID | `0x0007:0x4165` | `0x0007:0xC165` (documented LAN867x) |
| MII address decoding | answers at all 32 addresses | answers at address 0 only |
| `ANAR` / `ANLPAR` | `0x0021` / `0x0021` | `0x0000` / `0x0000` |

Both devices need `SetConfiguration` before the MAC/PHY register block is reachable: each presents
`bDeviceProtocol 1` in its normal state and macOS leaves them unconfigured.
