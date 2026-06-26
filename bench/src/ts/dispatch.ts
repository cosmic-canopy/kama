abstract class Shape { abstract area(): number; }
class Circle extends Shape { constructor(private r: number){ super(); } area(){ return this.r*this.r; } }
class Square extends Shape { constructor(private s: number){ super(); } area(){ return this.s*this.s; } }
function measure(sh: Shape): number { return sh.area(); }
const c=new Circle(3), q=new Square(4);
let sum=0;
for(let i=0;i<8000000;i++){ if(i%2===0) sum+=measure(c); else sum+=measure(q); }
process.exit(sum % 256);

export {};
