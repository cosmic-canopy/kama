#!/bin/sh
# check-simd-native.sh — kama's math actually auto-vectorizes on a native target. The instrument for a
# claim that has been load-bearing in the docs for months with nothing behind it.
#
# What it guards. SPEC.md (*Math*) and ENGINE_READINESS.md both promise that `std::math`'s value types
# have a SIMD-ready layout and that a `--release` build packs the elementwise ops to NEON on aarch64 and
# SSE on the x86-64 baseline, "landing hot math at C parity". Nothing checked it. That is not a
# hypothetical risk here: the SAME claim was measured WRONG in both directions inside one week — first
# reported absent when it was present (a bad probe, see below), then found genuinely absent on wasm,
# where it had been documented as present for months. A throughput claim nobody instruments is a claim
# that drifts silently, because no fixture's exit code changes when it breaks.
#
# ⚠️ THE GUARD PROVES ITS OWN INSTRUMENT, EVERY RUN. The same C is compiled twice — once at -O3, once at
# -O0 — and -O0 must yield ZERO matches. Without that, a grep pattern that can never match (the exact
# defect that produced this repo's earlier false "no SIMD" finding: Apple's assembler writes
# `fadd.4s v0, v0, v1`, NOT `fadd v0.4s`, so the obvious pattern reports zero on a fully vectorized
# function) would sit green forever. Three fixtures have already shipped in this repo that could not
# fail; a negative control is the cheapest defense against a fourth.
#
# The probe's shape is load-bearing too, and its own header says why: tests/support/simd_probe.kama.
#
# SKIPS, visibly, on a host with no clang or on an architecture with no pattern here. A visible SKIP is
# legible in the run output; a silent pass is not. Same shape as check-softfloat.sh.
#
# check-legs: native
#
# Sibling: check-simd-wasm.sh, which makes the same assertion about the wasm target — and which
# `./dev check` CANNOT run. Between them the claim is covered on every tier kama ships.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
PROBE="$ROOT/tests/support/simd_probe.kama"

if [ ! -x "$KAMA" ]; then echo "check-simd-native: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$PROBE" ]; then echo "check-simd-native: missing $PROBE" >&2; exit 1; fi

if ! command -v clang >/dev/null 2>&1; then
    echo "SKIP check-simd-native (no clang on PATH — the asm this reads is clang's)"
    exit 0
fi

# The vector-instruction pattern for this host's ISA, and NOTHING wider. A pattern that also matched the
# scalar forms (`fadd s0` / `addss`) would pass against a de-vectorized compiler.
#   aarch64 — TWO spellings, and matching only one is how this was got wrong before:
#             Apple puts the arrangement on the OPCODE (`fadd.4s v0, v0, v1`);
#             GNU/LLVM puts it on the REGISTERS (`fadd v0.4s, v1.4s, v2.4s`).
#   x86-64  — the PACKED forms only. `addss`/`mulss` are the scalar ones and must not match.
case "$(uname -m)" in
    arm64|aarch64)  PAT='[a-z][a-z0-9]*\.4s|v[0-9]+\.4s' ; ISA='NEON (.4s)' ;;
    x86_64|amd64)   PAT='(^|[^a-z])v?(addps|mulps|subps)([^a-z]|$)' ; ISA='SSE (packed)' ;;
    *)              echo "SKIP check-simd-native (no vector-instruction pattern for $(uname -m))"; exit 0 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$KAMA" transpile "$PROBE" -o "$tmp/p.c" >/dev/null 2>"$tmp/transpile.err" || {
    echo "check-simd-native: FAIL — kama transpile failed on the probe" >&2
    sed 's/^/  /' "$tmp/transpile.err" >&2; exit 1; }

# `--release` is `-O3 -DNDEBUG` on native (kama.driver.cpp, the release block) — the tier the docs'
# claim is about. Reproduced here exactly, so what this reads is what a user's release build gets.
clang -std=c11 -O3 -DNDEBUG -I "$ROOT/include" -S "$tmp/p.c" -o "$tmp/o3.s" 2>"$tmp/cc.err" || {
    echo "check-simd-native: FAIL — clang could not compile the transpiled probe at -O3" >&2
    sed 's/^/  /' "$tmp/cc.err" >&2; exit 1; }
clang -std=c11 -O0 -I "$ROOT/include" -S "$tmp/p.c" -o "$tmp/o0.s" 2>>"$tmp/cc.err" || {
    echo "check-simd-native: FAIL — clang could not compile the transpiled probe at -O0" >&2
    sed 's/^/  /' "$tmp/cc.err" >&2; exit 1; }

hits3=$(grep -cE "$PAT" "$tmp/o3.s" || true)
hits0=$(grep -cE "$PAT" "$tmp/o0.s" || true)

# The negative control FIRST: if -O0 matches, the pattern is measuring something other than
# vectorization and the positive result below would be meaningless.
if [ "$hits0" -ne 0 ]; then
    echo "check-simd-native: FAIL — the instrument is broken, not the compiler." >&2
    echo "  The UNOPTIMIZED (-O0) build matched $hits0 '$ISA' instruction(s); it must match zero." >&2
    echo "  The pattern is too loose, so a green result here would prove nothing. Pattern: $PAT" >&2
    exit 1
fi

if [ "$hits3" -eq 0 ]; then
    echo "check-simd-native: FAIL — a --release-tier build of std::math emits NO $ISA instructions." >&2
    echo "  SPEC.md (*Math*) and ENGINE_READINESS.md both claim it does. One of them is now wrong." >&2
    echo "  Probe: tests/support/simd_probe.kama    asm: -O3 -DNDEBUG, $(uname -m)" >&2
    echo "  Before believing this: read the probe's header. A kernel measured at an ABI boundary, or" >&2
    echo "  one the optimizer can prove dead, looks scalar for reasons unrelated to the vectorizer." >&2
    exit 1
fi

echo "check-simd-native: PASS ($hits3 $ISA instruction(s) at -O3, 0 at -O0 — the control holds)"
