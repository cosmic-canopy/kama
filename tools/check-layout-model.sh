#!/bin/sh
# check-layout-model.sh — kama's layout model agrees with the C compiler on every arch it covers (KR-94).
#
# A compile-time `sizeof`/`alignof` is folded from a model of the target (CEmitter::targetLayoutModel and the
# layout walk), and every type it bakes gets a `_Static_assert(sizeof(T) == N && _Alignof(T) == A)` in the
# emitted C. That makes a wrong model a BUILD failure — but only on a target something builds. The suite builds
# the host and wasm32; this guard reaches the rest. It transpiles tests/comptime_layout.kama, which bakes every
# kind of type there is, for each modelled arch and hands the C to a compiler for that triple:
#   - a hosted triple to `zig cc`, whose bundled libc headers make a cross compile need nothing installed;
#   - a bare-metal triple to `clang -fsyntax-only`, with stub `<string.h>`/`<math.h>` (a freestanding target
#     has no libc, and only declarations are needed to reach the assertions).
# A toolchain this host lacks skips its triples and says so; a compile that fails for ANY reason fails the guard,
# since a C file that does not compile has not had its assertions checked.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/comptime_layout.kama"
if [ ! -x "$KAMA" ]; then echo "check-layout-model: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export ZIG_GLOBAL_CACHE_DIR="$tmp/zig" ZIG_LOCAL_CACHE_DIR="$tmp/zig"
mkdir -p "$tmp/stub"
printf '#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);\nvoid *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t); size_t strlen(const char *);\n' > "$tmp/stub/string.h"
: > "$tmp/stub/math.h"

fail=0; checked=""; skipped=""
# compile <kama triple> <how> <compiler triple> [extra flags…]
compile() {
    t=$1; how=$2; ct=$3; shift 3
    c="$tmp/$t.c"
    if ! "$KAMA" transpile "$FIXTURE" --target "$t" -o "$c" > "$tmp/$t.kama.log" 2>&1; then
        echo "check-layout-model: FAIL — kama could not transpile for $t" >&2; sed -n '1,5p' "$tmp/$t.kama.log" >&2; fail=1; return
    fi
    if [ "$how" = zig ]; then
        zig cc -target "$ct" "$@" -I"$ROOT/include" -c -o "$tmp/$t.o" "$c" > "$tmp/$t.cc.log" 2>&1 && rc=0 || rc=$?
    else
        clang -target "$ct" -ffreestanding -DKAMA_TARGET_EMBEDDED -isystem "$tmp/stub" \
              -Wno-implicit-function-declaration "$@" -I"$ROOT/include" -fsyntax-only "$c" > "$tmp/$t.cc.log" 2>&1 && rc=0 || rc=$?
    fi
    if [ "$rc" -ne 0 ]; then
        if grep -q 'static assertion failed' "$tmp/$t.cc.log"; then
            echo "check-layout-model: FAIL — kama's layout model of $t disagrees with the C compiler:" >&2
            grep -A1 'static assertion failed' "$tmp/$t.cc.log" | sed -n '1,8p' >&2
        else
            echo "check-layout-model: FAIL — the $t C did not compile, so its assertions were never checked:" >&2
            grep 'error' "$tmp/$t.cc.log" | sed -n '1,5p' >&2
        fi
        fail=1; return
    fi
    checked="$checked $t"
}

if command -v zig > /dev/null 2>&1; then
    compile x86_64-linux-gnu    zig x86_64-linux-gnu
    compile aarch64-linux-gnu   zig aarch64-linux-gnu
    compile riscv64-linux-gnu   zig riscv64-linux-gnu
    compile arm-linux-gnueabihf zig arm-linux-gnueabihf
    compile x86_64-windows-gnu  zig x86_64-windows-gnu      # LLP64: a 4-byte `clong`
    compile aarch64-windows-gnu zig aarch64-windows-gnu
    compile x86_64-macos-none   zig x86_64-macos            # Darwin caps a vector's alignment at 16
    compile wasm32-emscripten-none zig wasm32-wasi-musl -D_WASI_EMULATED_SIGNAL -Wno-implicit-function-declaration
else
    skipped="$skipped hosted-triples(no zig)"
fi
if command -v clang > /dev/null 2>&1; then
    for t in thumbv7em-none-eabihf thumbv6m-none-eabi aarch64-none-none x86_64-none-none riscv32-none-elf xtensa-none-elf; do
        if printf 'int x;\n' | clang -target "$t" -ffreestanding -fsyntax-only -x c - > /dev/null 2>&1; then
            compile "$t" clang "$t"
        else
            skipped="$skipped $t(clang knows no such target)"
        fi
    done
else
    skipped="$skipped bare-metal-triples(no clang)"
fi

[ "$fail" -eq 0 ] || exit 1
[ -n "$checked" ] || { echo "check-layout-model: SKIP — no cross toolchain on this host:$skipped"; exit 0; }
echo "check-layout-model: PASS ($(echo $checked | wc -w | tr -d ' ') targets agree with the model:$checked)${skipped:+; skipped:$skipped}"
