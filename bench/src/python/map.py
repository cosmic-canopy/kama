N = 100000
PASSES = 10
m = {}
for i in range(N):
    m[i] = i * 2
s = 0
for _ in range(PASSES):
    for i in range(N):
        k = (i * 2654435761) % N
        s += m.get(k, 0)
import sys
sys.exit(s % 256)
