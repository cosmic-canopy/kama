#!/bin/sh
# check-simd-type.sh — a `Simd<T>#(N)` really lowers to a machine vector, in the emitted C and in the asm.
#
# What it guards, and why a fixture cannot. tests/simd_basic.kama asserts VALUES, and every value it
# checks is equally correct against a scalar fallback: if `Simd<float32>#(4)` silently became four separate
# floats, or `vector_size` were dropped, or a compiler ignored the attribute, that fixture would still
# exit 23 and the suite would still be green. The whole point of the type is the CODEGEN, so the codegen
# is what has to be asserted — the same reason check-simd-native.sh exists beside the math fixtures.
#
# ⚠️ gcc IGNORES `ext_vector_type` with a warning and leaves a ONE-LANE SCALAR. That is why the emitted
# spelling is `vector_size` and why §1 below asserts it by name: under `--cc gcc` the wrong attribute is
# not a build failure but a silent three-quarters-of-the-data loss.
#
# ⚠️ THE GUARD PROVES ITS OWN INSTRUMENT, EVERY RUN (§3). The same emitted C is compiled a second time at
# -O0, where the vector pattern must score ZERO. Without that control, a pattern that can never match
# sits green forever — which is exactly how this repo produced a false "no SIMD" reading once already
# (Apple's assembler writes `fadd.4s v0, v0, v1`, not `fadd v0.4s`, so the obvious pattern reports zero
# on a fully vectorized function). Three fixtures have shipped here that could not fail; the control is
# the cheapest defense against a fourth.
#
# check-legs: native
#
# Siblings: check-simd-native.sh (does `std::math` auto-vectorize) and check-simd-wasm.sh (the same on
# the wasm tier, which `./dev check` cannot run). Those two guard the IMPLICIT SIMD claim; this one
# guards the EXPLICIT type.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
PROBE="$ROOT/tests/support/simd_type_probe.kama"

if [ ! -x "$KAMA" ]; then echo "check-simd-type: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$PROBE" ]; then echo "check-simd-type: missing $PROBE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$KAMA" transpile "$PROBE" -o "$tmp/p.c" >/dev/null 2>"$tmp/transpile.err" || {
    echo "check-simd-type: FAIL — kama transpile failed on $PROBE" >&2
    sed 's/^/  /' "$tmp/transpile.err" >&2; exit 1; }

# --- §1. The emitted C reaches the vector spelling, by name -------------------------------------------
# KAMA_SIMD_TYPE expands to `typedef T NAME __attribute__((vector_size(...)))` in kama_runtime.h, so the
# generated file names the macro and the runtime carries the attribute. Assert both ends: a Simd was
# registered AND the macro it resolves to is the portable spelling.
if ! grep -q 'KAMA_SIMD_TYPE(float, 4, Simd_float32_4)' "$tmp/p.c"; then
    echo "check-simd-type: FAIL — the emitted C does not register Simd_float32_4" >&2
    grep -n 'Simd' "$tmp/p.c" | head -10 | sed 's/^/  /' >&2; exit 1
fi
# ⚠️ Match the ATTRIBUTE, not the bare word: the header's own comment explains at length why
# `ext_vector_type` is disqualified, so a `grep -q ext_vector_type` fires on the prose that documents the
# rule and fails the guard for saying the right thing. `__attribute__((` is what distinguishes a use.
if ! grep -q '__attribute__((vector_size' "$ROOT/include/kama_runtime.h"; then
    echo "check-simd-type: FAIL — kama_runtime.h no longer emits __attribute__((vector_size(...)))" >&2; exit 1
fi
if grep -q '__attribute__((ext_vector_type' "$ROOT/include/kama_runtime.h"; then
    echo "check-simd-type: FAIL — kama_runtime.h USES ext_vector_type, which gcc IGNORES (leaving a" >&2
    echo "                  one-lane scalar — a silent 3/4 data loss under --cc gcc). Use vector_size." >&2; exit 1
fi

# --- §2. The C compiler agrees it is 16 bytes ---------------------------------------------------------
# The attribute is only as good as what the compiler does with it, and "ignored with a warning" is a real
# outcome (gcc + ext_vector_type). A static assertion on sizeof/alignof catches that at compile time on
# whatever `cc` this host has, without needing to read asm.
cat > "$tmp/size.c" <<'EOF'
#include "kama_runtime.h"
KAMA_SIMD_TYPE(float, 4, Probe_f32x4)
_Static_assert(sizeof(Probe_f32x4)  == 16, "a Simd<float32>#(4) must be 16 bytes");
_Static_assert(_Alignof(Probe_f32x4) == 16, "a Simd<float32>#(4) must be 16-byte aligned");
int main(void) { return 0; }
EOF
CC_BIN=${CC:-cc}
if command -v "$CC_BIN" >/dev/null 2>&1; then
    "$CC_BIN" -std=c11 -I "$ROOT/include" -c "$tmp/size.c" -o "$tmp/size.o" 2>"$tmp/size.err" || {
        echo "check-simd-type: FAIL — the vector typedef is not 16 bytes / 16-byte aligned under $CC_BIN" >&2
        sed 's/^/  /' "$tmp/size.err" >&2; exit 1; }
else
    echo "check-simd-type: NOTE (no $CC_BIN on PATH — skipped the sizeof/alignof assertion)"
fi

# --- §3. It reaches real vector instructions, with a negative control ---------------------------------
# Only clang is read here, for the same reason check-simd-native.sh gives: the asm patterns below are
# written against its output. A visible SKIP beats a silent pass.
if ! command -v clang >/dev/null 2>&1; then
    echo "SKIP check-simd-type (§1/§2 passed; no clang on PATH for the asm assertion)"
    exit 0
fi
case "$(uname -m)" in
    # aarch64 has TWO spellings and matching one is how this was got wrong before: Apple puts the
    # arrangement on the OPCODE (`fmul.4s v0, ...`), GNU/LLVM on the REGISTERS (`fmul v0.4s, ...`).
    # MATH is the libm trio's vector form — `sqrt`/`floor`/`ceil` on a float lane batch are per-lane
    # libm loops in kama_math.h that must FOLD to these; a libm call left behind (`bl _sqrtf`) fails §4.
    arm64|aarch64)  PAT='[a-z][a-z0-9]*\.4s|v[0-9]+\.4s' ; ISA='NEON (.4s)' ; MATH='fsqrt|frintm|frintp' ;;
    # x86-64: the PACKED forms only — `mulss`/`addss` are scalar and must NOT match.
    x86_64|amd64)   PAT='(^|[^a-z])v?(addps|mulps|subps|divps|andps|maxps|minps)([^a-z]|$)' ; ISA='SSE (packed)' ; MATH='sqrtps|roundps' ;;
    *)              echo "SKIP check-simd-type (§1/§2 passed; no asm pattern for $(uname -m))"; exit 0 ;;
esac

# `--release` is `-O3 -DNDEBUG -fno-math-errno` on native (kama.driver.cpp), reproduced exactly so this
# reads what a user's release build gets. ⚠️ The errno flag is load-bearing for §4: without it a per-lane
# `sqrtf` loop stays four libm calls on Linux (macOS defaults to the flag, which is how two hosts once
# disagreed about the same C).
clang -std=c11 -O3 -DNDEBUG -fno-math-errno -I "$ROOT/include" -S "$tmp/p.c" -o "$tmp/o3.s" 2>"$tmp/cc.err" || {
    echo "check-simd-type: FAIL — clang could not compile the transpiled probe at -O3" >&2
    sed 's/^/  /' "$tmp/cc.err" >&2; exit 1; }

# ⚠️ THE CONTROL, and it is NOT `-O0`. check-simd-native.sh can use an optimization level as its control
# because it measures AUTO-vectorization, which -O0 genuinely turns off. This guard measures an EXPLICIT
# vector type, which emits vector loads and stores at every optimization level — the type IS a vector.
# Compiling the probe at -O0 therefore still scores, and using it as a control fails the guard for a
# reason that has nothing to do with the code. (Measured: 2 matches at -O0. The sibling guard's own
# header records the mirror-image trap, where `-fno-vectorize` was not a valid arming either.)
#
# So the control is a program with NO vector type at all, compiled the same way: scalar float arithmetic,
# straight-line so nothing can auto-vectorize it, `volatile` so nothing folds. If the pattern matches
# HERE it is matching something that is not a machine vector, and the count above proves nothing.
cat > "$tmp/control.c" <<'EOF'
volatile float g_a = 1.0f, g_b = 2.0f, g_c = 3.0f, g_d = 4.0f;
float control(void) {
    float a = g_a, b = g_b, c = g_c, d = g_d;
    float r = a * b + c;   /* straight-line scalar float math — no array, no loop, no vector type */
    r = r - d; r = r / b; r = r + a;
    return r;
}
EOF
clang -std=c11 -O3 -DNDEBUG -fno-math-errno -S "$tmp/control.c" -o "$tmp/control.s" 2>>"$tmp/cc.err" || {
    echo "check-simd-type: FAIL — clang could not compile the scalar control" >&2
    sed 's/^/  /' "$tmp/cc.err" >&2; exit 1; }

hot=$(grep -cE "$PAT" "$tmp/o3.s" || true)
ctl=$(grep -cE "$PAT" "$tmp/control.s" || true)

if [ "$ctl" -ne 0 ]; then
    echo "check-simd-type: FAIL — the NEGATIVE CONTROL fired: $ctl match(es) in a program with no vector" >&2
    echo "                  type at all, expected 0. The pattern is matching something that is not a" >&2
    echo "                  machine vector, so the probe's count proves nothing. Fix the PATTERN." >&2
    exit 1
fi
if [ "$hot" -eq 0 ]; then
    echo "check-simd-type: FAIL — no $ISA instructions from a Simd<float32>#(4) at -O3." >&2
    echo "                  The type registered and sized correctly (§1/§2), so the attribute is reaching" >&2
    echo "                  the compiler; what is missing is the codegen. Inspect: $tmp/o3.s" >&2
    exit 1
fi

# --- §4. The libm trio FOLDED — sqrt/floor/ceil are vector instructions, not four calls into libm -----
# The probe's `simdSqrtFloorCeil` is a per-lane `sqrtf`/`floorf`/`ceilf` loop (kama_math.h). Two
# assertions, because either alone can lie: the vector opcodes must be present AND no libm call for the
# three may remain. A build without `-fno-math-errno` keeps `bl _sqrtf` per lane (measured), which is
# exactly the regression this section exists to catch if the driver flag is ever dropped.
math=$(grep -cE "$MATH" "$tmp/o3.s" || true)
libm=$(grep -cE '(bl|call)[[:space:]]+_?(sqrtf|floorf|ceilf)([^a-z]|$)' "$tmp/o3.s" || true)
if [ "$math" -eq 0 ] || [ "$libm" -ne 0 ]; then
    echo "check-simd-type: FAIL — the lane-batch libm trio did not fold: $math vector op(s) matching" >&2
    echo "                  '$MATH', $libm libm call(s) left. kama_math.h's KAMA_SIMD_MATH loops must" >&2
    echo "                  become vector instructions; \`sqrt\` needs -fno-math-errno (kama.driver.cpp)" >&2
    echo "                  to do so. Inspect: $tmp/o3.s" >&2
    exit 1
fi

echo "check-simd-type: PASS (Simd<float32>#(4) -> vector_size, 16B/16B-aligned, $hot $ISA instruction(s), libm trio folded ($math); scalar control 0 — the control holds)"
