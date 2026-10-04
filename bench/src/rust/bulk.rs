fn chunk(k: usize) -> usize { match k % 5 { 0 => 7, 1 => 64, 2 => 1000, 3 => 4096, _ => 65536 } }
fn main(){
    const SRC: usize = 65536; const TARGET: usize = 4194304;
    let src: Vec<u8> = (0..SRC).map(|i| (i * 7 + (i >> 8)) as u8).collect();
    let mut dst = vec![0u8; TARGET];
    let mut total: u64 = 0;
    for r in 0..64usize {
        let mut buf: Vec<u8> = Vec::new();
        let mut k = r; let mut off = r * 13;
        while buf.len() < TARGET {
            let mut c = chunk(k); k += 1;
            if c > TARGET - buf.len() { c = TARGET - buf.len(); }
            off = (off + 4099) % (SRC - c + 1);
            buf.extend_from_slice(&src[off..off + c]);
        }
        dst.copy_from_slice(&buf);
        let mut s: u64 = 0; let mut i = r; while i < TARGET { s += dst[i] as u64; i += 4093; }
        total += s;
    }
    std::process::exit((total % 256) as i32);
}
