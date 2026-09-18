#!/bin/sh
# check-lf-output.sh — every file kama writes is LF, on every host (KB-28).
#
# `kama pkg install` wrote kama.lock through a TEXT-mode ofstream, so on Windows every `\n` became `\r\n`: each
# committed lock came back "modified" after a first Windows install, byte-identical once normalised. The
# function's own comment promises "byte-stable => reproducible re-install", and a lock whose bytes depend on
# the host is not. The emitted C and the generated header went the same way. kama's own `std::fs` opens
# everything binary; now the driver does too.
#
# Portable ON PURPOSE: a text-mode stream IS binary on POSIX, so this is green there from the day it is written
# and red only on Windows, where the translation is real.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-lf-output: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-lf-output: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }
# `crs <file>`: how many carriage-return BYTES it holds. ⚠️ Not `grep -c $'\r'`: msys2's grep reads a text file
# in text mode and strips the CR at each end of line before matching, so on the one host this guard exists for
# it counted zero and PASSED against the unfixed compiler (measured writing it). `tr` sees the bytes.
crs() { LC_ALL=C tr -cd '\r' < "$1" | wc -c | tr -d ' '; }

mkdir -p "$tmp/dep/src" "$tmp/app/src" "$tmp/out"
printf '{ "name": "dep", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$tmp/dep/kama.json"
printf 'export { seven };\nfn int32 seven() { return 7; }\n' > "$tmp/dep/src/dep.kama"
printf '{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",\n  "dependencies": { "dep": { "path": "../dep" } }, "modules": { ".": { "visibility": "internal" } } }\n' > "$tmp/app/kama.json"
printf 'import { dep::seven };\nfn int32 main() { return seven(); }\n' > "$tmp/app/src/main.kama"

"$KAMA" pkg install "$tmp/app/kama.json" > "$tmp/install.log" 2>&1 || fail "pkg install failed:" "$tmp/install.log"
[ -f "$tmp/app/kama.lock" ] || fail "pkg install wrote no kama.lock"
n=$(crs "$tmp/app/kama.lock")
[ "$n" = 0 ] || fail "kama.lock holds $n carriage return(s) — its bytes depend on the host, so it is not byte-stable"

exe="$tmp/out/app"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe="$exe.exe" ;; esac
"$KAMA" build "$tmp/app/kama.json" --keep-c -o "$exe" > "$tmp/build.log" 2>&1 || fail "the build failed:" "$tmp/build.log"
found=0
for f in "$tmp/out"/*.c "$tmp/out"/*.h; do
    [ -f "$f" ] || continue
    found=$((found + 1))
    n=$(crs "$f")
    [ "$n" = 0 ] || fail "${f##*/} holds $n carriage return(s) — the emitted C differs by host"
done
[ "$found" -ge 2 ] || fail "--keep-c left fewer than two generated files to check (found $found), so this proved nothing"

echo "check-lf-output: PASS (kama.lock and $found generated C files are LF-only)"
