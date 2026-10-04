// TS: the JS program, typed. JS has no growable byte array: a Uint8Array doubled by hand, filled with `set`.
function chunk(k: number): number { switch (k % 5) { case 0: return 7; case 1: return 64; case 2: return 1000; case 3: return 4096; default: return 65536; } }
const SRC = 65536, TARGET = 4194304;
const src = new Uint8Array(SRC);
for (let i = 0; i < SRC; i++) src[i] = (i * 7 + (i >> 8)) & 0xff;
const dst = new Uint8Array(TARGET);
let total = 0;
for (let r = 0; r < 64; r++) {
  let buf = new Uint8Array(0), len = 0;
  let k = r, off = r * 13;
  while (len < TARGET) {
    let c = chunk(k++);
    if (c > TARGET - len) c = TARGET - len;
    off = (off + 4099) % (SRC - c + 1);
    if (len + c > buf.length) {
      let nc = buf.length ? buf.length * 2 : 4;
      while (nc < len + c) nc *= 2;
      const nb = new Uint8Array(nc); nb.set(buf.subarray(0, len)); buf = nb;
    }
    buf.set(src.subarray(off, off + c), len); len += c;
  }
  dst.set(buf.subarray(0, len));
  let s = 0; for (let i = r; i < TARGET; i += 4093) s += dst[i];
  total += s;
}
process.exit(total % 256);
export {};
