#!/bin/sh
# check-comptime.sh — const-eval 6b-3 guard for `comptime fn` table baking. The payoff of the whole
# const-eval ladder is that a compile-time function's result is a `static const` aggregate in the emitted
# C — computed once by the compiler, sitting in .rodata/flash, with NO runtime fill. Both properties are
# host-checkable on the transpiled C, so we assert them here rather than trusting the exit code alone:
#   1. BAKED       — the CRC table appears as a `static const InlineArray_uint8_256 … = { .v = { … } }`.
#   2. COMPTIME-ONLY — the `comptime fn` itself (`crcTable`) is NEVER emitted as a C symbol.
# The subject (tests/comptime_fn_crc.kama) also runs as an ordinary fixture on the native/ASan/wasm legs
# (its exit code asserts the baked table equals a runtime recomputation). Fails (exit 1) if either breaks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/comptime_fn_crc.kama"

if [ ! -x "$KAMA" ]; then echo "check-comptime: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-comptime: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cfile="$tmp/crc.c"

"$KAMA" transpile "$FIXTURE" -o "$cfile" >/dev/null

# 1. BAKED — the table is a compile-time-computed static const aggregate (first two entries are 0, 94).
if ! grep -q 'static const InlineArray_uint8_256 .*CRC = { .v = { 0u, 94u,' "$cfile"; then
    echo "check-comptime: FAIL — baked CRC table not found in emitted C (comptime fn did not fold?)" >&2
    exit 1
fi

# 2. COMPTIME-ONLY — the comptime fn is never lowered to a runtime C function.
if grep -q 'crcTable' "$cfile"; then
    echo "check-comptime: FAIL — comptime fn 'crcTable' leaked into emitted C (should never be emitted)" >&2
    exit 1
fi

# 3. WARNING-FREE — a `foreach` over a baked table must compile CLEANLY. The exit-code suite cannot see a
#    warning, so it is asserted here: the lowering used to take a plain `T*` to a `static const` aggregate,
#    which is a const-discard on every iteration of every lookup table (and one -Werror from a hard error).
FE="$(dirname "$0")/../tests/comptime_foreach_const.kama"
if [ -f "$FE" ]; then
    if ! "$KAMA" build "$FE" -o "$tmp/fe" >"$tmp/fe.out" 2>"$tmp/fe.err"; then
        echo "check-comptime: FAIL — tests/comptime_foreach_const.kama did not build" >&2
        head -5 "$tmp/fe.err" >&2; exit 1
    fi
    if grep -qi 'warning' "$tmp/fe.err"; then
        echo "check-comptime: FAIL — foreach over a comptime constant emitted a compiler warning:" >&2
        grep -i 'warning' "$tmp/fe.err" | head -5 >&2; exit 1
    fi
fi

echo "check-comptime: PASS (CRC table baked at compile time; comptime fn not emitted; foreach over a constant is warning-free)"
