#!/bin/sh
# check-batch.sh — `kama check --each` must answer exactly what one-process-per-file answers.
#
# `--each` runs N independent programs in ONE process, reusing the parsed prelude and the shared `std::`
# import closure across all of them. That reuse is what makes the suite's analysis-agreement phase 2x
# faster, and it is sound only while re-analysis leaves the shared ASTs alone. Two emitters over one AST
# is already the norm (the prelude has been shared since M5.1), but the emitter has ONE pass that writes
# THROUGH to the AST — CEmitter::pruneInactiveDecls, which drops `@compileFor`-inactive decls in place.
# So this guard asserts the property that pass can break:
#
#   1. IDEMPOTENCE — the same file twice in one process gives byte-identical output. Run under
#      `--no-heap`, because that is the flag that makes the prune actually drop something: `sort`/
#      `sortWith` in lib/std/collections/sort.kama are `@compileFor(!NOHEAP)` AND named in the module's
#      `export` list. Before the fix that made the pruned-name set live on the CompilationUnit rather
#      than only on the emitter, the SECOND program in the batch reported a phantom
#      "export list names `sort` but there is no such top-level declaration". That is the exact shape of
#      failure to expect if another in-place AST rewrite is ever added — a later program in a batch
#      seeing the leftovers of an earlier one.
#   2. AGREEMENT — a batch's per-file verdict and stderr equal what a solo `kama check` produces. Kept
#      to a handful of files here; the full-corpus sweep is the acceptance evidence, and is what
#      run_tests.sh's KAMA_NO_BATCH=1 escape hatch re-runs on demand:
#         ls tests/*.kama tests/xfail/*.kama > /tmp/f; split -l 32 /tmp/f /tmp/chunk.
#         for c in /tmp/chunk.*; do "$KAMA" check --each $(cat "$c"); done
#      (923 fixtures, identical exit codes, and each chunk's stderr byte-identical to the concatenation
#      of its files' solo stderr. Run it under sh, not zsh — zsh does not word-split `$(cat …)`.)
#   3. SCOPE — `--each` is rejected for any subcommand but `check`, so `build --each` can never silently
#      build only the first input.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-batch: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# Absolute paths throughout rather than a `cd $ROOT`: the guards run in parallel from one runner, so
# none of them may change directory outside a subshell.

# --- 1. idempotence: the same program twice in one process ------------------------------------------
# Pick a fixture that imports the gated-and-exported decls, so the prune has something to drop.
GATED="$ROOT/tests/sort_basic.kama"
[ -f "$GATED" ] || { echo "check-batch: $GATED is gone — pick another fixture importing std::collections::sort" >&2; exit 1; }

for flags in "" "--no-heap"; do
    # shellcheck disable=SC2086
    "$KAMA" check --each $flags "$GATED" "$GATED" >"$tmp/each.out" 2>"$tmp/each.err" || true
    # Split the two runs on the verdict lines --each writes to stdout, then compare the halves.
    runs=$(grep -c . "$tmp/each.out" || true)
    if [ "$runs" != "2" ]; then
        bad "check --each $flags <f> <f> printed $runs verdict line(s), expected 2"
        continue
    fi
    a=$(head -1 "$tmp/each.out"); b=$(tail -1 "$tmp/each.out")
    [ "$a" = "$b" ] || bad "check --each $flags: the two verdicts differ ($a vs $b)"
    # stderr must be the same text twice: split it in half and diff.
    n=$(wc -l <"$tmp/each.err"); half=$((n / 2))
    head -"$half" "$tmp/each.err" >"$tmp/first"; tail -"$half" "$tmp/each.err" >"$tmp/second"
    if [ "$n" -eq 0 ] || [ $((half * 2)) -ne "$n" ]; then
        bad "check --each $flags: stderr is $n line(s) — not two equal halves, so a run diverged"
    elif diff "$tmp/first" "$tmp/second" >"$tmp/d" 2>&1; then
        ok "check --each $flags <f> <f>: both programs identical"
    else
        bad "check --each $flags: the second program's diagnostics differ from the first's"
        sed 's/^/    /' "$tmp/d" | head -6 >&2
    fi
done

# --- 2. agreement with the unbatched path ------------------------------------------------------------
# A mixed set: fixtures that pass and fixtures that fail, so both verdicts are exercised.
SET="$GATED $ROOT/tests/parse_radix.kama $ROOT/tests/xfail/$(ls "$ROOT/tests/xfail" | head -1)"
: >"$tmp/solo.v"; : >"$tmp/solo.err"
for f in $SET; do
    "$KAMA" check "$f" >/dev/null 2>>"$tmp/solo.err" && rc=0 || rc=$?
    # kama's OWN spelling of the path, because that is what `--each` prints in its verdict lines and this
    # file is diffed against them. Under msys2 the shell holds `/c/Users/…` while kama — which received the
    # path already converted, as arguments always are — reports `C:/Users/…`. Same file, and the diff was
    # 1,3c1,3 on every line. Identity everywhere else.
    echo "$rc $(kama_native_path "$f")" >>"$tmp/solo.v"
done
# shellcheck disable=SC2086
"$KAMA" check --each $SET >"$tmp/each.v" 2>"$tmp/each.err" || true
if diff "$tmp/solo.v" "$tmp/each.v" >"$tmp/d" 2>&1; then ok "batched verdicts match solo verdicts"
else bad "batched verdicts differ from solo"; sed 's/^/    /' "$tmp/d" | head -8 >&2; fi
if diff "$tmp/solo.err" "$tmp/each.err" >"$tmp/d" 2>&1; then ok "batched stderr matches concatenated solo stderr"
else bad "batched stderr differs from solo"; sed 's/^/    /' "$tmp/d" | head -8 >&2; fi

# --- 3. --each is check-only -------------------------------------------------------------------------
if "$KAMA" build --each "$GATED" -o "$tmp/x" >/dev/null 2>"$tmp/e"; then
    bad "\`kama build --each\` was accepted — it would silently build only the first input"
elif grep -q '\--each is only meaningful' "$tmp/e"; then
    ok "\`kama build --each\` is rejected, by name"
else
    bad "\`kama build --each\` failed, but not with the --each message"
fi

[ "$fail" -eq 0 ] && echo "check-batch: OK" || echo "check-batch: FAILED" >&2
exit "$fail"
