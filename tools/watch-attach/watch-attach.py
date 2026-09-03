#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Watch for supported 10BASE-T1S dongle attaches and classify each one.
#
# Handles both supported units. The MACH SYSTEMS 10BASET1S-USB-IF has an unreliable
# EEPROM auto-load, so a given replug lands in one of several states and only one is
# useful for driver testing; the Microchip EVB-LAN8670-USB reads cleanly and has only
# a healthy state. Working out which state you
# got, by hand, on every cycle, is slow and error-prone -- particularly because two of
# the bad states look superficially like a driver bug rather than a hardware fault.
#
#   ./watch-attach.py                 # watch until interrupted
#   ./watch-attach.py --until-good    # exit 0 on the first healthy attach
#
# Finds the dongle by looking for the SMSC USB2422 hub inside it (0x0424:0x2422) and
# taking that hub's non-STM32 child, rather than by USB port or by the LAN9500A's own
# vendor/product -- because in the corrupt-descriptor state the LAN9500A reports neither
# its real vendor nor its real product, so anything keyed on those would simply not see
# the device it is trying to report on.
#
# States it distinguishes:
#   HEALTHY   idProduct 0x9905, descriptors sane. EEPROM auto-load worked. Test on this.
#   EEPROM    idProduct 0x9E00. Auto-load failed; the chip fell back to its default ID.
#             The driver refuses to start rather than invent a MAC, which is correct --
#             so this is also the state that proves the provenance gate still works.
#   CORRUPT   descriptors are garbage (wrong vendor, impossible bMaxPacketSize0, and so
#             on). The device is not identifiable, so no personality can match it. This
#             is a hardware fault, not a driver problem.

import plistlib
import subprocess
import sys
import time

MACH_HUB_VID  = 0x0424
MACH_HUB_PID  = 0x2422
STM32_VID     = 0x0483
LAN9500A_VID  = 0x0424
EVB_VID       = 0x184F
EVB_PID       = 0x0051
PID_EEPROM_OK = 0x9905
PID_DEFAULT   = 0x9E00

# The programmed MAC of each unit, used only to report which one turned up. Seeing the
# wrong unit's MAC would mean the driver had served an address it did not read from that
# chip's EEPROM, which is the one thing the provenance gate exists to prevent.
EXPECTED_MAC  = {"MACH": "fc:61:79:90:04:56", "EVB": "9c:95:6e:b5:9b:62"}


def ioreg_tree():
    """The IOUSB plane as a plist. -a gives parseable output; scraping the text tree
    misattributes properties to the wrong node, which produced a wrong conclusion once
    already."""
    out = subprocess.run(["ioreg", "-a", "-p", "IOUSB", "-l", "-w0"],
                         capture_output=True)
    if out.returncode != 0 or not out.stdout:
        return None
    try:
        return plistlib.loads(out.stdout)
    except Exception:
        return None


def walk(node, depth=0):
    yield node, depth
    for child in node.get("IORegistryEntryChildren", []) or []:
        yield from walk(child, depth + 1)


def find_lan9500a(tree):
    """The MACH's internal hub, then its child that is not the STM32 control function.

    Deliberately NOT keyed on the LAN9500A's own vendor/product: in the corrupt-descriptor
    state it reports neither, so anything matching on those would fail to see the very
    device it is meant to report on."""
    if tree is None:
        return None
    for node, _ in walk(tree):
        if (node.get("idVendor") == MACH_HUB_VID
                and node.get("idProduct") == MACH_HUB_PID):
            for child in node.get("IORegistryEntryChildren", []) or []:
                if child.get("idVendor") == STM32_VID:
                    continue          # the STM32 CDC control function
                if "idVendor" not in child:
                    continue          # an interface node, not a device
                return child
    return None


def find_evb(tree):
    """The Microchip EVB-LAN8670-USB: a single function, no internal hub, no control
    channel. Its EEPROM reads cleanly, so it has only one healthy state and can be
    matched on vendor/product directly."""
    if tree is None:
        return None
    for node, _ in walk(tree):
        if (node.get("idVendor") == EVB_VID
                and node.get("idProduct") == EVB_PID):
            return node
    return None


def find_dongle(tree):
    """Returns (node, kind) for whichever supported dongle is attached."""
    dev = find_lan9500a(tree)
    if dev is not None:
        return dev, "MACH"
    dev = find_evb(tree)
    if dev is not None:
        return dev, "EVB"
    return None, None


def classify_evb(dev):
    if dev.get("bMaxPacketSize0") in (0, None) or dev.get("bNumConfigurations") != 1:
        return "CORRUPT", "descriptors are garbage"
    return "HEALTHY", "EVB, single function, EEPROM reads cleanly"


def classify(dev):
    vid  = dev.get("idVendor")
    pid  = dev.get("idProduct")
    mps0 = dev.get("bMaxPacketSize0")
    ncfg = dev.get("bNumConfigurations")

    # bMaxPacketSize0 of 0 is impossible under the USB spec: endpoint 0 must be 8, 16,
    # 32 or 64. Treat it as the definitive corruption tell rather than guessing from
    # the product ID, which is itself EEPROM-derived and therefore also suspect.
    if vid != LAN9500A_VID or mps0 in (0, None) or ncfg != 1:
        return "CORRUPT", "descriptors are garbage; not identifiable as ours"
    if pid == PID_EEPROM_OK:
        return "HEALTHY", "EEPROM auto-load succeeded"
    if pid == PID_DEFAULT:
        return "EEPROM", "auto-load failed; chip default ID"
    return "CORRUPT", f"unexpected idProduct 0x{pid:04x}"


def driver_state():
    """What the driver and the network stack made of this attach."""
    def sh(cmd):
        return subprocess.run(cmd, shell=True, capture_output=True,
                              text=True).stdout.strip()

    ioreg_txt = sh("ioreg -p IOUSB -l -w0 2>/dev/null")
    dev_drv   = "SMSC95xxUSBDevice" in ioreg_txt
    eth_drv   = "SMSC95xxDriver" in ioreg_txt

    en = sh("for i in $(ifconfig -l); do case $i in en*) "
            "ifconfig $i 2>/dev/null | grep -qE 'fc:61:79:90:04:56|9c:95:6e:b5:9b:62' "
            "&& echo $i;; esac; done")
    name = sh("networksetup -listallhardwareports 2>/dev/null "
              "| grep -B1 " + en + " | grep 'Hardware Port' | head -1") if en else ""
    return dev_drv, eth_drv, en, name


def main():
    # Line-buffer stdout. Python block-buffers when stdout is not a terminal, so piping
    # this to a file or another process shows nothing at all until the buffer fills --
    # which looks exactly like the watcher being broken.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    until_good = "--until-good" in sys.argv
    print("Watching for MACH and EVB dongle attaches. Ctrl-C to stop.")
    print("Replug the dongle; each attach is classified below.\n")

    last_session = None
    cycle = 0
    while True:
        dev, kind = find_dongle(ioreg_tree())
        session = dev.get("sessionID") if dev else None

        if session != last_session:
            last_session = session
            if dev is None:
                print("  [detached]")
            else:
                cycle += 1
                state, why = (classify_evb(dev) if kind == "EVB"
                              else classify(dev))
                vid, pid = dev.get("idVendor"), dev.get("idProduct")
                print(f"--- attach #{cycle}: {kind} {state} -- {why}")
                print(f"    idVendor=0x{vid:04x} idProduct=0x{pid:04x} "
                      f"bMaxPacketSize0={dev.get('bMaxPacketSize0')} "
                      f"bNumConfigurations={dev.get('bNumConfigurations')}")

                time.sleep(4)          # let the driver start and log
                dev_drv, eth_drv, en, name = driver_state()
                print(f"    device driver attached : {dev_drv}")
                print(f"    ethernet driver attached: {eth_drv}")
                print(f"    interface              : {en or 'none'}")
                if en:
                    import subprocess as _sp
                    got = _sp.run(f"ifconfig {en} | awk '/ether/{{print $2}}'",
                                  shell=True, capture_output=True,
                                  text=True).stdout.strip()
                    want = EXPECTED_MAC.get(kind)
                    flag = "" if got == want else f"  <-- EXPECTED {want}"
                    print(f"    MAC                    : {got}{flag}")
                if name:
                    print(f"    display name           : {name}")

                if state == "HEALTHY":
                    print("\n    ^^^ HEALTHY attach -- good one to test on.")
                    if until_good:
                        return 0
                else:
                    print("    (not a hardware fault in our driver -- replug again)")
                print()
        time.sleep(1.5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nstopped")
