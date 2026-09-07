#!/bin/sh
# check-path-unicode.sh — std::fs round-trips a non-ASCII path, byte for byte.
#
# The companion to tools/check-long-path.sh, and the more commonly hit of the two defects it guards.
# `kama_os.h`'s Windows branch used the ANSI path functions, which decode a path in the process ANSI
# code page — while a kama string is UTF-8 BY DEFINITION (lib/std/path/path.kama:6-7 says so, and says
# kama deliberately has no OsString to hide the difference in). So a directory called `日本語` was
# mojibake before it reached the filesystem, with no MAX_PATH involved and at any length.
#
# Portable on purpose, same as the long-path guard: APFS and ext4 take UTF-8 bytes, so this goes green
# on Linux and macOS from the day it is written, and the Linux CI leg is the witness that it is not
# vacuous.
#
# ⚠️ The names live in the .kama source as `\u{...}` escapes and are NEVER passed through argv — msys2
# converts a native child's arguments through the ANSI code page, which would corrupt them before kama
# saw them. Only the short ASCII working directory crosses that boundary. The one name this script
# writes itself is composed here with `printf` of explicit UTF-8 bytes, for the same reason.
#
# ⚠️ No precomposed Latin accents anywhere in this guard. HFS+ normalizes them to NFD, so `café` would
# fail a byte-identity assertion on macOS for a reason unrelated to what is under test.
#
# check-legs: native
#
# Native only: runs a native binary against a real filesystem. Not `check-heavy` — private mktemp -d,
# no worktree writes, reaches the compiler through $KAMA.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/support/unicodepath_probe.kama"

if [ ! -x "$KAMA" ];    then echo "check-path-unicode: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-path-unicode: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

bin="$tmp/up"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) bin="$bin.exe" ;; esac

"$KAMA" build "$FIXTURE" -o "$bin" >"$tmp/build.log" 2>&1 || {
    echo "check-path-unicode: FAIL — the probe did not build" >&2; sed -n '1,20p' "$tmp/build.log" >&2; exit 1
}

work="$tmp/w"
mkdir -p "$work"

# The "kama did not write this one" case. `Привет-shell.txt` as explicit UTF-8 bytes — spelled with
# printf escapes rather than literal characters so this script stays pure ASCII on disk, exactly as the
# probe does. These bytes must match unicodepath_probe.kama's shellMade().
shellname=$(printf '\320\237\321\200\320\270\320\262\320\265\321\202-shell.txt')
: > "$work/$shellname" 2>/dev/null || {
    echo "check-path-unicode: FAIL — the filesystem under $work rejected a UTF-8 filename;" >&2
    echo "  nothing below this could be tested. (An ASCII-only filesystem is not a kama defect,"  >&2
    echo "  but it does mean this guard cannot report a pass here.)" >&2
    exit 1
}

# VACUITY CONTROL. If the name did not survive creation byte-for-byte, the probe's step 4 would fail
# for a reason that is the shell's, not kama's — so check the premise before blaming the subject.
if [ ! -f "$work/$shellname" ]; then
    echo "check-path-unicode: FAIL — the shell-created UTF-8 filename is not readable back by the" >&2
    echo "  shell itself. The premise is broken; this run proves nothing about kama." >&2
    exit 1
fi

rc=0
"$bin" "$work" >"$tmp/run.log" 2>&1 || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-path-unicode: FAIL — std::fs broke on a non-ASCII path (probe exit $rc)" >&2
    case "$rc" in
        1)  echo "  no directory argument reached the probe" >&2 ;;
        2|3)   echo "  createDirAll/exists failed on a CJK (3-byte-per-character) directory name" >&2 ;;
        4|5)   echo "  stat on the non-ASCII directory failed or misreported isDir" >&2 ;;
        6|7)   echo "  writeFile failed on a name mixing 2-byte Cyrillic and a 4-byte emoji" >&2 ;;
        8|9)   echo "  readFile could not read back what writeFile wrote" >&2 ;;
        10|11) echo "  readDir did not return kama's OWN filename byte-for-byte — the conversion" >&2
               echo "  is lossy in one direction or the other" >&2 ;;
        12|13) echo "  readDir did not return the filename the SHELL created — kama cannot read" >&2
               echo "  non-ASCII names it did not write" >&2 ;;
        14|15|16) echo "  rename between two non-ASCII names failed" >&2 ;;
        17|18)    echo "  removeDirAll could not tear the non-ASCII tree down" >&2 ;;
        *)  echo "  unrecognized probe exit code" >&2 ;;
    esac
    echo "  (the codes are enumerated in $FIXTURE)" >&2
    sed -n '1,10p' "$tmp/run.log" >&2
    exit 1
fi

echo "  ok: std::fs round-trips 2-, 3- and 4-byte UTF-8 path names (incl. a surrogate pair)"
