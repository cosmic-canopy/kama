#!/bin/sh
# check-comptime-width.sh — compile time computes `usize`, `isize` and `clong` at the TARGET's width (KR-357).
#
# Compile time modelled `usize` as an unsigned 64-bit on every target and had no `isize` or `clong` at all. On
# wasm32, `comptime usize Big = 4294967295; comptime usize Half = (Big + 1) / 2;` baked 2^31 where run time gives 0,
# and `comptime usize B = 4294967296;` was judged by its sign alone and baked for clang to truncate to 0 with a
# warning. The fixture tests/comptime_target_width runs the agreement on the native and wasm legs; this guard
# reaches the targets no leg runs here. It TRANSPILES for each, which needs no C toolchain, so it runs on every host:
#   - a value that does not fit a 32-bit `usize`/`isize` is refused for wasm32 and accepted for x86_64 Linux;
#   - a `clong` that overflows 32 bits is refused for Windows (LLP64) and accepted for x86_64 Linux;
#   - the 32-bit target bakes the wrapped `(Big + 1) / 2`, 0, and the 64-bit one 2^31.
# Each case is checked both ways, so an instrument that could never fail (a target the flag no longer reaches, a
# message reworded) fails the guard rather than passing it.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-comptime-width: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0; n=0

# refused <name> <target> <message substring> / accepted <name> <target>: transpile $tmp/<name>.kama for <target>
refused() {
    n=$((n + 1))
    if "$KAMA" transpile "$tmp/$1.kama" --target "$2" -o "$tmp/$1-$2.c" > "$tmp/$1-$2.log" 2>&1; then
        echo "check-comptime-width: FAIL — $1 was accepted for $2; it must be refused" >&2; fail=1
    elif ! grep -qF -- "$3" "$tmp/$1-$2.log"; then
        echo "check-comptime-width: FAIL — $1 for $2 was refused, but not with \"$3\":" >&2
        sed -n '1,4p' "$tmp/$1-$2.log" >&2; fail=1
    fi
}
accepted() {
    n=$((n + 1))
    if ! "$KAMA" transpile "$tmp/$1.kama" --target "$2" -o "$tmp/$1-$2.c" > "$tmp/$1-$2.log" 2>&1; then
        echo "check-comptime-width: FAIL — $1 was refused for $2; it must build there:" >&2
        sed -n '1,4p' "$tmp/$1-$2.log" >&2; fail=1
    fi
}
# baked <name> <target> <C regex>: the transpiled C bakes this constant
baked() {
    n=$((n + 1))
    if ! grep -q -- "$3" "$tmp/$1-$2.c" 2>/dev/null; then
        echo "check-comptime-width: FAIL — $1 for $2 does not bake /$3/:" >&2
        grep -- '__Half' "$tmp/$1-$2.c" 2>/dev/null | sed -n '1,3p' >&2; fail=1
    fi
}

printf 'comptime usize B = 4294967296;\nfn uint8 main() { usize b = B; return 0; }\n' > "$tmp/usize_lit.kama"
refused  usize_lit wasm "\`usize\` (0 to 4294967295)"
accepted usize_lit x86_64-linux-gnu

printf 'comptime fn isize next(isize n) { return n + 1; }\ncomptime isize X = next(n: 2147483647);\nfn uint8 main() { isize x = X; return 0; }\n' > "$tmp/isize_ovf.kama"
refused  isize_ovf wasm "overflows the 32-bit signed type"
accepted isize_ovf x86_64-linux-gnu

printf 'comptime clong L = 2147483647;\ncomptime clong L2 = L + 1;\nfn uint8 main() { clong l = L2; return 0; }\n' > "$tmp/clong_ovf.kama"
refused  clong_ovf x86_64-windows-gnu "overflows the 32-bit signed type"
accepted clong_ovf x86_64-linux-gnu

printf 'comptime usize Big = 4294967295;\ncomptime usize Half = (Big + 1) / 2;\nfn uint8 main() { usize h = Half; return 0; }\n' > "$tmp/usize_half.kama"
accepted usize_half wasm
baked    usize_half wasm '__Half = 0u*;'
accepted usize_half x86_64-linux-gnu
baked    usize_half x86_64-linux-gnu '__Half = 2147483648u*;'

[ "$fail" -eq 0 ] || exit 1
echo "check-comptime-width: PASS ($n checks: usize, isize and clong at wasm32, Windows LLP64 and x86_64 Linux widths)"
