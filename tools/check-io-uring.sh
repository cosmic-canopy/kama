#!/bin/sh
# check-io-uring.sh — the fiber scheduler's two Linux arms both hold (KR-253, docs/design/io_uring.md). The ring arm runs
# by default wherever the kernel allows it, so the native suite on such a host runs the ring and never the epoll
# fallback, which is what every container, every kernel before 6.1, and `KAMA_IO_URING=0` get. So this runs the fiber
# fixtures again under `KAMA_IO_URING=0`. A fallback that hides a broken ring would also pass every fixture, so it also
# checks which arm ran: decision 7 says it is visible to the OS, as an `anon_inode:[io_uring]` descriptor.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ "$(uname -s)" != Linux ]; then echo "check-io-uring: PASS (skipped: the ring arm is Linux only)"; exit 0; fi
if [ ! -x "$KAMA" ]; then echo "check-io-uring: $KAMA not built" >&2; exit 1; fi
export KAMA   # the fixture run below uses this compiler, not the ./kama symlink another guard may be repointing

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-io-uring: FAIL — $1" >&2; [ $# -lt 2 ] || sed 's/^/  /' "$2" >&2; exit 1; }

# Whether this host allows the ring at all, asked the way the scheduler's probe asks (decision 6).
cat > "$tmp/probe.c" <<'C'
#include <linux/io_uring.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
#if defined(IORING_SETUP_DEFER_TASKRUN)
    struct io_uring_params p; memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    return syscall(__NR_io_uring_setup, 4, &p) >= 0 ? 0 : 1;
#else
    return 1;
#endif
}
C
allowed=no
if ${CC:-cc} -o "$tmp/probe" "$tmp/probe.c" 2>/dev/null && "$tmp/probe"; then allowed=yes; fi

# A fiber program, parked long enough to look at its descriptors.
cat > "$tmp/nap.kama" <<'KAMA'
import { std::concurrent::Fiber, std::time::sleep, std::time::Duration };
fn void nap(int64 ms) { sleep(d: Duration.fromMillis(ms: ms)); }
fn uint8 main() { scope { spawn fiber nap(ms: 1500); } return 0; }
KAMA
"$KAMA" build "$tmp/nap.kama" -o "$tmp/nap" >/dev/null 2>"$tmp/build.err" || fail "the fiber program did not build:" "$tmp/build.err"
rings() {   # rings <KAMA_IO_URING value> — how many ring descriptors the running program holds, or "failed"
    KAMA_IO_URING=$1 "$tmp/nap" & pid=$!
    sleep 0.5
    n=$(ls -l "/proc/$pid/fd" 2>/dev/null | grep -c 'anon_inode:\[io_uring\]' || true)
    if wait "$pid"; then echo "$n"; else echo failed; fi
}
on=$(rings 1); off=$(rings 0)
[ "$on" != failed ] && [ "$off" != failed ] || fail "the fiber program failed (ring: $on, KAMA_IO_URING=0: $off)"
[ "$off" = 0 ] || fail "KAMA_IO_URING=0 still made a ring"
if [ "$allowed" = yes ]; then
    [ "$on" -ge 1 ] || fail "this kernel allows io_uring, and the scheduler fell back to epoll anyway"
else
    [ "$on" = 0 ] || fail "the probe found io_uring refused here, and the scheduler made a ring anyway"
fi

# Reads and writes go to the kernel (steps 2 and 3, decision 2, and step 5's remedy). On the ring arm a read that finds
# nothing waiting is a RECV whose completion carries the data, so no recvfrom ever returns data here. The write rule
# differs by architecture (kama_net_write). On x86-64 every write is a SEND, so there is no sendto at all. Elsewhere a
# write makes its system call first, so these small ones, which always find room, are sendto calls. The writer waits half a second, so
# the reader's first attempt finds nothing, and every read after that follows a short one and makes no system call at
# all, so no amount of load on the machine changes the counts. Epoll's counts show strace is counting. Skipped without
# strace.
cat > "$tmp/reads.kama" <<'KAMA'
import { std::net::TcpListener, std::net::TcpStream, std::io::IoError, std::io::writeAll, std::concurrent::Fiber,
         std::time::Duration, std::time::sleep, std::collections::FixedArray };
static int32 got = 0;
unsafe fn void reader(ref TcpStream s) {
    FixedArray<uint8> buf = FixedArray.make(size: 64);
    while (got < 10) {
        Result<isize, IoError> r = s.read(into: buf.viewMut());
        int32 n = match (r) { case Ok(value: k): cast<int32>(k); case Err(error: e): 0; };
        if (n == 0) { return; }
        got = got + n;
    }
}
unsafe fn void writer(ref TcpStream s) {
    FixedArray<uint8> b = FixedArray.make(size: 5);
    sleep(d: Duration.fromMillis(ms: 500));
    _ = writeAll(w: s, bytes: b.view());
    sleep(d: Duration.fromMillis(ms: 100));
    _ = writeAll(w: s, bytes: b.view());
}
unsafe fn uint8 run() {
    Result<TcpListener, IoError> rl = TcpListener.bind(host: "127.0.0.1", port: 0ui16);
    TcpListener l = match (give rl) { case Ok(value: x): give x; case Err(error: e): { return 2; } };
    uint16 port = match (l.localAddr()) { case Ok(value: a): a.port; case Err(error: e): 0ui16; };
    Result<TcpStream, IoError> rc = TcpStream.connect(host: "127.0.0.1", port: port);
    TcpStream near = match (give rc) { case Ok(value: x): give x; case Err(error: e): { return 3; } };
    Result<TcpStream, IoError> ra = l.accept();
    TcpStream far = match (give ra) { case Ok(value: x): give x; case Err(error: e): { return 4; } };
    scope { spawn fiber reader(s: ref near); spawn fiber writer(s: ref far); }
    return got == 10 ? 0 : 5;
}
fn uint8 main() { return run(); }
KAMA
# A write that would block is a SEND (step 5's remedy, io_uring.md §6). The writer hands 16 MiB to writeAll while the
# reader sleeps, so the socket fills. On epoll every byte still leaves through sendto. On the ring the writes after a
# would-block or a short one leave as SENDs, which strace does not see, so sendto carries fewer than all of them, and on
# x86-64, where every write is a SEND, it carries none.
cat > "$tmp/writes.kama" <<'KAMA'
import { std::net::TcpListener, std::net::TcpStream, std::io::IoError, std::io::writeAll, std::concurrent::Fiber,
         std::time::Duration, std::time::sleep, std::collections::FixedArray };
static int64 got = 0;
unsafe fn void reader(ref TcpStream s) {
    FixedArray<uint8> buf = FixedArray.make(size: 65536);
    sleep(d: Duration.fromMillis(ms: 300));
    while (got < 16777216i64) {
        Result<isize, IoError> r = s.read(into: buf.viewMut());
        int64 n = match (r) { case Ok(value: k): cast<int64>(k); case Err(error: e): 0i64; };
        if (n == 0i64) { return; }
        got = got + n;
    }
}
unsafe fn void writer(ref TcpStream s) {
    FixedArray<uint8> b = FixedArray.make(size: 16777216);
    _ = writeAll(w: s, bytes: b.view());
}
unsafe fn uint8 run() {
    Result<TcpListener, IoError> rl = TcpListener.bind(host: "127.0.0.1", port: 0ui16);
    TcpListener l = match (give rl) { case Ok(value: x): give x; case Err(error: e): { return 2; } };
    uint16 port = match (l.localAddr()) { case Ok(value: a): a.port; case Err(error: e): 0ui16; };
    Result<TcpStream, IoError> rc = TcpStream.connect(host: "127.0.0.1", port: port);
    TcpStream near = match (give rc) { case Ok(value: x): give x; case Err(error: e): { return 3; } };
    Result<TcpStream, IoError> ra = l.accept();
    TcpStream far = match (give ra) { case Ok(value: x): give x; case Err(error: e): { return 4; } };
    scope { spawn fiber reader(s: ref near); spawn fiber writer(s: ref far); }
    return got == 16777216i64 ? 0 : 5;
}
fn uint8 main() { return run(); }
KAMA
reads=skipped
if [ "$allowed" = yes ] && command -v strace >/dev/null 2>&1; then
    "$KAMA" build "$tmp/reads.kama" -o "$tmp/reads" >/dev/null 2>"$tmp/build.err" || fail "the read program did not build:" "$tmp/build.err"
    "$KAMA" build "$tmp/writes.kama" -o "$tmp/writes" >/dev/null 2>"$tmp/build.err" || fail "the write program did not build:" "$tmp/build.err"
    sent() {   # sent <KAMA_IO_URING value> — the bytes the write program's sendto calls carried, or "none"
        if KAMA_IO_URING=$1 strace -f -qq -e trace=sendto -o "$tmp/sw$1" "$tmp/writes" >/dev/null 2>&1; then
            sed -n 's/^.*sendto(.*) = \([0-9][0-9]*\)$/\1/p' "$tmp/sw$1" | awk '{ t += $1 } END { print t + 0 }'
        else echo none; fi
    }
    traced() {   # traced <KAMA_IO_URING value> — "<recvfrom calls> <of them, returned data> <sendto calls>", or "none"
        if KAMA_IO_URING=$1 strace -f -qq -e trace=recvfrom,sendto -o "$tmp/st$1" "$tmp/reads" >/dev/null 2>&1; then
            echo "$(grep -c 'recvfrom(' "$tmp/st$1" || true) $(grep 'recvfrom(' "$tmp/st$1" | grep -c ') = [0-9]' || true)" \
                 "$(grep -c 'sendto(' "$tmp/st$1" || true)"
        else echo none; fi
    }
    sendAll=no; [ "$(uname -m)" = x86_64 ] && sendAll=yes   # the architecture's write rule, as kama_net_write gates it
    ring=$(traced 1); epoll=$(traced 0)
    if [ "$ring" != none ] && [ "$epoll" != none ]; then
        set -- $ring; [ "$1" -ge 1 ] && [ "$2" = 0 ] \
            || fail "on the ring a recvfrom returned data ($2 of $1 calls): reads are not reaching the kernel as RECVs"
        if [ "$sendAll" = yes ]; then
            [ "$3" = 0 ] || fail "on x86-64 every stream write on the ring is a SEND, and $3 were sendto calls"
        else
            [ "$3" -ge 1 ] || fail "on the ring a stream write made no sendto: a write that can go now must make its system call"
        fi
        set -- $epoll; [ "$2" -ge 1 ] && [ "$3" -ge 1 ] \
            || fail "strace saw no recvfrom return data ($2) or no sendto ($3) on epoll either, so it is not counting"
        wring=$(sent 1); wepoll=$(sent 0)
        [ "$wring" != none ] && [ "$wepoll" != none ] || fail "the write program failed (ring: $wring, epoll: $wepoll)"
        [ "$wepoll" = 16777216 ] || fail "on epoll sendto carried $wepoll of 16777216 bytes, so strace is not counting"
        [ "$wring" -lt 16777216 ] \
            || fail "on the ring sendto carried all 16777216 bytes: a write that would block is not reaching the kernel as a SEND"
        if [ "$sendAll" = yes ]; then
            [ "$wring" = 0 ] || fail "on x86-64 every stream write on the ring is a SEND, and sendto carried $wring bytes"
            reads="no recvfrom returned data on the ring; every write was a SEND (x86-64), no sendto"
        else
            reads="no recvfrom returned data on the ring; small writes are sendto, and $((16777216 - wring)) of 16777216 bytes left as SENDs"
        fi
    fi
fi

# The epoll arm, over every fiber fixture.
fixtures=$(cd "$ROOT/tests" && ls -d fiber_*.kama fiber_*.d | sed 's/\.kama$//')
(cd "$ROOT" && KAMA_IO_URING=0 KAMA_SKIP_CHECKS=1 ./run_tests.sh $fixtures) > "$tmp/epoll.log" 2>&1 \
    || fail "a fiber fixture failed on the epoll arm (KAMA_IO_URING=0):" "$tmp/epoll.log"
n=$(echo "$fixtures" | wc -l | tr -d ' ')

echo "check-io-uring: PASS (ring arm $([ "$allowed" = yes ] && echo in use || echo refused here, epoll in its place); reads and writes: $reads; $n fiber fixtures pass on epoll)"
