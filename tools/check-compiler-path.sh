#!/bin/sh
# check-compiler-path.sh — the COMPILER builds a project that lives at a non-ASCII or >260-character path.
#
# The third of the path guards, and the one about kama.exe itself rather than a program it built.
# tools/check-long-path.sh and tools/check-path-unicode.sh prove std::fs is UTF-8 and long-path clean;
# this one proves `kama build` is, on the other side of the same boundary: its own argv, the manifest and
# sources it reads, the module discovery that lists directories, the output directory it creates, the
# generated C it writes, and the C compiler command lines it runs. Measured on Windows before the fix
# (0.9.218): the compiler's argv was ANSI-decoded, so a `日本語` project directory arrived as `???` and the
# first thing it printed was `kama: cannot create output directory` — the same first ceiling a 300-char
# directory hit. Both cases are here, plus the two combined.
#
# Portable on purpose, same as its two siblings: Linux and macOS take these paths without comment, so
# this goes green there from the day it is written and the Linux CI leg is the witness that it is not
# vacuous. It fails only where the ceiling is real.
#
# ⚠️ The probe is a kama-spawns-kama program, and that is load-bearing, not a convenience: msys2 converts
# a native child's arguments through the ANSI code page, so a non-ASCII path in THIS script's argv would be
# corrupted before kama saw it and the guard would be measuring the shell. Only three short ASCII strings
# cross argv here — the compiler, the working directory, the executable suffix — and the probe composes the
# pathological paths on the far side and hands them to the compiler through std::process, which is UTF-16
# at the edge on Windows.
#
# What "handles" covers, because the first ceiling was not the last (each of these was RED in turn):
# the compiler's own argv (the manifest); the manifest and sources it reads and the output directory it
# creates (`\\?\` at the OS edge); module discovery (mingw's opendir lists the WRONG directory on a `\\?\`
# path — FindFirstFile instead); the `-j` pool (a generated .bat is parsed in the console code page —
# CreateProcess with the line instead); and the link, where GNU ld and ar ANSI-decode their argv and
# cmd.exe's redirections stop at MAX_PATH (an 8.3 alias for every path on those lines). What it does NOT
# cover is starting an executable past MAX_PATH — a CreateProcessW limit — which is why the deep cases
# are run from a second build into a short directory (the probe says so where it does it).
#
# check-legs: native
#
# Native only: runs a native binary and the native compiler against a real filesystem. Not `check-heavy`
# — private mktemp -d, no worktree writes, reaches the compiler through $KAMA. The probe's inner builds
# ask for `-j 2` explicitly (the runner exports KAMA_BUILD_JOBS=1, which would otherwise bypass the Windows
# pool this guard exists to exercise).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/support/compilerpath_probe.kama"

if [ ! -x "$KAMA" ];    then echo "check-compiler-path: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-compiler-path: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
# ⚠️ This trap owns the cleanup, including the >260 tree and the program the probe has just run (Windows
# keeps a just-exited image open a moment, which is why the probe does not remove it). msys2's rm copes.
trap 'rm -rf "$tmp"' EXIT

bin="$tmp/cp"; suffix=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) bin="$bin.exe"; suffix=".exe" ;; esac

"$KAMA" build "$FIXTURE" -o "$bin" >"$tmp/build.log" 2>&1 || {
    echo "check-compiler-path: FAIL — the probe did not build" >&2; sed -n '1,20p' "$tmp/build.log" >&2; exit 1
}

work="$tmp/w"
mkdir -p "$work"

# The compiler's path is handed over as an ARGUMENT, so msys2 rewrites it to the native spelling on its
# own; kama_native_path is belt and braces for the case where $KAMA was exported already-native.
rc=0
"$bin" "$(kama_native_path "$KAMA")" "$work" "$suffix" >"$tmp/run.log" 2>&1 || rc=$?

len=$(sed -n 's/^len=\([0-9][0-9]*\)$/\1/p' "$tmp/run.log" | head -1)

# VACUITY CONTROL. A guard that cannot fail proves nothing. If the temp root is short enough that the
# deepest project directory never passes 260, this run could go green on Windows while testing nothing
# about length — so refuse to report success instead.
if [ -z "$len" ]; then
    echo "check-compiler-path: FAIL — the probe printed no len= line; got:" >&2
    sed -n '1,10p' "$tmp/run.log" >&2; exit 1
fi
if [ "$len" -le 260 ]; then
    echo "check-compiler-path: FAIL — the deepest project directory is only $len chars, so this run" >&2
    echo "  could not have tested the length axis. The temp root ($work) is too short." >&2
    exit 1
fi

if [ "$rc" -ne 0 ]; then
    echo "check-compiler-path: FAIL — the compiler broke on a project path (probe exit $rc)" >&2
    case "$rc" in
        1)  echo "  the probe did not receive its three arguments" >&2 ;;
        1[0-9]) echo "  case: a NON-ASCII project directory (3-byte CJK + 2-byte Cyrillic)" >&2 ;;
        2[0-9]) echo "  case: a >260-character project directory" >&2 ;;
        3[0-9]) echo "  case: a non-ASCII directory at the bottom of the >260-character tree" >&2 ;;
        5[0-9]) echo "  case: the non-ASCII directory again, with a non-ASCII OUTPUT NAME (-o .../<CJK>$suffix)" >&2 ;;
        *)  echo "  unrecognized probe exit code" >&2 ;;
    esac
    case "$rc" in
        ?1) echo "  step: createDirAll (the project's src/, or the short run directory) failed — std::fs, not the compiler" >&2 ;;
        ?2) echo "  step: writing kama.json / the two modules failed (std::fs — not the compiler)" >&2 ;;
        ?3) echo "  step: the compiler could not be SPAWNED" >&2 ;;
        ?4) echo "  step: \`kama build <proj>/kama.json -o … -j 2\` exited nonzero (into the project's out/, or" >&2
            echo "        for the deep cases the second build into a short directory the program is run from)" >&2 ;;
        ?5) echo "  step: the build reported success but the output does not exist" >&2 ;;
        ?6) echo "  step: the built program could not be spawned" >&2 ;;
        ?7) echo "  step: the built program ran but did not return 42 — the WRONG modules were compiled" >&2 ;;
    esac
    echo "  (the codes are enumerated in $FIXTURE; the compiler's own output follows)" >&2
    sed -n '1,20p' "$tmp/run.log" >&2
    exit 1
fi

echo "  ok: kama build handles a non-ASCII and a $len-char project directory (manifest, modules, -j 2, run)"
