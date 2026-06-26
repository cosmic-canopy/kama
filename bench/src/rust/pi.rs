fn main(){
  let mut pi=0.0f64; let mut sign=1.0f64;
  for i in 0u64..20000000 { let d=2*i+1; pi += sign/(d as f64); sign=-sign; }
  let scaled=pi*4000000000.0; let check=scaled as u64; std::process::exit((check%256) as i32);
}
