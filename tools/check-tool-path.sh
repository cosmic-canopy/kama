#!/bin/sh
# check-tool-path.sh — a failed C compile names the compiler the OS ran, not only the name kama was given.
#
# `kama: clang failed (exit 1)` named the configured compiler, so with emsdk's `upstream/bin` ahead of msys2's
# on PATH a native build ran emsdk's clang, which has no Windows sysroot, failed on <stdio.h>, and nothing said
# which clang that was: it read as a broken install (0.9.805). Here a `clang` that fails comes first on PATH and
# the failure must name its path. A failed `ar` is named the same way, and once: it used to be followed by a
# second line blaming the compiler.
#
# The stubs are kama programs rather than shell scripts, because on Windows cmd.exe runs a build's commands and
# could not start a script. Each prints `clang version` first, since the driver asks a compiler's family with
# `-v` before it compiles anything.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-tool-path: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
exe=
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=.exe ;; esac
fail() { echo "check-tool-path: FAIL — $1" >&2; [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/    /' >&2; exit 1; }

mkdir -p "$tmp/ccbin" "$tmp/arbin"
printf 'import { core::println };\nfn uint8 main() { println(s: "clang version 0.0.0 (a stub that fails)"); return 1; }\n' > "$tmp/stub.kama"
"$KAMA" build "$tmp/stub.kama" -o "$tmp/ccbin/clang$exe" >"$tmp/stub.out" 2>&1 || fail "the stub did not build:" "$(cat "$tmp/stub.out")"
cp "$tmp/ccbin/clang$exe" "$tmp/arbin/ar$exe"
printf 'fn uint8 main() { return 0; }\n' > "$tmp/hello.kama"
where=$(basename "$tmp")

set +e
out=$(PATH="$tmp/ccbin:$PATH" "$KAMA" build "$tmp/hello.kama" --cc clang -o "$tmp/hello" 2>&1)
set -e
printf '%s' "$out" | grep -qF "kama: clang (" || fail "a failed compile did not name the clang that ran:" "$out"
printf '%s' "$out" | grep -qF "$where/ccbin/clang$exe) failed" || fail "the clang named is not the one first on PATH:" "$out"

set +e
out=$(PATH="$tmp/arbin:$PATH" "$KAMA" build "$tmp/hello.kama" --select OUTPUT=STATIC -o "$tmp/libhello.a" 2>&1)
set -e
printf '%s' "$out" | grep -qF "$where/arbin/ar$exe) failed" || fail "a failed ar did not name the ar that ran:" "$out"
if printf '%s' "$out" | grep -q "kama: clang"; then fail "a failed ar was reported against the compiler too:" "$out"; fi

echo "check-tool-path: OK (a failed compile and a failed ar each name the program PATH resolved, once)"
