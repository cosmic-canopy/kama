class Shape { area(){ return 0; } }
class Circle extends Shape { constructor(r){ super(); this.r=r; } area(){ return this.r*this.r; } }
class Square extends Shape { constructor(s){ super(); this.s=s; } area(){ return this.s*this.s; } }
function measure(sh){ return sh.area(); }
const c=new Circle(3), q=new Square(4);
let sum=0;
for(let i=0;i<8000000;i++){ if(i%2===0) sum+=measure(c); else sum+=measure(q); }
process.exit(sum % 256);
