#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Ask AMFI whether a provisioning profile authorises a candidate entitlements file,
# without needing the hardware.
#
# The problem this solves: a dext whose entitlements the profile does not authorise
# still signs cleanly and still reports "valid on disk" from `codesign -v`. The
# rejection only appears when the dext is launched, which normally means installing
# the system extension and physically replugging the device -- a slow loop, and one
# that needs a human, so working out which entitlement shape a profile accepts by
# trial and error is expensive.
#
# The shortcut: profile authorisation is checked by amfid in the exec path, before
# the kernel gets far enough to care that a DriverKit Mach-O cannot run as an
# ordinary process. So exec'ing the dext binary directly provokes the full
# amfid + taskgated-helper check and leaves the verdict in the unified log. The
# process then dies (SIGKILL, "Killed: 9"), which is expected and not the result --
# the log line is the result.
#
#   ./probe-entitlements.sh <bundle.dext> <identity> <candidate.entitlements>
#
# Prints ACCEPTED, REJECTED or INCONCLUSIVE. The bundle must already contain the
# embedded.provisionprofile you want to test against; this script re-signs a
# throwaway copy, so the bundle you pass is left untouched.
#
# Always probe a control case with the restricted entitlement absent -- it must come
# back ACCEPTED. Without that, a harness that reports ACCEPTED for everything looks
# like a pass.
#
# Measured results for this driver's profile are in
# reference/signing-entitlement-match.txt.
set -uo pipefail

[ $# -eq 3 ] || {
    echo "usage: $0 <bundle.dext> <signing-identity> <candidate.entitlements>" >&2
    exit 2
}
bundle=$1 identity=$2 ent=$3

[ -d "$bundle" ] || { echo "no such bundle: $bundle" >&2; exit 1; }
[ -f "$ent" ]    || { echo "no such entitlements file: $ent" >&2; exit 1; }

# The executable inside a flat dext bundle is named after the bundle identifier,
# which is also the bundle's own name minus the .dext suffix.
dext_id=$(basename "$bundle" .dext)
[ -f "$bundle/$dext_id" ] || {
    echo "no executable $dext_id inside $bundle" >&2; exit 1; }
[ -f "$bundle/embedded.provisionprofile" ] || {
    echo "warning: $bundle has no embedded.provisionprofile; there is nothing to" >&2
    echo "         authorise against and every candidate will be rejected" >&2
}

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cp -R "$bundle" "$work/" || exit 1
copy="$work/$(basename "$bundle")"

# --identifier has to be set explicitly: the copy lives under a temp path, and the
# identifier must keep matching the profile's App ID or the failure we measure is
# the wrong one.
if ! codesign --force --sign "$identity" --entitlements "$ent" \
              --identifier "$dext_id" --timestamp=none "$copy" 2>"$work/err"; then
    echo "SIGN FAILED: $(cat "$work/err")"
    exit 1
fi

# Bracket the log query by time rather than by pid: the interesting lines come from
# amfid and taskgated-helper, which are long-lived daemons, not from our process.
start=$(date '+%Y-%m-%d %H:%M:%S')
sleep 1
# Run it under a nested shell so that shell, and not ours, is the one printing
# "Killed: 9" when the process dies -- the death is expected, and reporting it
# would make a successful probe look like a failure.
bash -c '"$0" >/dev/null 2>&1' "$copy/$dext_id" 2>/dev/null
sleep 2

# /usr/bin/log, not `log` -- the bare name hits a zsh builtin.
out=$(/usr/bin/log show --start "$start" \
        --predicate 'process == "taskgated-helper"' --style compact 2>/dev/null \
      | grep -iE "matchEntitlement|Unsatisfied|Disallow|Checking profile")

if grep -q "Unsatisfied" <<<"$out"; then
    echo "REJECTED: $(grep -oE 'matchEntitlement.*' <<<"$out" | head -1)"
    exit 1
elif grep -q "Checking profile" <<<"$out"; then
    echo "ACCEPTED (profile checked, no unsatisfied entitlements)"
    exit 0
else
    # No taskgated-helper activity at all. Usually means the binary never reached
    # the profile check -- a bad path, or a signature so broken that AMFI rejected
    # it earlier. Check the log by hand before believing a candidate is fine.
    echo "INCONCLUSIVE (no taskgated-helper activity; check the log by hand)"
    exit 2
fi
