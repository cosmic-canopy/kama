#!/bin/sh
# check-release-arith.sh — the RELEASE tier's arithmetic contract, which nothing could reach before.
#
# SPEC promises three things about signed arithmetic, and two of them are about a tier the fixture suite
# never builds:
#
#   1. debug   `+ - *` overflow TRAPS
#   2. release `+ - *` overflow WRAPS (defined two's-complement, `-fwrapv`) and is ZERO-COST
#   3. `TYPE_MIN / -1` TRAPS in EVERY build
#
# `tests/trap/signed_overflow` covers (1). Nothing covered (2) or (3)-in-release, because run_tests.sh
# builds trap fixtures in debug — `tests/trap/intmin_div.kama` even STATES the release half in its own
# comment ("it traps even though ordinary +/-/* wrap in release") and has never once tested it.
#
# ⚠️ WHAT THAT COST, measured 2026-08-31. The driver passed `-fsanitize=signed-integer-overflow` in BOTH
# tiers, keeping it in release for (3) alone, on the assumption that `-fwrapv` suppresses it for the
# ordinary ops. That assumption holds on Ubuntu clang 18.1.3 and gcc 13.3 and NOT on Apple clang 21. So a
# macOS release build TRAPPED where a Linux one wrapped — from one source and one set of flags — and paid
# `adds; b.vs; brk` on every signed add and `smull; cmp; b.ne; brk` on every multiply, against a bare
# `add`/`mul` on Linux. A compare and a branch on arithmetic the project's headline invariant calls "at C
# parity", on one of its two main platforms, for as long as that clang has been current.
#
# The fix was to make the sanitizer debug-only and check `TYPE_MIN / -1` explicitly. This guard is what
# keeps the trade honest: it asserts the semantics AND the cost, so neither half can drift back silently.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-release-arith: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# Operands come from `std::num` calls so nothing folds them; a constant that does not fit is a compile
# error on a different path entirely (rejectConstOutOfRange).
cat > "$tmp/add.kama" <<'EOF'
import { std::num::int32Max };
fn int32 main() {
    int32 m = int32Max();
    int32 one = int32Max() - int32Max() + 1;
    return m + one;
}
EOF
cat > "$tmp/div.kama" <<'EOF'
import { std::num::int32Min };
fn int32 main() {
    int32 neg1 = 0 - 1;
    return int32Min() / neg1;
}
EOF
cat > "$tmp/div64.kama" <<'EOF'
import { std::num::int64Min };
fn int32 main() {
    int64 neg1 = 0i64 - 1i64;
    int64 q = int64Min() / neg1;
    return cast<int32>(q & 1i64);
}
EOF

# `trapped` is "killed by a signal or aborted". ⚠️ NOT a bare `>= 128` test: these programs RETURN
# values, and a returned value's low byte can land there on its own — the exact ambiguity that made two
# trap fixtures in tests/trap/ need a return offset to stay honest.
run() {  # run <binary>; echoes "trap" or the exit code
    "$1" >/dev/null 2>&1 && { echo 0; return; }
    rc=$?
    if [ "$rc" -ge 128 ] && [ "$rc" -le 165 ]; then echo trap; else echo "$rc"; fi
}
build() { "$KAMA" build "$1" -o "$2" $3 >/dev/null 2>"$tmp/err" || {
    echo "check-release-arith: FAIL — build failed: $1 $3" >&2; sed 's/^/  /' "$tmp/err" >&2; exit 1; }; }

# --- 1. debug `+ - *` overflow traps ------------------------------------------------------------------
build "$tmp/add.kama" "$tmp/add_d" ""
got=$(run "$tmp/add_d")
[ "$got" = trap ] || { echo "check-release-arith: FAIL — debug int32Max + 1 did not trap (got $got)" >&2; fail=1; }

# --- 2. release `+ - *` overflow WRAPS ----------------------------------------------------------------
# int32Max + 1 wraps to INT32_MIN, whose low byte is 0 — so a wrapped run exits 0.
build "$tmp/add.kama" "$tmp/add_r" "--release"
got=$(run "$tmp/add_r")
if [ "$got" = trap ]; then
    echo "check-release-arith: FAIL — release int32Max + 1 TRAPPED; SPEC says it wraps." >&2
    echo "                    This is the Apple-clang/-fwrapv interaction: if the driver is passing" >&2
    echo "                    -fsanitize=signed-integer-overflow in the release tier again, that flag" >&2
    echo "                    is not suppressed by -fwrapv on every toolchain. See the driver's note." >&2
    fail=1
elif [ "$got" != 0 ]; then
    echo "check-release-arith: FAIL — release int32Max + 1 exited $got, expected 0 (wrapped to INT32_MIN)" >&2
    fail=1
fi

# --- 3. TYPE_MIN / -1 traps in BOTH tiers -------------------------------------------------------------
# This is what made dropping the release sanitizer safe: the check is emitted (kama_sdiv_i32/_i64), not
# supplied by a flag. int64 is here because it is the case a RESULT-based check would get wrong —
# INT64_MIN / -1 overflows the very `long long` such a check would compute it in.
for w in div div64; do
    for tier in "" "--release"; do
        build "$tmp/$w.kama" "$tmp/${w}_b" "$tier"
        got=$(run "$tmp/${w}_b")
        [ "$got" = trap ] || {
            echo "check-release-arith: FAIL — ${tier:-debug} $w did not trap (got $got); SPEC says" >&2
            echo "                    TYPE_MIN / -1 traps in EVERY build." >&2; fail=1; }
    done
done

# --- 4. ...and release arithmetic is ZERO-COST ---------------------------------------------------------
# The semantics above can be right while the cost is wrong — that is exactly what the divergence was. So
# read the asm: a release signed add/mul must be ONE instruction with no trap block anywhere near it.
if command -v clang >/dev/null 2>&1; then
    cat > "$tmp/parity.kama" <<'EOF'
expose fn int32 padd(int32 a, int32 b) { return a + b; }
expose fn int32 pmul(int32 a, int32 b) { return a * b; }
fn int32 main() { return 0; }
EOF
    "$KAMA" transpile "$tmp/parity.kama" -o "$tmp/parity.c" >/dev/null 2>&1 || {
        echo "check-release-arith: FAIL — transpile failed on the parity probe" >&2; exit 1; }
    # kama's release C flags, reproduced (kama.driver.cpp, the release block).
    # (No `-fsanitize` here since 0.9.160: division, shifts and float casts are kama's own checks in the
    # emitted C, and the release line carries none.)
    clang -std=c11 -O3 -DNDEBUG -fwrapv \
          -I "$ROOT/include" -S "$tmp/parity.c" -o "$tmp/parity.s" 2>/dev/null || {
        echo "check-release-arith: FAIL — clang could not compile the parity probe" >&2; exit 1; }
    # A trap block reachable from plain arithmetic is the signature of the regression.
    traps=$(grep -cE '\b(brk|ud2|udf)\b' "$tmp/parity.s" || true)
    if [ "$traps" -ne 0 ]; then
        echo "check-release-arith: FAIL — $traps trap instruction(s) in a release build of two signed" >&2
        echo "                    arithmetic functions. Release arithmetic must be zero-cost; this is" >&2
        echo "                    the overflow check being emitted in the wrong tier again." >&2
        fail=1
    fi
else
    echo "check-release-arith: NOTE (no clang on PATH — skipped the zero-cost assertion)"
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-release-arith: PASS (debug traps, release wraps, TYPE_MIN/-1 traps in both tiers at 32 and 64 bits, release arithmetic is trap-free)"
