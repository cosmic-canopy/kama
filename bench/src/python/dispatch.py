import sys
class Shape:
    def area(self): return 0
class Circle(Shape):
    def __init__(self,r): self.r=r
    def area(self): return self.r*self.r
class Square(Shape):
    def __init__(self,s): self.s=s
    def area(self): return self.s*self.s
def measure(sh): return sh.area()
c=Circle(3); q=Square(4)
total=0
for i in range(8000000):
    if i%2==0: total+=measure(c)
    else: total+=measure(q)
sys.exit(total % 256)
