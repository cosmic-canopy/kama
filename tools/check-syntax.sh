#!/bin/sh
# check-syntax.sh — the SYNTAX-HIGHLIGHTING oracle. Companion to check-syntax-drift.sh, which greps the
# TextMate grammar and can therefore only see that a rule EXISTS. This one runs the real vscode-textmate
# engine (the same tokenizer VS Code itself uses) and can see whether a rule FIRES.
#
# That distinction is not academic. Before M6 Stage B the grammar's `#declarations` and `#cast` rules were
# DEAD: present in the file, correctly written, and unreachable — TextMate breaks a same-position tie in
# favour of the earlier include, so `#keywords` matching bare `type`/`cast` starved them. The consequence
# was that a contextual kind word (`type value Point`), which is an ordinary IDENTIFIER in kama.y and so
# can be coloured by nothing else, came back with NO SCOPE AT ALL — while check-syntax-drift.sh's own
# comment claimed the declarations rule highlighted it. A grep cannot catch that. This can.
#
# THREE oracles run here, and a fixture has to satisfy all three:
#
#   1. SNAPSHOTS — tests/syntax/<f>.kama.snap holds the committed token/scope output for every character
#      of <f>.kama. Same idea as a tests/*.expect file: an unintended scope change is a diff, not a
#      judgement call. Re-bless deliberately with:
#          cd editor/vscode && node_modules/.bin/vscode-tmgrammar-snap -u '../../tests/syntax/*.kama'
#      (`-g` is NOT needed and is silently IGNORED — the grammar comes from editor/vscode/package.json's
#      `contributes.grammars`, which is what a real VS Code install reads too.)
#
#   2. THE COMPILER — every fixture except bad.kama must pass `kama check`, and bad.kama must FAIL it.
#      This is the oracle that makes the grammar's claims true rather than plausible, and it is not
#      theoretical: while these fixtures were being written it rejected `fn name() -> T` (kama has no
#      `->`), a bare `ctor(…)` (a ctor is named), a bare `@generate`, a `type abstract resource` with no
#      overridable method, `${s.length()}` (no calls in an interpolation hole) and `${s:>8}` (not a legal
#      format spec) — six spellings that would each have shipped a confidently-wrong grammar assertion.
#
#   3. DEFECT INVARIANTS — the greps at the tail, one per audited defect. A snapshot alone is re-blessable
#      by accident, so the findings that motivated the audit are asserted by NAME. If one of these fires,
#      a specific defect has come back; the message says which.
#
# Skips (exit 0, loudly) when node or editor/vscode/node_modules is absent, so a fresh clone that has not
# run `npm install` is not failed for it.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
EXT="$ROOT/editor/vscode"
FIX="$ROOT/tests/syntax"
SNAP="$EXT/node_modules/.bin/vscode-tmgrammar-snap"

if ! command -v node >/dev/null 2>&1; then
    echo "SKIP syntax (no node on PATH — the TextMate tokenizer needs it)"; exit 0
fi
if [ ! -x "$SNAP" ]; then
    echo "SKIP syntax (editor/vscode/node_modules missing — run: cd editor/vscode && npm install)"; exit 0
fi
if [ ! -x "$KAMA" ]; then echo "FAIL syntax: $KAMA not built" >&2; exit 1; fi
if [ ! -d "$FIX" ]; then echo "FAIL syntax: missing $FIX" >&2; exit 1; fi

fails=0
note() { echo "FAIL syntax: $1" >&2; fails=$((fails+1)); }

# ---- 1. snapshots -----------------------------------------------------------------------------------
# Run from the extension dir so the default --config (package.json) resolves, exactly as VS Code does.
# The capture goes to a private tmp dir, not into tests/syntax/: the guards now share a machine, and a
# guard that writes to the worktree is one `git status` away from looking like an uncommitted change.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
snapout="$tmp/snapout"
if ! (cd "$EXT" && "$SNAP" "$FIX/"'*.kama' >"$snapout" 2>&1); then
    note "TextMate scopes differ from the committed snapshots:"
    sed 's/^/    /' "$snapout" >&2
    echo "    Re-bless ONLY if the change is intended:" >&2
    echo "    (cd editor/vscode && node_modules/.bin/vscode-tmgrammar-snap -u '../../tests/syntax/*.kama')" >&2
fi

# A missing .snap is generated silently by the tool above, which would make a brand-new fixture
# self-approving. Require one per fixture explicitly.
for f in "$FIX"/*.kama; do
    [ -f "$f.snap" ] || note "$(basename "$f") has no committed .snap (generate and REVIEW it, then commit)"
done

# ---- 2. the compiler oracle ------------------------------------------------------------------------
for f in "$FIX"/*.kama; do
    base=$(basename "$f")
    if [ "$base" = "bad.kama" ]; then
        if "$KAMA" check "$f" >/dev/null 2>&1; then
            note "bad.kama COMPILES — it exists to hold spellings the compiler rejects, so every"
            echo "    construct in it must stay rejected or its invalid.illegal.* scopes are lies." >&2
        fi
    else
        if ! "$KAMA" check "$f" >/dev/null 2>&1; then
            note "$base does not pass 'kama check' — a scope assertion about code the compiler"
            echo "    rejects proves nothing. Output:" >&2
            "$KAMA" check "$f" 2>&1 | sed 's/^/      /' >&2 || true
        fi
    fi
done

# ---- 3. defect invariants (one per audited finding) --------------------------------------------------
D="$FIX/declarations.kama.snap"
S="$FIX/strings.kama.snap"
B="$FIX/bad.kama.snap"

has()  { grep -qF -- "$2" "$1" 2>/dev/null; }
want() { has "$1" "$2" || note "$3"; }
deny() { has "$1" "$2" && note "$3" || true; }

# #11 — the dead-rule defect. The kind word can be coloured by NOTHING else, so its scope vanishing is
# the single most reliable signal that #declarations has been re-buried behind #keywords.
want "$D" "storage.modifier.kind.kama" \
    "the contextual kind word (value/resource/view/contract) has no scope — #declarations is DEAD again. It MUST be included BEFORE #keywords (TextMate breaks a same-position tie in favour of the earlier include)."
want "$D" "storage.type.kama" \
    "'type' is not scoped storage.type.kama — #declarations is being starved by #keywords again (defect 11)."
# #5 — modifiers between `type` and the kind word (`type immutable value`, `type final resource`).
want "$D" "keyword.other.modifier.kama" \
    "a modifier between 'type' and the kind word lost its scope (defect 5: type immutable value / type final resource / type extern value are all in-tree)."
# #6 — attributes, including the one that used to mis-scope as a function call.
want "$D" "entity.name.function.decorator.kama" \
    "attributes are unscoped again (defect 6). '@compileFor(X)' in particular used to mis-scope as a FUNCTION CALL via #functions' trailing-'(' lookahead."
# #9 — the ignored preprocessor line.
want "$D" "comment.line.number-sign.kama" \
    "the '#region' line is unscoped (defect 9). kama.l DROPS ^[ \\t]*#.* silently, so colouring it like a comment is what the compiler actually does with it."
# #10 — asm's body is embedded assembly and its string is NOT interpolated.
want "$D" "meta.embedded.assembly" \
    "asm(\"…\") lost its embedded-assembly scope (defect 10)."
# #7 — operators.
want "$D" "keyword.operator." \
    "operators are unscoped again (defect 7): kama had no operator rules at all."

# #2 — the verbatim quote-escape. `""` is the escape, and the OLD rule ended the string on its first half,
# mis-colouring the rest of the line.
want "$S" "string.quoted.verbatim.kama constant.character.escape.kama" \
    "the verbatim '\"\"' quote-escape is not an escape inside @\"…\" (defect 2). The end pattern needs its negative lookahead: TextMate prefers 'end' over a content pattern at a tie."

# The numeric SUFFIX SPLIT (`i32` in `42i32` colours like the int32 keyword, not like the digits) — the
# shape `139fb10` established when the numeric rules were found disagreeing with the compiler four ways.
want "$FIX/numbers.kama.snap" "storage.type.numeric.kama" \
    "a numeric type suffix (42i32 / 42ui32 / 1.5f32) is no longer split from its digits."

# The converse of bad.kama, asserted over EVERY valid fixture so a new one is covered without being added
# here: a file the compiler accepts must contain nothing the grammar calls broken.
for f in "$FIX"/*.kama.snap; do
    [ "$(basename "$f")" = "bad.kama.snap" ] && continue
    deny "$f" "invalid.illegal" \
        "$(basename "$f" .snap) has an invalid.illegal.* scope, but 'kama check' ACCEPTS it — the grammar is calling correct code broken, which is the mirror image of the defect class this audit closed."
done

# #1/#1b/#3 — and the whole point of bad.kama: rejected code must LOOK rejected.
want "$B" "invalid.illegal.unknown-escape.kama" \
    "\\e / \\x41 / \\q / \\u{1234567} are not flagged (defects 1 and 1b). kama.l's escape set is exactly ['\"\\\\0abfnrtv\$] plus \\u{1..6 hex}."
want "$B" "invalid.illegal.char-literal.kama" \
    "'abc' and '' are not flagged (defect 3): a char is ONE Unicode scalar value, so both are rejected by the compiler."
want "$B" "invalid.illegal.numeric.kama" \
    "42u32 / 42f32 / 1_000 / 0b1010 are not flagged — the numeric rules regressed (the class of defect that started this audit)."

# #4 — `using` was highlighted and is not a keyword (retired in favour of `import`). Asserted against the
# grammar itself: no fixture can contain it, because the compiler would reject the file.
deny "$EXT/syntaxes/kama.tmLanguage.json" "using|" \
    "the grammar highlights 'using', which is NOT in kama.l's keyword table — it was retired in favour of 'import' (defect 4)."

if [ "$fails" -ne 0 ]; then
    echo "FAIL syntax ($fails problem(s))" >&2
    exit 1
fi
echo "PASS syntax (TextMate scopes match the committed snapshots; fixtures agree with the compiler; 13 defect invariants hold)"
