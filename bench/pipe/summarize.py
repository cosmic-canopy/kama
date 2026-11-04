#!/usr/bin/env python3
"""summarize.py — KR-264's pipe table, from run.sh's TSV.

For every workload and size: each arm's nanoseconds per operation (min and median over the rounds; a round trip for
`pingpong`, a chunk for `bulk`, all four calls for `ops`, a chunk each way for `split`), MB/s one way for `bulk` and `split`,
and each arm against `kama`, paired by round:
the median ratio, and in how many rounds the arm was the faster. `kama#2` is in the same table: its gap to `kama` is the
noise floor a difference has to clear.
"""
import csv
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
by = defaultdict(dict)  # (workload, bytes, capacity) -> slot -> round -> ns per operation
for r in rows:
    key = (r["workload"], int(r["bytes"]), int(r["capacity"]))
    by[key].setdefault(r["slot"], {})[int(r["round"])] = int(r["ns"]) / int(r["count"])

print(f"## {len(set(r['round'] for r in rows))} rounds\n")
print("| workload | bytes | ring | arm | ns/op min | ns/op median | MB/s (min) | ÷ kama, paired | faster |")
print("|---|---|---|---|---|---|---|---|---|")
for (workload, bytes_, capacity) in sorted(by):
    slots = by[(workload, bytes_, capacity)]
    base = slots.get("kama", {})
    for slot in sorted(slots, key=lambda s: (s != "kama", s)):
        v = slots[slot]
        best = min(v.values())
        rate = f"{bytes_ * 1000 / best:,.0f}" if workload in ("bulk", "split") else ""
        pair = " | "
        if slot != "kama" and base:
            common = [r for r in v if r in base]
            if common:
                ratios = [v[r] / base[r] for r in common]
                pair = f"{statistics.median(ratios):.3f} | {sum(1 for x in ratios if x < 1)}/{len(common)}"
        print(f"| {workload} | {bytes_:,} | {capacity:,} | {slot} | {best:,.1f} | {statistics.median(v.values()):,.1f} | {rate} | {pair} |")
