#!/bin/sh
# check-wait-wasm.sh — `Atomic.wait` in a wasm build with no threads (KR-3 step 2, W5).
#
# A wasm program holding no thread form is built without threads (no SharedArrayBuffer, no workers), and wasm has no
# fibers yet, so nothing but the waiter itself ever runs. A wait whose cell already differs answers at once; one with a
# deadline waits it out, as `sleep` does there; and one that nothing could ever end panics, saying so, rather than
# spin forever in a browser tab. None of this is a fixture: the same program on a native build would sleep for good,
# since a native thread can be woken by threads a C library starts.
#
# check-legs: wasm
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
fail() { echo "check-wait-wasm: FAIL — $*" >&2; exit 1; }
command -v "${EMCC:-emcc}" >/dev/null 2>&1 || fail "no emcc: this guard belongs to the wasm leg"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
build() { "$KAMA" build "$tmp/$1.kama" --target wasm --cc "${EMCC:-emcc}" -o "$tmp/$1.js" >/dev/null 2>"$tmp/$1.err" \
              || { sed 's/^/  /' "$tmp/$1.err" >&2; fail "$1 did not build for wasm"; }; }
run() { st=0; (cd "$tmp" && node --no-concurrent-recompilation "$1.js") >"$tmp/$1.out" 2>&1 || st=$?; }

cat > "$tmp/answers.kama" <<'EOF'
import { std::concurrent::Atomic, std::time::Duration, std::time::Instant, std::time::monotonicNow };
fn uint8 main() {
    Atomic<uint32> c = Atomic.make(value: 7);
    if (c.wait(whileEquals: 0) != 7) { return 1; }
    Instant t0 = monotonicNow();
    match (c.waitUntil(whileEquals: 7, deadline: t0 + Duration.fromMillis(ms: 20))) {
        case Ok(value: v): { return 2; }
        case Err(error: e): { }
    };
    if (t0.elapsed() < Duration.fromMillis(ms: 20)) { return 3; }
    c.notifyAll();
    return 42;
}
EOF
cat > "$tmp/forever.kama" <<'EOF'
import { std::concurrent::Atomic };
fn uint8 main() {
    Atomic<uint32> c = Atomic.make(value: 7);
    return cast<uint8>(c.wait(whileEquals: 7));
}
EOF
build answers
build forever
if grep -q 'SharedArrayBuffer' "$tmp/answers.js"; then fail "a program with no thread form was built with threads"; fi

run answers
[ "$st" = 42 ] || { sed 's/^/  /' "$tmp/answers.out" >&2; fail "a wait with no one to end it answered $st, not 42"; }
run forever
[ "$st" != 0 ] || fail "a wait nothing could end returned"
grep -q 'nothing in this program can change the cell' "$tmp/forever.out" \
    || { sed 's/^/  /' "$tmp/forever.out" >&2; fail "a wait nothing could end did not panic with its reason"; }

echo "PASS check-wait-wasm (no threads: a changed cell answers, a deadline is kept, a wait nothing could end panics)"
