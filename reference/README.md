# Hardware reference

Ground truth captured from the real device, so that the macOS driver can be written against
measured behaviour rather than assumptions.

Everything here was produced on a Raspberry Pi 4 (Debian 13 trixie, kernel
`6.18.34+rpt-rpi-v8`) running the in-tree Linux `smsc95xx v2.0.0` driver, captured on
2026-08-30 against both supported dongles.

## Files

### MACH SYSTEMS 10BASET1S-USB-IF (`0424:9e00`)

| File | What it is |
|---|---|
| `mach-init-sequence.txt` | Decoded named register/MII trace. **Start here.** |
| `mach-bringup.pcap` | `usbmon` capture of the full bring-up — enumeration, driver bind, link up. Open in Wireshark for native USB dissection. |
| `mach-report.txt` | `dmesg`, `ethtool`, `ip link`, `lsusb` from the same session. |
| `mach-lan9500a-descriptors.txt` | `lsusb -v` for the LAN9500A Ethernet function. |
| `mach-hub-descriptors.txt` | `lsusb -v` for the SMSC USB2422 hub (`0424:2422`). |
| `mach-stm32-descriptors.txt` | `lsusb -v` for the STM32 CDC control function (`0483:5740`). |

### Microchip EVB-LAN8670-USB-D (`184f:0051`)

| File | What it is |
|---|---|
| `evb-init-sequence.txt` | Decoded named register/MII trace. |
| `evb-bringup.pcap` | `usbmon` capture of the bring-up, trimmed to the first 1500 frames. The original was 26288 frames / 2.1 MB, 95% of it interrupt-endpoint polling; the trim was verified to preserve all 204 distinct register accesses and every register write. |
| `evb-report.txt` | `dmesg`, `ethtool`, `ip link`, `lsusb` from the same session. |
| `evb-descriptors.txt` | `lsusb -v` for the device. |

The two initialization sequences are functionally identical; diffing them is a quick way to see
exactly which values are device-specific and which are not.

### M3 dext loading evidence

| File | What it is |
|---|---|
| `m3-dext-match.txt` | The M3 evidence: the dext loads, matches the LAN9500A, and reads its registers. Covers both provenance paths — EEPROM-loaded (`0424:9905`), where the real MAC is read and `Start()` succeeds, and auto-load-failed (`0424:9E00`), where the driver refuses. Also records the four requirements for a dext to load, each with the error it produces, and a list of causes already ruled out. See `dext/README.md` for how to build and load. |
| `m3-attach-state.txt` | The USB device tree at clean attachment (before driver startup) — proves the device arrives unconfigured with no interface nodes, explaining why device-level matching is necessary. |

## Reproducing a capture

Use `tools/capture-usb-bringup.sh` on a Linux host. It starts a `usbmon` capture, waits for the
device's network interface to appear, then collects link state and descriptors:

```sh
sudo modprobe usbmon
./capture-usb-bringup.sh mylabel        # then plug the device in
```

It triggers on "a new network interface appeared" rather than a specific VID:PID, so it works
for any LAN950x variant without being told which to expect. Outputs
`/tmp/mylabel-bringup.pcap` and `/tmp/mylabel-report.txt`.

Then decode:

```sh
python3 tools/decode-usbmon.py mylabel-bringup.pcap --device N
```

`--device N` is the USB device address, visible in `dmesg` as
`new high-speed USB device number N`.

## Reading the decoded trace

Each line is one vendor control transfer:

```
    150  W   HW_CFG       0x00000008   LRST
    ^    ^   ^            ^            ^
    |    |   |            |            decoded bit flags
    |    |   |            little-endian u32 payload
    |    |   register name (from offset in wIndex)
    |    W = write (0xA0), R = read (0xA1)
    pcap frame number
```

MII (PHY) transactions are reconstructed from the `MII_ADDR`/`MII_DATA` register pair and
annotated as `-> PHY<addr>.<REG> <read|write>`.

The traces are ~560–570 register accesses each. Most of the volume is `MII_ADDR` busy-polling,
the 32-address PHY scan, and phylib's steady-state `BMCR`/`BMSR` poll loop — so the interesting
part is the first ~60 lines.

## Caveats

- `ethtool` lives in `/usr/sbin` on Debian, which is not on the `PATH` for non-interactive SSH
  sessions. Use the full path.
- The bit-flag annotations come from the Linux driver's register definitions, transcribed by
  hand. The raw hex values are authoritative; if a flag name looks wrong, trust the hex.
- Each capture is of *one* physical unit. The PHY-ID discrepancy between the two boards
  (`0x4165` vs `0xC165`) is a per-unit observation, which is exactly why the driver must not
  validate PHY IDs.
- `bInterval` on the interrupt endpoint and `MaxPower` differ between the devices. Neither
  matters to the driver, but do not treat either as a constant.
