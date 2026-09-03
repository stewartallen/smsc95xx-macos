#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Resolve an exported symbol inside a DriverKit framework to a shared-cache file offset.

The DriverKit frameworks ship only as headers and .tbd stubs; the code lives in the
DriverKit dyld shared cache, and no cache-extraction tool is installed on a stock macOS.
This walks the cache directly: find the dylib by its LC_ID_DYLIB path, parse its export
trie, and print the file offset of every matching symbol. Feed that offset to disasm.sh.

    ./find-symbol.py NetworkingDriverKit enqueuePackets
    ./find-symbol.py NetworkingDriverKit ''          # list everything

Get mangled names from the SDK stub, which lists every export:
    grep -o '_ZN24IOUserNetworkPacketQueue[A-Za-z0-9_]*' \
        "$(xcrun --show-sdk-path --sdk driverkit 2>/dev/null || echo \
          /Applications/Xcode.app/Contents/Developer/Platforms/DriverKit.platform/Developer/SDKs/DriverKit25.5.sdk\
)/System/DriverKit/System/Library/Frameworks/NetworkingDriverKit.framework/NetworkingDriverKit.tbd"

Why this beats guessing: it answers "what does the family actually do" from the family's
own machine code, with no hardware cycle and no dext install. Two datapath root causes in
this project were settled this way.
"""
import struct
import sys

CACHE = ("/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit"
         "/System/Library/dyld/dyld_shared_cache_arm64e")

MH_MAGIC_64 = 0xfeedfacf
MH_DYLIB = 6
LC_SEGMENT_64 = 0x19
LC_ID_DYLIB = 0xd
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_EXPORTS_TRIE = 0x80000033


def read_uleb(buf, p):
    result = shift = 0
    while True:
        b = buf[p]
        p += 1
        result |= (b & 0x7f) << shift
        if not b & 0x80:
            break
        shift += 7
    return result, p


def find_dylib(data, want):
    """Scan for the Mach-O header whose LC_ID_DYLIB path contains `want`."""
    pos = 0
    needle = struct.pack("<I", MH_MAGIC_64)
    while True:
        pos = data.find(needle, pos)
        if pos < 0:
            return None
        _, _, _, filetype, ncmds, sizeofcmds = struct.unpack_from("<IiiIII", data, pos)
        if filetype == MH_DYLIB and 0 < ncmds < 200 and sizeofcmds < 0x8000:
            off = pos + 32
            for _ in range(ncmds):
                cmd, cmdsize = struct.unpack_from("<II", data, off)
                if not 0 < cmdsize <= sizeofcmds:
                    break
                if cmd == LC_ID_DYLIB:
                    nameoff = struct.unpack_from("<I", data, off + 8)[0]
                    end = data.find(b"\0", off + nameoff)
                    if want.encode() in data[off + nameoff:end]:
                        return pos, ncmds
                    break
                off += cmdsize
        pos += 4


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    framework = sys.argv[1]
    pattern = (sys.argv[2] if len(sys.argv) > 2 else "").encode()

    data = open(CACHE, "rb").read()
    if not data.startswith(b"dyld_v1"):
        sys.exit("%s is not a dyld shared cache" % CACHE)

    mappingOffset, mappingCount = struct.unpack_from("<II", data, 16)
    mappings = [struct.unpack_from("<QQQ", data, mappingOffset + i * 32)
                for i in range(mappingCount)]

    def vm_to_file(vmaddr):
        for addr, size, fileOff in mappings:
            if addr <= vmaddr < addr + size:
                return fileOff + (vmaddr - addr)
        return None

    hit = find_dylib(data, framework)
    if hit is None:
        sys.exit("no dylib in the cache whose install name contains %r" % framework)
    mhOff, ncmds = hit

    textVM = None
    trie = None
    off = mhOff + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            if data[off + 8:off + 24].rstrip(b"\0") == b"__TEXT":
                textVM = struct.unpack_from("<Q", data, off + 24)[0]
        elif cmd == LC_DYLD_EXPORTS_TRIE:
            trie = struct.unpack_from("<II", data, off + 8)
        elif cmd == LC_DYLD_INFO_ONLY:
            vals = struct.unpack_from("<10I", data, off + 8)
            trie = (vals[8], vals[9])
        off += cmdsize
    if textVM is None or trie is None:
        sys.exit("dylib found at 0x%x but it has no __TEXT or no export trie" % mhOff)

    buf = data[trie[0]:trie[0] + trie[1]]
    found = {}

    def walk(p, prefix):
        terminalSize, q = read_uleb(buf, p)
        if terminalSize:
            r = q
            flags, r = read_uleb(buf, r)
            if not flags & 0x08:            # not a re-export
                imageOff, r = read_uleb(buf, r)
                found[prefix] = imageOff
        p = q + terminalSize
        childCount = buf[p]
        p += 1
        for _ in range(childCount):
            end = buf.index(b"\0", p)
            edge = buf[p:end]
            childOff, p = read_uleb(buf, end + 1)
            walk(childOff, prefix + edge)

    sys.setrecursionlimit(20000)
    walk(0, b"")

    matches = sorted(s for s in found if pattern in s)
    print("# %s: __TEXT vmaddr 0x%x, %d exports, %d matching %r"
          % (framework, textVM, len(found), len(matches), pattern.decode()))
    print("# feed the file offset to ./disasm.sh <fileoff> <bytes>")
    for sym in matches:
        vm = textVM + found[sym]
        print("0x%-8x  %s" % (vm_to_file(vm), sym.decode()))


if __name__ == "__main__":
    main()
