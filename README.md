# smsc95xx-macos

A macOS driver for the Microchip/SMSC **LAN9500A** USB-to-Ethernet controller, built with
**DriverKit** (`USBDriverKit` + `NetworkingDriverKit`).

macOS has no driver for the LAN950x family. The chip presents itself as
`bDeviceClass 255` / `bDeviceProtocol 255` — fully vendor-specific — so no built-in class
driver (CDC-ECM, NCM, or otherwise) will ever bind it. On Linux this hardware is handled by
the in-tree `smsc95xx` driver; this project is a port of that driver's programming sequence
to a userspace macOS DriverKit extension.

The immediate motivation is a **10BASE-T1S** dongle (MACH SYSTEMS 10BASET1S-USB-IF) that uses
a LAN9500A as its USB-Ethernet bridge, but nothing here is T1S-specific beyond the link
configuration.

> **Status: pre-implementation.** The hardware has been fully characterized against a working
> Linux reference (see [`reference/`](reference/)) and the driver architecture is settled. No
> driver code has been written yet.

---

## Target hardware

The MACH SYSTEMS dongle is a composite device — an SMSC hub fronting two independent
functions:

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

### LAN9500A endpoints

| Endpoint | Type | Max packet | Role |
|---|---|---|---|
| `0x81` IN | Bulk | 512 B | RX packets |
| `0x02` OUT | Bulk | 512 B | TX packets |
| `0x83` IN | Interrupt | 16 B, `bInterval` 1 | PHY / status events |

Interface 0, `bInterfaceClass` 255, `bInterfaceProtocol` 255, `MaxPower` 500 mA.

### A note on device disambiguation

The Microchip **EVB-LAN8670-USB** (EV08L38A) reference board is *also* a LAN950x device, and
the MACH dongle appears to be derived from that reference design with a hub and STM32 added.
Both may therefore present identical `0424:9e00` descriptors. If you have both attached,
distinguish them by USB topology (the MACH unit sits behind a `0424:2422` hub alongside the
STM32 function), not by VID/PID alone.

---

## What the hardware actually does

Everything below was measured, not assumed — captured from a Linux host running the in-tree
`smsc95xx` driver against this exact dongle. Raw artifacts are in [`reference/`](reference/).

| Property | Value | Consequence for this driver |
|---|---|---|
| `ID_REV` | `0x9E000002` — chip `0x9E00`, rev `0x0002` | Confirms LAN9500A |
| PHY | **External**, MII address 0, ID `0007:4165` | Internal 10/100 PHY is bypassed |
| Autonegotiation | **Not supported** (`BMSR` bit 3 clear) | No autoneg state machine needed |
| Link | **10 Mb/s half-duplex**, fixed | Advertise `10BaseT \| HalfDuplex` only |
| `BMCR` | `0x0000` | PHY control left entirely zeroed |
| `BMSR` | `0x0805` — link up, 10 Mb/s HDX capable | Link detection = poll bit 2 |
| MAC address | **From EEPROM**, offsets `0x01`–`0x06` | Read it; no MAC generation needed |
| Flow control | Off | `FLOW = 0` |

Two findings worth calling out, because they are easy to get wrong:

**The PHY answers at every MII address.** A scan of addresses 0–31 returns the same ID
(`0x0007`/`0x4165`) at every one. This is a PHY that does not decode its address, not a
floating bus (which would read `0xFFFF`). Linux's phylib simply binds the first hit, address 0.

**Half-duplex requires two non-obvious register settings.** On the half-duplex path the Linux
driver sets `MAC_CR.RCVOWN` (receive own transmissions) while leaving `FDPX` clear, and ORs the
low nibble of `AFC_CFG` to `0xF` (`0x00F830A1` → `0x00F830AF`). Neither falls out of the
datasheet; both come from the captured trace.

### Verified initialization sequence

Distinct register writes in first-occurrence order, as performed by Linux `smsc95xx v2.0.0`:

```
read EEPROM[0x01..0x06]        -> MAC 4a:f8:f8:c2:c2:f2
HW_CFG       0x00000008         LRST (lite reset), poll until clear
ADDRL        0xC2F8F84A         MAC low
ADDRH        0x0000F2C2         MAC high
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

Three units with narrow interfaces, so that the hard parts can be tested independently.

**1. `tools/smsc95xx-probe` — userspace protocol validation**

An ordinary userspace program driving the chip over `IOUSBHost`. Needs no entitlements, no
SIP changes, and no dext loading. Its job is to prove the register sequence and framing
against real hardware with a fast edit-run loop. The two hard problems in this project — *is
my protocol code right* and *can I get a dext signed and loaded* — are independent, and this
lets us solve the first one without touching the second.

**2. `SMSC95xxDevice` — register access layer**

Owns the `IOUSBHostInterface` and its three pipes. Exposes `readRegister`/`writeRegister`,
`miiRead`/`miiWrite` via the `MII_ADDR`/`MII_DATA` pair, `eepromRead` via `E2P_CMD`/`E2P_DATA`,
and `reset()`. Knows nothing about the network stack.

**3. `SMSC95xxEthernet` — the `IOUserNetworkEthernet` subclass**

Owns the packet buffer pool, the four queues, frame framing, and link state. Knows nothing
about USB control transfers.

The seam between 2 and 3 is what makes framing logic testable off-device: TX command-word
construction and RX status-word parsing become pure functions over byte buffers, unit-testable
on the host with no DriverKit involved.

### Data flow

- **TX** — stack → `IOUserNetworkTxSubmissionQueue` → `TxDispatchQueue` → prepend `TX_CMD_A`
  (length + first/last segment flags) and `TX_CMD_B` → async bulk OUT on `0x02` →
  `IOUserNetworkTxCompletionQueue`.
- **RX** — pre-posted bulk IN reads on `0x81` sized to `BURST_CAP` → walk the buffer parsing
  32-bit RX status words (frame length in bits 30:16, error bits below), each frame 4-byte
  aligned → wrap into `IOUserNetworkPacket` → `IOUserNetworkRxCompletionQueue` → stack.
- **Link** — poll `BMSR` bit 2 (matching phylib's approach) and/or take PHY events from the
  interrupt endpoint, then `reportLinkStatus(kIOUserNetworkLinkStatusActive, 10BaseT|HDX)`.

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

An earlier version of this rig looped the T1S link back to a second USB adapter on the same
Mac. That does not work as a test: macOS resolves a destination that is a local address
through **`lo0`**, so the packet never reaches the wire. The driver would appear to work while
transmitting nothing — a false positive. `ping -b <if>` binds the source interface but does not
defeat the destination shortcut.

Use a genuinely separate host (the Pi), or raw L2 frames via BPF, which bypass the IP stack
entirely and also give byte-exact control for validating TX/RX framing.

---

## Milestones

| | |
|---|---|
| M0 | Repo, README, characterized hardware reference — **done** |
| M1 | Probe tool reads `ID_REV`, dumps PHY registers, reads EEPROM MAC |
| M2 | Probe does full init, transmits one frame, receives one frame |
| M3 | Dext loads and matches the device in `ioreg` |
| M4 | Interface appears in `ifconfig` with the EEPROM MAC |
| M5 | Frames cross the T1S link to the Pi |
| M6 | `tcpdump` works via the BPF tap; throughput measured |

Throughput expectations should stay modest: the segment is 10 Mb/s half-duplex, and the media
converter rate-adapts from 100BASE-TX, so loss under load is expected.

### v1 scope

Link up plus RX/TX only. Deliberately excluded: checksum offload (`COE_CR`), Wake-on-LAN
(`WUFF`/`WUCSR`), VLAN offload, suspend/resume, and EEPROM writing. The captured trace shows
Linux enabling checksum offload, but on a 10 Mb/s link it buys nothing and complicates RX
parsing, so v1 leaves it off.

---

## Repository layout

```
reference/    Captured hardware ground truth — usbmon pcap, decoded register
              trace, USB descriptors, Linux driver report. See reference/README.md.
tools/        Analysis and validation tooling (decode-usbmon.py; probe tool to come).
```

---

## License and provenance

**GPL-2.0.** This is a port of the Linux `smsc95xx` driver: its reset sequencing, register
programming order, and half-duplex handling are derived from that GPL-2.0 source, so this work
is derivative and carries the same license.

Register addresses and bit definitions are facts from Microchip's LAN9500A datasheet, but the
*sequences* come from Linux, and that is the part worth having.
