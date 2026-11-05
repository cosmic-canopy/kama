#!/usr/bin/env python3
"""summarize.py — KR-3's pool table, from run.py's TSV.

For every shape and worker count, each arm's cost per barrier in microseconds: the mean per run (min and median over the rounds),
the p50 and p99 inside a run (median over the rounds), `extra`, the CPU cores burned beyond the one the caller spins on (median),
and the arm against `kama`, paired by round. `kama#2` is in the same table: its gap to `kama` is the noise floor.
"""
import csv
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
by = defaultdict(lambda: defaultdict(dict))
for r in rows:
    by[(r["shape"], int(r["workers"]))][r["slot"]][int(r["round"])] = r
base_arm = "kama"
print(f"## {len(set(r['round'] for r in rows))} rounds\n")
print("| shape | k | arm | µs mean min | µs mean median | µs p50 | µs p99 | extra cores | ÷ kama, paired |")
print("|---|---|---|---|---|---|---|---|---|")
order = {"tight": 0, "frame": 1, "phase-100us": 2, "phase-1ms": 3, "nested": 4}
for key in sorted(by, key=lambda k: (order.get(k[0], 9), k[1])):
    shape, k = key
    slots = by[key]
    per = lambda r: int(r["total_ns"]) / int(r["calls"]) / 1000
    base = slots.get(base_arm, {})
    for slot in sorted(slots, key=lambda s: (s != base_arm, s)):
        v = slots[slot]
        means = [per(x) for x in v.values()]
        p50 = statistics.median(int(x["p50_ns"]) / 1000 for x in v.values())
        p99 = statistics.median(int(x["p99_ns"]) / 1000 for x in v.values())
        extra = ""
        if all(x["cpu_s"] for x in v.values()):
            extra = f"{statistics.median((float(x['cpu_s']) - float(x['wall_s'])) / float(x['wall_s']) for x in v.values()):.2f}"
        pair = ""
        if slot != base_arm and base:
            common = [r for r in v if r in base]
            if common:
                pair = f"{statistics.median(per(v[r]) / per(base[r]) for r in common):.3f}"
        print(f"| {shape} | {k} | {slot} | {min(means):,.2f} | {statistics.median(means):,.2f} | {p50:,.2f} | {p99:,.2f} | {extra} | {pair} |")
