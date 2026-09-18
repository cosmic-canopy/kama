#!/bin/sh
# check-winargv.sh — kama's command-line splitter agrees with CommandLineToArgvW (KR-73).
#
# On Windows the runtime converts the command line to UTF-8 itself (kama__cmdline_split, kama_runtime.h)
# rather than calling CommandLineToArgvW, because that function returns LocalAlloc memory — a FOREIGN
# allocation a declared `@globalAllocator` cannot serve and `--no-heap` cannot admit. The rules it implements
# are documented but full of corners (backslash runs before a quote, `""` inside quotes, a quoted argv[0]),
# and the only proof worth having is the function itself: this compiles tests/support/winargv_probe.c and
# runs both splitters over the same cases. A single mismatch fails.
#
# Windows only — the oracle is Windows. Everywhere else it SKIPS with exit 0, like check-softfloat does
# without its toolchain, so the suite stays green where the question cannot be asked.
#
# check-legs: native
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "SKIP check-winargv (Windows only: it diffs kama's splitter against CommandLineToArgvW)"; exit 0 ;;
esac
CC=${CC:-clang}
command -v "$CC" >/dev/null 2>&1 || { echo "check-winargv: no $CC on PATH" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$CC" -std=c11 -I"$ROOT/include" "$ROOT/tests/support/winargv_probe.c" -o "$tmp/probe.exe" -lshell32 \
    > "$tmp/build.log" 2>&1 || { echo "check-winargv: FAIL — the probe did not build" >&2; sed -n '1,20p' "$tmp/build.log" >&2; exit 1; }

if "$tmp/probe.exe" > "$tmp/out" 2>&1; then
    echo "check-winargv: PASS ($(tail -1 "$tmp/out"))"
else
    echo "check-winargv: FAIL — kama__cmdline_split disagrees with CommandLineToArgvW:" >&2
    grep -A2 "MISMATCH" "$tmp/out" | sed 's/^/  /' >&2
    exit 1
fi
