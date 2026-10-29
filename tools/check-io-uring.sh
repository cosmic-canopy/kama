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

# The epoll arm, over every fiber fixture.
fixtures=$(cd "$ROOT/tests" && ls -d fiber_*.kama fiber_*.d | sed 's/\.kama$//')
(cd "$ROOT" && KAMA_IO_URING=0 KAMA_SKIP_CHECKS=1 ./run_tests.sh $fixtures) > "$tmp/epoll.log" 2>&1 \
    || fail "a fiber fixture failed on the epoll arm (KAMA_IO_URING=0):" "$tmp/epoll.log"
n=$(echo "$fixtures" | wc -l | tr -d ' ')

echo "check-io-uring: PASS (ring arm $([ "$allowed" = yes ] && echo in use || echo refused here, epoll in its place); $n fiber fixtures pass on epoll)"
