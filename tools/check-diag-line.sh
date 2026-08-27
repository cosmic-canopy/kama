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

# ---- the OTHER line attribution: `#line` in the emitted C -------------------------------------------
#
# A `#line` is a diagnostic too — it is how the C compiler's errors and a debugger's breakpoints are placed
# back onto kama source — and it was the same defect one layer down. `line()` read `_sourcePath`, "whatever
# the emitter was constructed with", so the PRELUDE's static-inline bodies, which belong to no module, came
# out stamped with the entry file: a ONE-LINE program emitted 391 `#line` directives, 389 of them past its
# own end, the highest claiming line 633. In a multi-file build the same bodies claimed the empty file name.
#
# The rule asserted here is the one the fixtures assert for diagnostics: a position must name a file that
# EXISTS and a line that file HAS. `<prelude>` is a synthetic unit, not a path, so the honest answer there
# is no directive at all — which is why this checks the directives that ARE emitted rather than counting them.
if [ ! -x "${KAMA:-}" ] && [ -f "$ROOT/tools/kama-bin.sh" ]; then . "$ROOT/tools/kama-bin.sh"; fi
if [ -x "${KAMA:-}" ]; then
    mkdir -p "$tmp/c/lib"
    cat > "$tmp/c/lib/box.kama" <<'EOF'
export { Box };
type value Box<T> {
    public T item;
    public ctor make(T item) { this.item = item; }
    public fn int32 size() { return 1; }
}
EOF
    cat > "$tmp/c/user.kama" <<'EOF'
import { lib::Box };
fn int32 main() {
    Box<int32> b = Box.make(item: 1);
    return b.size();
}
EOF
    ( cd "$tmp/c" && "$KAMA" build user.kama lib/box.kama -o out.bin --keep-c >/dev/null 2>&1 ) || {
        echo "check-diag-line: FAIL — the #line probe program did not build" >&2; exit 1; }

    # ⚠️ Run from the BUILD directory. `#line` records the path as the build saw it — `lib/box.kama`,
    # relative to the cwd of that build — so resolving it anywhere else silently fails to stat every file,
    # and a check that cannot read its inputs reports no findings, which reads exactly like passing. It did.
    n_seen=$(cd "$tmp/c" && LC_ALL=C grep -ho '#line [0-9]* "[^"]*"' ./*.c ./*.h 2>/dev/null | wc -l | tr -d ' ')
    bad=$(cd "$tmp/c" && LC_ALL=C grep -ho '#line [0-9]* "[^"]*"' ./*.c ./*.h 2>/dev/null |
          LC_ALL=C awk '{ n = $2; p = $0; sub(/^#line [0-9]* "/, "", p); sub(/"$/, "", p)
                          if (p == "") { print "  an EMPTY file name, at line " n; next }
                          if (substr(p,1,1) == "<") { print "  the synthetic unit " p " named as a path"; next }
                          max = ""
                          cmd = "wc -l < \"" p "\" 2>/dev/null"; cmd | getline max; close(cmd)
                          if (max == "") { print "  " p ": no such file, so its line " n " is unreadable"; next }
                          if (n+0 < 1 || n+0 > max+0) print "  " p ":" n " (the file has " max+0 " lines)" }' |
          LC_ALL=C sort -u)
    # A probe that emitted nothing would also produce no findings. Say so rather than call it a pass.
    if [ "${n_seen:-0}" -lt 4 ]; then
        echo "check-diag-line: FAIL — the #line probe found only ${n_seen:-0} directives; it is not testing anything." >&2
        fail=1
    elif [ -n "$bad" ]; then
        echo "check-diag-line: FAIL — a #line directive points nowhere real. Every C-compiler error and" >&2
        echo "  every breakpoint inside that code is relocated to it:" >&2
        printf '%s\n' "$bad" >&2
        fail=1
    else
        echo "  ok: all $n_seen #line directives name a real file at a line it has"
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-diag-line: PASS ($(LC_ALL=C grep -cv '^#' "$DL") rows, one per xfail fixture, none orphaned;"
echo "                       and every emitted #line names a file that exists at a line it has)"
