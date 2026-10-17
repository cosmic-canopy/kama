#!/bin/sh
# check-comptime.sh — const-eval 6b-3 guard for `comptime fn` table baking. The payoff of the whole
# const-eval ladder is that a compile-time function's result is a `static const` aggregate in the emitted
# C — computed once by the compiler, sitting in .rodata/flash, with NO runtime fill. Both properties are
# host-checkable on the transpiled C, so we assert them here rather than trusting the exit code alone:
#   1. BAKED       — the CRC table appears as a `static const InlineArray_uint8_256 … = { .kama_v = { … } }`.
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
if ! grep -q 'static const InlineArray_uint8_256 .*CRC = { .kama_v = { 0u, 94u,' "$cfile"; then
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

# 4. LAYOUT ASSERTS — invisible to the exit-code suite, so asserted on the transpiled C, the way the baked CRC
#    table above is. Three facts, since KR-94 folds every layout kama has a model of:
#      a. a baked layout is CHECKED by the C compiler: tests/comptime_assert_layout.kama's types each get one
#         `_Static_assert(sizeof(T) == N && _Alignof(T) == A, …)`, which is what makes the model safe;
#      b. a predicate over fixed-width scalars bakes nothing and emits nothing — kama answers it;
#      c. a predicate over a layout kama has NO model of — a `type extern value`'s — still reaches C as the
#         predicate itself, with kama's message carried through as the assert text.
LA="$ROOT/tests/comptime_assert_layout.kama"
if [ -f "$LA" ]; then
    lac="$tmp/layout.c"
    "$KAMA" transpile "$LA" -o "$lac" >/dev/null
    for want in 'sizeof(std__math__Vec4) == 16 && _Alignof(std__math__Vec4) == 4' 'sizeof(size_t) == '; do
        if ! grep -qF "_Static_assert($want" "$lac"; then
            echo "check-comptime: FAIL — no baked-layout _Static_assert matching '$want' (a folded layout went unchecked?)" >&2
            grep '_Static_assert' "$lac" | head -5 >&2
            exit 1
        fi
    done

    SA="$ROOT/tests/comptime_assert_scalar.kama"
    if [ -f "$SA" ]; then
        sac="$tmp/scalar.c"
        "$KAMA" transpile "$SA" -o "$sac" >/dev/null
        if grep -q '_Static_assert' "$sac"; then
            echo "check-comptime: FAIL — a fixed-width scalar `comptime assert` emitted a _Static_assert; kama answers it, and a scalar's size needs no check" >&2
            grep '_Static_assert' "$sac" | head -3 >&2; exit 1
        fi
    fi

    XA="$ROOT/tests/xfail/comptime_assert_extern_layout.d/main.kama"
    if [ -f "$XA" ]; then
        xac="$tmp/extern.c"
        "$KAMA" transpile "$XA" -o "$xac" >/dev/null
        if ! grep -qF '_Static_assert((sizeof(Sample)) == (8), "kama: a Sample is a level and a gain' "$xac"; then
            echo "check-comptime: FAIL — a layout claim over a \`type extern value\` did not reach C as its predicate" >&2
            grep '_Static_assert' "$xac" | head -3 >&2; exit 1
        fi
    fi
fi

echo "check-comptime: PASS (CRC table baked at compile time; comptime fn not emitted; foreach over a constant is warning-free; baked layouts asserted in C, scalar predicates not, an extern layout claim lowered to C)"
