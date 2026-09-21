use std::collections::HashMap;
fn main() {
    const N: i64 = 100000;
    const PASSES: i64 = 10;
    let mut m: HashMap<i32, i64> = HashMap::with_capacity((N * 2) as usize);   // pre-sized
    for i in 0..N { m.insert(i as i32, i * 2); }
    let mut sum: u64 = 0;
    for _ in 0..PASSES {
        for i in 0..N {
            let k = ((i * 2654435761) % N) as i32;   // bijection over 0..N-1
            sum += *m.get(&k).unwrap_or(&0) as u64;
        }
    }
    std::process::exit((sum % 256) as i32);
}
