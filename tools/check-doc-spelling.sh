#!/bin/sh
# check-doc-spelling.sh — a kama code block in the docs must not teach a spelling the compiler refuses.
#
# The defect this guards. `docs/SPEC.md` line 1062 says, in prose, "**Bare `int` is not a kama type.**" The
# same file then used `int` as a type in SEVENTEEN code blocks — `fn int add(int a, int b)`, `int n = 0`,
# `public fn int get()`. Those snippets were legal once. The module campaign (`0.9.80`) made every C keyword
# a RESERVED WORD, so today each one is a hard lexical error with a message telling the reader what to write
# instead, and nothing noticed, because nothing reads the docs' code. Measured when this guard was written:
# 29 × bare `int` in SPEC.md, 10 × bare `float` in TYPE_MODEL.md, and 8 × a module-import form the same
# campaign deleted.
#
# This is the house rule pointed at the docs. A doc is not evidence — but a doc that teaches a syntax error
# is worse than one that is merely stale, because a reader (or an LLM) copies it, and the first thing they
# meet is a compiler refusing the language's own specification.
#
# ⚠️ WHY NOT SIMPLY COMPILE EVERY BLOCK. That was the first design, and it is wrong. Measured: 81 of the 139
# fenced kama blocks in `docs/` do not PARSE standalone, and almost none of those is a defect — they are
# deliberate fragments (a bare statement, a lone field, a literal `…` ellipsis). Compiling them means
# annotating ~110 blocks with opt-out markers: an L of churn that would not have caught one of the 47 real
# defects above. A LEXICAL check needs no annotation, because a reserved word is refused wherever it appears
# — fragment or not. That is the whole reason this guard works on text rather than on the compiler.
#
# The flagged set is DERIVED, never listed here: `c_reserved[]` in `kama.l` minus that file's own keyword
# table, which is the set of C words kama reserves but does not itself use (38 today). Hardcoding it is
# what would let this guard rot the same way the docs did — the reservation is expected to change, and
# `tools/check-syntax-drift.sh` already reads these same two tables for the same reason.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LEXER="$ROOT/src/kama.l"
. "$ROOT/tools/kama-bin.sh"          # exports $KAMA (absolute; never the ./kama symlink — see AGENTS.md)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-doc-spelling: FAIL — $1" >&2; fail=1; }

# ---- the flagged set ---------------------------------------------------------------------------------
# Every C keyword kama reserves, minus the ones kama uses as its own keywords (`bool`, `case`, `return`,
# `void`, … — those are legitimate in a snippet and never reach the lexer's identifier rule).
sed -n '/static struct name_value keywords/,/};/p' "$LEXER" \
    | grep -oE '\{"[a-z0-9_]+"' | tr -d '{"' | sort -u > "$tmp/kama_kw"
sed -n '/static const char\* c_reserved/,/};/p' "$LEXER" \
    | grep -oE '"[A-Za-z_][A-Za-z0-9_]*"' | tr -d '"' | sort -u > "$tmp/c_reserved"
comm -23 "$tmp/c_reserved" "$tmp/kama_kw" > "$tmp/flagged"

if [ ! -s "$tmp/flagged" ]; then
    echo "check-doc-spelling: FAIL — derived an EMPTY flag set from $LEXER." >&2
    echo "  Either table moved or was renamed; a guard that reads nothing passes everything." >&2
    exit 1
fi

# ⚠️ The derivation is the guard's only oracle, so prove it answers about the real compiler rather than
# about a table that has drifted from it. `int` is the spelling this guard was written for and the one with
# the friendliest message; if the compiler ever ACCEPTS it, every hit below is a false positive.
grep -qx 'int' "$tmp/flagged" || {
    echo "check-doc-spelling: FAIL — `int` is not in the derived flag set; the derivation is broken." >&2; exit 1; }
printf 'fn int main() { return 0; }\n' > "$tmp/probe.kama"
if "$KAMA" check "$tmp/probe.kama" >/dev/null 2>&1; then
    echo "check-doc-spelling: FAIL — the compiler ACCEPTS `fn int main()`, so this guard is flagging" >&2
    echo "  a legal spelling. Reconcile with kama.l's c_reserved[] before trusting any hit below." >&2
    exit 1
fi

# ---- 1. fenced ```kama blocks must not use a reserved spelling ----------------------------------------
#
# ⚠️ STRIP STRINGS AND COMMENTS FIRST — this is load-bearing, not tidiness. Without it the corpus yields
# false positives that would have to be waived one by one, and a guard with a waiver list is a guard nobody
# trusts: `// typedef int (*CompareFn)(...)` (a C prototype, correctly shown as C), `// auto-deref`,
# ``// inline `new` in return position``, `// ASan: attempting double-free`. With it, every remaining hit
# across all 139 blocks is a real defect. Zero false positives — measured, not assumed.
docs=$(find "$ROOT/docs" -name '*.md' | sort)

for f in $docs; do
    rel=${f#"$ROOT"/}
    awk -v rel="$rel" '
        /^[[:space:]]*```kama/ { inblk = 1; next }
        /^[[:space:]]*```/     { inblk = 0; next }
        inblk { print rel ":" NR ":" $0 }
    ' "$f"
done | sed 's/"[^"]*"//g; s://.*::' > "$tmp/blocklines"

while read -r word; do
    hits=$(grep -nw -- "$word" "$tmp/blocklines" | cut -d: -f2- || true)
    [ -z "$hits" ] && continue
    n=$(printf '%s\n' "$hits" | wc -l | tr -d ' ')
    note "a kama code block spells \`$word\`, which the compiler reserves ($n line(s)):"
    printf '%s\n' "$hits" | sed 's/^/    /' | head -8 >&2
    [ "$n" -gt 8 ] && echo "    … and $((n - 8)) more" >&2
done < "$tmp/flagged"

# ---- 2. the module-import form the module campaign deleted --------------------------------------------
#
# Scanned over the WHOLE document, not just the fenced blocks: all eight live in prose, inside inline
# backticks (`import std::math::{Vec3, Mat4, …}`), which a block-only scan misses entirely. It is a hard
# parse error — "syntax error, unexpected IDENTIFIER, expecting {" — and the one form that parses is
# `import { std::math::Vec3 };`, brace first.
badimp=$(grep -rn "import [A-Za-z_][A-Za-z0-9_:]*::{" "$ROOT/docs" || true)
if [ -n "$badimp" ]; then
    n=$(printf '%s\n' "$badimp" | wc -l | tr -d ' ')
    note "the docs teach \`import <path>::{…}\`, deleted by the module campaign ($n line(s)):"
    printf '%s\n' "$badimp" | sed "s|$ROOT/||; s/^/    /" | head -8 >&2
fi

[ "$fail" = 0 ] || {
    echo "" >&2
    echo "  Write the spelling the compiler accepts. \`int\` -> \`int32\` (or \`isize\` for a length or" >&2
    echo "  index), \`float\`/\`double\` -> \`float32\`/\`float64\`, \`import p::{X}\` -> \`import { p::X }\`." >&2
    echo "  PROSE is untouched by the reserved-word half — a sentence may name \`int\` to say it is not a" >&2
    echo "  kama type, which is exactly what SPEC.md and agents/AGENTS.md correctly do. The import half" >&2
    echo "  reads prose too (all eight originals lived there), so to DESCRIBE the deleted form rather" >&2
    echo "  than teach it, name the \`p::{X}\` spelling without the \`import\` in front of it." >&2
    exit 1
}

nblk=$(grep -c '' "$tmp/blocklines" || true)
nflag=$(grep -c '' "$tmp/flagged" || true)
echo "check-doc-spelling: PASS ($nblk lines of fenced kama across docs/ use none of the $nflag C words"
echo "                          kama reserves, and no doc teaches the deleted \`import p::{X}\` form)"
