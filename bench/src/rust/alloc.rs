fn main(){
    let mut total:u64 = 0;
    for _ in 0..2000 {
        let mut xs:Vec<i32> = Vec::new();
        for j in 1..=1000i32 { xs.push(j); }
        let mut s:u64 = 0; for v in &xs { s += *v as u64; }
        total += s;
    }
    std::process::exit((total % 256) as i32);
}
