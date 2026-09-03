#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Capture the USB bring-up of an smsc95xx-class device on a Linux host.
#
# Starts a usbmon capture, waits for the device to enumerate and its network
# interface to appear, then records the resulting driver/link state and full USB
# descriptors. The output pair (pcap + report) is the ground truth this project's
# macOS driver is written against.
#
# Trigger is "a new network interface appeared", not a specific VID:PID, so this
# works for any LAN950x variant without being told which one to expect.
#
# Usage:
#   ./capture-usb-bringup.sh LABEL [SETTLE_SECS] [APPEAR_TIMEOUT_SECS]
#
# Example:
#   sudo modprobe usbmon
#   ./capture-usb-bringup.sh evb            # then plug the device in
#
# Outputs /tmp/<LABEL>-bringup.pcap and /tmp/<LABEL>-report.txt.

set -u

LABEL=${1:?usage: capture-usb-bringup.sh LABEL [SETTLE] [TIMEOUT]}
SETTLE=${2:-12}
APPEAR_TIMEOUT=${3:-150}

PCAP=/tmp/${LABEL}-bringup.pcap
REPORT=/tmp/${LABEL}-report.txt
ETHTOOL=${ETHTOOL:-/usr/sbin/ethtool}   # not on PATH for non-interactive SSH on Debian

if [ ! -d /sys/kernel/debug/usb/usbmon ]; then
    echo "usbmon not available; run: sudo modprobe usbmon" >&2
    exit 1
fi

: > "$REPORT"
log() { echo "$@" | tee -a "$REPORT"; }
record() { { echo; echo "### $1"; shift; "$@" 2>&1; } >> "$REPORT"; }

log "=== $LABEL: baseline (before plug-in) ==="
log "date:   $(date -Is)"
log "kernel: $(uname -r)"
record "lsusb (before)" lsusb
record "ip -br link (before)" ip -br link

IFACES_BEFORE=$(ip -br link | awk '{print $1}' | sort)
USB_BEFORE=$(lsusb | awk '{print $6}' | sort)
DMESG_MARK=$(dmesg | wc -l)

sudo rm -f "$PCAP"
sudo tcpdump -i usbmon0 -s 0 -w "$PCAP" >/dev/null 2>&1 &
sleep 1
log ""
log ">>> CAPTURING -- plug in the '$LABEL' device now (timeout ${APPEAR_TIMEOUT}s) <<<"

# Wait for a new network interface, which is the signal that a USB-Ethernet
# device enumerated *and* a driver bound it.
new_if=""
for i in $(seq 1 "$APPEAR_TIMEOUT"); do
    new_if=$(comm -13 <(echo "$IFACES_BEFORE") \
                      <(ip -br link | awk '{print $1}' | sort) | head -1)
    [ -n "$new_if" ] && break
    sleep 1
done

if [ -n "$new_if" ]; then
    log ">>> interface '$new_if' appeared after ${i}s; capturing ${SETTLE}s more for link-up <<<"
    sleep "$SETTLE"
else
    log ">>> TIMEOUT: no new network interface after ${APPEAR_TIMEOUT}s <<<"
    log ">>> (device may have enumerated without a driver binding -- descriptors still collected)"
fi

sudo pkill -f "tcpdump -i usbmon0" 2>/dev/null
sleep 1

log ""
log "=== $LABEL: after plug-in ==="
record "dmesg (new lines)" sh -c "dmesg | tail -n +$((DMESG_MARK + 1))"
record "lsusb (after)" lsusb
record "lsusb -t" lsusb -t
record "ip -br link (after)" ip -br link

NEW_USB=$(comm -13 <(echo "$USB_BEFORE") <(lsusb | awk '{print $6}' | sort))
log "new USB IDs:   ${NEW_USB:-<none>}"
log "new interface: ${new_if:-<none>}"

if [ -n "$new_if" ]; then
    sudo ip link set "$new_if" up 2>>"$REPORT"
    sleep 4
    record "ethtool $new_if"    sudo "$ETHTOOL" "$new_if"
    record "ethtool -i $new_if" sudo "$ETHTOOL" -i "$new_if"
    record "ip -d link show $new_if" ip -d link show "$new_if"
    record "ip addr show $new_if"    ip addr show "$new_if"
fi

# Full descriptors for every newly-arrived device, by VID:PID.
for id in $NEW_USB; do
    record "lsusb -v -d $id" sudo lsusb -v -d "$id"
done

log ""
log "pcap:   $PCAP ($(sudo stat -c %s "$PCAP" 2>/dev/null || echo 0) bytes)"
log "report: $REPORT"
log ""
log "Decode with: python3 tools/decode-usbmon.py $(basename "$PCAP") --device N"
log "  (N is the USB device address from 'new high-speed USB device number N' in dmesg)"
