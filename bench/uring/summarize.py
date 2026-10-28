#!/usr/bin/env python3
"""summarize.py — the per-request table for KR-253 decision 8, from run.sh's TSV.

For every load (connections, reply size): each arm's server CPU per request (the server thread plus any io-wq worker
the ring ran for it; min and median over the rounds), its system calls per request (waits, reads and writes), its
ring submissions per request, and the arm against the reference `et` (kama's epoll protocol today), paired by round:
the median ratio, and in how many rounds the arm used less CPU. `et#2`, the reference run again, is the gap a real
difference has to clear. Decision 8's bar (ruled 2026-10-28): an arm is built only if it uses at least 5% less CPU
per request than `et` at 1,000 and 10,000 connections, in at least 12 of 15 rounds, on both machines.
"""
import csv
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
REF, BAR = "et", 0.95
by = defaultdict(dict)  # (conns, reply) -> slot -> round -> row
for r in rows:
    by[(int(r["conns"]), int(r["reply"]))].setdefault(r["slot"], {})[int(r["round"])] = r


def per(r, *fields):
    return sum(int(r[f]) for f in fields) / int(r["requests"])


print(f"## kernel {rows[0]['kernel']}: {len(set(r['round'] for r in rows))} rounds\n")
clears = defaultdict(list)  # arm -> [(load, cleared)] at 1,000 and 10,000 connections
for (conns, reply), slots in sorted(by.items()):
    print(f"### {conns} connections, {reply}-byte reply\n")
    print("| arm | CPU ns/req min | median | syscalls/req | submissions/req | vs et: median ratio | less CPU in |")
    print("|---|---|---|---|---|---|---|")
    base = slots[REF]
    for slot in sorted(slots, key=lambda s: (s.split("#")[0] != REF, s)):
        runs = slots[slot]
        cpu = sorted(per(r, "cpu_ns", "wq_ns") for r in runs.values())
        sc = statistics.median(per(r, "waits", "reads", "writes") for r in runs.values())
        sq = statistics.median(per(r, "sqes") for r in runs.values())
        if slot == REF:
            cmp_ = "—", "—"
        else:
            paired = [(per(runs[k], "cpu_ns", "wq_ns"), per(base[k], "cpu_ns", "wq_ns")) for k in runs if k in base]
            ratio = statistics.median(a / b for a, b in paired)
            fewer = sum(a < b for a, b in paired)
            cmp_ = f"{ratio:.3f}", f"{fewer}/{len(paired)}"
            if conns >= 1000 and "#" not in slot:
                clears[slot].append((f"{conns}/{reply}", ratio <= BAR and fewer >= len(paired) * 12 / 15))
        print(f"| {slot} | {cpu[0]:.0f} | {statistics.median(cpu):.0f} | {sc:.3f} | {sq:.3f} | {cmp_[0]} | {cmp_[1]} |")
    print()

print("### Decision 8's bar on this machine (5% less CPU per request than et, 12/15 rounds, at 1,000 and 10,000)\n")
for arm, loads in sorted(clears.items()):
    verdict = "clears it" if all(ok for _, ok in loads) else "does not"
    print(f"- {arm}: {verdict} ({', '.join(f'{l} {chr(10003) if ok else chr(10007)}' for l, ok in loads)})")
