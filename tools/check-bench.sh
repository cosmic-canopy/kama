#!/bin/sh
# check-bench.sh — every kama program under bench/ still passes `kama check` (KR-321).
#
# Nothing else reaches bench/: run_tests.sh compiles tests/, and each guard what it names. So a program there stopped
# compiling in silence until a later session needed its numbers (bench/fiber/coldswitch.kama, found 2026-11-04). A
# project is checked once, through its kama.json; every other .kama file is a program of its own. `kama check` reads
# every configuration a file's gates name, so a program for another OS (bench/winio, bench/server/kama/fibers.kama) is
# read on every host.
#
# The C beside them is not compiled. Those are the prototypes kama's numbers are measured against (bench/jobs/pool.c,
# bench/jobs/probes/pool_seam's seam.c), nothing kama emits reaches them, and each wants its own OS's flags: seam.c's
# `fork` and SIGUSR1 have no Windows twin. A compile per program would also add minutes to a gate for code kama
# cannot break. Ruled when this guard was written.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-bench: $KAMA not built" >&2; exit 1; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

n=0
bad=0
check() {
    n=$((n + 1))
    if ! "$KAMA" check "$1" > "$tmp/log" 2>&1; then
        echo "check-bench: FAIL — \`kama check ${1#"$ROOT"/}\`:" >&2
        sed -n '1,12p' "$tmp/log" >&2
        bad=$((bad + 1))
    fi
}

for m in $(find "$ROOT/bench" -name kama.json | sort); do check "$m"; done
for f in $(find "$ROOT/bench" -name '*.kama' | sort); do
    d=$(dirname "$f")
    if [ -f "$d/kama.json" ] || [ -f "$d/../kama.json" ]; then continue; fi   # a project's own source: checked above
    check "$f"
done

if [ "$bad" -ne 0 ]; then
    echo "check-bench: FAIL — $bad of $n bench programs no longer pass \`kama check\`" >&2
    exit 1
fi
echo "check-bench: OK ($n bench programs pass \`kama check\`)"
