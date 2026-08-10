#!/bin/sh
# check-opt.sh — the kama compiler itself is built OPTIMIZED.
#
# Why this guard exists at all. Until 2026-08-10 the Makefile carried no -O flag whatsoever, so every
# kama binary ever built — including the ones CI released — ran at -O0. Nobody noticed, because the -O
# flags people DO look at are the ones `kama build --release` hands the C compiler for the *user's*
# program (kama.driver.cpp, `-O3` native / `-Oz` wasm). That is a different codebase one level down.
# The cost of the oversight was not the wall-clock (8.2x on the front end, 5.4x over the fixture
# corpus, for byte-identical emitted C) but the measurements: a whole build-performance campaign was
# calibrated against an unoptimized compiler, and its conclusions had to be re-derived.
#
# It is a source check, not an artifact check, and deliberately so: the honest artifact-level test is
# "is the binary fast", which is a wall-clock threshold, and these guards run in a saturated parallel
# pool where a wall-clock threshold is a flake generator. The Makefile is the thing that regressed and
# the Makefile is the thing this reads.
#
# Cheap: `make -n` executes nothing. Native leg, no compiler build, so it does not block the pool.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# -B so the rule prints even when the object is already up to date; -n so nothing runs.
line=$(make -C "$ROOT" -n -B kama 2>/dev/null | grep -E 'kama\.cemit\.o' | head -1)

if [ -z "$line" ]; then
    echo "check-opt: FAIL — could not recover the compile line for kama.cemit.o from the Makefile" >&2
    exit 1
fi

# -O0 must NOT satisfy this, which is the whole point.
case "$line" in
    *" -O1 "*|*" -O2 "*|*" -O3 "*|*" -Os "*|*" -Oz "*) ;;
    *)
        echo "check-opt: FAIL — the compiler is being built without optimization." >&2
        echo "  compile line: $line" >&2
        echo "  Set OPT in the Makefile (default -O2). See ROADMAP section 9." >&2
        exit 1 ;;
esac

# The level stays overridable for compiler debugging — prove the variable is actually honored, so a
# future refactor cannot hardcode -O2 into CXXFLAGS and silently strand `make OPT=-O0`.
over=$(make -C "$ROOT" -n -B OPT=-O0 kama 2>/dev/null | grep -E 'kama\.cemit\.o' | head -1)
case "$over" in
    *" -O0 "*) ;;
    *)
        echo "check-opt: FAIL — OPT= is not honored; \`make OPT=-O0\` still optimizes." >&2
        echo "  compile line: $over" >&2
        exit 1 ;;
esac

echo "check-opt: OK"
