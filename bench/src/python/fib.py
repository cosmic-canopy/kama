import sys
def fib(n): return n if n<2 else fib(n-1)+fib(n-2)
s=0
for i in range(32): s+=fib(i)
sys.exit(s % 256)
