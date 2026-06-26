fn fib(n:u64)->u64{ if n<2 {n} else {fib(n-1)+fib(n-2)} }
fn main(){ let mut s:u64=0; for i in 0..32u64 { s+=fib(i); } std::process::exit((s%256) as i32); }
