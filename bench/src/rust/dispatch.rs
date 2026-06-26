trait Shape { fn area(&self)->i64; }
struct Circle{r:i64}
struct Square{s:i64}
impl Shape for Circle { fn area(&self)->i64{ self.r*self.r } }
impl Shape for Square { fn area(&self)->i64{ self.s*self.s } }
fn measure(sh:&dyn Shape)->i64{ sh.area() }
fn main(){
  let c=Circle{r:3}; let q=Square{s:4}; let mut sum=0u64;
  for i in 0u64..8000000 { if i%2==0 { sum+=measure(&c) as u64 } else { sum+=measure(&q) as u64 } }
  std::process::exit((sum%256) as i32);
}
