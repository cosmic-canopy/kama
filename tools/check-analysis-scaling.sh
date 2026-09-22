#!/bin/sh
# check-analysis-scaling.sh — analysis of an operator chain stays proportional to the same work as statements.
#
# What it guards (KR-78). `f(x: x) + f(x: x) + …` in ONE expression grew as roughly n^3.5 in `kama check`:
# 0.43 s at 40 terms, 1.9 s at 60, 3.2 s at 70 (0.9.423, single file), while the SAME calls written as that
# many statements stayed at 0.02 s. The three expression classifiers — `typeOfExpr`, `exprClass`,
# `exprIsString` — each ask about an operator's operands, every level of the chain asked about its whole
# subtree, and every answer below it was recomputed per ask. The LSP re-analyzes on each keystroke, so a
# long arithmetic expression (engine math) felt it first. The fix is a memo per OUTERMOST classifier query
# (`ClassifierMemo` in kama.cemit.h); the corpus transpiled byte-identical with and without it.
#
# How it measures, and why a RATIO. The pool runs guards in parallel on a machine that drifts, so an
# absolute bound would be a flake generator. Instead it times the chain against its own linear reference —
# the same 60 calls as 60 statements, the same parse size — best of three each, back to back, so load moves
# both numbers together. Measured on 0.9.424: ~1.8×. Before the fix: ~80×. The bound is 10×, far from both.
# Each run is capped, so a regression fails in seconds instead of stalling the pool.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-analysis-scaling: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
N=60
BOUND=10
CAP=20   # seconds per run; the old shape took ~1.9 s here, so only a far worse regression reaches it

{ echo 'fn int32 f(int32 x) { return x; }'
  printf 'fn int32 main() { int32 x = 1; int32 s = f(x: x)'
  i=1; while [ $i -lt $N ]; do printf ' + f(x: x)'; i=$((i+1)); done
  echo "; return s - $N; }"; } > "$tmp/chain.kama"
{ echo 'fn int32 f(int32 x) { return x; }'
  printf 'fn int32 main() { int32 x = 1; int32 s = f(x: x);'
  i=1; while [ $i -lt $N ]; do printf ' s = s + f(x: x);'; i=$((i+1)); done
  echo " return s - $N; }"; } > "$tmp/stmts.kama"

# best <file>: the fastest of three `kama check` runs, in seconds; "cap" if one hit the cap or failed.
best() {
    perl -MTime::HiRes=time -e '
        my ($kama, $file, $cap) = @ARGV; my $best;
        for (1..3) {
            my $t0 = time;
            my $pid = fork // die "fork: $!";
            if (!$pid) { open STDOUT, ">", "/dev/null"; open STDERR, ">", "/dev/null"; exec $kama, "check", $file; exit 127 }
            local $SIG{ALRM} = sub { kill "KILL", $pid; waitpid $pid, 0; print "cap\n"; exit 0 };
            alarm $cap; waitpid $pid, 0; alarm 0;
            if ($? != 0) { print "cap\n"; exit 0 }
            my $dt = time - $t0; $best = $dt if !defined $best || $dt < $best;
        }
        printf "%.4f\n", $best;' "$KAMA" "$1" "$CAP"
}

ts=$(best "$tmp/stmts.kama")
tc=$(best "$tmp/chain.kama")
if [ "$ts" = cap ]; then echo "check-analysis-scaling: FAIL — the $N-statement reference did not check cleanly" >&2; exit 1; fi
if [ "$tc" = cap ]; then
    echo "check-analysis-scaling: FAIL — a $N-term operator chain did not finish \`kama check\` in ${CAP}s (KR-78)" >&2
    exit 1
fi
ratio=$(awk -v c="$tc" -v s="$ts" 'BEGIN { printf "%.1f", c / s }')
if awk -v r="$ratio" -v b="$BOUND" 'BEGIN { exit !(r >= b) }'; then
    echo "check-analysis-scaling: FAIL — a $N-term operator chain took ${tc}s against ${ts}s for the same" >&2
    echo "  calls as statements (${ratio}x, bound ${BOUND}x). Analysis is re-walking an operator's subtree at" >&2
    echo "  every level again — see ClassifierMemo in src/kama.cemit.h and KR-78 in the git log." >&2
    exit 1
fi
echo "check-analysis-scaling: PASS (a $N-term chain checks in ${ratio}x the time of the same calls as statements)"
