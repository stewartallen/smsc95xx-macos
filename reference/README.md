# Hardware reference

Ground truth captured from the real device, so that the macOS driver can be written against
measured behaviour rather than assumptions.

Everything here was produced on a Raspberry Pi 4 (Debian 13 trixie, kernel
`6.18.34+rpt-rpi-v8`) running the in-tree Linux `smsc95xx v2.0.0` driver against the MACH
SYSTEMS 10BASET1S-USB-IF dongle, captured on 2026-08-30.

## Files

| File | What it is |
|---|---|
| `mach-bringup.pcap` | `usbmon` capture of the full USB bring-up — enumeration, driver bind, link up. Open in Wireshark for native USB dissection. |
| `mach-init-sequence.txt` | The above, decoded into a named register/MII trace by `tools/decode-usbmon.py`. **Start here.** |
| `mach-report.txt` | Collected `dmesg`, `ethtool`, `ip link`, and `lsusb` output from the same session. |
| `mach-lan9500a-descriptors.txt` | Full `lsusb -v` for the LAN9500A Ethernet function (`0424:9e00`). |
| `mach-hub-descriptors.txt` | Full `lsusb -v` for the SMSC USB2422 hub (`0424:2422`). |
| `mach-stm32-descriptors.txt` | Full `lsusb -v` for the STM32 CDC control function (`0483:5740`). |

## Reproducing a capture

On a Linux host with the dongle attached:

```sh
sudo modprobe usbmon
sudo tcpdump -i usbmon0 -s 0 -w bringup.pcap     # then plug the device in
```

`usbmon0` captures all buses, which avoids having to know in advance which bus the device
lands on. Then decode:

```sh
python3 tools/decode-usbmon.py bringup.pcap --device N
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

The trace is 559 register accesses: 371 reads, 188 writes. Most of the volume is the
`MII_ADDR` busy-polling and phylib's steady-state `BMCR`/`BMSR` poll loop, so the interesting
part is the first ~60 lines.

## Caveats

- `ethtool` lives in `/usr/sbin` on Debian, which is not on the `PATH` for non-interactive SSH
  sessions. Use the full path.
- The bit-flag annotations come from the Linux driver's register definitions, transcribed by
  hand. The raw hex values are authoritative; if a flag name looks wrong, trust the hex.
- This capture is of *one* dongle. The Microchip EVB-LAN8670-USB may present identical
  descriptors — see the disambiguation note in the top-level README.
