#!/usr/bin/env python3
"""summarize.py — the per-request table for KR-3 decision 6, from run.sh's TSV.

For every workload and connection count: each arm's server CPU per request (min and median over the rounds), its
system calls per request (a wait, plus an explicit registration call), its kevent change records per request, and
its reports nobody was waiting for. Then each arm against the reference arm (`et` on epoll, `clear` on kqueue),
paired by round: the median ratio, and in how many rounds the arm was the slower. The reference's own second run
(`<ref>#2`) is in the same table: the gap that a real difference has to clear. `REF=<arm>` picks another reference.
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
if not rows:
    sys.exit("summarize.py: no rows")
backend = rows[0]["backend"]
ref = os.environ.get("REF") or {"epoll": "et", "kqueue": "clear"}[backend]
by = defaultdict(dict)  # (workload, conns) -> slot -> round -> row
for r in rows:
    by[(r["workload"], int(r["conns"]))].setdefault(r["slot"], {})[int(r["round"])] = r


def per(r, field):
    return int(r[field]) / int(r["requests"])


print(f"## {backend}: {len(set(r['round'] for r in rows))} rounds\n")
for (workload, conns), slots in sorted(by.items()):
    print(f"### {workload}, {conns} connections\n")
    print("| arm | CPU ns/req min | median | syscalls/req (wait + ctl) | ctl/req | changes/req | unwanted reports/req | vs ref: median ratio | slower in |")
    print("|---|---|---|---|---|---|---|---|---|")
    base = slots[ref]
    for slot in sorted(slots, key=lambda s: (s.split("#")[0] != ref, s)):
        runs = slots[slot]
        cpu = sorted(per(r, "cpu_ns") for r in runs.values())
        sys_ = statistics.median((int(r["waits"]) + int(r["ctl"])) / int(r["requests"]) for r in runs.values())
        ctl = statistics.median(per(r, "ctl") for r in runs.values())
        chg = statistics.median(per(r, "changes") for r in runs.values())
        idle = statistics.median(per(r, "idle") for r in runs.values())
        if slot == ref:
            cmp_ = "—", "—"
        else:
            paired = [(per(runs[k], "cpu_ns"), per(base[k], "cpu_ns")) for k in runs if k in base]
            ratio = statistics.median(a / b for a, b in paired)
            slower = sum(a > b for a, b in paired)
            cmp_ = f"{ratio:.3f}", f"{slower}/{len(paired)}"
        print(f"| {slot} | {cpu[0]:.0f} | {statistics.median(cpu):.0f} | {sys_:.4f} | {ctl:.4f} | {chg:.4f} | {idle:.4f} | {cmp_[0]} | {cmp_[1]} |")
    print()
