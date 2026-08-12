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

# 4. M7 LAYOUT ASSERTS — the aggregate lowering of `comptime assert` is INVISIBLE to the exit-code suite.
#    tests/comptime_assert_layout.kama passes as a fixture whether or not a single `_Static_assert` was
#    emitted: nothing it returns depends on them. So the property is asserted on the transpiled C, the same
#    way the baked CRC table above is. Both halves matter — that the asserts are THERE, and that a
#    scalar predicate kama can answer itself does NOT become one (it would move a diagnostic the LSP can
#    see to one it cannot, which is the whole reason the two lowerings are split).
LA="$ROOT/tests/comptime_assert_layout.kama"
if [ -f "$LA" ]; then
    lac="$tmp/layout.c"
    "$KAMA" transpile "$LA" -o "$lac" >/dev/null

    # the aggregate cases reached C, with kama's message carried through as the assert text
    for want in 'sizeof(std__math__Vec4)) == (16)' '_Alignof(std__math__Vec4)' 'sizeof(size_t)'; do
        if ! grep -qF "_Static_assert((" "$lac" || ! grep -qF "$want" "$lac"; then
            echo "check-comptime: FAIL — no emitted _Static_assert matching '$want' (M7 aggregate lowering did not fire?)" >&2
            grep -c '_Static_assert' "$lac" >&2 || true
            exit 1
        fi
    done

    # a scalar predicate is answered BY KAMA and must not reach C. `sizeof(int32)` folds (M6), so if the
    # scalar fixture emitted an assert at all, the lowering rule picked the wrong side.
    SA="$ROOT/tests/comptime_assert_scalar.kama"
    if [ -f "$SA" ]; then
        sac="$tmp/scalar.c"
        "$KAMA" transpile "$SA" -o "$sac" >/dev/null
        if grep -q '_Static_assert' "$sac"; then
            echo "check-comptime: FAIL — a foldable scalar `comptime assert` emitted a _Static_assert; it must be answered by kama (so `kama check` and the LSP see it)" >&2
            grep '_Static_assert' "$sac" | head -3 >&2; exit 1
        fi
    fi
fi

echo "check-comptime: PASS (CRC table baked at compile time; comptime fn not emitted; foreach over a constant is warning-free; M7 layout asserts lowered to C, scalar ones not)"
