#!/bin/sh
# check-fixture-reach.sh — every fixture the TEST HARNESS never builds must still analyze clean.
#
# The gap this closes, found 2026-08-24. `run_tests.sh` walks four globs and no more:
#
#     tests/*.kama          tests/*.d/         tests/xfail/          tests/trap/*.kama
#
# Anything in another subdirectory is never compiled by the suite. `tests/query/` is the big one — 19
# files driven by check-query and check-lsp through `kama query`, which reports diagnostics but asserts
# only on the ANSWER, never on the file being well-formed. So a fixture there can stop compiling and the
# whole gate stays green.
#
# It did. `tests/query/complete.kama` violated the no-widening rule for THREE WEEKS: its `Derived.total()`
# was written 2026-07-28 and the rule that forbids it landed 2026-08-02, whose commit had no reason to
# sweep a directory nothing builds. `tests/query/coverage/spellings.kama` had the same defect twice over.
# Both files carry a header telling the next editor to run `kama check` after any edit — an instruction
# with nothing behind it, which is exactly the failure mode AGENTS.md's house rule is about: a prose claim
# is not a guard.
#
# The set is DERIVED, not listed, so a new fixture directory is covered the day it appears rather than the
# day someone remembers this file.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-fixture-reach: $KAMA not built" >&2; exit 1; fi

cd "$ROOT"
fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=$((fail+1)); }

# The scope operand an EDITOR would compute for a file: the workspace file if one is above it, else the
# nearest project manifest, else nothing. Same walk check-query's `scope_for` does, and the same asymmetry
# §2g.35 designed — a member file is not a program, so handing it over bare is not the question to ask.
scope_for() {
    _d=$(dirname "$1"); _s=""
    while [ "$_d" != "/" ] && [ "$_d" != "." ]; do
        if [ -f "$_d/kama_workspace.json" ]; then echo "$_d/kama_workspace.json"; return; fi
        if [ -z "$_s" ] && [ -f "$_d/kama.json" ]; then _s="$_d/kama.json"; fi
        _d=$(dirname "$_d")
    done
    echo "$_s"
}

# ---------------------------------------------------------------------------------------------------
echo "check-fixture-reach: a fixture the suite never builds still has to analyze"

unwalked=""
for f in $(git ls-files 'tests/*.kama' 'tests/**/*.kama'); do
    case "$f" in
        tests/xfail/*|tests/trap/*) continue ;;      # walked: the xfail and trap legs
        tests/*/*)                                   # a subdirectory — walked only if it is a `.d` fixture
            d=${f#tests/}; d=${d%%/*}
            case "$d" in *.d) continue ;; esac
            unwalked="$unwalked $f" ;;
        *) continue ;;                               # tests/<file>.kama — the single-file leg
    esac
done

[ -n "$unwalked" ] || { echo "check-fixture-reach: found no unwalked fixtures — has the layout moved?" >&2; exit 1; }

n=0; skipped=0
for f in $unwalked; do
    n=$((n+1))
    # The ONE exemption, and it is asserted rather than trusted: this fixture exists to be REJECTED, and
    # tools/check-syntax.sh is what says so. Checking it here would be checking the opposite claim.
    if [ "$f" = "tests/syntax/bad.kama" ]; then
        skipped=$((skipped+1))
        if "$KAMA" check "$f" >/dev/null 2>&1; then
            bad "$f is the deliberately-invalid syntax fixture, but it CHECKS CLEAN — the exemption below is now hiding a real pass"
        fi
        continue
    fi
    s=$(scope_for "$f")
    if [ -n "$s" ]; then out=$("$KAMA" query "$s" "$f" --diagnostics 2>&1 || true)
    else                out=$("$KAMA" check "$f" 2>&1 || true)
    fi
    if printf '%s' "$out" | grep -qE "error:|does not export"; then
        bad "$f does not analyze:"
        printf '%s\n' "$out" | grep -E "error:|does not export" | head -2 | sed 's/^/        /' >&2
    fi
done

[ "$fail" -eq 0 ] && ok "$n fixture(s) outside the suite's four globs analyze clean ($skipped deliberately-invalid, asserted invalid)"

# ---------------------------------------------------------------------------------------------------
# The same failure mode one glob over, found 2026-08-28: a fixture the suite DOES walk but then declines
# to assert. `run_tests.sh` skips any `tests/<name>.kama` with no `tests/<name>.expect` (and any
# `tests/<name>.d/` with no `expect` inside it) — printing SKIP, counting neither pass nor fail. The
# fixture is compiled by the analysis leg, so it looks alive in the log; nothing ever RUNS it or checks
# what it returned.
#
# It swallowed three fixtures the hour this section was written: the whole point of them was the exit
# code, and all three sat green as SKIPs. A fixture with no expected value is not coverage, exactly as a
# DIAGNOSTIC_LINES row with no fixture is not — same doctrine, same guard.
#
# Cheap: a set comparison over the worktree, no compiler.
missing=""
# ⚠️ The FILESYSTEM, not `git ls-files` — these are the two shell globs run_tests.sh itself iterates, and
# a guard about what the harness walks has to walk the same thing. A brand-new fixture is UNTRACKED until
# it is committed, which is precisely when this is worth catching; asking git would have made the guard
# blind to every fixture it exists for. (Only `tests/xfail/` and `tests/trap/` live deeper, and their legs
# assert a rejection or a trap rather than an exit code.)
for f in tests/*.kama; do
    [ -e "$f" ] || continue
    [ -f "${f%.kama}.expect" ] || missing="$missing $f"
done
for d in tests/*.d; do
    [ -d "$d" ] || continue
    [ -f "$d/expect" ] || missing="$missing $d/"
done
if [ -n "$missing" ]; then
    bad "these fixtures are walked by the suite but SKIPPED — no expected exit code, so nothing runs them:"
    for m in $missing; do echo "        $m" >&2; done
    echo "        add <name>.expect (a .d fixture: expect inside it) holding the expected exit code" >&2
else
    ok "every fixture the suite walks has an expected exit code"
fi

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-fixture-reach: PASS" || { echo "check-fixture-reach: FAIL" >&2; exit 1; }
