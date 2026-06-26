import sys
pi=0.0; sign=1.0
for i in range(20000000):
    d=2*i+1
    pi += sign/d
    sign=-sign
scaled=pi*4000000000.0
sys.exit(int(scaled) % 256)
