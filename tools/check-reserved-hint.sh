#!/bin/sh
# check-reserved-hint.sh — the "that word is reserved" note fires at a NAMING position and nowhere else.
#
# What this guards, and why a fixture cannot. kama reserves ~80 words, and using one as a name is a parse
# error. The note added in 0.9.140 turns `unexpected OUT` into a sentence that names the word, points at
# the published list, and says which reserved words CAN name a binding. It has two halves, and a
# `tests/xfail/*.msg` can only assert the first: `.msg` is a positive substring match, so nothing can say
# "and this message must NOT appear". The half with no instrument is the one that broke.
#
# The defect (KB-9, reported by the first external project on 0.9.140, fixed in 0.9.142):
#
#     isize out = 1;              -> the full note
#     Thing out = Thing.make();   -> bare `syntax error, unexpected OUT`
#
# and the second is the shape most declarations take. The cause was not the parser. The note gated on the
# string `IDENTIFIER` appearing in bison's message, and bison prints its `expecting …` clause only when at
# most FOUR tokens are expected, dropping it entirely past that. Three are expected after a builtin type
# name; twenty-two after a user-defined one, because a leading identifier might still be an expression
# statement. So the note was reading a truncated message.
#
# The fix asks the parser instead (`%define parse.error custom` + `yypcontext_expected_tokens`, which is
# uncapped). ⚠️ And "IDENTIFIER is expected" is NOT by itself the right gate — that is the trap this guard
# exists for. An identifier also starts an expression, so `1 + else` and `return break` expect one too,
# and neither is a naming mistake. The discriminator was measured: a binding site expects
# `IDENTIFIER SLOT TYPE` plus operators and NO literal, an expression site expects IDENTIFIER plus every
# literal form. Hence "admits a name but not a number".
#
# Both directions are checked below, because each protects against the opposite regression: widening the
# gate brings the noise back, narrowing it brings KB-9 back.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-reserved-hint: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*" >&2; fail=1; }
ok()  { echo "  ok: $*"; }

NOTE='is a reserved word, so it cannot be used as a name here'

# $1 = body, $2 = want|reject, $3 = label
probe() {
    {
        echo 'type value Thing { public int32 a; public ctor make() { this.a = 0; } }'
        echo 'fn int32 main()'
        echo '{'
        echo "$1"
        echo '    return 0;'
        echo '}'
    } > "$tmp/p.kama"
    "$KAMA" check "$tmp/p.kama" > "$tmp/p.log" 2>&1 && {
        bad "$3: expected a parse error, but the file compiled"
        return
    }
    if grep -qF "$NOTE" "$tmp/p.log"; then
        if [ "$2" = want ]; then ok "$3 — explained"
        else bad "$3: the reserved-word note fired where the parser wanted an EXPRESSION, not a name"; fi
    else
        if [ "$2" = reject ]; then ok "$3 — correctly silent"
        else bad "$3: no reserved-word note (KB-9 — is the gate reading bison's message again?)"; fi
    fi
}

echo 'check-reserved-hint: a naming position explains itself'
# ⚠️ The user-defined-type case is KB-9 itself: 22 tokens are expected there, so it is exactly the one a
# message-string gate cannot see. The builtin case is its control — it worked before the fix and must
# keep working, or the display half regressed.
probe '    Thing out = Thing.make();' want   'a user-defined type (`Thing out`)'
probe '    isize out = 1;'            want   'a builtin type (`isize out`)'
probe '    int32 base = 1;'           want   '`base`, the one no contextual rule rescued'
probe '    Thing when = Thing.make();' want  '`when`, whose token carries a string alias'

echo 'check-reserved-hint: an expression position stays quiet'
probe '    int32 x = 1 + else;'          reject 'a reserved word mid-expression'
probe '    return break;'                reject 'a reserved word after `return`'
probe '    for (else;;) { }'             reject 'a reserved word in a `for` initializer'
probe '    if (true) { } else else { }'  reject 'a doubled `else`'

# The display half: the note is an ADDITION, never a replacement. bison's own wording has to survive, or
# every fixture asserting a parse message moves.
#
# ⚠️ THIS CLAUSE IS NOW AT THE CEILING. bison prints `expecting …` only while at most FOUR tokens are
# expected and drops the clause entirely past that (see the `parse.error custom` note in kama.y), and a
# binding site expects exactly four: IDENTIFIER, `file`, SLOT, TYPE. `file` was the fourth, added with the
# file gate in 0.9.143. A SIXTH contextual keyword would silently delete this half of every such message —
# the NOTE would survive, because its gate reads the uncapped expected-token SET rather than the printed
# prose, which is the whole reason that indirection exists. Expect this assertion to be what tells you.
printf 'fn int32 main() { isize out = 1; return 0; }\n' > "$tmp/d.kama"
"$KAMA" check "$tmp/d.kama" > "$tmp/d.log" 2>&1 || true
if grep -qF 'syntax error, unexpected OUT, expecting IDENTIFIER or file or SLOT or TYPE' "$tmp/d.log"; then
    ok "bison's own wording is preserved ahead of the note"
else
    bad "the base message changed — `parse.error custom` must reproduce `detailed` verbatim"
fi

[ "$fail" -eq 0 ] || { echo "check-reserved-hint: FAILED" >&2; exit 1; }
echo "check-reserved-hint: PASS (4 naming positions explained, 4 expression positions silent, wording intact)"
