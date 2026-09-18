#!/bin/sh
# check-cc-gcc.sh — `--cc gcc` builds a kama program, including the FFI callback seam.
#
# What it guards. docs/targets.md names gcc as a `cc` and the driver derives `gcc`→`g++` for C++
# `csources`, so gcc is a SUPPORTED path — and it could not build a hello-world for as long as that was
# written down (KR-72, found 2026-09-17 on msys2 gcc 16.2, reproduced on Linux gcc 13.3). Every compile
# command carried clang's `-Wno-error=incompatible-function-pointer-types`, and an unknown `-Wno-error=`
# spelling is a HARD ERROR on gcc, not an ignored flag:
#
#   cc1: error: '-Wno-error=incompatible-function-pointer-types': no option
#   '-Wincompatible-function-pointer-types'; did you mean '-Wincompatible-pointer-types'?
#
# ⚠️ Hello-world is the shallow half. The DEEP half is the FFI callback seam — handing a kama `fnptr` to
# a C callback field, which is why the demoting flag exists at all. clang names the function-pointer case
# separately, so kama promotes every incompatible pointer to an error and demotes that one back. gcc has
# ONE name for both, so the same promotion REJECTS the seam (measured: `-Werror=incompatible-pointer-types`
# naming the function-pointer assignment). That is why the driver asks the compiler what it is rather than
# stripping one flag, and why this guard builds `tests/callback_qsort.d` rather than just a `main`.
#
# A name test cannot answer the question: `cc` is gcc on most Linux and clang on macOS, and a cross prefix
# or a wrapper can be either. The driver probes `--version` once per distinct `cc` string, so THIS guard
# also pins the `cc` spelling — the one a name test gets wrong.
#
# SKIPS, visibly, where there is no gcc. A visible SKIP is legible in the run output; a silent pass is not.
# Same shape as check-simd-native.sh.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-cc-gcc: $KAMA not built" >&2; exit 1; fi

if ! command -v gcc >/dev/null 2>&1; then
    echo "SKIP check-cc-gcc (no gcc on PATH — this guard is about gcc's own flag spellings)"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() { echo "check-cc-gcc: FAIL — $1" >&2; exit 1; }

# 1. A hello-world, the shape that could not build at all.
cat > "$TMP/hello.kama" <<'KAMA'
fn int32 main() { return 7; }
KAMA
if ! "$KAMA" build "$TMP/hello.kama" --cc gcc -o "$TMP/hello" > "$TMP/hello.log" 2>&1; then
    sed 's/^/    /' "$TMP/hello.log" >&2
    fail "\`--cc gcc\` did not build a hello-world"
fi
"$TMP/hello" || rc=$?
[ "${rc:-0}" = 7 ] || fail "the gcc-built hello-world exited ${rc:-0}, expected 7"

# 2. The FFI callback seam: a kama `fnptr` into a C callback field, through gcc's own pointer diagnostics.
EXPECT=$(cat "$ROOT/tests/callback_qsort.d/expect")
if ! "$KAMA" build "$ROOT/tests/callback_qsort.d/sort.kama" --cc gcc -o "$TMP/cb" > "$TMP/cb.log" 2>&1; then
    sed 's/^/    /' "$TMP/cb.log" >&2
    fail "\`--cc gcc\` did not build the FFI callback fixture (tests/callback_qsort.d)"
fi
crc=0; "$TMP/cb" || crc=$?
[ "$crc" = "$EXPECT" ] || fail "the gcc-built callback fixture exited $crc, expected $EXPECT"

# 3. `--cc cc`, the spelling a name test gets wrong. Skipped where `cc` is absent.
if command -v cc >/dev/null 2>&1; then
    if ! "$KAMA" build "$TMP/hello.kama" --cc cc -o "$TMP/hello_cc" > "$TMP/cc.log" 2>&1; then
        sed 's/^/    /' "$TMP/cc.log" >&2
        fail "\`--cc cc\` did not build a hello-world (the driver must ASK what \`cc\` is, not guess)"
    fi
    crc=0; "$TMP/hello_cc" || crc=$?
    [ "$crc" = 7 ] || fail "the \`cc\`-built hello-world exited $crc, expected 7"
fi

ALSO=""
command -v cc >/dev/null 2>&1 && ALSO=", and so does cc"
echo "check-cc-gcc: PASS (gcc builds a program and the FFI callback seam$ALSO)"
