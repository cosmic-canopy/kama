#!/bin/sh
# check-embedded.sh — MCU campaign step 3 guard for `--target embedded` (the freestanding bare-metal build).
# A thumb ELF can't run on the CI host, so instead of running it we assert the two things that make a
# freestanding build correct, both host-checkable:
#   1. SHAPE — the emitted C carries the guarded freestanding entry (`int main(void)` that never returns,
#      no argv), selected by KAMA_TARGET_EMBEDDED.
#   2. FREESTANDING COMPILE — `kama build --target embedded` produces a `-ffreestanding -nostdlib` OBJECT
#      that compiles with NO libc pulled in (no undefined write/abort/malloc/... references).
# The value-only blink fixture (tests/embedded_blink.kama) is the subject; it also runs as an ordinary
# hosted fixture on the native/wasm/ASan legs (the #if guard keeps the hosted form intact). Fails (exit 1)
# with a diagnostic if either property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/embedded_blink.kama"

if [ ! -x "$KAMA" ]; then echo "check-embedded: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-embedded: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cfile="$tmp/blink.c"
obj="$tmp/blink.o"

# `--target embedded` is triple-AGNOSTIC by design: it hands the freestanding flags to whatever `--cc`
# emits, and a firmware author supplies their own cross compiler. That makes the host default fine on
# Linux (ELF) but wrong on macOS, where clang defaults to mach-o and rejects the ELF section names the
# @section fixture uses ("mach-o section specifier requires a segment and section separated by a comma").
# Pin a bare-metal ELF triple there — which is what a real firmware build does anyway. Compile-only
# (`-c`), so no sysroot or cross libc is needed; `nm` reads the resulting ARM ELF object fine.
if [ "$(uname -s)" = "Darwin" ]; then
    set -- --cc "clang --target=armv7m-none-eabi"
else
    set --
fi

# 1. SHAPE — transpile (target-agnostic) and confirm the guarded freestanding entry is emitted.
"$KAMA" transpile "$FIXTURE" -o "$cfile" >/dev/null
if ! grep -q '#if defined(KAMA_TARGET_EMBEDDED)' "$cfile"; then
    echo "check-embedded: FAIL — no #if defined(KAMA_TARGET_EMBEDDED) entry guard in emitted C" >&2; exit 1
fi
if ! grep -q 'int main(void)' "$cfile"; then
    echo "check-embedded: FAIL — freestanding 'int main(void)' entry not emitted" >&2; exit 1
fi

# 2. FREESTANDING COMPILE — drive the real `--target embedded` path to an object. This exercises the driver
#    flag plumbing (-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED -c) and proves the value-only program
#    compiles freestanding. Warnings (e.g. unused-allocator out-of-scope notes) are fine; a nonzero exit is not.
if ! "$KAMA" build "$FIXTURE" --target embedded "$@" -o "$obj" >/dev/null 2>"$tmp/build.err"; then
    echo "check-embedded: FAIL — 'kama build --target embedded' did not compile" >&2
    sed 's/^/  /' "$tmp/build.err" >&2; exit 1
fi
if [ ! -s "$obj" ]; then
    echo "check-embedded: FAIL — --target embedded produced no object" >&2; exit 1
fi

# 2b. BONUS (when nm is available) — assert the freestanding object references NO libc symbol. A value-only
#     firmware must not drag in write/abort/malloc/free/printf; if it does, the freestanding runtime seam leaks.
if command -v nm >/dev/null 2>&1; then
    leaked=$(nm "$obj" 2>/dev/null | grep -iE ' U (write|_write|abort|malloc|free|calloc|realloc|printf|memcpy)$' || true)
    if [ -n "$leaked" ]; then
        echo "check-embedded: FAIL — freestanding object references libc symbols:" >&2
        echo "$leaked" | sed 's/^/  /' >&2; exit 1
    fi
fi

# 3. MCU STEP 4 — ISR entry attribute + linker-section placement.
#   3a. SHAPE — transpile the ISR subject and confirm `@interrupt`/`@section` lower to the C attributes.
#       (Not compiled here: `__attribute__((interrupt))` is the Cortex-M/RISC-V/classic-ARM ISR calling
#        convention and the x86 CI host rejects it on a `void(void)`; the emitted-C shape is what we assert.)
ISR="$ROOT/tests/support/embedded_isr.kama"
isrc="$tmp/isr.c"
if [ ! -f "$ISR" ]; then echo "check-embedded: missing $ISR" >&2; exit 1; fi
"$KAMA" transpile "$ISR" -o "$isrc" >/dev/null
if ! grep -q '__attribute__((interrupt, used))' "$isrc"; then
    echo "check-embedded: FAIL — @interrupt did not lower to __attribute__((interrupt, used))" >&2; exit 1
fi
if ! grep -q '__attribute__((section(".isr_vector")))' "$isrc"; then
    echo "check-embedded: FAIL — @section did not lower to __attribute__((section(...)))" >&2; exit 1
fi
#   3b. FREESTANDING COMPILE — a section-only program (no @interrupt) must still build to a -nostdlib object,
#       proving `@section` placement does not break the freestanding path (the attribute is arch-agnostic).
SEC="$ROOT/tests/support/embedded_section.kama"
secobj="$tmp/section.o"
if [ ! -f "$SEC" ]; then echo "check-embedded: missing $SEC" >&2; exit 1; fi
if ! "$KAMA" build "$SEC" --target embedded "$@" -o "$secobj" >/dev/null 2>"$tmp/sec.err"; then
    echo "check-embedded: FAIL — @section program did not compile --target embedded" >&2
    sed 's/^/  /' "$tmp/sec.err" >&2; exit 1
fi
if [ ! -s "$secobj" ]; then
    echo "check-embedded: FAIL — @section --target embedded produced no object" >&2; exit 1
fi

# 4. MCU STEP 6a — inline assembly. `wfi`/`cpsid i`/`dsb` are ARM-only (the x86 host can't assemble them),
#    so we assert the emitted-C SHAPE: `asm("...")` inside an `unsafe fn` lowers to the volatile + memory-
#    clobber form, with multi-instruction strings correctly C-escaped (`\n`). Same transpile-grep rationale
#    as @interrupt above.
ASM="$ROOT/tests/support/embedded_asm.kama"
asmc="$tmp/asm.c"
if [ ! -f "$ASM" ]; then echo "check-embedded: missing $ASM" >&2; exit 1; fi
"$KAMA" transpile "$ASM" -o "$asmc" >/dev/null
if ! grep -qF '__asm__ __volatile__("wfi" : : : "memory")' "$asmc"; then
    echo "check-embedded: FAIL — asm(\"wfi\") did not lower to __asm__ __volatile__(\"wfi\" : : : \"memory\")" >&2; exit 1
fi
if ! grep -qF '__asm__ __volatile__("cpsid i\n\tdsb" : : : "memory")' "$asmc"; then
    echo "check-embedded: FAIL — multi-instruction asm did not C-escape the newline into one volatile asm" >&2; exit 1
fi

echo "PASS check-embedded (--target embedded: guarded freestanding entry + libc-free object; step-4 @interrupt/@section; step-6a inline asm)"
