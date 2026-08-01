# Equal-workload kernel -- see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
# fixed prealloc, no stdlib dict (Python's `dict` is measured in the `map` row instead).
import sys

CAP = 262144          # > N / 0.75, power of 2
N, PASSES = 100000, 10

keys = [0] * CAP
vals = [0] * CAP
used = bytearray(CAP)


def slot_for(k):
    return ((k & 0xFFFFFFFF) * 2654435761) % CAP    # hash


def put(k, v):
    i = slot_for(k)
    while used[i]:
        if keys[i] == k:
            vals[i] = v
            return
        i += 1
        if i >= CAP:
            i = 0
    keys[i] = k
    vals[i] = v
    used[i] = 1


def get(k):
    i = slot_for(k)
    while used[i]:
        if keys[i] == k:
            return vals[i]
        i += 1
        if i >= CAP:
            i = 0
    return 0


for i in range(N):
    put(i, i * 2)
s = 0
for _ in range(PASSES):
    for i in range(N):
        s += get((i * 2654435761) % N)
sys.exit(s % 256)
