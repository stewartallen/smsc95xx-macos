#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Dump what a provisioning profile actually grants.
#
# A .provisionprofile is a CMS-signed plist, so its contents are not readable directly.
# What matters for a dext is the Entitlements dictionary: a restricted entitlement in the
# binary is only honoured if the profile grants it, AND the granted value has to be
# compatible with the value in the entitlements file. Our transport.usb entitlement is an
# array of {idVendor, idProduct} dicts rather than a boolean, so the form matters.
#
#   ./inspect-profile.sh some.provisionprofile
set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 <file.provisionprofile>" >&2; exit 2; }
f=$1
[ -f "$f" ] || { echo "no such file: $f" >&2; exit 1; }

plist=$(mktemp); trap 'rm -f "$plist"' EXIT
security cms -D -i "$f" -o "$plist" 2>/dev/null || {
    echo "could not decode $f -- is it a provisioning profile?" >&2; exit 1; }

get() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || echo "(absent)"; }

echo "Name:        $(get Name)"
echo "AppIDName:   $(get AppIDName)"
echo "TeamName:    $(get TeamName)"
echo "Platform:    $(get 'Platform:0')"
echo "Created:     $(get CreationDate)"
echo "Expires:     $(get ExpirationDate)"
echo "Team IDs:    $(get 'TeamIdentifier:0')"
echo
echo "Application identifier granted:"
echo "  $(get 'Entitlements:com.apple.application-identifier')"
echo
echo "Devices in profile: $(/usr/libexec/PlistBuddy -c 'Print :ProvisionedDevices' "$plist" 2>/dev/null | grep -c . || echo 0)"
echo
echo "=== Entitlements granted (this is the part that must match the binary) ==="
/usr/libexec/PlistBuddy -c "Print :Entitlements" "$plist" 2>/dev/null || echo "  (none)"
echo
echo "=== DriverKit / system-extension entitlements specifically ==="
/usr/libexec/PlistBuddy -c "Print :Entitlements" "$plist" 2>/dev/null \
  | grep -iE "driverkit|system-extension" || echo "  none present -- the profile grants no DriverKit or system-extension entitlement"
