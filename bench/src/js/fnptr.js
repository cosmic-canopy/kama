function add1(x){ return x+1; }
function mul3(x){ return x*3; }
function apply(op, x){ return op(x); }
const a=add1, b=mul3;
let sum=0;
for(let i=0;i<8000000;i++){ if(i%2===0) sum+=apply(a,i); else sum+=apply(b,i); }
process.exit(sum % 256);
