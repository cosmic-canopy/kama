// dispatch — dynamic dispatch over a heterogeneous array of shapes built at runtime.
class Shape { area(){ return 0; } }
class Circle extends Shape { constructor(r){ super(); this.r=r; } area(){ return this.r*this.r; } }
class Square extends Shape { constructor(s){ super(); this.s=s; } area(){ return this.s*this.s; } }
const N=512;
const shapes=[];
for(let j=0;j<N;j++){ shapes.push(j%2===0 ? new Circle(3) : new Square(4)); }
let sum=0;
for(let i=0;i<8000000;i++){ sum+=shapes[i%N].area(); }
process.exit(sum % 256);
