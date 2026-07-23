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
echo "PASS syntax-drift (VSCode grammar covers every kama.l keyword)"
