import sys
def add1(x): return x+1
def mul3(x): return x*3
def apply(op, x): return op(x)
a, b = add1, mul3
total=0
for i in range(8000000):
    if i%2==0: total+=apply(a,i)
    else: total+=apply(b,i)
sys.exit(total % 256)
