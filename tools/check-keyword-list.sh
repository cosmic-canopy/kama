#!/bin/sh
# check-keyword-list.sh — SPEC publishes kama's keywords, and the published list stays equal to the
# lexer's table.
#
# Why this exists. Every keyword that is NOT published is found by walking into it. The first external
# project found three that way — `base`, `type`, `slot` — and each cost a build cycle, because the parse
# error names the token (`unexpected SLOT`) without saying that the word is reserved or why. `type` and
# `slot` have since become contextual; `base` has not. The fix for the general case is a published list,
# and a published list is worth exactly as much as the guarantee that it is current — hence this guard.
#
# The direction of authority is lexer -> doc. `kama.l`'s `keywords` table is what the compiler actually
# reserves; SPEC's fenced block under "## kama's keywords" must equal it as a SET. Adding a keyword
# without documenting it fails here, which is the case worth catching: a keyword is added for a feature,
# the feature ships, and the word silently becomes unusable as a name.
#
# ⚠️ Set equality, both directions, deliberately. Doc-only entries are as bad as missing ones — a
# published list containing a word the compiler does not reserve sends someone renaming a binding that
# was always legal.
#
# Sibling guards: check-c-keywords.sh (kama reserves every C keyword) and check-syntax-drift.sh (the
# VSCode grammar highlights every kama.l keyword). All three read the same table for the same reason.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LEXER="$ROOT/src/kama.l"
SPEC="$ROOT/docs/SPEC.md"

fail() { echo "check-keyword-list: FAIL — $1" >&2; exit 1; }

[ -f "$LEXER" ] || fail "no src/kama.l"
[ -f "$SPEC" ]  || fail "no docs/SPEC.md"

tmp=$(mktemp -d) || fail "mktemp"
trap 'rm -rf "$tmp"' EXIT INT TERM

# The compiler's truth: the `{"word", TOKEN}` rows of the keyword table.
sed -n '/static struct name_value keywords/,/};/p' "$LEXER" \
    | grep -oE '"[a-z0-9_]+"' | tr -d '"' | LC_ALL=C sort -u > "$tmp/lex"

# The doc's claim: the first fenced block after the "## kama's keywords" heading.
awk '
    /^## kama.s keywords/      { inSec = 1; next }
    inSec && /^```/            { inBlk = !inBlk; if (!inBlk) exit; next }
    inBlk                      { print }
' "$SPEC" | tr -s ' \t' '\n' | grep -vE '^$' | LC_ALL=C sort -u > "$tmp/doc"

[ -s "$tmp/lex" ] || fail "read no keywords from src/kama.l — the table's shape changed, so this guard is measuring nothing"
[ -s "$tmp/doc" ] || fail "read no keywords from docs/SPEC.md — is the '## kama's keywords' section or its fenced block gone?"

missing=$(LC_ALL=C comm -23 "$tmp/lex" "$tmp/doc" | tr '\n' ' ')
extra=$(LC_ALL=C comm -13 "$tmp/lex" "$tmp/doc" | tr '\n' ' ')

rc=0
if [ -n "$(printf '%s' "$missing" | tr -d ' ')" ]; then
    echo "check-keyword-list: FAIL — kama.l reserves words SPEC does not publish:" >&2
    echo "    $missing" >&2
    echo "  Add them to the fenced list under '## kama's keywords' in docs/SPEC.md. A keyword nobody" >&2
    echo "  published is one a user finds by hitting it — which is how base/type/slot were each found." >&2
    rc=1
fi
if [ -n "$(printf '%s' "$extra" | tr -d ' ')" ]; then
    echo "check-keyword-list: FAIL — SPEC publishes words kama.l does not reserve:" >&2
    echo "    $extra" >&2
    echo "  Remove them: a list claiming a word is reserved sends someone renaming a legal binding." >&2
    rc=1
fi
[ "$rc" -eq 0 ] || exit 1

echo "check-keyword-list: PASS ($(wc -l < "$tmp/lex" | tr -d ' ') keywords, SPEC matches kama.l exactly)"
