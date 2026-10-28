#!/usr/bin/env python3
"""summarize.py — KR-243's tables, from bench/server/run.sh's TSV.

For every config (mode, connections): each arm's requests/s, p50/p99/p99.9 latency, server CPU per request and how busy
the server was (medians over the rounds), its RSS, and against the reference arm (`c` unless REF=<arm>), paired by
round: the median requests/s ratio and in how many rounds the arm was the faster, and the median CPU-per-request ratio
and in how many rounds the arm spent less. CPU is the server's inside the client's measured window (`srv_cpu_s`); a TSV
from before that column has only the whole run's (`cpu_s`, start and warm-up included), and no busy figure. Busy is the
share of the server's cores that was not idle over the window (`srv_busy`, from /proc/stat, so it includes the kernel's
softirq time, which no process is charged for): near 100% the server was the limit, well under it the client was. In a
skew config the latencies are the light connections' only. The reference's own second run (`<ref>#2`) is in the same
table: the gap a real difference has to clear. A claim needs >= 12 of 15 paired rounds beyond that gap (the house
rule). Idle configs report resident memory divided by the connections held instead. RSS is the server's process tree:
its peak on Linux, its size at the end of the run on macOS, which keeps no peak per process. A run with any error — a
wrong reply, an unexpected close, a failed connect — is reported and kept out of every number.
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
ref = os.environ.get("REF", "c")
by = defaultdict(lambda: defaultdict(dict))  # (mode, conns) -> slot -> round -> row
bad = []
for r in rows:
    if r["errors"] not in ("0",):
        bad.append(r)
        continue
    by[(r["mode"], int(r["conns"]))][r["slot"]][int(r["round"])] = r

rounds = len({r["round"] for r in rows})
print(f"## {rounds} rounds\n")
if bad:
    print(f"**{len(bad)} run(s) had errors and are left out:** " +
          ", ".join(f"{r['slot']} {r['mode']}:{r['conns']} (round {r['round']}, {r['errors']})" for r in bad) + "\n")

med = statistics.median


def cpu_us(r):   # server CPU per request, µs: the measured window's when the TSV has it
    w = float(r.get("srv_cpu_s") or 0)
    return (w if w > 0 else float(r["cpu_s"])) * 1e6 / max(int(r["requests"]), 1)


def busy(r):
    b = float(r.get("srv_busy") or 0)
    return b if b > 0 else None
for (mode, conns), slots in sorted(by.items(), key=lambda kv: (kv[0][0] == "idle", kv[0])):
    print(f"### {mode}, {conns} connections\n")
    order = sorted(slots, key=lambda s: (s.split("#")[0] != ref, s))
    if mode == "idle":
        print("| arm | held | RSS MiB | RSS ÷ held, KiB (process baseline included) |")
        print("|---|---|---|---|")
        for slot in order:
            runs = list(slots[slot].values())
            held = med(int(r["requests"]) for r in runs)
            rss = med(int(r["rss_kb"]) for r in runs)
            print(f"| {slot} | {held:.0f} | {rss / 1024:.1f} | {rss / max(held, 1):.1f} |")
        print()
        continue
    print("| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB "
          "| vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    base = slots.get(ref, {})
    for slot in order:
        runs = slots[slot]
        v = list(runs.values())
        rps = med(float(r["rps"]) for r in v)
        cpu = med(cpu_us(r) for r in v)
        b = [x for x in (busy(r) for r in v) if x is not None]
        rss = med(int(r["rss_kb"]) for r in v) / 1024
        cmp_ = ("—",) * 4
        paired = [k for k in runs if k in base] if slot != ref else []
        if paired:
            rr = [float(runs[k]["rps"]) / float(base[k]["rps"]) for k in paired]
            cr = [cpu_us(runs[k]) / cpu_us(base[k]) for k in paired]
            cmp_ = (f"{med(rr):.3f}", f"{sum(x > 1 for x in rr)}/{len(paired)}",
                    f"{med(cr):.3f}", f"{sum(x < 1 for x in cr)}/{len(paired)}")
        print(f"| {slot} | {rps:,.0f} | {med(float(r['p50_us']) for r in v):.0f} | {med(float(r['p99_us']) for r in v):.0f} "
              f"| {med(float(r['p999_us']) for r in v):.0f} | {cpu:.2f} | {f'{med(b):.0%}' if b else '—'} | {rss:.1f} "
              f"| {' | '.join(cmp_)} |")
    print()
