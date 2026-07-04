// dispatch — genuine dynamic dispatch over a heap-owned heterogeneous Vec<Box<dyn Shape>>,
// built at runtime so the concrete type is not knowable at the call site (no devirtualization).
trait Shape { fn area(&self)->i64; }
struct Circle{r:i64}
struct Square{s:i64}
impl Shape for Circle { fn area(&self)->i64{ self.r*self.r } }
impl Shape for Square { fn area(&self)->i64{ self.s*self.s } }
fn main(){
  const N:usize=512;
  let mut shapes:Vec<Box<dyn Shape>>=Vec::with_capacity(N);
  for j in 0..N { if j%2==0 { shapes.push(Box::new(Circle{r:3})) } else { shapes.push(Box::new(Square{s:4})) } }
  let mut sum=0u64;
  for i in 0u64..8000000 { sum+=shapes[(i as usize)%N].area() as u64 }
  std::process::exit((sum%256) as i32);
}
