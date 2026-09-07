#!/bin/sh
# check-long-path.sh — std::fs works on a path longer than Windows' MAX_PATH (260).
#
# Portable ON PURPOSE, and that is the whole design of this guard: it runs on Linux and macOS, where
# 359 characters is unremarkable, so it goes green there from the day it is written and the Linux CI
# leg proves it is not vacuous. It fails only where the ceiling is real. A Windows-only guard would
# have no such witness — nobody would know whether it passed because the code was right or because it
# had quietly stopped testing anything.
#
# What it caught when written (msys2 UCRT64, 2026-09-06): exit 2 — `createDirAll` could not build the
# tree at all, because the shipped runtime's Windows branch used the ANSI/narrow-CRT path calls
# (`_mkdir`, `_open`, `_stat64`, `GetFileAttributesA`, `FindFirstFileA`), none of which can exceed
# MAX_PATH by construction.
#
# ⚠️ Length is only half of what that seam got wrong; the other half is encoding, and it has its own
# guard — see tools/check-path-unicode.sh.
#
# check-legs: native
#
# Native only: it runs a native binary against a real filesystem. Not `check-heavy` — it works in a
# private mktemp -d, never writes the worktree, and reaches the compiler through $KAMA.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/support/longpath_probe.kama"

if [ ! -x "$KAMA" ];   then echo "check-long-path: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-long-path: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
# ⚠️ The probe removes its own tree on success, so this only has to clean up after a FAILURE — which is
# exactly the case where a partial >260 tree is left behind. msys2's rm goes through the msys2 runtime's
# NT-path conversion and copes with those; cmd's `rmdir /s /q` was measured to cope too.
trap 'rm -rf "$tmp"' EXIT

bin="$tmp/lp"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) bin="$bin.exe" ;; esac

"$KAMA" build "$FIXTURE" -o "$bin" >"$tmp/build.log" 2>&1 || {
    echo "check-long-path: FAIL — the probe did not build" >&2; sed -n '1,20p' "$tmp/build.log" >&2; exit 1
}

# The probe is handed a SHORT directory and builds the depth itself (see its header for why). Give it
# its own subdirectory so the guard's own scratch files are not in the tree it walks.
work="$tmp/w"
mkdir -p "$work"

out=$("$bin" "$work" 2>&1) && rc=0 || rc=$?
len=$(printf '%s\n' "$out" | sed -n 's/^len=\([0-9][0-9]*\)$/\1/p')

# VACUITY CONTROL. A guard that cannot fail proves nothing (docs/ROADMAP_DETAIL.md, the SIMD probes).
# If the temp root is short enough that the composed path never passes 260, this guard would go green on
# Windows while testing nothing at all — so refuse to report success instead.
if [ -z "$len" ]; then
    echo "check-long-path: FAIL — the probe printed no len= line; got: $out" >&2; exit 1
fi
if [ "$len" -le 260 ]; then
    echo "check-long-path: FAIL — composed path is only $len chars, so this run could not have" >&2
    echo "  tested anything. The temp root ($work) is too short; nothing is proven either way." >&2
    exit 1
fi

if [ "$rc" -ne 0 ]; then
    echo "check-long-path: FAIL — std::fs broke on a $len-character path (probe exit $rc)" >&2
    case "$rc" in
        1)  echo "  no directory argument reached the probe" >&2 ;;
        2)  echo "  createDirAll could not create the tree" >&2 ;;
        3)  echo "  the tree was created but exists() denies it" >&2 ;;
        4|5)   echo "  stat on the deep DIRECTORY failed or misreported isDir" >&2 ;;
        6|7|8) echo "  writeFile/readFile failed at the bottom of the tree" >&2 ;;
        9|10|11) echo "  stat on the deep FILE failed or misreported size/isDir" >&2 ;;
        12|13)   echo "  readDir failed, or did not list the file it should have" >&2 ;;
        14|15|16) echo "  rename inside the deep directory failed" >&2 ;;
        17|18|19) echo "  remove/removeDirAll could not tear the tree down" >&2 ;;
        *)  echo "  unrecognized probe exit code" >&2 ;;
    esac
    echo "  (the codes are enumerated in $FIXTURE)" >&2
    exit 1
fi

echo "  ok: std::fs round-trips a $len-character path (create/stat/write/read/list/rename/remove)"
