import sys
# dispatch — dynamic dispatch over a heterogeneous list of shapes built at runtime.
class Shape:
    def area(self): return 0
class Circle(Shape):
    def __init__(self,r): self.r=r
    def area(self): return self.r*self.r
class Square(Shape):
    def __init__(self,s): self.s=s
    def area(self): return self.s*self.s
N=512
shapes=[Circle(3) if j%2==0 else Square(4) for j in range(N)]
total=0
for i in range(8000000):
    total+=shapes[i%N].area()
sys.exit(total % 256)
