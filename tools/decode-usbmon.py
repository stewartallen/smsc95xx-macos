#!/usr/bin/env python3
"""Decode a Linux usbmon pcap into an annotated smsc95xx register/MII trace.

Reads a usbmon capture of the smsc95xx driver bringing up a LAN9500A and prints
each vendor control transfer as a named register access, reconstructing MII
(PHY) transactions from the MII_ADDR/MII_DATA register pair.

Usage:
    ./decode-usbmon.py CAPTURE.pcap [--device N]

Requires tshark on PATH.
"""

import argparse
import shutil
import subprocess
import sys

# Register map, from the Linux smsc95xx driver's register definitions.
REGS = {
    0x000: "ID_REV",
    0x004: "INT_STS_ALT",
    0x008: "INT_STS",
    0x00C: "RX_CFG",
    0x010: "TX_CFG",
    0x014: "HW_CFG",
    0x018: "RX_FIFO_INF",
    0x01C: "TX_FIFO_INF",
    0x020: "PM_CTRL",
    0x024: "LED_GPIO_CFG",
    0x028: "GPIO_CFG",
    0x02C: "AFC_CFG",
    0x030: "E2P_CMD",
    0x034: "E2P_DATA",
    0x038: "BURST_CAP",
    0x044: "DP_SEL",
    0x048: "DP_CMD",
    0x04C: "DP_ADDR",
    0x050: "DP_DATA0",
    0x054: "DP_DATA1",
    0x064: "GPIO_WAKE",
    0x068: "INT_EP_CTL",
    0x06C: "BULK_IN_DLY",
    0x100: "MAC_CR",
    0x104: "ADDRH",
    0x108: "ADDRL",
    0x10C: "HASHH",
    0x110: "HASHL",
    0x114: "MII_ADDR",
    0x118: "MII_DATA",
    0x11C: "FLOW",
    0x120: "VLAN1",
    0x124: "VLAN2",
    0x128: "WUFF",
    0x12C: "WUCSR",
    0x130: "COE_CR",
}

# IEEE 802.3 clause 22 PHY registers.
MII_REGS = {
    0x00: "BMCR",
    0x01: "BMSR",
    0x02: "PHYID1",
    0x03: "PHYID2",
    0x04: "ANAR",
    0x05: "ANLPAR",
    0x06: "ANER",
    0x07: "ANNP",
    0x08: "ANLPNP",
    0x0D: "MMD_CTRL",
    0x0E: "MMD_DATA",
    0x11: "PHY_INT_SRC",
    0x1B: "SPECIAL_CTRL",
    0x1D: "PHY_INT_MASK",
    0x1F: "PHY_SPECIAL",
}

# Bit decodes as (mask, label) for the registers that matter during bring-up.
# Masks follow the Linux smsc95xx register definitions.
BITS = {
    "HW_CFG": [(0x00001000, "BIR"), (0x00000800, "LEDB"), (0x00000600, "RXDOFF"),
               (0x00000040, "DRP"), (0x00000020, "MEF"), (0x00000008, "LRST"),
               (0x00000004, "PSEL"), (0x00000002, "BCE"), (0x00000001, "SRST")],
    "MAC_CR": [(0x80000000, "RXALL"), (0x00800000, "RCVOWN"), (0x00200000, "LOOPBK"),
               (0x00100000, "FDPX"), (0x00080000, "MCPAS"), (0x00040000, "PRMS"),
               (0x00020000, "INVFILT"), (0x00010000, "PASSBAD"), (0x00008000, "HFILT"),
               (0x00002000, "HPFILT"), (0x00000800, "LCOLL"), (0x00000400, "BCAST_DIS"),
               (0x00000100, "PADSTR"), (0x00000020, "DFCHK"),
               (0x00000008, "TXEN"), (0x00000004, "RXEN")],
    "TX_CFG": [(0x00000004, "TX_ON"), (0x00000002, "TX_STOP"),
               (0x00000001, "FIFO_FLUSH")],
    "RX_CFG": [(0x00000001, "FIFO_FLUSH")],
    "E2P_CMD": [(0x80000000, "BUSY"), (0x00000400, "TIMEOUT"),
                (0x00000200, "LOADED"), (0x70000000, "OP")],
    "PM_CTRL": [(0x00000010, "PHY_RST"), (0x00000008, "WOL_EN"),
                (0x00000004, "ED_EN"), (0x00000002, "WUPS_MULTI"),
                (0x00000001, "WUPS_LINK")],
    "INT_STS": [(0x00008000, "PHY_INT"), (0x00004000, "TX_STOP"),
                (0x00001000, "RX_STOP"), (0x00000800, "PHY_INT_ALT"),
                (0x00000400, "TXE"), (0x00000200, "TDFU"),
                (0x00000100, "TDFO"), (0x00000080, "RXDF")],
    "INT_EP_CTL": [(0x00008000, "PHY_INT_EN"), (0x00004000, "TX_STOP_EN"),
                   (0x00001000, "RX_STOP_EN"), (0x00000400, "TXE_EN"),
                   (0x00000200, "TDFU_EN"), (0x00000100, "TDFO_EN"),
                   (0x00000080, "RXDF_EN")],
    "COE_CR": [(0x00010000, "TX_COE_EN"), (0x00000002, "RX_COE_MODE"),
               (0x00000001, "RX_COE_EN")],
}

MII_BUSY = 1 << 0
MII_WRITE = 1 << 1

# Register writes carry their payload on the submission URB (data_fragment);
# reads carry it on the completion URB (control.Response).
TSHARK_FIELDS = [
    "frame.number",
    "usb.urb_type",
    "usb.endpoint_address.direction",
    "usb.setup.bRequest",
    "usb.setup.wIndex",
    "usb.setup.wLength",
    "usb.data_fragment",
    "usb.control.Response",
]


def le32(hexstr):
    """Interpret a little-endian hex byte string as a u32.

    Accepts both of tshark's renderings: colon-separated ("4a:f8:f8:c2") and
    bare ("4af8f8c2").
    """
    if not hexstr:
        return None
    clean = hexstr.replace(":", "").strip()
    if len(clean) < 8:
        return None
    raw = bytes.fromhex(clean[:8])
    return int.from_bytes(raw, "little")


def decode_bits(name, value):
    """Return a human-readable bit summary for a known register, else ''."""
    flags = [label for mask, label in BITS.get(name, []) if value & mask]
    return " " + "|".join(flags) if flags else ""


def describe_mii_addr(value):
    phy = (value >> 11) & 0x1F
    idx = (value >> 6) & 0x1F
    op = "write" if value & MII_WRITE else "read"
    busy = " BUSY" if value & MII_BUSY else ""
    return phy, idx, op, busy


def run_tshark(pcap, device):
    cmd = ["tshark", "-r", pcap, "-Y", f"usb.device_address == {device}",
           "-T", "fields"]
    for field in TSHARK_FIELDS:
        cmd += ["-e", field]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"tshark failed: {proc.stderr.strip()}")
    return proc.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--device", type=int, default=4,
                    help="USB device address of the LAN9500A (default 4)")
    args = ap.parse_args()

    if not shutil.which("tshark"):
        sys.exit("tshark not found on PATH")

    rows = []
    for line in run_tshark(args.pcap, args.device):
        cols = line.split("\t")
        cols += [""] * (len(TSHARK_FIELDS) - len(cols))
        # tshark renders usb.urb_type as a quoted character, e.g. 'S'.
        cols[1] = cols[1].strip("'")
        rows.append(cols)

    # Pair each control submission with the completion that follows it, so that
    # register reads (whose data arrives on the completion URB) resolve.
    pending = None
    mii_data = None
    out = []

    for frame, urb, _direction, req, windex, _wlen, fragment, response in rows:
        is_write = req == "160"   # 0xA0 WRITE_REGISTER
        is_read = req == "161"    # 0xA1 READ_REGISTER

        if urb == "S" and (is_write or is_read):
            offset = int(windex) if windex else 0
            if is_write:
                # Value travels with the submission; emit immediately.
                out.append((frame, "W", offset, le32(fragment)))
                pending = None
            else:
                pending = (frame, "R", offset)
            continue

        if urb == "C" and pending is not None:
            frame_no, op, offset = pending
            out.append((frame_no, op, offset, le32(response or fragment)))
            pending = None

    print(f"# smsc95xx register trace  ({args.pcap}, USB device {args.device})")
    print(f"# {len(out)} vendor register accesses\n")
    print(f"{'frame':>7}  {'op':2}  {'reg':<12} {'value':<12} notes")
    print("-" * 72)

    for frame_no, op, offset, value in out:
        name = REGS.get(offset, f"0x{offset:03X}")
        vstr = "?" if value is None else f"0x{value:08X}"
        notes = ""

        if value is not None:
            notes = decode_bits(name, value)
            if name == "MII_DATA":
                mii_data = value
            elif name == "MII_ADDR":
                phy, idx, mop, busy = describe_mii_addr(value)
                mname = MII_REGS.get(idx, f"reg{idx}")
                notes = f" -> PHY{phy}.{mname} {mop}{busy}"
                if mop == "write" and mii_data is not None:
                    notes += f" val=0x{mii_data:04X}"
            elif name == "ID_REV":
                notes = f" chip=0x{value >> 16:04X} rev=0x{value & 0xFFFF:04X}"

        print(f"{frame_no:>7}  {op:2}  {name:<12} {vstr:<12}{notes}")


if __name__ == "__main__":
    main()
