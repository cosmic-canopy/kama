#!/bin/sh
# reach.sh — what a job could call (KR-3's pool brief, decision 5): the park rule refuses a `Shared` window around anything that
# reaches a fiber switch, the same leaf the job rule would use, so it answers per call today. Prints each call and its verdict.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$here/../../.." && pwd); . "$ROOT/tools/kama-bin.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
while IFS='|' read -r name imports body; do
    cat > "$tmp/t.kama" <<KAMA
import { core::println, std::concurrent::Fiber, std::concurrent::Isolate, std::collections::DynamicArray $imports };
type value Box { public int32 n = 0; public ctor make() { } }
fn void noop(ref int32 x) { x = 1; }
unsafe fn void tested() { $body }
unsafe fn uint8 entry() {
    Shared<Box> s = new Box.make();
    int32 v = 0;
    scope { spawn fiber noop(x: ref v); }
    borrow s.n as w { tested(); }
    return 0;
}
fn uint8 main() { return entry(); }
KAMA
    if "$KAMA" check "$tmp/t.kama" >/dev/null 2>&1; then v=passes; else v=refused; fi
    printf '%-20s %s\n' "$name" "$v"
done <<'CALLS'
println||println(s: "x");
logInfo|, std::log::logInfo|logInfo(tag: "t", msg: "m");
allocation||DynamicArray<int32> d = DynamicArray::<int32>.empty(); d.add(item: 1);
interpolation||int32 a = 1; string t = "v=${a}";
monotonicNow|, std::time::monotonicNow, std::time::Instant|Instant t = monotonicNow();
Atomic|, std::concurrent::Atomic|Atomic<int32> a = Atomic.make(value: 1); a.store(value: 2);
Locked window|, std::concurrent::Locked|Locked<int32> l = Locked.make(value: 1); borrow l.lock().value() as x { x = 2; }
tryRecv|, std::concurrent::Channel, std::concurrent::Receiver, std::concurrent::RecvError|Channel<int32> c = Channel.bounded(capacity: 1); Receiver<int32> r = c.receiver(); match (r.tryRecv()) { case Ok(value: v): { } case Err(error: e): { } };
File.open|, std::fs::File, std::fs::OpenMode|match (File.open(path: "/tmp/x", mode: OpenMode::Read)) { case Ok(value: f): { } case Err(error: e): { } };
send|, std::concurrent::Channel, std::concurrent::Sender|Channel<int32> c = Channel.bounded(capacity: 1); Sender<int32> t = c.sender(); _ = t.send(item: 1);
recv|, std::concurrent::Channel, std::concurrent::Receiver|Channel<int32> c = Channel.bounded(capacity: 1); Receiver<int32> r = c.receiver(); Optional<int32> o = r.recv();
sleep|, std::time::sleep, std::time::Duration|sleep(d: Duration.fromMillis(ms: 1));
File.read|, std::fs::File, std::fs::OpenMode, std::collections::FixedArray|match (File.open(path: "/tmp/x", mode: OpenMode::Read)) { case Ok(value: f): { FixedArray<uint8> b = FixedArray.make(size: 8); _ = f.read(into: b.viewMut()); } case Err(error: e): { } };
TcpStream.connect|, std::net::TcpStream|match (TcpStream.connect(host: "127.0.0.1", port: 1ui16)) { case Ok(value: s2): { } case Err(error: e): { } };
isolate join||int32 q = 0; scope { spawn noop(x: ref q); }
parallel_for||DynamicArray<int32> d = DynamicArray::<int32>.empty(); parallel_for (ref int32 e in d, chunks: 2) { e = 1; }
CALLS
