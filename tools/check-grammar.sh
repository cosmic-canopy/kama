#!/bin/sh
# check-grammar.sh — docs/grammar.bnf is REGENERATED, so it must match src/kama.y.
#
# Why this guard exists. docs/SPEC.md says the grammar is authoritative and points at grammar.bnf;
# grammar.bnf's own banner says it is generated from src/kama.y and must be regenerated after any
# grammar change. Nothing enforced that, and it drifted: the committed file still documented
# `implements C for T` (retroactive contract conformance, DELETED in contract-model campaign 1) and had
# never heard of `type intrinsic <…> implements C`, which replaced it. So the file the spec calls
# authoritative described a construct the compiler rejects, and omitted the one it accepts.
#
# That is the worst failure mode a generated file has: it stays plausible. Nobody re-reads a BNF they
# did not just change, and a reader who trusts it writes code the parser refuses.
#
# The check is the obvious one — regenerate into a temp dir, diff — and it is cheap because
# tools/gen-grammar is a single awk pass over the .y file, no build required.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu
exec </dev/null

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

if [ ! -f "$ROOT/docs/grammar.bnf" ]; then
    echo "check-grammar: FAIL — docs/grammar.bnf is missing; run tools/gen-grammar" >&2
    exit 1
fi

# Run it DIRECTLY, not as `sh tools/gen-grammar`: the script declares `#!/usr/bin/env bash` and uses
# `set -o pipefail`, which is a bashism. Forcing `sh` discards that shebang, and `sh` is bash on macOS
# but DASH on Debian/Ubuntu — so this passed on a developer's Mac and failed on the Linux CI leg with
# "Illegal option -o pipefail". Keep gen-grammar's stderr too: swallowing it turned a one-line shell
# error into "did not run", which says nothing about what went wrong.
"$ROOT/tools/gen-grammar" "$tmp/grammar.bnf" >/dev/null 2>"$tmp/generr" || {
    echo "check-grammar: FAIL — tools/gen-grammar did not run" >&2
    sed 's/^/    /' "$tmp/generr" >&2
    exit 1; }

if diff -u "$ROOT/docs/grammar.bnf" "$tmp/grammar.bnf" > "$tmp/drift"; then
    echo "check-grammar: PASS (docs/grammar.bnf matches src/kama.y)"
    exit 0
fi

echo "check-grammar: FAIL — docs/grammar.bnf is stale against src/kama.y." >&2
echo "  docs/SPEC.md calls the grammar authoritative, so a stale one actively misinforms." >&2
echo "  Regenerate and commit it:  tools/gen-grammar" >&2
echo >&2
sed -n '1,40p' "$tmp/drift" >&2
_n=$(wc -l < "$tmp/drift")
[ "$_n" -gt 40 ] && echo "  … $((_n - 40)) more diff lines" >&2
exit 1
