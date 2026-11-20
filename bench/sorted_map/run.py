#!/usr/bin/env python3
"""Time phases.kama built by two compilers, interleaved, and print each phase's minimum ns per key (KR-423).

Interleaved because this kind of host drifts by more than the effect being measured between two sequential batches.
Usage: bench/sorted_map/run.py [rounds]   (default 9)
  KAMA_BEFORE  the compiler to compare against: an install layout (bin/kama, include/, lib/kama/) of the commit before
               the change, since a bare copy of the binary reads the worktree's lib/std and so builds the NEW map
  KAMA         the compiler under test (default: ./kama)
  SORTED_N     keys per phase (default 200,000)
With no KAMA_BEFORE, only KAMA is timed.
"""
import os, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(here))
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 9
arms = []
if os.environ.get("KAMA_BEFORE"):
    arms.append(("before", os.path.abspath(os.environ["KAMA_BEFORE"])))
arms.append(("after" if arms else "kama", os.path.abspath(os.environ.get("KAMA", os.path.join(root, "kama")))))

with tempfile.TemporaryDirectory() as out:
    exe = {}
    for name, kama in arms:
        exe[name] = os.path.join(out, name)
        subprocess.run([kama, "build", os.path.join(here, "phases.kama"), "-o", exe[name], "--release"],
                       check=True, stdout=subprocess.DEVNULL)
    best, phases, checksum = {}, [], {}
    for r in range(rounds):
        for name, _ in (arms if r % 2 == 0 else arms[::-1]):
            lines = subprocess.run([exe[name]], check=True, capture_output=True, text=True).stdout.split()
            pairs = list(zip(lines[0::2], lines[1::2]))
            for phase, v in pairs:
                if phase == "checksum":
                    checksum.setdefault(name, v)
                    continue
                if phase not in phases:
                    phases.append(phase)
                key = (name, phase)
                best[key] = min(best.get(key, int(v)), int(v))

if len(set(checksum.values())) > 1:
    sys.exit("checksums differ: %s" % checksum)
names = [n for n, _ in arms]
print("%-12s" % "ns/key" + "".join("%10s" % n for n in names) + ("  after/before" if len(names) == 2 else ""))
for phase in phases:
    row = "%-12s" % phase + "".join("%10d" % best[(n, phase)] for n in names)
    if len(names) == 2 and best[("before", phase)] > 0:
        row += "  %11.2fx" % (best[("after", phase)] / best[("before", phase)])
    print(row)
