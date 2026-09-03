#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0
#
# Disassemble a range of the DriverKit dyld shared cache.
#
#   ./disasm.sh <file-offset-hex> [byte-count]
#
# Get the offset from ./find-symbol.py. There is no cache-extraction tool on a stock
# macOS and otool cannot open a path that only exists inside the cache, so the bytes are
# carved out, re-emitted as .byte directives, assembled, and disassembled. Branch targets
# are therefore relative to the start of the carved range, not to the real load address.
set -e

CACHE=/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/dyld_shared_cache_arm64e
OFF=${1:?usage: disasm.sh <file-offset-hex> [byte-count]}
LEN=${2:-256}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

/usr/bin/python3 - "$CACHE" "$OFF" "$LEN" > "$OUT/x.s" <<'PY'
import sys
cache, off, length = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
with open(cache, 'rb') as f:
    f.seek(off)
    blob = f.read(length)
print('.text')
print('.globl _start_of_range')
print('_start_of_range:')
for i in range(0, len(blob), 4):
    print('.byte ' + ','.join('0x%02x' % b for b in blob[i:i + 4]))
PY

xcrun clang -c -arch arm64e "$OUT/x.s" -o "$OUT/x.o"
otool -arch arm64e -tV "$OUT/x.o" | tail -n +4
