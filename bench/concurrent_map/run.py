#!/usr/bin/env python3
"""Time one_lock.kama against sharded.kama, interleaved, and print the minimum of each (KR-232).

Interleaved because this kind of host drifts by more than the effect being measured between two sequential batches.
Usage: bench/concurrent_map/run.py [rounds]   (builds both with ./kama --release into a temporary directory)
"""
import os, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(here))
kama = os.environ.get("KAMA", os.path.join(root, "kama"))
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 7
names = ["one_lock", "sharded"]
with tempfile.TemporaryDirectory() as out:
    for n in names:
        subprocess.run([kama, "build", os.path.join(here, n + ".kama"), "-o", os.path.join(out, n), "--release"],
                       check=True, stdout=subprocess.DEVNULL)
    times = {n: [] for n in names}
    for r in range(rounds):
        for n in (names if r % 2 == 0 else names[::-1]):
            t = time.perf_counter()
            subprocess.run([os.path.join(out, n)], check=True)
            times[n].append(time.perf_counter() - t)
for n in names:
    print("%-9s min %6.1f ms" % (n, min(times[n]) * 1000))
print("one lock / sharded: %.1fx" % (min(times["one_lock"]) / min(times["sharded"])))
