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

# ---- 4. every row id is well-formed and UNIQUE -------------------------------------------------
#
# `KR-<n>` is a PERMANENT id: assigned once, never reused, never renumbered, so a row keeps it wherever it
# moves and a shipped row's id retires with it, leaving a gap. Gaps are therefore CORRECT and are not
# checked — what would be a defect is two rows answering to the same name, because every citation of that
# id then means two things at once. A new row takes one more than the highest id present, which is safe
# precisely because none is ever reused.
#
# This replaced position numbering, where deleting a shipped row renumbered every row below it and silently
# re-pointed every `row N` written in prose — in this file, in the detail, in a commit message, in someone's
# notes. It had already happened twice; closing two rows in one sitting broke two more. The old guard could
# only catch a row citing ITSELF or an out-of-range number, and explicitly could not catch a reference that
# had come to point at a real but WRONG row. With permanent ids that whole failure mode is gone, so the
# check below is the stronger one it could not be before: a citation either resolves or it does not.
ids=$(LC_ALL=C grep -oE '^\| KR-[0-9]+ \|' "$RM" | tr -d '| ' )
rows=$(printf '%s\n' "$ids" | grep -c . )
dupes=$(printf '%s\n' "$ids" | sort | uniq -d)
if [ -n "$dupes" ]; then
    echo "check-roadmap: FAIL — a row id is used twice:" >&2
    printf '    %s\n' $dupes >&2
    echo "  A \`KR-\` id names one row forever. Give the newer row the next unused id; never reuse one," >&2
    echo "  and never renumber to close a gap — a gap is a shipped row, which is a good thing to see." >&2
    exit 1
fi
# A stray `| 12 |`-style row is the old scheme creeping back (or a hand-edited table).
legacy=$(LC_ALL=C grep -nE '^\| [0-9]+ \|' "$RM" || true)
if [ -n "$legacy" ]; then
    echo "check-roadmap: FAIL — a row is numbered by position instead of carrying a \`KR-\` id:" >&2
    printf '%s\n' "$legacy" >&2
    exit 1
fi

# ---- 5. every `KR-<n>` citation names a row that exists ----------------------------------------
#
# Checked across BOTH files: the detail cites rows too, and so does prose above the tables.
bad=$(LC_ALL=C grep -ohE 'KR-[0-9]+' "$RM" "$RD" | sort -u | while read -r ref; do
    printf '%s\n' "$ids" | grep -qx "$ref" || echo "    $ref is cited but no row has that id"
done)
if [ -n "$bad" ]; then
    echo "check-roadmap: FAIL — a \`KR-\` citation names no row:" >&2
    printf '%s\n' "$bad" >&2
    echo "  Either the row shipped (cite the SPEC/docs record instead, or say it shipped) or the id is a" >&2
    echo "  typo. An id is never reused, so a citation of a retired row can only ever be stale." >&2
    exit 1
fi

n=$(printf '%s\n' "$want" | grep -c . )
echo "check-roadmap: OK (ROADMAP.md $lines/$MAX_LINES lines; $rows rows, ids unique; $n detail sections,"
echo "                   all linked, none orphaned; every \`KR-\` citation resolves)"
