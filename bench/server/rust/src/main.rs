// main.rs — the tokio arm of KR-243.
//
// Usage: server <echo|http> <port> <multi|per-core> <threads>
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

async fn accept_loop(listener: TcpListener, http_mode: bool) {
    loop {
        if let Ok((s, _)) = listener.accept().await {
            if http_mode {
                tokio::spawn(http(s));
            } else {
                tokio::spawn(echo(s));
            }
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
        eprintln!("usage: server <echo|http> <port> <multi|per-core> <threads>");
        std::process::exit(64);
    }
    let http_mode = match args[1].as_str() {
        "echo" => false,
        "http" => true,
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
                accept_loop(l, http_mode).await
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
                        accept_loop(l, http_mode).await
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
