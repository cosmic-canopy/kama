#!/bin/sh
# check-syntax-drift.sh — guard against the hand-maintained VSCode TextMate grammar drifting from the
# compiler's actual keyword set. The lexer's keyword table (kama.l) is the source of truth; every reserved
# keyword MUST be highlighted by editor/vscode/syntaxes/kama.tmLanguage.json. Fails (exit 1) listing any
# keyword the highlighter is missing. Run standalone or from run_tests.sh.
#
# Contextual kind words (value/resource/view/contract) are NOT lexer keywords (they lex as identifiers and
# are highlighted only right after `type` by the "declarations" rule), so they are not in the lexer table
# and correctly not required here.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LEXER="$ROOT/kama.l"
GRAMMAR="$ROOT/editor/vscode/syntaxes/kama.tmLanguage.json"

# Keywords the compiler reserves — the `{"word", TOKEN}` rows of the sorted keyword table in kama.l.
lexer_keywords() {
    sed -n '/static struct name_value keywords/,/};/p' "$LEXER" \
        | grep -oE '\{"[a-z_]+"' | tr -d '{"'
}

# Every bare word the TextMate grammar highlights: the alternations inside `\b(...)\b`, plus the kind words
# in the `type <kind>` declaration rule. One word per line.
grammar_tokens() {
    grep -oE '\\\\b\(([a-z0-9_|]+)\)' "$GRAMMAR" | sed -E 's/\\\\b\(//; s/\)//' | tr '|' '\n'
    # the `type (value|resource|view|contract)` declaration capture
    grep -oE '\(value\|resource\|view\|contract\)' "$GRAMMAR" | tr -d '()' | tr '|' '\n'
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
lexer_keywords  | sort -u > "$tmp/lex"
grammar_tokens  | sort -u > "$tmp/gram"
missing=$(comm -23 "$tmp/lex" "$tmp/gram")

if [ -n "$missing" ]; then
    echo "FAIL syntax-drift: kama.l reserves keywords the VSCode grammar does not highlight:" >&2
    echo "$missing" | sed 's/^/  - /' >&2
    echo "Add them to editor/vscode/syntaxes/kama.tmLanguage.json (keywords or types patterns)." >&2
    exit 1
fi

# Numeric literal SUFFIXES drift the same way, and did: the grammar highlighted `42u32` (which the
# compiler rejects — the suffix is `u?i…`, so it is `42ui32`) and failed to highlight `42ui32` at all, so
# the editor coloured broken code as valid and valid code as an identifier. The lexer's `int_suffix` macro
# is the source of truth; the grammar must use the same alternation, spelled as a non-capturing group.
lex_int_suffix=$(grep -E '^int_suffix[[:space:]]' "$LEXER" | awk '{print $2}')       # u?i(8|16|32|64)
want_int=$(printf '%s' "$lex_int_suffix" | sed 's/(/(?:/')                           # u?i(?:8|16|32|64)
if ! grep -qF -- "$want_int" "$GRAMMAR"; then
    echo "FAIL syntax-drift: kama.l's int_suffix is '$lex_int_suffix', so the VSCode grammar's numeric" >&2
    echo "  rules must match '$want_int' — not found in $GRAMMAR." >&2
    exit 1
fi
# Float suffixes come from the float_literal32/64 macros (`…f(32)` / `…f(64)`).
for fs in f32 f64; do
    if ! grep -qE "float_literal${fs#f}[[:space:]].*f\(${fs#f}\)" "$LEXER"; then continue; fi
    grep -qF -- "$fs" "$GRAMMAR" || { echo "FAIL syntax-drift: float suffix '$fs' unhighlighted" >&2; exit 1; }
done
# A bare `u<width>` suffix does not exist in kama; highlighting one would colour rejected code as valid.
if grep -qE '\|u(8|16|32|64)[|)]' "$GRAMMAR"; then
    echo "FAIL syntax-drift: the grammar highlights a bare 'u8/u16/u32/u64' numeric suffix, which kama" >&2
    echo "  does not accept (unsigned is 'ui8'…'ui64', from int_suffix = $lex_int_suffix)." >&2
    exit 1
fi
echo "PASS syntax-drift (VSCode grammar covers every kama.l keyword + numeric suffix)"
