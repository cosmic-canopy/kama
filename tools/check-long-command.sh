#!/bin/sh
# check-long-command.sh — a legal manifest builds however long its C command lines get (KB-27).
#
# Every C compile and link goes through the host's shell, and cmd.exe stops at 8,191 characters. Each compile
# carries one `-I` per distinct `csources` DIRECTORY, so a package with enough of them — @kama/sodium has 120
# sources in 78 — could not build on Windows at all: cmd answered "The command line is too long." and kama
# reported `clang failed (exit 1)`, on a manifest that builds everywhere else. The driver now moves the lists
# IT generated (includes, inputs, objects) into response files when a command would not fit (fitCommand,
# kama.driver.cpp); a command that fits keeps the exact bytes it always had.
#
# Portable ON PURPOSE, like check-long-path.sh: 100 directories is unremarkable to `sh`, so this goes green
# on Linux and macOS from the day it is written and fails only where the ceiling is real. Both halves are
# asserted, so neither host is vacuous: where the command does not fit (Windows) a response file MUST have been
# written, and where it fits (POSIX) one must NOT — that is the promise that today's builds are untouched.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-long-command: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-long-command: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }

p="$tmp/proj"; out="$tmp/out"
mkdir -p "$p/src" "$p/csrc/inc" "$out"
N=100
i=0; srcs=""; sum="0"
: > "$p/csrc/decls.h"
while [ "$i" -lt "$N" ]; do
    n=$(printf '%03d' "$i")
    d="csrc/a_long_directory_name_for_primitive_family_$n/and_a_nested_one_for_variant_$n"
    mkdir -p "$p/$d"
    printf 'int kb_unit_%s(void) { return 1; }\n' "$n" > "$p/$d/unit_with_a_deliberately_long_file_name_so_the_object_list_is_over_the_limit_too_$n.c"
    printf 'int kb_unit_%s(void);\n' "$n" >> "$p/csrc/decls.h"
    srcs="$srcs\"$d/unit_with_a_deliberately_long_file_name_so_the_object_list_is_over_the_limit_too_$n.c\", "
    sum="$sum + kb_unit_$n()"
    i=$((i + 1))
done
printf 'int kb_sum(void);\n' > "$p/csrc/inc/kb.h"
printf '#include "decls.h"\nint kb_sum(void) { return %s; }\n' "$sum" > "$p/csrc/sum.c"
printf 'extern "kb.h";\nextern fn int32 kb_sum();\nunsafe fn int32 total() { return kb_sum(); }\nfn int32 main() { return total(); }\n' > "$p/src/main.kama"
printf '{ "name": "longcmd", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",\n  "csources": [%s"csrc/sum.c"], "cincludes": ["csrc/inc"], "modules": { ".": { "visibility": "internal" } } }\n' "$srcs" > "$p/kama.json"

exe="$out/longcmd"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe="$exe.exe"; windows=1 ;; *) windows=0 ;; esac

# Both build shapes reach the shell: the per-file `-j` pool, and `-j 1` (one job at a time, through system()).
for jobs in 4 1; do
    rm -rf "$out"; mkdir -p "$out"
    "$KAMA" build "$p/kama.json" --keep-c -j "$jobs" -o "$exe" > "$tmp/build.log" 2>&1 \
        || fail "a manifest with $N csources directories did not build at -j $jobs:" "$tmp/build.log"
    rc=0; "$exe" || rc=$?
    [ "$rc" = "$N" ] || fail "the program returned $rc at -j $jobs, expected $N (every csource linked in)"
    if [ "$windows" = 1 ]; then
        [ -f "$out/longcmd.includes.rsp" ] || fail "no response file at -j $jobs — the command was over cmd.exe's limit, so how did it run?"
        # One token per line, double-quoted: the GNU response-file form clang, gcc, zig cc, emcc and ar share.
        [ "$(grep -c '^"-I' "$out/longcmd.includes.rsp")" -ge "$N" ] || fail "the response file does not hold the include list:" "$out/longcmd.includes.rsp"
        # The LINK names every object. Lists leave the line only until it FITS (includes first), so the object
        # list is spilled only when it is over the limit by itself — which is why the source file names above
        # are as long as they are. Both arms: the pool's link line, and the one-invocation command `-j 1` takes.
        [ -f "$out/longcmd.objects.rsp" ] || fail "no object response file at -j $jobs — the link line names $N objects"
    else
        [ ! -f "$out/longcmd.includes.rsp" ] || fail "a response file was written for a command the host shell accepts — a build that fit changed shape"
    fi
done

# A STATIC library archives the same objects with `ar`, which is a third command line and its own code path.
rm -rf "$out"; mkdir -p "$out"
lib="$out/liblongcmd.a"
"$KAMA" build "$p/kama.json" --keep-c --select OUTPUT=STATIC -o "$lib" > "$tmp/static.log" 2>&1 \
    || fail "the same manifest did not archive as OUTPUT=STATIC:" "$tmp/static.log"
[ -f "$lib" ] || fail "OUTPUT=STATIC reported success and wrote no archive"
members=$(ar t "$lib" | wc -l | tr -d ' ')
[ "$members" -gt "$N" ] || fail "the archive holds $members member(s), expected more than $N (every csource, and kama's own C)"
if [ "$windows" = 1 ]; then
    [ -f "$out/liblongcmd.objects.rsp" ] || fail "no response file for \`ar\` — its line names $members objects, over cmd.exe's limit"
fi

echo "check-long-command: PASS ($N csources directories build and run at -j 4 and -j 1, and archive ($members members); response files exactly where the host shell needs them)"
