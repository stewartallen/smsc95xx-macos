#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Aggregate an xctrace Time Profiler capture into a self-weight table and a flame tree.

`xctrace record --template 'Time Profiler' --attach <pid>` works on a running dext
without root (the DriverKit process is attachable even though `sample` refuses), but
Instruments' GUI is the only supported reader. This turns the capture into text: total
on-CPU time, self weight by leaf function, and a root->leaf flame tree — which is all
a "where do the cycles go" question needs.

Usage:
    ./aggregate-time-profile.py CAPTURE.trace     # runs xctrace export itself
    ./aggregate-time-profile.py EXPORT.xml        # from an existing export

The XML export id/ref-compresses every element (the first occurrence carries id=,
repeats carry ref=), so frames and weights must be resolved through a registry before
the backtraces can be read.
"""

import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

TIME_PROFILE_XPATH = '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'
PRUNE = 0.015  # flame-tree nodes under 1.5% of total are hidden
BAR_WIDTH = 24


def short_name(name):
    """Strip C++ argument lists so tree labels stay one line."""
    cut = name.find("(")
    base = name[:cut] if cut > 0 else name
    return base.replace("invocation function for block in ", "block in ")


def find_xctrace():
    for candidate in (
        ["xcrun", "--find", "xctrace"],
    ):
        try:
            path = subprocess.check_output(candidate, text=True,
                                           stderr=subprocess.DEVNULL).strip()
            if path:
                return path
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    # xcode-select may point at CommandLineTools, which has no xctrace.
    fallback = "/Applications/Xcode.app/Contents/Developer/usr/bin/xctrace"
    if Path(fallback).exists():
        return fallback
    sys.exit("xctrace not found: install Xcode or pass an exported .xml instead")


def export_trace(trace_path):
    out = tempfile.NamedTemporaryFile(suffix=".xml", delete=False)
    out.close()
    subprocess.run([find_xctrace(), "export", "--input", trace_path,
                    "--xpath", TIME_PROFILE_XPATH, "--output", out.name],
                   check=True, stdout=subprocess.DEVNULL)
    return out.name


def main(path):
    if path.endswith(".trace"):
        path = export_trace(path)
    tree = ET.parse(path)

    registry = {}
    for elem in tree.iter():
        eid = elem.get("id")
        if eid is not None:
            registry[eid] = elem

    def resolve(elem):
        ref = elem.get("ref")
        return registry[ref] if ref is not None else elem

    total_weight = 0
    self_weight = defaultdict(int)  # (binary, function) -> ns
    flame_root = {"w": 0, "self": 0, "kids": {}}
    nrows = 0

    for row in tree.iter("row"):
        nrows += 1
        weight = int(resolve(row.find("weight")).text)
        total_weight += weight

        bt = row.find("tagged-backtrace")
        if bt is None:
            bt = row.find("backtrace")
        if bt is None:
            self_weight[("", "<no backtrace>")] += weight
            continue
        frames = []
        for f in resolve(bt).iter("frame"):
            f = resolve(f)
            name = f.get("name") or f.get("addr") or "?"
            binel = f.find("binary")
            binname = resolve(binel).get("name") or "" if binel is not None else ""
            frames.append((binname, name))
        if frames:
            self_weight[frames[0]] += weight
            node = flame_root
            for _, name in reversed(frames):  # root -> leaf
                node = node["kids"].setdefault(short_name(name),
                                               {"w": 0, "self": 0, "kids": {}})
                node["w"] += weight
            node["self"] += weight

    ms = lambda ns: ns / 1e6
    print(f"samples={nrows}  total on-CPU={ms(total_weight):.1f} ms")
    print("\n== self weight by leaf function (top 25) ==")
    for (b, n), w in sorted(self_weight.items(), key=lambda kv: -kv[1])[:25]:
        print(f"{ms(w):9.1f} ms  {100 * w / total_weight:5.1f}%  [{b}] {n}")

    print(f"\n== flame tree, root->leaf (nodes under {PRUNE * 100:.1f}% pruned) ==")

    def render(node, name, indent, is_last):
        # Collapse runs of single children so deep dispatch preambles stay short.
        chain = [name]
        while len(node["kids"]) == 1 and node["self"] / total_weight < PRUNE:
            only_name, only = next(iter(node["kids"].items()))
            if (node["w"] - only["w"]) / total_weight > PRUNE:
                break
            chain.append(only_name)
            node = only
        label = " > ".join(chain) if len(chain) <= 4 else \
            chain[0] + " > … > " + " > ".join(chain[-2:])
        pct = node["w"] / total_weight
        bar = "█" * max(1, round(pct * BAR_WIDTH))
        print(f"{pct * 100:5.1f}% {bar:<{BAR_WIDTH}} "
              f"{indent}{'└─ ' if is_last else '├─ '}{label}")
        kids = sorted(((k, v) for k, v in node["kids"].items()
                       if v["w"] / total_weight >= PRUNE),
                      key=lambda kv: -kv[1]["w"])
        shown = sum(v["w"] for _, v in kids)
        child_indent = indent + ("   " if is_last else "│  ")
        rest = node["w"] - shown - node["self"]
        for i, (k, v) in enumerate(kids):
            render(v, k, child_indent,
                   i == len(kids) - 1 and rest / total_weight < PRUNE)
        if rest / total_weight >= PRUNE:
            print(f"{rest / total_weight * 100:5.1f}% {'':<{BAR_WIDTH}} "
                  f"{child_indent}└─ (other)")

    roots = sorted(flame_root["kids"].items(), key=lambda kv: -kv[1]["w"])
    for i, (k, v) in enumerate(roots):
        if v["w"] / total_weight >= PRUNE:
            render(v, k, "", i == len(roots) - 1)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    main(sys.argv[1])
