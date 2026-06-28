type Op = fn(i64)->i64;
fn add1(x:i64)->i64{ x+1 }
fn mul3(x:i64)->i64{ x*3 }
fn apply(op:Op, x:i64)->i64{ op(x) }
fn main(){
  let a:Op=add1; let b:Op=mul3; let mut sum=0u64;
  for i in 0u64..8000000 { if i%2==0 { sum+=apply(a,i as i64) as u64 } else { sum+=apply(b,i as i64) as u64 } }
  std::process::exit((sum%256) as i32);
}
