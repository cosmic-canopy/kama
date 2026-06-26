import sys
def clen(n):
    s=0
    while n!=1:
        if n%2==0: n=n//2
        else: n=3*n+1
        s+=1
    return s
s=0
for i in range(1,700000): s+=clen(i)
sys.exit(s % 256)
