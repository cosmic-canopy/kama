#!/bin/sh
# check-slot.sh — guard for `slot` drop elision, the payoff of the uninitialized-storage campaign.
#
# `slot T x;` declares a HOLE: storage an `out` argument is about to fill. The point is not the diagnostic
# (that a read before assignment is rejected — tests/xfail/slot_*.kama cover it) but the CODEGEN: where the
# slot is provably still empty, NO destructor is emitted at all, so "drop only if live" is proven
# statically rather than defended against at runtime with a niche check. That property is invisible to an exit-code fixture — a missing
# drop and a drop that happens to be a no-op both exit 7 — so it is asserted here against the emitted C.
#
# Subject: tests/slot_drop_elided.kama. The differential lives WITHIN one slot, across two exit points —
# a slot that is never filled is now an error, so "assigned vs unassigned" can no longer be two locals.
# The emitter tracks move state in EMISSION ORDER, so an exit before the fill is provably empty (no dtor)
# while an exit after it is live (dtor). Checking both directions is what keeps this from passing on a
# dtor that is simply never emitted anywhere.
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

# The body of run(), where the slot lives, without the #line directives.
# Emitted names carry a per-file prefix (`_F<file>__run`), so match the suffix rather than the whole name.
BODY=$(awk '/^int32_t .*run\(bool k_early\)$/,/^}/' "$OUT" | grep -v '^#line')
# Everything up to and including the `out` fill — i.e. the region where the slot is provably still empty.
PRE=$(printf '%s\n' "$BODY" | sed -n '1,/makeInto/p')
fail=0

# 1. The slot DOES drop after it is filled — proves the dtor is emitted at all, so (2) is a real signal.
if ! printf '%s\n' "$BODY" | grep -q 'Tracker__dtor(&k_t)'; then
    echo "check-slot: FAIL — the filled Tracker 't' has no dtor call anywhere; the differential is void" >&2
    printf '%s\n' "$BODY" >&2
    fail=1
else
    echo "  ok: the filled slot drops (Tracker__dtor(&k_t))"
fi

# 2. ...and the exit BEFORE the fill does not. This is the property the campaign exists to deliver.
if printf '%s\n' "$PRE" | grep -q 'Tracker__dtor(&k_t)'; then
    echo "check-slot: FAIL — the early return emits a destructor for a slot that cannot hold a value yet;" >&2
    echo "  drop elision regressed (per-exit-point move state)" >&2
    printf '%s\n' "$PRE" >&2
    fail=1
else
    echo "  ok: the exit before the fill emits no destructor (drop elided per exit point)"
fi

if [ "$fail" -ne 0 ]; then echo "FAIL check-slot" >&2; exit 1; fi
echo "PASS check-slot (drop only if live, proven statically)"
