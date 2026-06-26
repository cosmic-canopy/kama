let pi=0.0, sign=1.0;
for(let i=0;i<20000000;i++){ const d=2*i+1; pi += sign/d; sign=-sign; }
const scaled=pi*4000000000.0;
process.exit(Math.floor(scaled) % 256);
