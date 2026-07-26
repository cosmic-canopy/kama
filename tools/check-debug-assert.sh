#!/bin/sh
# check-debug-assert.sh — proves the `debugAssert` strip on the transpiled C. `assert` is always-on;
# `debugAssert` is the `--release`-stripped dev-only variant (like C's NDEBUG / Rust's debug_assert!).
# The Kama compiler does the strip at emit time (emitting `(void)0`), so we assert on the emitted C, not
# just an exit code. tests/debug_assert.kama carries two distinctive message strings:
#   assert(..., msg: "keepcheck")       — must survive BOTH builds
#   debugAssert(..., msg: "dbgcheck")   — must survive DEBUG, be DROPPED under --release
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA="$ROOT/kama"
FIXTURE="$ROOT/tests/debug_assert.kama"

if [ ! -x "$KAMA" ]; then echo "check-debug-assert: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-debug-assert: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
dbgc="$tmp/dbg.c"
relc="$tmp/rel.c"

# 1. DEBUG — both the assert and the debugAssert are compiled in.
"$KAMA" transpile --no-line "$FIXTURE" -o "$dbgc" >/dev/null
if ! grep -q 'keepcheck' "$dbgc"; then
    echo "check-debug-assert: FAIL — DEBUG build dropped the always-on assert ('keepcheck')" >&2
    exit 1
fi
if ! grep -q 'dbgcheck' "$dbgc"; then
    echo "check-debug-assert: FAIL — DEBUG build dropped the debugAssert ('dbgcheck') — it must be kept in debug" >&2
    exit 1
fi

# 2. RELEASE — the assert survives; the debugAssert is stripped (emitted as (void)0, so 'dbgcheck' is gone).
"$KAMA" transpile --no-line --release "$FIXTURE" -o "$relc" >/dev/null
if ! grep -q 'keepcheck' "$relc"; then
    echo "check-debug-assert: FAIL — RELEASE build dropped the always-on assert ('keepcheck')" >&2
    exit 1
fi
if grep -q 'dbgcheck' "$relc"; then
    echo "check-debug-assert: FAIL — RELEASE build STILL contains the debugAssert ('dbgcheck'); it must be stripped" >&2
    exit 1
fi

echo "check-debug-assert: PASS (assert always-on; debugAssert stripped under --release)"
