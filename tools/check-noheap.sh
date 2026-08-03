#!/bin/sh
# check-noheap.sh — MCU campaign step 5 guard for the `--no-heap` build flag (the whole-program no-heap
# subset). The `tests/xfail/*` loop builds every fixture with NO extra flags, so a flag-driven rejection
# can't be tested there — this dedicated guard drives the real `--no-heap` path and asserts:
#   1. REJECT   — a program that heap-allocates (`new`) FAILS to build under `--no-heap`, with the
#                 "heap allocation ... is forbidden" diagnostic (the right error, not any failure).
#   2. CONTROL  — the SAME program builds fine WITHOUT `--no-heap` (so the flag is what rejects it).
#   3. COMPOSES — `--no-heap` composes with `--target embedded` (independent axes) and still rejects.
# `@noheap` (the per-region attribute) is exercised by tests/xfail/noheap_* instead. Fails (exit 1) with a
# diagnostic if any property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-noheap: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src="$tmp/has_new.kama"
cat > "$src" <<'EOF'
import std::memory::{Owned};
type resource Box { int32 v; public ctor make(int32 v) { this.v = v; } public fn int32 get() { return this.v; } }
fn int32 main() { Owned<Box> b = new Box.make(v: 3); return b.get(); }
EOF

# 1. REJECT — `--no-heap` must reject the heap allocation with the specific diagnostic.
if "$KAMA" build --no-heap "$src" -o "$tmp/a.out" >/dev/null 2>"$tmp/nh.err"; then
    echo "check-noheap: FAIL — '--no-heap' accepted a program that calls 'new'" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/nh.err"; then
    echo "check-noheap: FAIL — '--no-heap' rejected, but not with the no-heap diagnostic:" >&2
    sed 's/^/  /' "$tmp/nh.err" >&2; exit 1
fi

# 2. CONTROL — the identical program must build WITHOUT the flag (proves the flag is the cause).
if ! "$KAMA" build "$src" -o "$tmp/b.out" >/dev/null 2>"$tmp/ctrl.err"; then
    echo "check-noheap: FAIL — the program does not build even WITHOUT '--no-heap':" >&2
    sed 's/^/  /' "$tmp/ctrl.err" >&2; exit 1
fi

# 3. COMPOSES — `--no-heap` is target-independent; it must still reject under `--target embedded`.
if "$KAMA" build --no-heap --target embedded "$src" -o "$tmp/c.o" >/dev/null 2>"$tmp/emb.err"; then
    echo "check-noheap: FAIL — '--no-heap --target embedded' accepted a 'new'" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/emb.err"; then
    echo "check-noheap: FAIL — '--no-heap --target embedded' rejected without the no-heap diagnostic:" >&2
    sed 's/^/  /' "$tmp/emb.err" >&2; exit 1
fi

echo "PASS no-heap (--no-heap rejects heap allocation, composes with --target embedded)"
