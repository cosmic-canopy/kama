import sys
total = 0
for _ in range(2000):
    xs = []
    for j in range(1, 1001):
        xs.append(j)
    s = 0
    for v in xs:
        s += v
    total += s
sys.exit(total % 256)
