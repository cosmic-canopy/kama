#!/bin/sh
# check-slot.sh — guard for `slot` drop elision, the payoff of the uninitialized-storage campaign.
#
# `slot T x;` declares a HOLE: storage with no value in it yet. The point is not the diagnostic (that a
# read before assignment is rejected — tests/xfail/slot_*.kama cover it) but the CODEGEN: an unassigned
# slot has NO destructor emitted at all, so "drop only if live" is proven statically rather than defended
# against at runtime with a niche check. That property is invisible to an exit-code fixture — a missing
# drop and a drop that happens to be a no-op both exit 7 — so it is asserted here against the emitted C.
#
# Subject: tests/slot_drop_elided.kama, which declares two Files side by side — one left unassigned, one
# assigned — so the check is differential and cannot pass by the dtor simply never being emitted anywhere.
# Fails (exit 1) with a diagnostic if either property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/slot_drop_elided.kama"

if [ ! -x "$KAMA" ];   then echo "check-slot: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-slot: missing $FIXTURE" >&2; exit 1; fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OUT="$TMP/slot_drop_elided.c"

"$KAMA" transpile "$FIXTURE" -o "$OUT" >/dev/null 2>&1 \
    || { echo "check-slot: transpile failed" >&2; exit 1; }

# The body of run(), where both locals live, without the #line directives.
# Emitted names carry a per-file prefix (`_F4__run`), so match the suffix rather than the whole name.
BODY=$(awk '/^int32_t .*run\(void\)$/,/^}/' "$OUT" | grep -v '^#line')
fail=0

# 1. The assigned local DOES drop — proves the dtor is emitted at all, so (2) is a real signal.
if ! printf '%s\n' "$BODY" | grep -q 'Tracker__dtor(&filled)'; then
    echo "check-slot: FAIL — the ASSIGNED Tracker 'filled' has no dtor call; the differential check is void" >&2
    fail=1
else
    echo "  ok: assigned Tracker 'filled' drops (Tracker__dtor(&filled))"
fi

# 2. ...and the unassigned slot does NOT. This is the property the campaign exists to deliver.
if printf '%s\n' "$BODY" | grep -q 'Tracker__dtor(&unfilled)'; then
    echo "check-slot: FAIL — unassigned 'slot Tracker unfilled' still emits a destructor; drop elision regressed" >&2
    printf '%s\n' "$BODY" >&2
    fail=1
else
    echo "  ok: unassigned 'slot Tracker unfilled' emits no destructor (drop elided)"
fi

if [ "$fail" -ne 0 ]; then echo "FAIL check-slot" >&2; exit 1; fi
echo "PASS check-slot (drop only if live, proven statically)"
