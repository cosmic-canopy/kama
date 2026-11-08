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

# From §5 on this builds static libraries for bare metal, and on a Mac that takes an archiver that keeps ELF objects, which Apple's `ar`
# does not (kama refuses it, naming the choices). GitHub's macOS runners keep Homebrew's LLVM off PATH: link its `llvm-ar`
# alone into this guard's own directory, so `clang` stays the one PATH names. With none at all, those are skipped here; the
# Linux and Windows legs run them, whose `ar` writes any format.
if [ "$(uname -s)" = Darwin ] && ! command -v llvm-ar >/dev/null 2>&1 && ! command -v zig >/dev/null 2>&1; then
    for d in /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@*/bin /usr/local/opt/llvm/bin; do
        if [ -x "$d/llvm-ar" ]; then mkdir -p "$tmp/bin"; ln -s "$d/llvm-ar" "$tmp/bin/llvm-ar"; PATH="$tmp/bin:$PATH"; export PATH; break; fi
    done
    if ! command -v llvm-ar >/dev/null 2>&1; then
        echo "check-embedded: SKIP from §5 on (this Mac has no archiver that keeps ELF objects: put llvm-ar or zig on PATH)"
        echo "PASS check-embedded (--target embedded: guarded freestanding entry + libc-free object; step-4 @interrupt/@section; step-6a inline asm)"
        exit 0
    fi
fi

# 5. KR-341 — `std::concurrent` and `std::time` on bare metal: `Atomic` (whose `wait` POLLS the cell, since an
#    `@interrupt` handler is the only other party), `Locked` with no lock, `cpuCount()` and `Duration` build, with no
#    thread, no OS wait, no table and no clock: a wait with no deadline must not name `kama_now_mono_ns`, which a board
#    supplies (KR-341's R2), so a board that declares no clock still links a program that waits.
CONC="$ROOT/tests/support/embedded_concurrent.kama"
if [ ! -f "$CONC" ]; then echo "check-embedded: missing $CONC" >&2; exit 1; fi
if ! "$KAMA" build "$CONC" --target embedded --select OUTPUT=STATIC "$@" -o "$tmp/conc/libconc.a" >/dev/null 2>"$tmp/conc.err"; then
    echo "check-embedded: FAIL — std::concurrent did not build for bare metal" >&2
    sed 's/^/  /' "$tmp/conc.err" >&2; exit 1
fi
if command -v nm >/dev/null 2>&1; then
    waits=$(nm "$tmp/conc/libconc.a" 2>/dev/null | grep -E ' U _?(os_sync_|syscall|WaitOnAddress|WakeByAddress|emscripten_futex|pthread_|kama__wait_table|clock_gettime|nanosleep|kama_now_mono_ns|kama_now_wall_ns|sysconf|memcmp)' | sort -u || true)
    if [ -n "$waits" ]; then
        echo "check-embedded: FAIL — std::concurrent on bare metal reaches a thread, an OS wait, the table or a clock:" >&2
        echo "$waits" | sed 's/^/  /' >&2; exit 1
    fi
fi

# 6 + 7 take a compiler with NO SYSROOT and an Arm backend, which is the bar a bare-metal build is held to (KR-341): a
#    board toolchain's headers are not kama's to assume. clang has one on every host here; where it is missing, say so.
BARE="clang --target=thumbv7m-none-eabi -mcpu=cortex-m3"
if ! printf 'int x;\n' | clang --target=thumbv7m-none-eabi -x c -c - -o "$tmp/probe.o" 2>/dev/null; then
    echo "check-embedded: SKIP §6-7 (no clang with an Arm backend)"
else
# 6. KR-341 — a board declared in kama: its monotonic clock (`@monotonicClock`), `sleep` and `Atomic.waitUntil` on it, a
#    collection, `sqrt` (tests/support/embedded_board/main.kama, which tools/check-mcu.sh also links with no C library and RUNS
#    on an emulated Cortex-M3). The archive must define the runtime's clock from the declaration, carry memcpy and sqrt
#    weak, and leave undefined only what every Arm toolchain's runtime supplies (the `__aeabi_*` soft-float and division
#    helpers) and the allocation funnel's malloc, which a board's --gc-sections drops when nothing reaches it.
BOARD="$ROOT/tests/support/embedded_board/main.kama"
if ! "$KAMA" build "$BOARD" --target thumbv7m-none-eabi --select OUTPUT=STATIC --cc "$BARE" -o "$tmp/board/libboard.a" \
        >/dev/null 2>"$tmp/board.err"; then
    echo "check-embedded: FAIL — a board declared in kama did not build for thumbv7m-none-eabi with no sysroot" >&2
    sed 's/^/  /' "$tmp/board.err" >&2; exit 1
fi
if command -v nm >/dev/null 2>&1; then
    nm "$tmp/board/libboard.a" 2>/dev/null > "$tmp/board.nm"
    for want in ' T kama_now_mono_ns' ' W memcpy' ' W sqrt' ' B kama_monotonic_clock'; do
        grep -q "$want\$" "$tmp/board.nm" || { echo "check-embedded: FAIL — the board archive has no '$want'" >&2; exit 1; }
    done
    awk '$2 ~ /^[TDBRW]$/ {print $3}' "$tmp/board.nm" | sort -u > "$tmp/board.def"
    awk '$1 == "U" {print $2}' "$tmp/board.nm" | sort -u | comm -23 - "$tmp/board.def" \
        | grep -vE '^(__aeabi_[a-z0-9]+|malloc|free|aligned_alloc|realloc)$' > "$tmp/board.undef" || true
    if [ -s "$tmp/board.undef" ]; then
        echo "check-embedded: FAIL — the board archive needs what no board supplies:" >&2
        sed 's/^/  /' "$tmp/board.undef" >&2; exit 1
    fi
fi

# 7. KR-341 — every std module on bare metal: one program per module importing every name it exports, for
#    thumbv7em-none-eabi with no sysroot. A name may be refused only as needing an operating system (it is set aside and
#    the program built without it), and a module only by saying the same of all of it; anything else is a failure, which
#    is how `std::collections` reading <string.h> stayed invisible until a program used one.
STD="$ROOT/lib/std"
built=0; refusedMods=0; platformMods=0
# A module made for one platform — every file gated to a named OS or ABI, as `std::app::android` is — is refused by its own gate
# on every other target, bare metal included. That is the right refusal: no operating system would do, only that one.
platformOnly() { for f in "$1"/*.kama; do head -1 "$f" | grep -qE '^file @compileFor\(([^)]*[(, ])?(OS|ABI)_' || return 1; done; }
for dir in $(cd "$STD" && find . -name '*.kama' -exec dirname {} \; | sort -u); do
    mod="std$(printf '%s' "${dir#.}" | sed 's#/#::#g')"
    names=""
    for f in "$STD/$dir"/*.kama; do
        first=$(head -1 "$f")
        case "$first" in *"file @compileFor(OS_WINDOWS)"*) continue ;; esac   # an OS twin: its sibling exports the name
        # The one `export { … };` a file has, which may span lines.
        names="$names $(awk '/^export \{/ { f = 1 } f { printf "%s ", $0 } f && /\}/ { exit }' "$f" \
                        | sed -E 's/.*export \{([^}]*)\}.*/\1/' | tr ',' ' ')"
    done
    names=$(printf '%s\n' $names | sort -u | tr '\n' ' ')
    [ -n "$(printf '%s' "$names" | tr -d ' ')" ] || continue
    tag=$(printf '%s' "$mod" | sed 's/::/_/g')
    tries=0
    while :; do
        tries=$((tries + 1))
        { printf 'import { '; first=1; for n in $names; do [ $first = 1 ] || printf ', '; printf '%s::%s' "$mod" "$n"; first=0; done
          printf ' };\nfn uint8 main() { return 0; }\n'; } > "$tmp/$tag.kama"
        if "$KAMA" build "$tmp/$tag.kama" --target thumbv7em-none-eabi --select OUTPUT=STATIC \
                --cc "clang --target=thumbv7em-none-eabi" -o "$tmp/$tag/lib.a" >/dev/null 2>"$tmp/$tag.err"; then
            built=$((built + 1)); break
        fi
        if grep -q "which needs an operating system" "$tmp/$tag.err"; then refusedMods=$((refusedMods + 1)); break; fi
        if grep -q 'by its own `file @compileFor(...)` gate' "$tmp/$tag.err" && platformOnly "$STD/$dir"; then
            platformMods=$((platformMods + 1)); break
        fi
        out=$(sed -n 's/.*`\([A-Za-z_][A-Za-z0-9_]*\)` needs an operating system, and a bare-metal build has none.*/\1/p' "$tmp/$tag.err" | sort -u)
        if [ -z "$out" ] || [ $tries -gt 3 ]; then
            echo "check-embedded: FAIL — $mod does not build for bare metal, and is not refused for needing an OS:" >&2
            grep -E 'error' "$tmp/$tag.err" | head -5 | sed 's/^/  /' >&2; exit 1
        fi
        for o in $out; do names=$(printf '%s\n' $names | grep -vx "$o" | tr '\n' ' '); done
        [ -n "$(printf '%s' "$names" | tr -d ' ')" ] || { refusedMods=$((refusedMods + 1)); break; }
    done
done
if [ "$built" -lt 31 ]; then
    echo "check-embedded: FAIL — only $built std modules built for bare metal (31 did when this was written)" >&2; exit 1
fi
echo "check-embedded: std on bare metal — $built modules build with no sysroot, $refusedMods refused for needing an OS, $platformMods for one platform"

# 8. KR-349 — `Atomic<T>` on a chip with no atomic of T's width: every width, every operation, a `Locked` and a `Shared<immutable>`
#    handle count (tests/support/embedded_atomic/main.kama, which tools/check-mcu.sh RUNS on each core). A Cortex-M0 has no
#    atomic read-modify-write and an M3 no 8-byte atomic, and there the C compiler's answer is a libatomic call no board has,
#    so the archive must name no `__atomic_*` or `__sync_*`. And the mask must be there: QEMU takes an interrupt only between
#    the blocks it translates, so nothing run there would notice its absence. The SysTick race builds too, for the M3.
for t in thumbv6m-none-eabi:cortex-m0 thumbv7m-none-eabi:cortex-m3; do
    tr=${t%%:*}; cpu=${t##*:}
    if ! "$KAMA" build "$ROOT/tests/support/embedded_atomic/main.kama" --target "$tr" --select OUTPUT=STATIC \
            --cc "clang --target=$tr -mcpu=$cpu" -o "$tmp/atomic-$tr/lib.a" >/dev/null 2>"$tmp/atomic-$tr.err"; then
        echo "check-embedded: FAIL — the every-width Atomic program did not build for $tr" >&2
        sed 's/^/  /' "$tmp/atomic-$tr.err" >&2; exit 1
    fi
    if command -v nm >/dev/null 2>&1 && nm "$tmp/atomic-$tr/lib.a" 2>/dev/null | grep -qE ' U _?_(_atomic|_sync)_'; then
        echo "check-embedded: FAIL — Atomic on $tr calls into a libatomic no board has:" >&2
        nm "$tmp/atomic-$tr/lib.a" | grep -E ' U _?_(_atomic|_sync)_' | sort -u | sed 's/^/  /' >&2; exit 1
    fi
done
if ! "$KAMA" build "$ROOT/tests/support/embedded_systick/main.kama" --target thumbv7m-none-eabi --select OUTPUT=STATIC \
        --cc "$BARE" -o "$tmp/systick/lib.a" >/dev/null 2>"$tmp/systick.err"; then
    echo "check-embedded: FAIL — the SysTick Atomic<uint64> program did not build for thumbv7m-none-eabi" >&2
    sed 's/^/  /' "$tmp/systick.err" >&2; exit 1
fi
printf '#include "kama_atomic.h"\nuint32_t f(uint32_t* c) { return kama__atomic_add4(c, 1, 5); }\n' \
    | clang --target=thumbv6m-none-eabi -mcpu=cortex-m0 -O2 -ffreestanding -DKAMA_TARGET_EMBEDDED -I "$ROOT/include" -S -x c - \
        -o "$tmp/mask.s"
for want in 'mrs.*primask' 'cpsid' 'msr.*primask'; do
    grep -qE "$want" "$tmp/mask.s" || { echo "check-embedded: FAIL — a Cortex-M0 fetchAdd has no '$want': its mask is gone" >&2; exit 1; }
done
if [ "$(grep -cE 'mrs.*primask' "$tmp/mask.s")" -lt 2 ]; then
    echo "check-embedded: FAIL — a Cortex-M0 fetchAdd does not read PRIMASK back, so an unprivileged caller would not be caught" >&2; exit 1
fi
# rv32imc has no A extension, so every read-modify-write masks `mstatus.MIE` there. Apple's clang has no RISC-V backend.
if printf 'int x;\n' | clang --target=riscv32-unknown-none-elf -x c -c - -o "$tmp/rvprobe.o" 2>/dev/null; then
    if ! "$KAMA" build "$ROOT/tests/support/embedded_atomic/main.kama" --target riscv32-unknown-none-elf --select OUTPUT=STATIC \
            --cc "clang --target=riscv32-unknown-none-elf -march=rv32imc -mabi=ilp32" -o "$tmp/atomic-rv/lib.a" \
            >/dev/null 2>"$tmp/atomic-rv.err"; then
        echo "check-embedded: FAIL — the every-width Atomic program did not build for riscv32 rv32imc" >&2
        sed 's/^/  /' "$tmp/atomic-rv.err" >&2; exit 1
    fi
    if command -v nm >/dev/null 2>&1 && nm "$tmp/atomic-rv/lib.a" 2>/dev/null | grep -qE ' U _?_(_atomic|_sync)_'; then
        echo "check-embedded: FAIL — Atomic on rv32imc calls into a libatomic no board has" >&2; exit 1
    fi
else
    echo "check-embedded: SKIP §8's rv32imc build (no clang with a RISC-V backend)"
fi
fi

echo "PASS check-embedded (--target embedded: guarded freestanding entry + libc-free object; step-4 @interrupt/@section; step-6a inline asm; std::concurrent on bare metal, Atomic.wait polls; a board in kama; every std module on bare metal; Atomic at every width with no libatomic, masked where the chip lacks one)"
