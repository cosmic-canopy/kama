// The Rayon arm of KR-3's pool benchmark. The shapes and the usage are jobs.kama's and pool.c's. BENCH_RAYON picks how the
// work reaches the pool, since Rayon's own cost depends on it:
//   outside  `install` from a thread that is not a worker, as a frame loop on the main thread is: the caller only waits
//   inside   the whole loop already on a worker, so the caller takes work too: Rayon's best case
//   scope    `scope` with one `spawn` per chunk, on a worker: tasks rather than splitting, what `spawn job` would be
use rayon::prelude::*;
use std::time::Instant;

fn setting(name: &str, dflt: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(dflt)
}

fn each(c: &mut [u32], rounds: u32) {
    for e in c.iter_mut() {
        let mut x = e.wrapping_add(1);
        for _ in 0..rounds {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        *e = x;
    }
}

fn fib_seq(n: i32) -> i64 {
    if n < 2 { n as i64 } else { fib_seq(n - 1) + fib_seq(n - 2) }
}
fn fib(n: i32, leaf: i32) -> i64 {
    if n < leaf {
        return fib_seq(n);
    }
    let (a, b) = rayon::join(|| fib(n - 1, leaf), || fib(n - 2, leaf));
    a + b
}

struct Shape {
    frame: bool,
    workers: usize,
    calls: usize,
    slot: u128,
    how: String,
    rounds: u32,
}

fn barrier(pool: &rayon::ThreadPool, how: &str, xs: &mut [u32], k: usize, rounds: u32) {
    let k = k.min(xs.len());
    let chunk = xs.len().div_ceil(k);
    match how {
        "outside" => pool.install(|| xs.par_chunks_mut(chunk).for_each(|c| each(c, rounds))),
        "scope" => rayon::scope(|s| {
            for c in xs.chunks_mut(chunk) {
                s.spawn(move |_| each(c, rounds));
            }
        }),
        _ => xs.par_chunks_mut(chunk).for_each(|c| each(c, rounds)),
    }
}

fn run(pool: &rayon::ThreadPool, s: &Shape, xs: &mut [u32], t: &mut Vec<i64>) -> i64 {
    let mut total = 0i64;
    let start = Instant::now();
    let mut next = 0u128;
    for _ in 0..s.calls {
        if s.frame {
            next += s.slot;
            while start.elapsed().as_nanos() < next {
                std::hint::spin_loop();
            }
        }
        let t0 = Instant::now();
        barrier(pool, &s.how, xs, s.workers, s.rounds);
        let ns = t0.elapsed().as_nanos() as i64;
        t.push(ns);
        total += ns;
    }
    total
}

fn main() {
    let shape = std::env::var("BENCH_SHAPE").unwrap_or_else(|_| "tight".into());
    let workers = setting("BENCH_WORKERS", 4) as usize;
    let threads = setting("BENCH_POOL", workers as u64) as usize;
    let calls = setting("BENCH_CALLS", 2000) as usize;
    let elems = setting("BENCH_ELEMS", 1024) as usize;
    let how = std::env::var("BENCH_RAYON").unwrap_or_else(|_| "outside".into());
    let pool = rayon::ThreadPoolBuilder::new().num_threads(threads).stack_size(8 << 20).build().unwrap();

    let mut t: Vec<i64> = Vec::with_capacity(calls);
    let mut check = 0i64;
    let total;
    if shape == "nested" {
        let t0 = Instant::now();
        for _ in 0..calls {
            let c0 = Instant::now();
            check += pool.install(|| fib(elems as i32, setting("BENCH_LEAF", 10) as i32));
            t.push(c0.elapsed().as_nanos() as i64);
        }
        total = t0.elapsed().as_nanos() as i64;
    } else {
        let mut xs = vec![0u32; elems];
        let s = Shape {
            frame: shape == "frame",
            workers,
            calls,
            slot: setting("BENCH_SLOT_US", 1389) as u128 * 1000,
            how: how.clone(),
            rounds: setting("BENCH_ROUNDS", 0) as u32,
        };
        total = if how == "outside" {
            run(&pool, &s, &mut xs, &mut t)
        } else {
            pool.install(|| run(&pool, &s, &mut xs, &mut t))
        };
        check = xs.iter().map(|x| *x as i64).sum();
    }
    t.sort_unstable();
    println!(
        "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
        shape, workers, calls, elems, total, t[calls / 2], t[calls - 1 - calls / 100], check
    );
}
