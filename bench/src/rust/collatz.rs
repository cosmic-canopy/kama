fn clen(mut n:u64)->u64{ let mut s=0u64; while n!=1 { if n%2==0 {n=n/2} else {n=3*n+1} s+=1; } s }
fn main(){ let mut s=0u64; for i in 1..700000u64 { s+=clen(i); } std::process::exit((s%256) as i32); }
