// Equal-workload kernel — see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
// fixed prealloc, no stdlib map (Rust's `HashMap` is measured in the `map` row instead).
struct Map { keys: Vec<i32>, vals: Vec<i64>, used: Vec<u8>, cap: i32 }

impl Map {
    fn new(cap: i32) -> Map {
        Map { keys: vec![0; cap as usize], vals: vec![0; cap as usize], used: vec![0; cap as usize], cap }
    }
    #[inline]
    fn slot_for(&self, k: i32) -> i32 {
        let x = (k as u32 as u64).wrapping_mul(2654435761);           // hash
        (x % self.cap as u64) as i32
    }
    fn put(&mut self, k: i32, v: i64) {
        let mut i = self.slot_for(k);
        while self.used[i as usize] != 0 {
            if self.keys[i as usize] == k { self.vals[i as usize] = v; return; }
            i += 1; if i >= self.cap { i = 0; }
        }
        self.keys[i as usize] = k; self.vals[i as usize] = v; self.used[i as usize] = 1;
    }
    fn get(&self, k: i32) -> i64 {
        let mut i = self.slot_for(k);
        while self.used[i as usize] != 0 {
            if self.keys[i as usize] == k { return self.vals[i as usize]; }
            i += 1; if i >= self.cap { i = 0; }
        }
        0
    }
}

fn main() {
    const N: i64 = 100000;
    const PASSES: i64 = 10;
    let mut m = Map::new(262144);                                     // > N / 0.75, power of 2
    for i in 0..N { m.put(i as i32, i * 2); }
    let mut sum: u64 = 0;
    for _ in 0..PASSES {
        for i in 0..N {
            let k = ((i * 2654435761i64) % N) as i32;
            sum = sum.wrapping_add(m.get(k) as u64);
        }
    }
    std::process::exit((sum % 256) as i32);
}
