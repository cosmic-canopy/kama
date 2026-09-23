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
# cmd.exe's redirections stop at MAX_PATH (an 8.3 alias for every path on those lines); and the DEPENDENCY
# LINK, `.kama/deps/<name>`, which is a junction on Windows and was made by `cmd /c mklink /J` until
# 0.9.303 — cmd is MAX_PATH-bound however the path is spelled, so `osp()`'s `\\?\` (a Win32 file-call edge)
# could not reach it and a path dependency past 260 could not be linked. That failed LOUDLY (`kama install:
# cannot link dependency`, nonzero) — what was silent is that nothing in the tree noticed, which is the
# gap this case closes: the fixture suite covers a path dependency at a SHORT path, and the cases above
# cover long paths with NO dependency. What it does NOT cover is
# starting an executable past MAX_PATH — a CreateProcessW limit — which is why the deep cases are run from
# a second build into a short directory (the probe says so where it does it).
#
# The dependency half carries a SHORT-PATH CONTROL and runs it first: a path dependency broken outright
# would fail the deep case too, and the guard would then blame the length for it.
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
deplen=$(sed -n 's/^deplen=\([0-9][0-9]*\)$/\1/p' "$tmp/run.log" | head -1)

# VACUITY CONTROL. A guard that cannot fail proves nothing. If the temp root is short enough that the
# deepest project directory never passes 260, this run could go green on Windows while testing nothing
# about length — so refuse to report success instead. Two numbers, because the dependency case lives one
# level further down and could in principle be the only one that stayed short.
if [ -z "$len" ] || [ -z "$deplen" ]; then
    echo "check-compiler-path: FAIL — the probe printed no len=/deplen= line; got:" >&2
    sed -n '1,10p' "$tmp/run.log" >&2; exit 1
fi
if [ "$len" -le 260 ]; then
    echo "check-compiler-path: FAIL — the deepest project directory is only $len chars, so this run" >&2
    echo "  could not have tested the length axis. The temp root ($work) is too short." >&2
    exit 1
fi
if [ "$deplen" -le 260 ]; then
    echo "check-compiler-path: FAIL — the deepest DEPENDENT project is only $deplen chars, so this run" >&2
    echo "  could not have tested \`kama pkg install\` against a long path. The temp root ($work) is too short." >&2
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
        6[0-9]) echo "  case: a path DEPENDENCY at a SHORT path — the control. A path dependency is broken" >&2
                echo "        outright here, not by length; the >260 case below cannot be read until this passes" >&2 ;;
        7[0-9]) echo "  case: a path DEPENDENCY under the >260-character tree — \`.kama/deps/<name>\`, which is" >&2
                echo "        a junction on Windows (FSCTL_SET_REPARSE_POINT since 0.9.303; \`cmd /c mklink /J\`" >&2
                echo "        before it, and cmd.exe is MAX_PATH-bound however the path is spelled)" >&2 ;;
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
        ?8) echo "  step: \`kama pkg install <app>/kama.json\` could not be spawned, or exited nonzero." >&2
            echo "        This is what the pre-0.9.303 junction bug looked like: \`The system cannot find the" >&2
            echo "        path specified.\` / \`kama pkg install: cannot link dependency 'helper'\`" >&2 ;;
        ?9) echo "  step: install reported SUCCESS but .kama/deps/helper is not there — a silent half-install," >&2
            echo "        which the loud ?8 above is not. (A link that exists but does not RESOLVE is ?4" >&2
            echo "        instead: the build through it fails.)" >&2 ;;
    esac
    echo "  (the codes are enumerated in $FIXTURE; the compiler's own output follows)" >&2
    sed -n '1,20p' "$tmp/run.log" >&2
    exit 1
fi

echo "  ok: kama build handles a non-ASCII and a $len-char project directory (manifest, modules, -j 2, run)"
echo "  ok: kama pkg install links a path dependency at a $deplen-char project, and the build resolves through it"

# WHICH ROUTE DID THE NON-ASCII CASE ACTUALLY TAKE? Not a pass/fail — a note, because a green run means
# two different things on two volumes and the difference is invisible otherwise.
#
# kama spells a non-ASCII path for ld either by borrowing the directory's 8.3 alias or, when there is
# none, by standing the directory in with a junction (kama_win_shortpath). 8dot3 creation is a PER-VOLUME
# setting, ON by default on the system volume and OFF by default on every other volume of a Windows
# Server — so a dev box on `C:` exercises the alias route and the GitHub runner, whose temp is on `D:`,
# exercises the junction one. Before 0.9.439 only the first worked, the helper handed ld the unchanged
# non-ASCII path when no alias existed, and this guard went green on every dev box while CI read
# `cannot open output file …/???-??????/out/app.exe: Invalid argument`. Saying which route ran is what
# makes a local PASS legible.
#
# ⚠️ `fsutil`, NOT `cmd //c dir //x`. The obvious way to read this is the 8.3 column of a `dir /x`, and
# from msys2 bash that command exits 0 and prints NOTHING (measured) — so the note would have reported
# "no alias, junction route" on every host, which is a worse thing to ship than no note at all.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*)
    vol=$(printf '%s' "$(kama_native_path "$work")" | cut -c1-2)
    case "$(fsutil 8dot3name query "$vol" 2>/dev/null | tr -d '\r')" in
        *"is ENABLED on"*)
            echo "  note: $vol keeps 8.3 names, so the non-ASCII case took the ALIAS route. The JUNCTION"
            echo "        route is the one exercised where 8dot3 is off — the CI runner's D:, by default." ;;
        *"is DISABLED on"*)
            echo "  note: $vol keeps NO 8.3 names, so the non-ASCII case took the JUNCTION route." ;;
    esac
;; esac
