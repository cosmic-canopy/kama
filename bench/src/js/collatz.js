function clen(n){ let s=0; while(n!==1){ if(n%2===0) n=Math.floor(n/2); else n=3*n+1; s++; } return s; }
let s=0; for(let i=1;i<700000;i++) s+=clen(i);
process.exit(s % 256);
