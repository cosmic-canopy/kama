// dispatch — dynamic dispatch over a heterogeneous array of shapes built at runtime.
abstract class Shape { abstract area(): number; }
class Circle extends Shape { constructor(private r: number){ super(); } area(){ return this.r*this.r; } }
class Square extends Shape { constructor(private s: number){ super(); } area(){ return this.s*this.s; } }
const N=512;
const shapes: Shape[]=[];
for(let j=0;j<N;j++){ shapes.push(j%2===0 ? new Circle(3) : new Square(4)); }
let sum=0;
for(let i=0;i<8000000;i++){ sum+=shapes[i%N].area(); }
process.exit(sum % 256);

export {};
