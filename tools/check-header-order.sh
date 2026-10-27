#!/bin/sh
# check-header-order.sh — every runtime header compiles before or after every other, and before or after the libc
# headers a program's own FFI include can bring.
#
# The defect this guards: macOS's <unistd.h> declares `read`, `write` and `close` with an asm label
# (`__DARWIN_ALIAS_C`), and clang refuses a labelled declaration that follows an unlabelled one ("cannot apply asm
# label to function after its first use"). kama_os.h declares those three itself rather than include <unistd.h>,
# and through 0.9.676 it declared them unlabelled, so any <unistd.h> AFTER it broke the build: kama_isolate.h's (for
# `sysconf`), or a C header a program names for FFI. Which header a unit gets first is decided by what the program
# imports, so the failure surfaced as "a release build that makes a `std::process::Command` beside
# `std::concurrent` stops in clang", while the debug build of the same program, whose units order their headers
# differently, built — the release unit put kama_os.h first. Two of the 380 ordered pairs below failed.
#
# A header's correctness must not depend on its neighbours' order, so this compiles all of them, pairwise, both
# ways, after kama_runtime.h (which every generated unit includes first). POSIX hosts only: Windows has its own,
# deliberate order contract (<winsock2.h> before <windows.h>, enforced at the top of kama_os.h), which is not a
# property of arbitrary pairs.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) echo "check-header-order: skipped (Windows has its own include-order contract)"; exit 0;; esac
CC=${CC:-cc}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The runtime headers a generated unit may include (kama_runtime.h is always first), and the libc headers that
# declare what they hand-declare. kama_lldb.py is not C.
HEADERS=""
for h in "$ROOT"/include/kama_*.h; do
    b=$(basename "$h")
    [ "$b" = kama_runtime.h ] || HEADERS="$HEADERS $b"
done
HEADERS="$HEADERS <unistd.h> <fcntl.h> <signal.h> <stdio.h> <stdlib.h> <pthread.h> <sys/socket.h> <time.h>"

line() { case $1 in "<"*) echo "#include $1";; *) echo "#include \"$1\"";; esac; }
fail=0; total=0
for a in $HEADERS; do
    for b in $HEADERS; do
        [ "$a" = "$b" ] && continue
        total=$((total + 1))
        { echo '#include "kama_runtime.h"'; line "$a"; line "$b"; } > "$tmp/t.c"
        if ! "$CC" -std=c11 -fsyntax-only -I"$ROOT/include" "$tmp/t.c" > "$tmp/err" 2>&1; then
            fail=$((fail + 1))
            echo "check-header-order: FAIL — $a then $b does not compile:" >&2
            grep -m 2 'error:' "$tmp/err" >&2 || sed -n '1,3p' "$tmp/err" >&2
        fi
    done
done
# A program that runs fibers defines KAMA_FIBERS before any kama header (KR-3 step 5), and then kama_os.h's socket
# wait reaches into kama_sched.h, which it includes at its end while kama_sched.h includes it at its top. Either may
# come first in a unit, so every pair holding a header on that seam compiles both ways under the define too —
# including kama_sched.h's own `#error` for kama_os.h read before the define, which no order here may trip. A channel's
# wait does the same since step 6, and kama_sched.h does not include kama_channel.h, so its half of that seam sits
# after its include guard, so either order defines it (tests/fiber_sched.d reads kama_sched.h first, and links).
SEAM="kama_channel.h kama_os.h kama_sched.h kama_time.h kama_fiber.h kama_isolate.h"
fibered=0
for a in $SEAM; do
    for b in $HEADERS; do
        [ "$a" = "$b" ] && continue
        for pair in "$a $b" "$b $a"; do
            set -- $pair
            total=$((total + 1)); fibered=$((fibered + 1))
            { echo '#define KAMA_FIBERS 1'; echo '#include "kama_runtime.h"'; line "$1"; line "$2"; } > "$tmp/t.c"
            if ! "$CC" -std=c11 -fsyntax-only -I"$ROOT/include" "$tmp/t.c" > "$tmp/err" 2>&1; then
                fail=$((fail + 1))
                echo "check-header-order: FAIL — with KAMA_FIBERS, $1 then $2 does not compile:" >&2
                grep -m 2 'error:' "$tmp/err" >&2 || sed -n '1,3p' "$tmp/err" >&2
            fi
        done
    done
done
if [ "$fail" -ne 0 ]; then
    echo "check-header-order: $fail of $total ordered pairs fail — a header must not depend on its neighbours' order" >&2
    exit 1
fi
echo "check-header-order: OK ($total ordered pairs of runtime and libc headers compile, $fibered of them with KAMA_FIBERS)"
