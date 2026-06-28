type Op = (x: number) => number;
function add1(x: number): number { return x+1; }
function mul3(x: number): number { return x*3; }
function apply(op: Op, x: number): number { return op(x); }
const a: Op = add1, b: Op = mul3;
let sum=0;
for(let i=0;i<8000000;i++){ if(i%2===0) sum+=apply(a,i); else sum+=apply(b,i); }
process.exit(sum % 256);

export {};
