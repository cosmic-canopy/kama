// The tokio arm of KR-264's pipe benchmark. The workloads and the usage are pipe.kama's. BENCH_TOKIO picks the shape:
//   server   the I/O driver on, every side a spawned task: what a server's tasks do
//   nodriver no I/O driver, every side a spawned task: tokio's best case, a runtime with nothing but tasks
//   test     the I/O driver on, one side in `block_on`, as the body of `#[tokio::test]` is
use std::time::Instant;
use tokio::io::{duplex, AsyncReadExt, AsyncWriteExt, DuplexStream};

fn setting(name: &str, dflt: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(dflt)
}

async fn ops(count: u64, bytes: usize, capacity: usize) -> u128 {
    let (mut a, mut b) = duplex(capacity);
    let mut buf = vec![0u8; bytes];
    let t0 = Instant::now();
    for _ in 0..count {
        a.write_all(&buf).await.unwrap();
        b.read_exact(&mut buf).await.unwrap();
        b.write_all(&buf).await.unwrap();
        a.read_exact(&mut buf).await.unwrap();
    }
    t0.elapsed().as_nanos()
}

async fn echo(mut b: DuplexStream, count: u64, bytes: usize) {
    let mut buf = vec![0u8; bytes];
    for _ in 0..count {
        b.read_exact(&mut buf).await.unwrap();
        b.write_all(&buf).await.unwrap();
    }
}

async fn ping(mut a: DuplexStream, count: u64, bytes: usize) -> u128 {
    let mut buf = vec![0u8; bytes];
    let t0 = Instant::now();
    for _ in 0..count {
        a.write_all(&buf).await.unwrap();
        a.read_exact(&mut buf).await.unwrap();
    }
    t0.elapsed().as_nanos()
}

async fn pingpong(count: u64, bytes: usize, capacity: usize, spawn_both: bool) -> u128 {
    let (a, b) = duplex(capacity);
    let peer = tokio::spawn(echo(b, count, bytes));
    let ns = if spawn_both { tokio::spawn(ping(a, count, bytes)).await.unwrap() } else { ping(a, count, bytes).await };
    peer.await.unwrap();
    ns
}

async fn drain(mut b: DuplexStream, bytes: usize) {
    let mut into = vec![0u8; bytes];
    while b.read(&mut into).await.unwrap() != 0 {}
}

async fn pour(mut a: DuplexStream, count: u64, bytes: usize) {
    let buf = vec![0u8; bytes];
    for _ in 0..count {
        a.write_all(&buf).await.unwrap();
    }
}

async fn bulk(count: u64, bytes: usize, capacity: usize, spawn_both: bool) -> u128 {
    let (a, b) = duplex(capacity);
    let t0 = Instant::now();
    let peer = tokio::spawn(drain(b, bytes));
    if spawn_both { tokio::spawn(pour(a, count, bytes)).await.unwrap() } else { pour(a, count, bytes).await }
    peer.await.unwrap();
    t0.elapsed().as_nanos()
}

fn main() {
    let workload = std::env::var("BENCH_WORKLOAD").unwrap_or_else(|_| "ops".to_string());
    let count = setting("BENCH_COUNT", 1_000_000);
    let bytes = setting("BENCH_BYTES", 64) as usize;
    let capacity = setting("BENCH_CAPACITY", 8192) as usize;
    let shape = std::env::var("BENCH_TOKIO").unwrap_or_else(|_| "server".to_string());
    let mut builder = tokio::runtime::Builder::new_current_thread();
    if shape != "nodriver" {
        builder.enable_all();
    }
    let rt = builder.build().unwrap();
    let spawn_both = shape != "test";
    let ns = rt.block_on(async {
        match workload.as_str() {
            "ops" => ops(count, bytes, capacity).await,
            "pingpong" => pingpong(count, bytes, capacity, spawn_both).await,
            "bulk" => bulk(count, bytes, capacity, spawn_both).await,
            _ => panic!("pipe: BENCH_WORKLOAD is ops, pingpong or bulk"),
        }
    });
    println!("{workload}\t{count}\t{bytes}\t{capacity}\t{ns}");
}
