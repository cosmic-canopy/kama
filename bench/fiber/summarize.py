#!/usr/bin/env python3
"""summarize.py — KR-254's cold-switch table, from run.sh's TSV.

For every ring size and token count: each arm's nanoseconds per hop (min and median over the rounds), and the
switch's own share, `ring` minus `touch` (the park, the wake and the switches, without the channels). Then each arm
against `ring`, paired by round: the median ratio, and in how many rounds the arm was the faster. `ring#2` is in the
same table: the gap a candidate (`ring-<name>`) has to clear.
"""
import csv
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
by = defaultdict(dict)  # (fibers, tokens) -> slot -> round -> ns per hop
for r in rows:
    by[(int(r["fibers"]), int(r["tokens"]))].setdefault(r["slot"], {})[int(r["round"])] = int(r["ns"]) / int(r["hops"])

print(f"## {len(set(r['round'] for r in rows))} rounds\n")
print("| fibers | tokens | arm | ns/hop min | ns/hop median | switch (ring − touch) | ÷ ring, paired | faster |")
print("|---|---|---|---|---|---|---|---|")
for (n, k) in sorted(by, key=lambda c: (c[1], c[0])):
    slots = by[(n, k)]
    touch = statistics.median(slots["touch"].values()) if "touch" in slots else None
    ring = slots.get("ring", {})
    for slot in sorted(slots, key=lambda s: (s != "ring", s)):
        v = slots[slot]
        med = statistics.median(v.values())
        sw = f"{med - touch:.1f}" if touch is not None and slot != "touch" else ""
        pair = ""
        if slot != "ring" and ring:
            common = [r for r in v if r in ring]
            if common:
                ratios = [v[r] / ring[r] for r in common]
                pair = f"{statistics.median(ratios):.3f} | {sum(1 for x in ratios if x < 1)}/{len(common)}"
        print(f"| {n:,} | {k} | {slot} | {min(v.values()):.1f} | {med:.1f} | {sw} | {pair or ' | '} |")
