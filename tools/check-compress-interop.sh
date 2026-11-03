#!/bin/sh
# check-compress-interop.sh — std::compress and std::archive::tar against another implementation, in both directions.
#
# A fixture pins what a stream was. This asks whether someone else agrees: at every level, over edge corpora, python3's zlib and gzip read what
# `ZlibWriter` and `GzipWriter` wrote and kama reads what they wrote, members one at a time and all together, and through a source that stalls
# before every byte. `tarfile` writes archives in its three formats, and `TarReader` lists them field by field as it does; `tarfile` reads what
# `TarWriter` wrote. Then `extractInto`, by trying to break it: adversarial archives from a small grammar of `..`, links, hard links and case
# variants, each extracted for real, and the filesystem asked directly: nothing outside the destination changed, every link left behind resolves
# inside it through any chain, and no mode keeps a bit that was to be dropped.
#
# The second half skips Windows, where links want a privilege and `realpath` is another function. All of it skips where python3 is absent (msys2 ships
# without it) and says so. `check-packages.sh` holds the other half, which needs no python: publish's own tarball rewritten through `Frozen`.
#
# ⚠️ A loop among links is not an escape. `os.path.realpath` gives up on one and joins what is left, and a `//` in that reads as absolute: `c/a ->
# a//up/a` "resolved" to `/up/a`, seven false alarms in 3,000 archives. The OS answers ELOOP, so it is asked first.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-compress-interop: $KAMA not built" >&2; exit 1; fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-compress-interop: OK — python3 not found, so nothing to compare against here (msys2 ships without it)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/iop.kama" <<'EOF'
import { core::println, std::compress::zlib::ZlibReader, std::compress::zlib::ZlibWriter, std::compress::gzip::GzipReader,
         std::compress::gzip::GzipWriter, std::compress::gzip::GzipMembers, std::compress::deflate::DeflateLevel, std::archive::tar::TarReader,
         std::archive::tar::TarWriter, std::archive::tar::TarHeader, std::archive::tar::TarKind, std::archive::tar::TarOwner,
         std::archive::tar::TarEndMarker, std::archive::tar::extractInto, std::archive::tar::TarExtractError, std::collections::DynamicArray,
         std::collections::FixedArray, std::collections::ConstView, std::collections::View, std::fs::readFile, std::fs::readText, std::fs::writeFile,
         std::io::Writer, std::io::Reader, std::io::BufReader, std::io::SliceReader, std::io::IoError, std::io::IoErrorKind, std::encoding::hex::encode,
         std::digest::crc32::Crc32, std::time::Timestamp };

// `every` > 0: all but every `every`-th call answers `WouldBlock`, and the rest take at most 777 bytes.
type resource Sink implements Writer {
    DynamicArray<uint8> got;
    int32 every;
    int32 calls;
    public ctor make(int32 every) { this.got = DynamicArray.empty(); this.every = every; this.calls = 0; }
    public fn Result<isize, IoError> write(ConstView<uint8> bytes) {
        this.calls = this.calls + 1;
        if (this.every > 0 && this.calls % this.every != 0) { return Result::Err(error: IoError.of(kind: IoErrorKind::WouldBlock)); }
        isize n = this.every > 0 && bytes.length() > 777 ? 777 : bytes.length();
        for (isize k = 0; k < n; k = k + 1) { this.got.add(item: bytes[k]); }
        return Result::Ok(value: n);
    }
    public fn Result<Unit, IoError> flush() { return Result::Ok(value: Unit::Unit); }
    public give fn DynamicArray<uint8> bytes() { return give this.got; }
}

// One byte a read, with a `WouldBlock` before each.
type resource Choppy implements Reader {
    DynamicArray<uint8> bytes;
    isize pos;
    bool ready;
    public ctor make(DynamicArray<uint8> bytes) { this.bytes = give bytes; this.pos = 0; this.ready = false; }
    public fn Result<isize, IoError> read(View<uint8> into) {
        if (!this.ready) { this.ready = true; return Result::Err(error: IoError.of(kind: IoErrorKind::WouldBlock)); }
        this.ready = false;
        if (this.pos >= this.bytes.length() || into.length() == 0) { return Result::Ok(value: 0); }
        into[0] = this.bytes[this.pos];
        this.pos = this.pos + 1;
        return Result::Ok(value: 1);
    }
}

fn DeflateLevel levelOf(int32 k) {
    InlineArray<DeflateLevel>#(11) all = [DeflateLevel::Store, DeflateLevel::Level1, DeflateLevel::Level2, DeflateLevel::Level3, DeflateLevel::Level4,
                                          DeflateLevel::Level5, DeflateLevel::Level6, DeflateLevel::Level7, DeflateLevel::Level8, DeflateLevel::Level9,
                                          DeflateLevel::Frozen];
    return all[k];
}

fn void save(const ref string path, const ref DynamicArray<uint8> b) {
    match (writeFile(path: path, bytes: b)) { case Ok(value: w): { } case Err(error: e): { panic(msg: "save"); } };
}

fn DynamicArray<uint8> load(const ref string path) {
    return match (readFile(path: path)) { case Ok(value: b): give b; case Err(error: e): DynamicArray.empty(); };
}

fn DynamicArray<uint8> zenc(const ref DynamicArray<uint8> in0, int32 level, isize a, isize b, int32 every) {
    ZlibWriter<Sink> w = ZlibWriter.make(sink: Sink.make(every: every), level: levelOf(k: level));
    isize at = 0;
    bool first = true;
    while (at < in0.length()) {
        isize want = first ? a : b;
        first = !first;
        isize n = in0.length() - at < want ? in0.length() - at : want;
        isize took = match (w.write(bytes: in0.view().slice(from: at, count: n))) { case Ok(value: m): m; case Err(error: e): 0; };
        at = at + took;
    }
    ZlibWriter<Sink> cur = give w;
    while (true) {
        match (give cur.finish()) {
            case Ok(value: s): { Sink done = give s; return give done.bytes(); }
            case Err(error: u): { match (give u) { case Pending(adapter: ad, error: e): { cur = give ad; } }; }
        };
    }
}

fn DynamicArray<uint8> genc(const ref DynamicArray<uint8> in0, int32 level, isize a, isize b, int32 every) {
    GzipWriter<Sink> w = GzipWriter.make(sink: Sink.make(every: every), level: levelOf(k: level));
    isize at = 0;
    bool first = true;
    while (at < in0.length()) {
        isize want = first ? a : b;
        first = !first;
        isize n = in0.length() - at < want ? in0.length() - at : want;
        isize took = match (w.write(bytes: in0.view().slice(from: at, count: n))) { case Ok(value: m): m; case Err(error: e): 0; };
        at = at + took;
    }
    GzipWriter<Sink> cur = give w;
    while (true) {
        match (give cur.finish()) {
            case Ok(value: s): { Sink done = give s; return give done.bytes(); }
            case Err(error: u): { match (give u) { case Pending(adapter: ad, error: e): { cur = give ad; } }; }
        };
    }
}

fn DynamicArray<uint8> zdec(const ref DynamicArray<uint8> s, ref int32 bad) {
    ZlibReader<SliceReader> z = ZlibReader.make(source: BufReader.make(inner: SliceReader.fromBytes(bytes: copy s)), limit: 1000000000ui64);
    DynamicArray<uint8> got = DynamicArray.empty();
    FixedArray<uint8> buf = FixedArray.make(size: 3333);
    while (true) {
        isize n = match (z.read(into: buf.viewMut())) { case Ok(value: k): k; case Err(error: e): -1; };
        if (n < 0) { bad = bad + 1; return got; }
        if (n == 0) { break; }
        for (isize i = 0; i < n; i = i + 1) { got.add(item: buf[i]); }
    }
    match (give z.finish()) { case Ok(value: b): { } case Err(error: u): { bad = bad + 1; } };
    return got;
}

fn DynamicArray<uint8> gdec(const ref DynamicArray<uint8> s, GzipMembers m, ref int32 bad) {
    GzipReader<SliceReader> g = GzipReader.make(source: BufReader.make(inner: SliceReader.fromBytes(bytes: copy s)), limit: 1000000000ui64, members: m);
    DynamicArray<uint8> got = DynamicArray.empty();
    FixedArray<uint8> buf = FixedArray.make(size: 3333);
    while (true) {
        isize n = match (g.read(into: buf.viewMut())) { case Ok(value: k): k; case Err(error: e): -1; };
        if (n < 0) { bad = bad + 1; return got; }
        if (n == 0) { break; }
        for (isize i = 0; i < n; i = i + 1) { got.add(item: buf[i]); }
    }
    match (give g.finish()) { case Ok(value: b): { } case Err(error: u): { bad = bad + 1; } };
    return got;
}

fn DynamicArray<uint8> gstall(const ref DynamicArray<uint8> s, ref int32 bad) {
    GzipReader<Choppy> g = GzipReader.make(source: BufReader.make(inner: Choppy.make(bytes: copy s)), limit: 1000000000ui64, members: GzipMembers::All);
    DynamicArray<uint8> got = DynamicArray.empty();
    FixedArray<uint8> buf = FixedArray.make(size: 777);
    while (true) {
        isize n = match (g.read(into: buf.viewMut())) {
            case Ok(value: k): k;
            case Err(error: e): match (e.kind()) { case WouldBlock: -2; case _: -1; };
        };
        if (n == -1) { bad = bad + 1; return got; }
        if (n == 0) { break; }
        if (n > 0) { for (isize i = 0; i < n; i = i + 1) { got.add(item: buf[i]); } }
    }
    match (give g.finish()) { case Ok(value: b): { } case Err(error: u): { bad = bad + 1; } };
    return got;
}

fn uint8 codec() {
    DynamicArray<uint8> in0 = load(path: "w.in");
    InlineArray<string>#(11) names = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "frozen"];
    for (int32 k = 0; k <= 10; k = k + 1) {
        save(path: "z." + names[k], b: zenc(in0: in0, level: k, a: 5000, b: 5000, every: 0));
        save(path: "g." + names[k], b: genc(in0: in0, level: k, a: 5000, b: 5000, every: 0));
    }
    save(path: "g.stall", b: genc(in0: in0, level: 10, a: 20000, b: 9000, every: 3));
    save(path: "z.stall", b: zenc(in0: in0, level: 10, a: 20000, b: 9000, every: 3));
    int32 bad = 0;
    save(path: "d.z", b: zdec(s: load(path: "p.z"), bad: ref bad));
    save(path: "d.g", b: gdec(s: load(path: "p.g"), m: GzipMembers::All, bad: ref bad));
    save(path: "d.g1", b: gdec(s: load(path: "p.g1"), m: GzipMembers::One, bad: ref bad));
    save(path: "d.gs", b: gstall(s: load(path: "p.g"), bad: ref bad));
    return bad == 0 ? 0 : 1;
}

fn string hexOf(const ref DynamicArray<uint8> b) { return encode(bytes: b.view()); }

fn string kindText(const ref TarHeader h) {
    return match (h.kind()) {
        case File(size: s): "file:${s}";
        case Directory: "dir";
        case Symlink(target: t): { string x = hexOf(b: t); := "sym:${x}"; }
        case HardLink(target: t): { string x = hexOf(b: t); := "hard:${x}"; }
        case CharDevice(major: a, minor: b): "chr:${a}:${b}";
        case BlockDevice(major: a, minor: b): "blk:${a}:${b}";
        case Fifo: "fifo";
        case Other(flag: f, size: s): "other:${f}:${s}";
        case _: "unknown";
    };
}

fn string ownerText(const ref TarHeader h) {
    return match (h.owner()) {
        case None: "none";
        case Named(uid: u, gid: g, uname: un, gname: gn): { string a = hexOf(b: un); string b = hexOf(b: gn); := "${u},${g},${a},${b}"; }
    };
}

fn string timeText(const ref TarHeader h) {
    Optional<Timestamp> t = h.mtime();
    return match (t) { case Some(value: x): { int64 n = x.asUnixNanos(); := "${n}"; } case None: "none"; };
}

// Lists tl.in to tl.out, a line an entry, every field and each body's CRC-32.
fn uint8 list() {
    TarReader<SliceReader> t = TarReader.make(source: SliceReader.fromBytes(bytes: load(path: "tl.in")), endMarker: TarEndMarker::NotRequired);
    string text = "";
    FixedArray<uint8> buf = FixedArray.make(size: 4096);
    while (true) {
        Optional<TarHeader> next = match (t.next()) { case Ok(value: o): give o; case Err(error: e): { return 1; } };
        bool done = false;
        match (give next) {
            case None: { done = true; }
            case Some(value: h): {
                Crc32 c = Crc32.make();
                isize len = 0;
                while (true) {
                    isize n = match (t.read(into: buf.viewMut())) { case Ok(value: k): k; case Err(error: e): -1; };
                    if (n < 0) { return 2; }
                    if (n == 0) { break; }
                    c.update(bytes: buf.view().slice(from: 0, count: n));
                    len = len + n;
                }
                string p = hexOf(b: h.path());
                string k = kindText(h: h);
                uint32 m = h.mode();
                string w = timeText(h: h);
                string o = ownerText(h: h);
                uint32 s = c.finish();
                text = text + "${p}|${k}|${m}|${w}|${o}|${len}|${s}\n";
            }
        };
        if (done) { break; }
    }
    DynamicArray<uint8> bytes = DynamicArray.empty();
    borrow text.bytes() as v { foreach (uint8 b in v) { bytes.add(item: b); } }
    save(path: "tl.out", b: bytes);
    return 0;
}

fn void entry(ref TarWriter<Sink> w, TarHeader h) {
    match (w.begin(header: h)) { case Ok(value: _): { } case Err(error: e): { panic(msg: "begin"); } };
}

fn DynamicArray<uint8> bytesOf(const ref string s) {
    DynamicArray<uint8> a = DynamicArray.empty();
    borrow s.bytes() as v { foreach (uint8 b in v) { a.add(item: b); } }
    return a;
}

// w.tar: every kind, and what no ustar field holds.
fn uint8 writes() {
    TarWriter<Sink> w = TarWriter.make(sink: Sink.make(every: 0));
    Timestamp t0 = Timestamp.fromUnixSecs(s: 315532800i64);
    entry(w: ref w, h: TarHeader.make(path: "file".bytes(), kind: TarKind::File(size: 5ui64), mode: 0o644, mtime: t0, owner: TarOwner::None));
    InlineArray<uint8>#(5) five = [104, 101, 108, 108, 111];
    match (w.write(bytes: five.view())) { case Ok(value: n): { } case Err(error: e): { return 1; } };
    entry(w: ref w, h: TarHeader.make(path: "dir/".bytes(), kind: TarKind::Directory, mode: 0o755, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "ln".bytes(), kind: TarKind::Symlink(target: bytesOf(s: "file")), mode: 0o777, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "hl".bytes(), kind: TarKind::HardLink(target: bytesOf(s: "file")), mode: 0o644, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "chr".bytes(), kind: TarKind::CharDevice(major: 1ui32, minor: 3ui32), mode: 0o644, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "blk".bytes(), kind: TarKind::BlockDevice(major: 8ui32, minor: 16ui32), mode: 0o644, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "fifo".bytes(), kind: TarKind::Fifo, mode: 0o644, mtime: t0, owner: TarOwner::None));
    DynamicArray<uint8> longName = DynamicArray.empty();
    for (isize k = 0; k < 150; k = k + 1) { longName.add(item: 110); }
    entry(w: ref w, h: TarHeader.make(path: longName.view(), kind: TarKind::Symlink(target: copy longName), mode: 0o777, mtime: t0, owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "owner".bytes(), kind: TarKind::Directory, mode: 0o4755, mtime: t0,
                                      owner: TarOwner::Named(uid: 3000000i64, gid: 7i64, uname: bytesOf(s: "alice"), gname: copy longName)));
    entry(w: ref w, h: TarHeader.make(path: "half".bytes(), kind: TarKind::Directory, mode: 0o755, mtime: Timestamp.fromUnixNanos(n: 1500000000i64), owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "old".bytes(), kind: TarKind::Directory, mode: 0o755, mtime: Timestamp.fromUnixSecs(s: -1i64), owner: TarOwner::None));
    entry(w: ref w, h: TarHeader.make(path: "far".bytes(), kind: TarKind::Directory, mode: 0o755, mtime: Timestamp.fromUnixSecs(s: 9000000000i64), owner: TarOwner::None));
    return match (give w.finish()) {
        case Ok(value: s): { Sink d = give s; DynamicArray<uint8> b = give d.bytes(); save(path: "w.tar", b: b); := 0; }
        case Err(error: u): 2;
    };
}

// x.tar under x.out. 0, or 1 and why.
fn uint8 extract() {
    TarReader<SliceReader> t = TarReader.make(source: SliceReader.fromBytes(bytes: load(path: "x.tar")), endMarker: TarEndMarker::NotRequired);
    Result<Unit, TarExtractError> r = extractInto(archive: ref t, dir: "x.out");
    return match (give r) {
        case Ok(value: _): 0;
        case Err(error: e): { string m = e.message(); println(s: give m); := 1; }
    };
}

fn uint8 main() {
    string mode = match (readText(path: "iop.mode")) { case Ok(value: t): give t; case Err(error: e): ""; };
    if (mode == "codec") { return codec(); }
    if (mode == "list") { return list(); }
    if (mode == "writes") { return writes(); }
    if (mode == "extract") { return extract(); }
    return 9;
}
EOF

cat > "$tmp/iop.py" <<'EOF'
import collections, errno, gzip, io, os, random, shutil, subprocess, struct, sys, tarfile, zlib
exe, unix = sys.argv[1], sys.argv[2] == 'unix'
random.seed(1234)
fails = []
def bad(*a):
    fails.append(' '.join(str(x) for x in a)[:200])
def run(mode):
    open('iop.mode', 'w').write(mode)
    return subprocess.run([exe], capture_output=True, text=True, timeout=300)

# ---- zlib and gzip, at every level
rnd = random.randbytes
words = ['alpha ', 'beta ', 'gamma ', 'delta\n', 'epsilon ', 'zeta ', 'eta ', 'theta\n']
text = ''.join(random.choice(words) for _ in range(20000)).encode()
far = rnd(600)
cases = {'empty': b'', 'one': b'x', 'short': b'the quick brown fox', 'text': text[:60000], 'random': rnd(40000), 'zeros': bytes(200000),
         'b16384': rnd(16384), 'r65536': rnd(65536), 'mix': text[:9000] + rnd(9000) + bytes(9000), 'far': far + bytes(32768 - 600) + far}
LV = ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'frozen']
FLG = {'0': 1, '1': 1, '2': 0x5E, '3': 0x5E, '4': 0x5E, '5': 0x5E, '6': 0x9C, '7': 0xDA, '8': 0xDA, '9': 0xDA, 'frozen': 0xDA}
for name, d in cases.items():
    open('w.in', 'wb').write(d)
    lvl = random.randrange(10)
    open('p.z', 'wb').write(zlib.compress(d, lvl))
    a, b = gzip.compress(d[:len(d) // 2], lvl, mtime=7), gzip.compress(d[len(d) // 2:], 9 - lvl, mtime=0)
    open('p.g', 'wb').write(a + b)
    open('p.g1', 'wb').write(a + b'TAIL')
    r = run('codec')
    if r.returncode != 0: bad(name, 'codec answered', r.returncode); continue
    if open('d.z', 'rb').read() != d: bad(name, 'ZlibReader over python3')
    if open('d.g', 'rb').read() != d: bad(name, 'GzipReader over two members')
    if open('d.gs', 'rb').read() != d: bad(name, 'GzipReader over a stalling source')
    if open('d.g1', 'rb').read() != d[:len(d) // 2]: bad(name, 'GzipMembers::One')
    if open('g.stall', 'rb').read() != open('g.frozen', 'rb').read(): bad(name, 'gzip differs through a stalling sink')
    if open('z.stall', 'rb').read() != open('z.frozen', 'rb').read(): bad(name, 'zlib differs through a stalling sink')
    for v in LV:
        z = open('z.' + v, 'rb').read()
        if z[0] != 0x78 or z[1] != FLG[v]: bad(name, 'zlib header', v, z[:2].hex())
        if v != 'frozen' and z[:2] != zlib.compress(b'x', int(v))[:2]: bad(name, 'zlib header differs from its own', v)
        o = zlib.decompressobj()
        try:
            if o.decompress(z) + o.flush() != d or not o.eof or o.unused_data: bad(name, 'zlib round trip', v)
        except Exception as e: bad(name, 'python3 refused zlib', v, e)
        try:
            if gzip.decompress(open('g.' + v, 'rb').read()) != d: bad(name, 'gzip round trip', v)
        except Exception as e: bad(name, 'python3 refused gzip', v, e)

# ---- tar: python3's archives through TarReader, field by field
def expect(m, body):
    n = m.name.encode('utf-8', 'surrogateescape'); link = m.linkname.encode('utf-8', 'surrogateescape')
    if m.isreg(): kind = 'file:%d' % m.size
    elif m.isdir(): kind = 'dir'
    elif m.issym(): kind = 'sym:' + link.hex()
    elif m.islnk(): kind = 'hard:' + link.hex()
    elif m.ischr(): kind = 'chr:%d:%d' % (m.devmajor, m.devminor)
    elif m.isblk(): kind = 'blk:%d:%d' % (m.devmajor, m.devminor)
    elif m.isfifo(): kind = 'fifo'
    else: kind = 'other:%d:%d' % (m.type[0], m.size)
    un, gn = m.uname.encode('utf-8', 'surrogateescape'), m.gname.encode('utf-8', 'surrogateescape')
    owner = 'none' if (m.uid == 0 and m.gid == 0 and not un and not gn) else '%d,%d,%s,%s' % (m.uid, m.gid, un.hex(), gn.hex())
    return (n.rstrip(b'/'), kind, m.mode, owner, len(body), zlib.crc32(body)), m.mtime
def add(t, name, data=b'', typ=tarfile.REGTYPE, link='', mode=0o644, mtime=0, uid=0, gid=0, un='', gn='', dev=(0, 0), pax=None):
    ti = tarfile.TarInfo(name)
    ti.type, ti.linkname, ti.mode, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = typ, link, mode, mtime, uid, gid, un, gn
    ti.devmajor, ti.devminor = dev
    if pax: ti.pax_headers = pax
    if typ == tarfile.REGTYPE: ti.size = len(data)
    t.addfile(ti, io.BytesIO(data) if typ == tarfile.REGTYPE else None)
def tree(t, fmt):
    add(t, 'plain.txt', b'hello', mode=0o4755, mtime=1234567890, uid=501, gid=20, un='alice', gn='staff')
    add(t, 'empty'); add(t, 'exact', b'x' * 512); add(t, 'over', b'y' * 513)
    add(t, 'd', typ=tarfile.DIRTYPE, mode=0o755); add(t, 'ln', typ=tarfile.SYMTYPE, link='plain.txt'); add(t, 'hl', typ=tarfile.LNKTYPE, link='plain.txt')
    add(t, 'pipe', typ=tarfile.FIFOTYPE); add(t, 'c', typ=tarfile.CHRTYPE, dev=(1, 3)); add(t, 'b', typ=tarfile.BLKTYPE, dev=(8, 0))
    add(t, 'p' * 60 + '/' + 'q' * 60, b'split')
    if fmt != tarfile.USTAR_FORMAT:
        add(t, 'n' * 150, b'long'); add(t, 'l', typ=tarfile.SYMTYPE, link='t' * 150); add(t, 'bigid', b'z', uid=3000000, gid=3000001)
    if fmt == tarfile.GNU_FORMAT:
        add(t, 'old', b'z', mtime=-1); add(t, 'caf\udce9', b'not utf-8')
    if fmt == tarfile.PAX_FORMAT:
        add(t, 'café', b'z'); add(t, 'half', b'z', mtime=1.5); add(t, 'o', b'z', un='u' * 40, gn='g' * 40); add(t, 'k', b'z', pax={'SCHILY.xattr.k': 'v' * 3000})
for label, fmt in (('ustar', tarfile.USTAR_FORMAT), ('gnu', tarfile.GNU_FORMAT), ('pax', tarfile.PAX_FORMAT)):
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode='w', format=fmt, encoding='utf-8', errors='surrogateescape') as t: tree(t, fmt)
    raw = bio.getvalue()
    open('tl.in', 'wb').write(raw)
    r = run('list')
    if r.returncode != 0: bad(label, 'list answered', r.returncode); continue
    got = []
    for l in open('tl.out', 'rb').read().split(b'\n')[:-1]:
        p, k, m, w, o, n, c = l.decode().split('|')
        got.append(((bytes.fromhex(p).rstrip(b'/'), k, int(m), o, int(n), int(c)), w))
    want = []
    with tarfile.open(fileobj=io.BytesIO(raw), encoding='utf-8', errors='surrogateescape') as t:
        for m in t.getmembers():
            fo = t.extractfile(m) if m.isreg() else None
            want.append(expect(m, fo.read() if fo else b''))
    if len(got) != len(want): bad(label, 'entries', len(got), len(want))
    for a, b in zip(got, want):
        if a[0] != b[0]: bad(label, 'entry differs', a[0], b[0])
        if a[1] == 'none' or abs(int(a[1]) / 1e9 - b[1]) > 1e-3: bad(label, 'time differs', a[0][0][:20], a[1], b[1])

# ---- tar: TarWriter's archive through python3
r = run('writes')
if r.returncode != 0: bad('writes answered', r.returncode)
else:
    with tarfile.open('w.tar') as t:
        ms = {m.name: m for m in t.getmembers()}
        if t.extractfile('file').read() != b'hello': bad('the file body')
    n = 'n' * 150
    want = {'file': (tarfile.REGTYPE, 5, 0o644, ''), 'dir': (tarfile.DIRTYPE, 0, 0o755, ''), 'ln': (tarfile.SYMTYPE, 0, 0o777, 'file'), 'hl': (tarfile.LNKTYPE, 0, 0o644, 'file'),
            'chr': (tarfile.CHRTYPE, 0, 0o644, ''), 'blk': (tarfile.BLKTYPE, 0, 0o644, ''), 'fifo': (tarfile.FIFOTYPE, 0, 0o644, ''), n: (tarfile.SYMTYPE, 0, 0o777, n)}
    for k, (ty, size, mode, link) in want.items():
        m = ms.get(k)
        if m is None: bad('no entry', k[:20]); continue
        if (m.type, m.size, m.mode, m.linkname) != (ty, size, mode, link): bad('entry', k[:20], m.type, m.size, oct(m.mode), m.linkname[:20])
    if ms['blk'].devmajor != 8 or ms['blk'].devminor != 16: bad('device numbers')
    o = ms['owner']
    if (o.uid, o.gid, o.uname, o.gname, o.mode) != (3000000, 7, 'alice', n, 0o4755): bad('owner', o.uid, o.gid, o.uname, o.gname[:8], oct(o.mode))
    if ms['half'].mtime != 1.5 or ms['old'].mtime != -1 or ms['far'].mtime != 9000000000: bad('times', ms['half'].mtime, ms['old'].mtime, ms['far'].mtime)

# ---- extractInto, by trying to break it
if unix:
    ATOMS = ['a', 'b', 'c', 'L', 'l', '..', '.', '', 'real', 'up']
    NAMES = ['a', 'b', 'L', 'l', 'up']
    def name():
        pick = random.choice([ATOMS, NAMES, NAMES])      # mostly names, so that some archives get far enough to make a chain
        parts = [random.choice(pick) for _ in range(random.randrange(1, 5))]
        s = '/'.join(parts)
        return '/' + s if random.random() < 0.05 else s
    def target():
        k = random.random()
        if k < 0.1: return os.path.abspath('outside')
        if k < 0.2: return '/etc'
        if k < 0.4: return '/'.join(['..'] * random.randrange(1, 5)) + random.choice(['', '/outside', '/a'])
        if k < 0.7: return name() + '/..' + random.choice(['', '/..', '/../..'])   # what a chain is made of
        return name()
    def archive():
        bio = io.BytesIO()
        with tarfile.open(fileobj=bio, mode='w', format=tarfile.PAX_FORMAT) as t:
            for _ in range(random.randrange(1, 14)):
                ti = tarfile.TarInfo(name() or 'x'); k = random.random()
                ti.mode = random.choice([0o644, 0o755, 0o777, 0o4755, 0o666])
                if k < 0.35: ti.size = 1; t.addfile(ti, io.BytesIO(b'X'))
                elif k < 0.5: ti.type = tarfile.DIRTYPE; t.addfile(ti)
                elif k < 0.9: ti.type = tarfile.SYMTYPE; ti.linkname = target(); t.addfile(ti)
                else: ti.type = tarfile.LNKTYPE; ti.linkname = target(); t.addfile(ti)
        return bio.getvalue()
    def snapshot(root):
        out = {}
        for d, ds, fs in os.walk(root):
            for n in ds + fs:
                p = os.path.join(d, n); st = os.lstat(p); out[p] = (st.st_mode, st.st_size, st.st_mtime_ns)
        return out
    os.makedirs('outside', exist_ok=True)
    open('outside/keep.txt', 'w').write('keep')
    before = snapshot('outside')
    dest = os.path.realpath('.') + '/x.out'
    links = 0
    for i in range(int(sys.argv[3])):
        open('x.tar', 'wb').write(archive())
        shutil.rmtree('x.out', ignore_errors=True)
        r = run('extract')
        if r.returncode not in (0, 1): bad('extract crashed', r.returncode, r.stderr[:120]); continue
        if snapshot('outside') != before: bad('changed outside the destination', i); before = snapshot('outside')
        if not os.path.exists('x.out'): continue
        for d, ds, fs in os.walk('x.out'):
            for n in ds + fs:
                p = os.path.join(d, n)
                if os.path.islink(p):
                    links += 1
                    try: os.stat(p)
                    except OSError as e:
                        if e.errno == errno.ELOOP: continue
                    real = os.path.realpath(p)
                    if real != dest and not real.startswith(dest + '/'): bad('a link leads out', p, os.readlink(p), real)
                else:
                    st = os.lstat(p)
                    if st.st_mode & 0o7000 or st.st_mode & 0o022: bad('a mode kept', p, oct(st.st_mode))
    print('links left behind', links)
for f in fails[:20]: print('FAIL', f)
print('fails', len(fails))
sys.exit(1 if fails else 0)
EOF

"$KAMA" build "$tmp/iop.kama" --release -o "$tmp/iop" >"$tmp/build.log" 2>&1 || { echo "check-compress-interop: FAIL — the tool does not build:" >&2; sed 's/^/  /' "$tmp/build.log" >&2; exit 1; }

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) how=windows; exe=$(kama_native_path "$tmp")/iop ;; *) how=unix; exe="$tmp/iop" ;; esac
if ! ( cd "$tmp" && python3 -I iop.py "$exe" "$how" 800 ) >"$tmp/run.log" 2>&1; then
    echo "check-compress-interop: FAIL:" >&2
    tail -24 "$tmp/run.log" | sed 's/^/  /' >&2
    exit 1
fi
echo "check-compress-interop: OK — every level both ways against python3's zlib and gzip, tar's three formats both ways, extractInto over 800 adversarial archives"
