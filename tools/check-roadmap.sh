#!/bin/sh
# check-roadmap.sh — ROADMAP.md stays the short ordered list; the reasoning stays in ROADMAP_DETAIL.md.
#
# Why this guard exists. ROADMAP.md is the answer to "what do we do next", and it is only useful if that
# answer is readable in one screen. It reached 1,279 lines by absorbing the reasoning for every open item
# inline, at which point finding the next task meant parsing the whole document — and its own maintenance
# banner already recorded the file drifting twice before, for a different reason (logging completions).
# Splitting it fixes nothing on its own: without a guard, the prose grows back one paragraph at a time,
# because each individual paragraph looks like it belongs.
#
# So the invariants are structural, not stylistic:
#   1. ROADMAP.md is under a line ceiling.
#   2. Every `ROADMAP_DETAIL.md#anchor` link in ROADMAP.md resolves to an anchor that EXISTS.
#   3. Every anchor in ROADMAP_DETAIL.md is REACHED by some ROADMAP.md row — orphaned detail is a
#      section describing work nothing is scheduling, which is how a plan turns back into a changelog.
#
# (2) and (3) together are what keep the two files one document rather than two that drift.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RM="$ROOT/docs/ROADMAP.md"
RD="$ROOT/docs/ROADMAP_DETAIL.md"

# The ceiling. Deliberately not tight — it is a drift alarm, not a style rule. Raise it only together
# with a reason; if the list genuinely has 60 items, the rows are fine and the prose is what to look for.
MAX_LINES=160

fail() { echo "check-roadmap: FAIL — $1" >&2; exit 1; }

[ -f "$RM" ] || fail "no docs/ROADMAP.md"
[ -f "$RD" ] || fail "no docs/ROADMAP_DETAIL.md"

# ---- 1. the ceiling ----------------------------------------------------------------------------
lines=$(wc -l < "$RM" | tr -d ' ')
if [ "$lines" -gt "$MAX_LINES" ]; then
    echo "check-roadmap: FAIL — docs/ROADMAP.md is $lines lines (ceiling $MAX_LINES)." >&2
    echo "  It is the ORDER, not the reasoning. Move the prose into docs/ROADMAP_DETAIL.md and" >&2
    echo "  leave a one-line row linking to it. See that file's header for the maintenance rule." >&2
    exit 1
fi

# ---- 2. every link resolves --------------------------------------------------------------------
want=$(grep -o 'ROADMAP_DETAIL\.md#[A-Za-z0-9_-]*' "$RM" | sed 's/.*#//' | sort -u)
have=$(grep -o '<a id="[A-Za-z0-9_-]*"' "$RD" | sed 's/.*id="//; s/"//' | sort -u)

missing=""
for a in $want; do
    printf '%s\n' "$have" | grep -qx "$a" || missing="$missing $a"
done
[ -z "$missing" ] || {
    echo "check-roadmap: FAIL — ROADMAP.md links to anchors that do not exist in ROADMAP_DETAIL.md:" >&2
    for a in $missing; do echo "    #$a" >&2; done
    echo "  Add \`<a id=\"NAME\"></a>\` above the section, or fix the link." >&2
    exit 1
}

# ---- 3. no orphaned detail ---------------------------------------------------------------------
orphan=""
for a in $have; do
    printf '%s\n' "$want" | grep -qx "$a" || orphan="$orphan $a"
done
[ -z "$orphan" ] || {
    echo "check-roadmap: FAIL — ROADMAP_DETAIL.md sections nothing in ROADMAP.md schedules:" >&2
    for a in $orphan; do echo "    #$a" >&2; done
    echo "  Either add a ROADMAP.md row pointing at it, or delete the section — a detail section" >&2
    echo "  with no row is work nobody has planned, which is what this split exists to prevent." >&2
    exit 1
}

n=$(printf '%s\n' "$want" | grep -c . )
echo "check-roadmap: OK (ROADMAP.md $lines/$MAX_LINES lines; $n detail sections, all linked, none orphaned)"
