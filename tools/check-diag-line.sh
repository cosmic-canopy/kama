#!/bin/sh
# check-diag-line.sh — tests/xfail/DIAGNOSTIC_LINES describes the fixtures that actually exist.
#
# What the record is. `.msg` asserts a diagnostic's MESSAGE and nothing asserted its POSITION, so every
# fixture under tests/xfail/ would have passed with every line wrong — measured: rendering each diagnostic
# one line off left 1197 of 1275 assertions green. DIAGNOSTIC_LINES closes that: one row per fixture, every
# `<file>:<line>` its diagnostics name. run_tests.sh compares each fixture against its row as the xfail leg
# runs, and refuses a fixture that has NO row — so "is each row right" and "does each fixture have one" are
# both already answered there, by the only thing that can answer them: a real build.
#
# What is left for this guard, and the whole reason it exists: a row whose fixture is GONE. Nothing reads
# it, so nothing notices it, and it sits in the file looking exactly like coverage. That is the same shape
# as check-agents.sh's `continue` arm, which hid a deleted flag for four releases — an entry that is never
# reached is not an entry that passed. Deleting a fixture must delete its row in the same commit.
#
# Cheap on purpose: no compiler, no build, no fixtures compiled. It is a set comparison over two listings.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DL="$ROOT/tests/xfail/DIAGNOSTIC_LINES"

[ -f "$DL" ] || { echo "check-diag-line: FAIL — no $DL (regenerate: KAMA_UPDATE_DIAG_LINES=1 ./dev test)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The fixtures the xfail leg actually reaches — the same two globs run_tests.sh iterates, nothing else.
for f in "$ROOT"/tests/xfail/*.kama "$ROOT"/tests/xfail/*.d; do
    [ -e "$f" ] || continue
    b=${f##*/}; b=${b%.kama}; b=${b%.d}
    echo "$b"
done | LC_ALL=C sort -u > "$tmp/fixtures"

LC_ALL=C grep -v '^#' "$DL" | cut -f1 | LC_ALL=C sort -u > "$tmp/rows"

fail=0

orphans=$(LC_ALL=C comm -13 "$tmp/fixtures" "$tmp/rows")
if [ -n "$orphans" ]; then
    echo "check-diag-line: FAIL — DIAGNOSTIC_LINES has rows for fixtures that no longer exist." >&2
    echo "  A row nothing reads is not coverage. Delete these, or restore the fixtures:" >&2
    printf '%s\n' "$orphans" | sed 's/^/    /' >&2
    fail=1
fi

missing=$(LC_ALL=C comm -23 "$tmp/fixtures" "$tmp/rows")
if [ -n "$missing" ]; then
    echo "check-diag-line: FAIL — these xfail fixtures have no row in DIAGNOSTIC_LINES." >&2
    echo "  Regenerate, then READ THE DIFF: KAMA_UPDATE_DIAG_LINES=1 ./dev test" >&2
    printf '%s\n' "$missing" | sed 's/^/    /' >&2
    fail=1
fi

# The generator sorts, so an unsorted or duplicated file means the record was hand-edited — which is how a
# row stops matching the fixture it claims to describe.
if ! LC_ALL=C grep -v '^#' "$DL" | cut -f1 | LC_ALL=C sort -c 2>/dev/null; then
    echo "check-diag-line: FAIL — rows are not sorted; the file is generated, not hand-written." >&2
    echo "  Regenerate: KAMA_UPDATE_DIAG_LINES=1 ./dev test" >&2
    fail=1
fi

dups=$(LC_ALL=C grep -v '^#' "$DL" | cut -f1 | LC_ALL=C uniq -d)
if [ -n "$dups" ]; then
    echo "check-diag-line: FAIL — duplicate rows; only the first is ever read:" >&2
    printf '%s\n' "$dups" | sed 's/^/    /' >&2
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-diag-line: PASS ($(LC_ALL=C grep -cv '^#' "$DL") rows, one per xfail fixture, none orphaned)"
