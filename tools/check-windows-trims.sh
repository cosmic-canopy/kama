#!/bin/sh
# check-windows-trims.sh — `<windows.h>` is trimmed only where nothing could miss what the trim removes (KR-68).
#
# kama_os.h, kama_time.h and kama_random.h define NOGDI, NOUSER and NOMINMAX ahead of `<windows.h>` when the
# compiler writes KAMA_OWN_HEADERS_ONLY. Those three remove declarations and macros for the rest of the TU, and
# `<windows.h>` is include-guarded, so a user's own C header after a trimmed one could never get `CreateWindowExW`
# or `min` back. A native release build folds every module into one TU, so the gate is per PROGRAM: written only
# when every header any unit names is kama's own or standard C, and then ahead of every header in every TU.
#
# This holds the gate down where it can be read on any host — in the emitted C — and, when `zig` is on PATH,
# cross-compiles the gated program for Windows against zig's mingw-w64 headers, which are the toolchain's. The
# trims removing something kama itself calls is a compile error there; a host without zig skips that half.
#
# check-legs: native
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA=${KAMA:-$ROOT/kama}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-windows-trims: FAIL — $1" >&2; exit 1; }

mkdir -p "$tmp/own" "$tmp/rel" "$tmp/user"
cat > "$tmp/own/main.kama" <<'EOF'
import { std::fs::exists, std::time::monotonicNow, std::time::Instant, std::random::Rng, std::net::TcpListener, std::io::IoError };
fn uint8 main() {
    Instant t = monotonicNow();
    Rng r = Rng.fromEntropy();
    Result<TcpListener, IoError> l = TcpListener.bind(host: "127.0.0.1", port: 0ui16);
    return exists(path: ".") ? 0 : 1;
}
EOF
cp "$tmp/own/main.kama" "$tmp/rel/main.kama"
printf 'static inline int own_twice(int x) { return 2 * x; }\n' > "$tmp/user/own.h"
cat > "$tmp/user/main.kama" <<'EOF'
import { std::fs::exists };
extern "own.h";
extern fn int32 own_twice(int32 x);
unsafe fn int32 twice(int32 x) { return own_twice(x: x); }
fn uint8 main() { return cast<uint8>(exists(path: ".") ? twice(x: 0) : 1); }
EOF

# 1. A std-only program: every TU it writes carries the gate.
"$KAMA" build "$tmp/own/main.kama" -o "$tmp/own/main" --keep-c > "$tmp/own.log" 2>&1 || { cat "$tmp/own.log" >&2; fail "the std-only program did not build"; }
n=0
for f in "$tmp"/own/*.c "$tmp"/own/*.gen.h; do
    n=$((n + 1))
    grep -q '^#define KAMA_OWN_HEADERS_ONLY 1$' "$f" || fail "$(basename "$f") of a std-only program lacks KAMA_OWN_HEADERS_ONLY"
done
[ "$n" -ge 3 ] || fail "expected a multi-TU build, found $n files"

# 2. Its release fold: the gate leads every header.
"$KAMA" build "$tmp/rel/main.kama" -o "$tmp/rel/main" --release --keep-c > "$tmp/rel.log" 2>&1 || { cat "$tmp/rel.log" >&2; fail "the release build did not build"; }
gate=$(grep -n '^#define KAMA_OWN_HEADERS_ONLY 1$' "$tmp/rel/main.c" | head -1 | cut -d: -f1)
first=$(grep -n '^#include "kama_[a-z_]*\.h"' "$tmp/rel/main.c" | grep -v kama_runtime.h | head -1 | cut -d: -f1)
[ -n "$gate" ] || fail "the release fold lacks KAMA_OWN_HEADERS_ONLY"
[ -n "$first" ] || fail "the release fold includes none of the seam headers — the probe no longer probes"
[ "$gate" -lt "$first" ] || fail "the release fold defines KAMA_OWN_HEADERS_ONLY at line $gate, after a seam header at line $first"

# 3. A program naming a C header of its own: no TU carries it.
"$KAMA" build "$tmp/user/main.kama" -o "$tmp/user/main" --keep-c > "$tmp/user.log" 2>&1 || { cat "$tmp/user.log" >&2; fail "the user-header program did not build"; }
if grep -l 'KAMA_OWN_HEADERS_ONLY' "$tmp"/user/*.c "$tmp"/user/*.gen.h > "$tmp/hits" 2>/dev/null; then
    fail "a program with its own C header is trimmed anyway: $(tr '\n' ' ' < "$tmp/hits")"
fi

# 4. The trimmed `<windows.h>` still compiles everything kama calls.
if command -v zig > /dev/null 2>&1; then
    mkdir -p "$tmp/win"
    cp "$tmp/own/main.kama" "$tmp/win/main.kama"
    "$KAMA" build "$tmp/win/main.kama" -o "$tmp/win/main.exe" --target x86_64-windows-gnu --cc "zig cc" > "$tmp/win.log" 2>&1 \
        || { sed -n '1,30p' "$tmp/win.log" >&2; fail "the gated program does not compile for Windows against mingw-w64's headers"; }
    echo "check-windows-trims: PASS ($n gated files, release fold gated first, user header ungated, Windows cross-compile clean)"
else
    echo "check-windows-trims: PASS ($n gated files, release fold gated first, user header ungated; no zig — Windows compile skipped)"
fi
