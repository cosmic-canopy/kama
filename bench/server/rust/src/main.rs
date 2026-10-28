// main.rs — the tokio arm of KR-243.
//
// Usage: server <echo|http|http-raw|skew> <port> <multi|per-core> <threads>
//   http      hyper, the full stack
//   http-raw  tokio's TCP and the C and kama arms' own minimal parse: find where each request ends, write a fixed reply
//   skew      each 64-byte request answered after the rounds of mixing it asks for (client.c says how)
//   multi     tokio's default multi-thread runtime with <threads> workers, which steal work from each other
//   per-core  <threads> single-thread runtimes, each with its own listener on the port (SO_REUSEPORT, Linux) — the
//             thread-per-core shape closest to kama's fibers. Off Linux, they share one listener.
use std::convert::Infallible;
use std::net::SocketAddr;

use bytes::Bytes;
use http_body_util::Full;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Request, Response};
use hyper_util::rt::TokioIo;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

async fn echo(mut s: TcpStream) {
    let _ = s.set_nodelay(true);
    let mut buf = [0u8; 4096];
    loop {
        match s.read(&mut buf).await {
            Ok(0) | Err(_) => return,
            Ok(n) => {
                if s.write_all(&buf[..n]).await.is_err() {
                    return;
                }
            }
        }
    }
}

const REPLY: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, World!";

// Answers each complete request (`\r\n\r\n` ends one), however the reads split it, until the client closes.
async fn http_raw(mut s: TcpStream) {
    let _ = s.set_nodelay(true);
    let mut buf = [0u8; 4096];
    let mut state = 0; // how much of "\r\n\r\n" the bytes so far end with
    loop {
        let n = match s.read(&mut buf).await {
            Ok(0) | Err(_) => return,
            Ok(n) => n,
        };
        for &b in &buf[..n] {
            state = match b {
                b'\r' => if state == 2 { 3 } else { 1 },
                b'\n' if state == 1 || state == 3 => state + 1,
                _ => 0,
            };
            if state == 4 {
                state = 0;
                if s.write_all(REPLY).await.is_err() {
                    return;
                }
            }
        }
    }
}

// The skew burst, the same in every arm: `rounds` passes of FNV-1a over the request's 64 bytes.
fn mix(b: &[u8; 64], rounds: u32) -> u32 {
    let mut h: u32 = 2166136261;
    for _ in 0..rounds {
        for &x in b {
            h = (h ^ x as u32).wrapping_mul(16777619);
        }
    }
    h
}

async fn skew(mut s: TcpStream) {
    let _ = s.set_nodelay(true);
    let mut buf = [0u8; 64];
    loop {
        if s.read_exact(&mut buf).await.is_err() {
            return;
        }
        let h = mix(&buf, u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]));
        buf[4..8].copy_from_slice(&h.to_le_bytes());
        if s.write_all(&buf).await.is_err() {
            return;
        }
    }
}

async fn hello(_: Request<hyper::body::Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    Ok(Response::builder()
        .header("Content-Type", "text/plain")
        .header("Content-Length", "13")
        .body(Full::new(Bytes::from_static(b"Hello, World!")))
        .unwrap())
}

async fn http(s: TcpStream) {
    let _ = s.set_nodelay(true);
    let _ = http1::Builder::new()
        .keep_alive(true)
        .serve_connection(TokioIo::new(s), service_fn(hello))
        .await;
}

#[derive(Clone, Copy)]
enum Mode {
    Echo,
    Http,
    HttpRaw,
    Skew,
}

async fn accept_loop(listener: TcpListener, mode: Mode) {
    loop {
        if let Ok((s, _)) = listener.accept().await {
            match mode {
                Mode::Echo => tokio::spawn(echo(s)),
                Mode::Http => tokio::spawn(http(s)),
                Mode::HttpRaw => tokio::spawn(http_raw(s)),
                Mode::Skew => tokio::spawn(skew(s)),
            };
        }
    }
}

fn std_listener(addr: SocketAddr, reuse_port: bool) -> std::net::TcpListener {
    let sock = socket2::Socket::new(socket2::Domain::IPV4, socket2::Type::STREAM, None).unwrap();
    sock.set_reuse_address(true).unwrap();
    #[cfg(target_os = "linux")]
    if reuse_port {
        sock.set_reuse_port(true).unwrap();
    }
    #[cfg(not(target_os = "linux"))]
    let _ = reuse_port;
    sock.bind(&addr.into()).unwrap();
    sock.listen(4096).unwrap();
    sock.set_nonblocking(true).unwrap();
    sock.into()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 5 {
        eprintln!("usage: server <echo|http|http-raw|skew> <port> <multi|per-core> <threads>");
        std::process::exit(64);
    }
    let mode = match args[1].as_str() {
        "echo" => Mode::Echo,
        "http" => Mode::Http,
        "http-raw" => Mode::HttpRaw,
        "skew" => Mode::Skew,
        m => {
            eprintln!("server: unknown mode '{m}'");
            std::process::exit(64)
        }
    };
    let addr: SocketAddr = format!("127.0.0.1:{}", args[2]).parse().unwrap();
    let threads: usize = args[4].parse().unwrap();
    match args[3].as_str() {
        "multi" => {
            let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(threads).enable_all().build().unwrap();
            rt.block_on(async move {
                let l = TcpListener::from_std(std_listener(addr, false)).unwrap();
                accept_loop(l, mode).await
            });
        }
        "per-core" => {
            let per_thread = cfg!(target_os = "linux");
            let shared = if per_thread { None } else { Some(std_listener(addr, false)) };
            let mut joins = Vec::new();
            for _ in 0..threads {
                let mine = match &shared {
                    Some(l) => l.try_clone().unwrap(),
                    None => std_listener(addr, true),
                };
                joins.push(std::thread::spawn(move || {
                    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
                    rt.block_on(async move {
                        let l = TcpListener::from_std(mine).unwrap();
                        accept_loop(l, mode).await
                    });
                }));
            }
            for j in joins {
                let _ = j.join();
            }
        }
        r => {
            eprintln!("server: unknown runtime '{r}'");
            std::process::exit(64)
        }
    }
}
