#!/bin/sh
# check-simd-wasm.sh — a `--release --target wasm` build must contain v128 instructions.
#
# The defect this exists to prevent, which had already happened. wasm SIMD is OPT-IN in emscripten, and
# kama never passed `-msimd128` — so every .wasm kama has ever produced held ZERO vector instructions,
# while SPEC.md said the math ops vectorize "to SSE/NEON/wasm128". Nothing caught it for months, because
# vectorization changes no fixture's exit code: the wasm leg was fully green against a compiler emitting
# entirely scalar code. The one-line driver fix is worth little without this; a flag is exactly the kind
# of thing a later refactor drops in passing.
#
# ⚠️ THE GUARD PROVES ITS OWN INSTRUMENT, EVERY RUN. It compiles the same transpiled C a second time
# with `emcc -c` and NO `-msimd128`, and that build must contain ZERO v128 — so a disassembler that
# silently produced nothing, or a pattern that could never match, fails here instead of passing
# everything. This repo has shipped three fixtures that could not fail; do not add a fourth.
#
# ⚠️ THIS GUARD DOES NOT RUN IN THE INNER LOOP. It needs emcc, so it is the repo's first
# `# check-legs: wasm` guard — and `./dev check` is `--leg native --all`. A green `./dev check` says
# NOTHING about wasm SIMD. Only `./dev test wasm` and `./dev matrix` reach it.
#
# check-legs: wasm
#
# Only the RELEASE tier is asserted. A debug build passes `-msimd128` too (both tiers share an
# instruction set, on purpose — see the driver comment), but emits no vector code at `-O0` for the same
# reason `clang -O0` does not, so asserting on it would be asserting on the optimizer.
#
# Sibling: check-simd-native.sh, which makes the same assertion about NEON/SSE on the native leg.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
PROBE="$ROOT/tests/support/simd_probe.kama"

if [ ! -x "$KAMA" ]; then echo "check-simd-wasm: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$PROBE" ]; then echo "check-simd-wasm: missing $PROBE" >&2; exit 1; fi

# ⚠️ The container has NO `wasm-objdump` (that is wabt, which the emsdk image does not ship). The
# disassembler that IS there is llvm-objdump, under the emsdk prefix rather than on PATH.
DIS=""
for c in "${EMSDK:-/emsdk}/upstream/bin/llvm-objdump" llvm-objdump wasm-objdump; do
    if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then DIS="$c"; break; fi
done
if [ -z "$DIS" ]; then
    echo "check-simd-wasm: FAIL — no wasm disassembler (looked for llvm-objdump under \$EMSDK, then on PATH)." >&2
    echo "  A guard that cannot read its input must not pass. Install one, or fix the emsdk prefix." >&2
    exit 1
fi

# `v128.load`/`v128.store`/`f32x4.*` — the whole vector opcode family is spelled with one of these two
# prefixes, so this cannot match a scalar build.
PAT='f32x4|v128'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- the positive: what a user's `--release --target wasm` build actually contains -------------------
"$KAMA" build "$PROBE" -o "$tmp/probe" --target wasm --release >"$tmp/build.log" 2>&1 || {
    echo "check-simd-wasm: FAIL — kama build --target wasm --release failed on the probe" >&2
    sed 's/^/  /' "$tmp/build.log" >&2; exit 1; }
[ -f "$tmp/probe.wasm" ] || {
    echo "check-simd-wasm: FAIL — no .wasm was produced (looked for $tmp/probe.wasm)" >&2
    ls -la "$tmp" >&2; exit 1; }

"$DIS" -d "$tmp/probe.wasm" >"$tmp/rel.txt" 2>"$tmp/dis.err" || {
    echo "check-simd-wasm: FAIL — $DIS could not disassemble the .wasm" >&2
    sed 's/^/  /' "$tmp/dis.err" >&2; exit 1; }
rel=$(grep -cE "$PAT" "$tmp/rel.txt" || true)

# ---- the negative control: the SAME C, compiled without the flag, must be scalar --------------------
"$KAMA" transpile "$PROBE" -o "$tmp/p.c" >/dev/null 2>"$tmp/transpile.err" || {
    echo "check-simd-wasm: FAIL — kama transpile failed on the probe" >&2
    sed 's/^/  /' "$tmp/transpile.err" >&2; exit 1; }
# `-c` (object, no link) rather than a second full emcc link: same measurement, a fraction of the cost.
emcc -std=c11 -Oz -DNDEBUG -I "$ROOT/include" -c "$tmp/p.c" -o "$tmp/noflag.o" 2>"$tmp/emcc.err" || {
    echo "check-simd-wasm: FAIL — emcc could not compile the transpiled probe" >&2
    sed 's/^/  /' "$tmp/emcc.err" >&2; exit 1; }
"$DIS" -d "$tmp/noflag.o" >"$tmp/ctl.txt" 2>/dev/null || true
ctl=$(grep -cE "$PAT" "$tmp/ctl.txt" || true)

if [ "$ctl" -ne 0 ]; then
    echo "check-simd-wasm: FAIL — the instrument is broken, not the compiler." >&2
    echo "  A build with NO -msimd128 matched $ctl vector instruction(s); it must match zero." >&2
    echo "  Until that holds, a green result above proves nothing. Pattern: $PAT   Disassembler: $DIS" >&2
    exit 1
fi

if [ "$rel" -eq 0 ]; then
    echo "check-simd-wasm: FAIL — a --release --target wasm build contains NO v128 instructions." >&2
    echo "  The wasm target vectorizes ONLY with -msimd128, which the driver passes for every wasm" >&2
    echo "  build (kama.driver.cpp, just above the release/debug split). It is almost certainly gone." >&2
    echo "  Control build (no flag) scored 0 as expected, so the disassembler and pattern are sound." >&2
    exit 1
fi

# The vectorized code must also be CORRECT. The probe's main seeds real data and checks the sums, so a
# nonzero exit is a wrong answer, not a missing feature — a grep alone would pass a miscompile.
if command -v node >/dev/null 2>&1 && [ -f "$tmp/probe" ]; then
    rc=0; node "$tmp/probe" >/dev/null 2>"$tmp/run.err" || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "check-simd-wasm: FAIL — the vectorized wasm build runs but computes the WRONG answer (exit $rc)." >&2
        sed 's/^/  /' "$tmp/run.err" >&2; exit 1
    fi
fi

echo "check-simd-wasm: PASS ($rel v128 instruction(s) in --release, 0 without -msimd128 — the control holds)"
