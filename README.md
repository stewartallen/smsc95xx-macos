# smsc95xx-macos

A macOS driver for the Microchip/SMSC **LAN9500A** USB-to-Ethernet controller, built with
**DriverKit** (`USBDriverKit` + `NetworkingDriverKit`).

macOS has no driver for the LAN950x family. The chip presents itself as
`bDeviceClass 255` / `bDeviceProtocol 255` — fully vendor-specific — so no built-in class
driver (CDC-ECM, NCM, or otherwise) will ever bind it. On Linux this hardware is handled by
the in-tree `smsc95xx` driver; this project is a port of that driver's programming sequence
to a userspace macOS DriverKit extension.

The immediate motivation is **10BASE-T1S** dongles that use a LAN9500A as their USB-Ethernet
bridge, but nothing here is T1S-specific beyond the link configuration.

## Supported devices

| Device | VID:PID | Notes |
|---|---|---|
| **MACH SYSTEMS 10BASET1S-USB-IF** | `0424:9e00` **and** `0424:9905` | Behind an SMSC hub, with an STM32 CDC control function alongside. Two IDs because the product ID is loaded from EEPROM — same physical device. |
| **Microchip EVB-LAN8670-USB-D** | `184f:0051` | Single function; Microchip's own 10BASE-T1S reference board. Validated on macOS — reads cleanly. Note `lsusb` reports vendor `184f` as **K2L GmbH**, a Microchip subsidiary. |

Both have been captured and verified against the Linux driver. From the driver's point of view
they are the **same device** — identical endpoints, identical initialization sequence, identical
link configuration — differing only in USB ID and MAC address. One implementation covers both;
only the IOKit matching dictionary needs two entries.

> **Status: M5 complete — the driver moves frames.** The DriverKit extension builds, loads with
> SIP disabled, matches both dongles, reads the MAC from EEPROM with provenance checks, initialises
> the chip, and carries Ethernet frames over the bulk pipes in both directions — `ping` works
> end to end across the T1S segment (see [`reference/m5-datapath.txt`](reference/m5-datapath.txt)).
> One USB transfer is outstanding per direction and every frame is copied once, so M5 is about
> correctness, not throughput; a descriptor ring and BPF-tap throughput work are M6. See the
> [milestones](#milestones) below for the full state.

---

## Target hardware

### MACH SYSTEMS 10BASET1S-USB-IF

A composite device — an SMSC hub fronting two independent functions:

```
USB host
└── 0424:2422   SMSC USB2422 hub                    (bcdDevice 0.a0)
    ├── 0424:9e00   LAN9500A/LAN9500Ai              (bcdDevice 3.00)  <-- this driver
    │               vendor-specific, 3 endpoints
    │               └── MII ──> external 10BASE-T1S PHY (ID 0007:4165)
    └── 0483:5740   STM32 CDC ACM                   "10BASET1S-USB-IF"
                    firmware update + T1S/PLCA control (macOS binds this already)
```

This driver targets **only** the `0424:9e00` Ethernet function. T1S and PLCA configuration is
the STM32's job over its CDC serial port, which macOS already supports via its built-in ACM
driver — so this driver never touches T1S settings.

### Microchip EVB-LAN8670-USB-D

Structurally simpler — one device, no hub, no separate control channel:

```
USB host
└── 184f:0051   MCHP "10BASE-T1S"                   (bcdDevice 2.00, serial 0005590)
                vendor-specific, 3 endpoints
                └── MII ──> LAN8670 10BASE-T1S PHY (ID 0007:C165)
```

Since there is no STM32 here, whatever configures its T1S side is either hardware-strapped or
reached through the PHY. It links up at 10 Mb/s half-duplex with no host intervention, so this
does not affect basic operation.

### Endpoints (both devices)

| Endpoint | Type | Max packet | Role |
|---|---|---|---|
| `0x81` IN | Bulk | 512 B | RX packets |
| `0x02` OUT | Bulk | 512 B | TX packets |
| `0x83` IN | Interrupt | 16 B | PHY / status events |

Interface 0, `bInterfaceClass` 255, `bInterfaceProtocol` 255. The only descriptor differences
between the two devices are the interrupt endpoint's `bInterval` (1 on MACH, 4 on EVB) and
`MaxPower` (500 mA vs 250 mA) — neither matters to the driver.

---

## What the hardware actually does

Everything below was measured, not assumed — captured from a Linux host running the in-tree
`smsc95xx` driver against both dongles. Raw artifacts are in [`reference/`](reference/).

| Property | MACH | EVB | Consequence for this driver |
|---|---|---|---|
| `ID_REV` | `0x9E000002` | `0x9E000002` | Confirms LAN9500A on both |
| PHY | **External**, MII addr 0 | **External**, MII addr 0 | Internal 10/100 PHY is bypassed |
| PHY ID | `0007:4165` | `0007:C165` | See below — do **not** validate against a whitelist |
| Autonegotiation | **Not supported** | **Not supported** | No autoneg state machine needed |
| Link | **10 Mb/s half-duplex** | **10 Mb/s half-duplex** | Advertise `10BaseT \| HalfDuplex` only |
| `BMCR` | `0x0000` | `0x0000` | PHY control left entirely zeroed |
| `BMSR` | `0x0805` | `0x0805` | **NOT usable for link detection** — see below |
| MAC source | EEPROM `0x01`–`0x06` | EEPROM `0x01`–`0x06` | Verify the `0xA5` signature first — see below |
| MAC | `fc:61:79:90:04:56` (OUI) | `9c:95:6e:b5:9b:62` (OUI) | the capture's `4a:f8:…` is a corrupted read |
| Flow control | Off | Off | `FLOW = 0` |

Three findings worth calling out, because they are easy to get wrong:

**Always use MII address 0, and never validate the PHY ID.** The two boards behave differently
here. The EVB's PHY decodes its address properly — it answers at 0 and returns `0xFFFF`
everywhere else — and reports `0x0007C165`, the documented Microchip LAN867x ID. The MACH unit's
PHY answers at *every* address 0–31 and reports `0x0007`/`0x4165`, with bit 15 of `PHYID2`
clear. Address 0 is the only choice that works on both, and an ID whitelist would reject the
MACH's working hardware.

**Half-duplex requires two non-obvious register settings.** On the half-duplex path the Linux
driver sets `MAC_CR.RCVOWN` (receive own transmissions) while leaving `FDPX` clear, and ORs the
low nibble of `AFC_CFG` to `0xF` (`0x00F830A1` → `0x00F830AF`). Neither falls out of the
datasheet; both come from the captured trace.

**`PM_CTRL` is read-modify-write.** The captured writes differ between devices (`0x1D0` vs
`0x1D1`) purely because the pre-existing register contents differed (`0x1C0` vs `0x1C1`). Both
are just setting `PHY_RST` (bit 4). Do not hardcode the literal value.

**The MAC in the captured trace is wrong, and the EEPROM signature is the only way to tell.**
The MACH unit's chip performs a power-on EEPROM auto-load that sometimes fails. When it fails the
device presents `0x9E00` with no strings, EEPROM offset 0 reads `0x4A`, and the MAC reads
`4a:f8:f8:c2:c2:f2`. When it succeeds the device presents **`0x9905`** with strings, offset 0 reads
the valid **`0xA5`** signature, and the MAC reads **`fc:61:79:90:04:56`** — identically on 10/10
reads.

The failed-state read is a systematic mis-clock, exactly `bad[k] == (real[k>>1] << 1) & 0xFF`:
`A5→4A`, `FC→F8`, `61→C2`, `79→F2`, each byte appearing at two consecutive addresses. One extra
clock and a halved address.

This defeats every obvious defence: the bad read is **stable**, so re-reading and requiring
agreement passes it, and `4a:f8:f8:c2:c2:f2` is neither zeros nor all-ones nor multicast, so
pattern validation passes it too. Only provenance works — `E2P_CMD` bit 9 (`LOADED`) and the `0xA5`
signature at offset 0. Stock Linux checks neither, so `reference/mach-init-sequence.txt` shows it
configuring `eth1` with the mis-clocked address.

**Also: the USB product ID comes from the EEPROM.** `0x9E00` and `0x9905` are the same physical
device in different auto-load states, so matching on `idProduct` matches EEPROM contents rather
than silicon and must accept both. Full analysis in
[`tools/smsc95xx-probe/README.md`](tools/smsc95xx-probe/README.md).

### Verified initialization sequence

Distinct register writes in first-occurrence order, as performed by Linux `smsc95xx v2.0.0`.
**This sequence is identical on both devices** — diffing the two captured traces yields only
the MAC bytes, the multicast hash values (which depend on which groups the host joined), and
the `PM_CTRL` read-modify-write noted above.

```
read EEPROM[0x01..0x06]        -> MAC (device-specific)
HW_CFG       0x00000008         LRST (lite reset), poll until clear
ADDRL        <MAC low>
ADDRH        <MAC high>
HW_CFG       0x00001004         BIR | PSEL
BURST_CAP    0x00000005
BULK_IN_DLY  0x00002000
HW_CFG       0x00001026         BIR | MEF | PSEL | BCE
INT_STS      0xFFFFFFFF         clear all
LED_GPIO_CFG 0x81110007
FLOW         0x00000000
AFC_CFG      0x00F830A1
VLAN1        0x00008100         ETH_P_8021Q
COE_CR       0x00010001         TX_COE_EN | RX_COE_EN
HASHH/HASHL  0x00000000
MAC_CR       0x00000000
INT_EP_CTL   0x00008000         PHY_INT_EN
MAC_CR       0x00000008         TXEN
TX_CFG       0x00000004         TX_ON
MAC_CR       0x0000000C         TXEN | RXEN
PM_CTRL      0x000001D0         PHY_RST
--- link comes up, half-duplex path ---
MAC_CR       0x0080200C         RCVOWN | HPFILT | TXEN | RXEN   (no FDPX)
AFC_CFG      0x00F830AF         low nibble -> 0xF
```

Register access uses vendor control transfers: `0xA0` = write register, `0xA1` = read
register, with the register offset in `wIndex` and a little-endian `u32` payload.

---

## Architecture

Two DriverKit classes plus a shared protocol layer and a userspace probe, with narrow interfaces
so the hard parts can be tested independently.

**1. `common/` — the pure protocol layer**

Register offsets and bit layouts, the parameterised power-on initialisation sequence, TX/RX
framing, and MAC-plausibility checks. No I/O, no allocation, no platform headers, and C++-safe,
so the probe and the dext share exactly one copy. TX command-word construction, RX status-word
parsing, and the init sequence become pure functions over byte buffers, unit-tested on the host
with no DriverKit involved (`tools/smsc95xx-probe/tests`).

**2. `tools/smsc95xx-probe` — userspace protocol validation**

An ordinary userspace program driving the chip over `IOUSBHost`. Needs no entitlements, no SIP
changes, and no dext loading. It proves the register sequence and framing against real hardware
with a fast edit-run loop — the two hard problems in this project (*is my protocol code right*
and *can I get a dext signed and loaded*) are independent, and this solves the first without
touching the second.

**3. `SMSC95xxUSBDevice` — the device-level class**

Matches `IOUSBHostDevice` and does one thing: selects configuration 1 with interface matching so
the `IOUSBHostInterface` node the driver binds to actually appears. It touches no registers and
no network state. See "Device matching" below for why this level is required.

**4. `SMSC95xxDriver` — the `IOUserNetworkEthernet` subclass**

Matches the resulting `IOUSBHostInterface` and owns everything else: the three pipes, register /
MII / EEPROM access over vendor control transfers, chip initialisation, the packet buffer pool
and four queues, frame framing, and link state. It is built from two translation units sharing
one ivar layout — `SMSC95xxDriver.cpp` (control path: registers, MII, EEPROM, init, network-stack
plumbing) and `SMSC95xxDriver_datapath.cpp` (the bulk endpoints and the TX/RX loops).

### Data flow

- **TX** — stack → `IOUserNetworkTxSubmissionQueue` → a doorbell (`TxDataAvailable`) → prepend the
  8-byte header (`TX_CMD_A` length + first/last-segment flags, `TX_CMD_B` length) → async bulk OUT
  on `0x02` → `TxComplete` returns the packet on `IOUserNetworkTxCompletionQueue`.
- **RX** — a bulk IN read on `0x81` into a 4 KB buffer → `RxComplete` walks the buffer parsing
  32-bit RX status words (frame length in bits 29:16, error bits below), each record 4-byte
  aligned → copy into an `IOUserNetworkPacket` from the RX submission queue → deliver on
  `IOUserNetworkRxCompletionQueue`. An idle-RX backoff timer keeps a device with nothing to say
  from spinning a CPU core.
- **Link** — `BMSR` bit 2 is **not usable for link detection** on this hardware: with the T1S
  cable physically unplugged it still reads set, because T1S is a multidrop bus with no continuous
  idle signalling while the medium is quiet. So v1 reports `reportLinkStatus(kIOUserNetworkLinkStatusActive,
  10BaseT|HDX)` for as long as the dongle is attached rather than polling a signal that would lie.
  Real link and PLCA state live in clause-45 MMD registers (reachable via clause-22 indirect
  registers 13/14) or on the interrupt endpoint — both are future work, not wired up in v1.

### NetworkingDriverKit surface

`IOUserNetworkEthernet` is the base class; packet plumbing is `IOUserNetworkPacketBufferPool`,
`IOUserNetworkPacket`, and `IOUserNetworkRx/TxSubmissionQueue` plus
`IOUserNetworkRx/TxCompletionQueue`. Apple's `connecting-a-network-driver` sample is the
closest reference.

**Gotcha:** in the DriverKit SDK the older PascalCase methods — `SetInterfaceEnable`,
`SetPromiscuousModeEnable`, `SetMulticastAddresses`, `SetAllMulticastModeEnable`,
`SelectMediaType`, `SetWakeOnMagicPacketEnable`, `SetMTU`, `GetMaxTransferUnit`,
`SetHardwareAssists`, `GetHardwareAssists` — are marked `__deprecated` **and** are still pure
virtual (`= 0`). They must all be stubbed even though the modern camelCase equivalents are the
ones you actually implement, or the class will not instantiate.

There is no T1S media type in `IOUserNetworkTypes.h`; `kIOUserNetworkMediaEthernet10BaseT`
with `kIOUserNetworkMediaOptionHalfDuplex` is the honest approximation.

---

## Requirements

- **Apple Silicon.** The dext is built `arm64e` (DriverKit on Apple Silicon requires pointer
  authentication); this has not been built or run on an Intel Mac.
- **Xcode** with the DriverKit SDK (developed against Xcode 26.6 / DriverKit 25.5)
- **macOS 11+** for `NetworkingDriverKit` (introduced DriverKit 19.0); developed on macOS 26.6
- Entitlements: `com.apple.developer.driverkit`,
  `com.apple.developer.driverkit.transport.usb`,
  `com.apple.developer.driverkit.family.networking`

Those last two are not self-serve — Apple grants them by request. This project is therefore
built for **local development with reduced security**, not for distribution:

```sh
# one-time, from Recovery on Apple Silicon: set Reduced Security
csrutil disable
systemextensionsctl developer on
```

Getting these entitlements approved for public distribution is out of scope.

---

## Test rig

Both ends of the link terminate on equipment we control, so the wire can be exercised
end-to-end:

```
Mac ── USB ── MACH dongle (LAN9500A) ── T1S ── media converter ── Ethernet ── Raspberry Pi
```

The Pi doubles as the **reference implementation host**: with the dongle plugged into it,
Linux's `smsc95xx` drives the hardware correctly, and `usbmon` captures exactly how. That is
where everything in [`reference/`](reference/) came from, and it is the fastest way to resolve
any future "what should the chip do here?" question.

### Do not test by pinging between two interfaces on one Mac

Testing on the same Mac by looping the T1S link back to a second USB adapter does not work:
macOS resolves a destination that is a local address through **`lo0`**, so the packet never
reaches the wire. The driver would appear to work while transmitting nothing — a false positive.
`ping -b <if>` binds the source interface but does not defeat the destination shortcut.

Use a genuinely separate host (the Pi), or raw L2 frames via BPF, which bypass the IP stack
entirely and also give byte-exact control for validating TX/RX framing.

---

## Milestones

| | |
|---|---|
| M0 | Repo, README, characterized hardware reference — **done** |
| M1 | Probe reads `ID_REV`, PHY registers, EEPROM MAC — **done** |
| M2 | Probe does full init, transmits one frame, receives one frame — **done** |
| M3 | Dext loads and matches the device in `ioreg` — **done** |
| M4 | Interface appears in `ifconfig` with the EEPROM MAC — **done** |
| M5 | Frames cross the T1S link to the Pi — also splits the driver in two — **done** |
| M6 | `tcpdump` works via the BPF tap; throughput measured |

Throughput expectations should stay modest: the segment is 10 Mb/s half-duplex, and the media
converter rate-adapts from 100BASE-TX, so loss under load is expected.

**M5 split the driver in two.** `SMSC95xxUSBDevice` matches `IOUSBHostDevice` and does nothing but
select configuration 1; `SMSC95xxDriver` matches the resulting `IOUSBHostInterface` and owns the pipes
and the network interface — the structure Apple's own USB Ethernet dext uses. The bulk endpoints live on
the interface rather than the device, so this is the natural shape for the datapath, and it also fixed
the interface's display name: SystemConfiguration names a USB network interface from `kUSBProductString`
found while walking the provider chain, and needs an `IOUSBHostInterface` in that chain to find it.
Attached directly to the device the interface showed as `Ethernet Adapter (enN)`; it now reads the
dongle's own product name. See `reference/m4-interface.txt` for what was ruled out, and
`reference/m5-datapath.txt` for the measured result — pings in both directions, the Pi's capture, and
the two framing bugs that were diagnosed by disassembling NetworkingDriverKit rather than by testing.

### v1 scope

Link up plus RX/TX only. Deliberately excluded: checksum offload (`COE_CR`), Wake-on-LAN
(`WUFF`/`WUCSR`), VLAN offload, suspend/resume, and EEPROM writing. The captured trace shows
Linux enabling checksum offload, but on a 10 Mb/s link it buys nothing and complicates RX
parsing, so v1 leaves it off.

---

## Repository layout

```
common/       The pure protocol layer: register offsets, bit layouts, TX/RX framing,
              MAC plausibility. No I/O, no allocation, no platform headers, and
              C++-safe, so the probe and the driver share exactly one copy.
dext/         The DriverKit extension and the host app that installs it.
              See dext/README.md for building, loading and debugging.
reference/    Captured hardware ground truth for both devices — usbmon pcaps,
              decoded register traces, USB descriptors, Linux driver reports,
              and the M3 load/match evidence. See reference/README.md.
tools/        decode-usbmon.py        turn a usbmon pcap into a named register trace
              capture-usb-bringup.sh  capture a device's bring-up on a Linux host
              smsc95xx-probe/         userspace protocol validation against real hardware
              usb-reenumerate/        force a USB re-enumeration so IOKit re-runs matching
              watch-attach/           watch the dext attach/match live during a plug-in
              inspect-profile/        show what a provisioning profile grants
              probe-entitlements/     measure which entitlement shape a profile authorises
              dsc-disasm/             disassemble a framework from the dyld shared cache
```

### Device matching

Five personalities across two provider classes, since M5 split the driver in two.

Three match `IOUSBHostDevice` and name `SMSC95xxUSBDevice` (`IOClass` `IOUserService`,
`CFBundleIdentifierKernel` `com.apple.kpi.iokit`):

| | `idVendor` | `idProduct` | Notes |
|---|---|---|---|
| MACH (normal) | `0x0424` | `0x9905` | EEPROM auto-load succeeded; device strings present |
| MACH (EEPROM failed) | `0x0424` | `0x9E00` | EEPROM auto-load failed; generic LAN9500A default |
| EVB | `0x184F` | `0x0051` | Microchip reference board |

Two match `IOUSBHostInterface` and name `SMSC95xxDriver` (`IOClass` `IOUserNetworkEthernet`,
`CFBundleIdentifierKernel` `com.apple.iokit.IOSkywalkFamily`) — one per vendor, because a single
personality dictionary cannot express two values of `idVendor`:

| | `idVendor` | `idProductArray` | `bInterfaceClass` | `bInterfaceNumber` | `bConfigurationValue` |
|---|---|---|---|---|---|
| MACH interface | `0x0424` | `0x9905`, `0x9E00` | 255 | 0 | 1 |
| EVB interface | `0x184F` | `0x0051` | 255 | 0 | 1 |

**Why the device level is still needed:** macOS leaves this device unconfigured at attach time,
and an unconfigured device has no interface nodes in the IOKit tree. An interface personality
alone would build, sign, install, and then silently never match. `SMSC95xxUSBDevice` selects
configuration 1 with interface matching enabled, which is what creates the nodes the interface
personality then matches. See `reference/m3-attach-state.txt` for the measured unconfigured state.

**The interface match keys fail silently if wrong.** A personality specifying `idVendor` MUST
also specify `idProduct` or `idProductArray`, plus `bConfigurationValue` and `bInterfaceNumber`.
`idVendor` alone is rejected before matching and logs nothing at any level, including
`--debug --info`. All 121 of Apple's own `IOUSBHostInterface` personalities were surveyed and
none uses `idVendor` without a product key — see `reference/m5-interface-matching.txt`.

**`0x0424:0x9905` vs `0x9E00`:** On this hardware unit the EEPROM auto-load sometimes fails
(cause not established). When it succeeds (normal case), the device reports `0x9905` with
strings; when it fails, it reports the generic LAN9500A default `0x9E00` without strings. Both
are the same physical chip, so the driver must match both IDs. The normal state is `0x9905`.
See `README.md` "The MAC in the captured trace is wrong, and the EEPROM signature is the only
way to tell" for full analysis.

Note that `0x0424:0x9E00` is the generic LAN9500A/LAN9500Ai ID, so that personality will match
*any* LAN9500A-based adapter, not only the MACH dongle. On a conventional 10/100 adapter the
internal PHY is in use and the link is autonegotiated, which this driver does not implement —
so such a device would bind. That is mitigated rather than open: `Start()` reads `BMSR` and refuses
any PHY advertising autonegotiation capability (bit 3), which is exactly what an internal-PHY
10/100BASE-TX adapter does and what a 10BASE-T1S PHY does not. Both supported dongles read
`BMSR 0x0805` and pass; an unsupported adapter is left unclaimed rather than half-working.

Narrowing the personality itself would still be preferable, but it cannot be done on vendor/product ID
alone — `0424:9E00` is the chip default and genuinely shared.

---

## License and provenance

**GPL-2.0.** This is a port of the Linux `smsc95xx` driver
(`drivers/net/usb/smsc95xx.c`, `SPDX-License-Identifier: GPL-2.0-or-later`,
Copyright (C) 2007-2008 SMSC — now Microchip Technology Inc.). Its reset sequencing, register
programming order, and half-duplex handling are derived from that source, so this work is
derivative and licensed GPL-2.0 (a permitted narrowing of the upstream "or later"). The full
attribution is in [`NOTICE`](NOTICE), and the license text is in [`LICENSE`](LICENSE); every
source file carries an `SPDX-License-Identifier: GPL-2.0` tag.

Register addresses and bit definitions are facts from Microchip's LAN9500A datasheet, but the
*sequences* come from Linux, and that is the part worth having.

Linking a GPL work against Apple's DriverKit system frameworks is covered by the GPL's system-library
exception (those frameworks are a normal part of the macOS platform, not distributed with this code).
