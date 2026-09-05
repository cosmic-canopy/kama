#!/bin/sh
# check-diag-drift.sh — a C-compiler error lands on the kama line it is on, not that line plus an offset.
#
# `#line` is how clang's errors are placed back onto kama source, and clang counts FORWARD from the last
# directive. The emitter stamps roughly one per statement, so every C line a lowering emits after its first
# was attributed to "the statement's line plus the offset" — and a `match`, whose lowering runs ~18 C lines,
# left the construct entirely. Measured before the fix: an error inside the third arm of a match on line 7
# reported at line 12 of an 11-line file.
#
# ⚠️ WHY THIS GUARD EXISTS SEPARATELY FROM check-diag-line.sh. That one validates the DIRECTIVES, and every
# directive here is already correct — it is clang's arithmetic BETWEEN them that leaves the file. No amount
# of checking what the emitter wrote can see this. The only oracle is to compile a probe with a deliberate
# C-level error and read what the C COMPILER says, which is what this does.
#
# It has to be a C-level error, not a kama one: kama's own diagnostics carry `node->line` and were never
# affected. The probe used to be a bad member access (`s.length`), which was exactly the class of real
# error the row was about — until kama learned to refuse it itself (tests/xfail/field_on_string). What
# still reaches clang is a `cast<int32>` of a `string`: kama checks a cast's TARGET, not its operand's
# kind, so the C compiler is the one that says "operand of type 'kama_string'". If a later change closes
# that too, this guard says so ("did not produce a C-compiler error") and wants a new probe, not a waiver.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"          # exports $KAMA (absolute; never the ./kama symlink — see AGENTS.md)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-diag-drift: FAIL — $1" >&2; fail=1; }

# One probe, two shapes at once: `cast<int32>(s)` of a kama string is refused by clang, not by kama. It is
# placed INSIDE a match arm (the long-lowering case) and AFTER a match (the leaked-offset case).
cat > "$tmp/drift.kama" <<'KEOF'
type enum Color : uint8 { Red, Green, Blue }
fn int32 main()
{
    Color c = Color::Green;
    string s = match (c) {
        case Red:   "r";
        case Green: "g";
        case Blue:  "b";
    };
    return cast<int32>(s);
}
KEOF
NLINES=$(wc -l < "$tmp/drift.kama" | tr -d ' ')

"$KAMA" build "$tmp/drift.kama" -o "$tmp/drift.out" > "$tmp/after.log" 2>&1 || true
# Every line the C compiler blamed in OUR file. `sort -u` because clang repeats a `_Generic` expansion.
LINES=$(grep -oE 'drift\.kama:[0-9]+' "$tmp/after.log" | cut -d: -f2 | sort -un || true)

[ -n "$LINES" ] || note "the probe did not produce a C-compiler error naming drift.kama — it must, or this guard proves nothing (see $tmp/after.log)"

for L in $LINES; do
    # The floor: an error must never be attributed past the end of the file it is in. This is the symptom
    # the roadmap row was written about, and the one a reader cannot possibly reconcile.
    [ "$L" -le "$NLINES" ] || note "a C-compiler error is reported at drift.kama:$L, but the file has only $NLINES lines"
    # And the actual claim: the error is on line 10, the only line with a bad expression.
    [ "$L" = "10" ] || note "a C-compiler error is reported at drift.kama:$L; the bad expression is on line 10"
done

# The arm case, where the drift was worst: the error moves INTO the lowering rather than after it.
cat > "$tmp/arm.kama" <<'KEOF'
type enum Color : uint8 { Red, Green, Blue }
fn int32 main()
{
    Color c = Color::Green;
    int32 n = match (c) {
        case Red:   1;
        case Green: cast<int32>("g");
        case Blue:  3;
    };
    return n;
}
KEOF
ANLINES=$(wc -l < "$tmp/arm.kama" | tr -d ' ')
"$KAMA" build "$tmp/arm.kama" -o "$tmp/arm.out" > "$tmp/arm.log" 2>&1 || true
ALINES=$(grep -oE 'arm\.kama:[0-9]+' "$tmp/arm.log" | cut -d: -f2 | sort -un || true)

[ -n "$ALINES" ] || note "the arm probe produced no C-compiler error naming arm.kama (see $tmp/arm.log)"

for L in $ALINES; do
    [ "$L" -le "$ANLINES" ] || note "an in-arm error is reported at arm.kama:$L, but the file has only $ANLINES lines"
    # Line 7 is the `case Green:` arm. Reporting line 8 would name the NEXT arm, which is worse than
    # vague — it points at code that is correct.
    [ "$L" = "7" ] || note "an error inside the arm on line 7 is reported at arm.kama:$L"
done

[ "$fail" = 0 ] || exit 1
echo "check-diag-drift: PASS (a C-compiler error lands on its own kama line, in a match arm and after one)"
