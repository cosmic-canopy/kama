#!/bin/sh
# check-treesitter.sh — the TREE-SITTER GRAMMAR oracle. kama has three grammars now: the compiler's own
# Flex/Bison front end (kama.l/kama.y, the source of truth), the TextMate grammar guarded by
# check-syntax-drift.sh + check-syntax.sh, and tree-sitter-kama/, guarded here.
#
# The TextMate pair splits along "a grep can see that a rule EXISTS; a real engine can see that a rule
# FIRES". That split is folded into this ONE file, along a second axis that matters more in practice —
# what a checkout can run WITHOUT the tree-sitter CLI installed:
#
#   ALWAYS (needs only $KAMA and grep — no CLI, so a fresh clone still gets real teeth):
#     1. KEYWORD DRIFT — every keyword in kama.l's table is a terminal in the generated grammar, the
#        numeric suffixes agree, and — the INVERSE check — the contextual kind words are NOT terminals.
#     2. THE SHARED FIXTURES — tests/syntax/*.kama agree with `kama check`, exactly as check-syntax.sh
#        asserts, so the two guards cannot drift apart about what that corpus means.
#
#   ONLY WITH THE PINNED CLI (skips loudly, like check-syntax.sh, so a fresh clone is not failed):
#     3. GENERATE IS A NO-OP — the committed src/ regenerates byte-identically from grammar.js.
#     4. CORPUS TESTS + QUERY COMPILATION — test/corpus/*.txt pass, and every .scm compiles against the
#        grammar. A query naming a node the grammar does not have is silently DEAD in Helix and Zed; that
#        is the same defect class as the TextMate dead-rule bug, and only a real engine can see it.
#     5. THE WHOLE-CORPUS ORACLE — every .kama file the COMPILER accepts parses with zero ERROR nodes,
#        and every file it rejects with a parse/lexical error produces one. This is the oracle that makes
#        the grammar's claims true rather than plausible: it caught a missing `base.m()` callee on its
#        first run, in a file no fixture covered.
#
# The CLI is PINNED EXACT in tree-sitter-kama/package.json because `tree-sitter generate` output is not
# byte-stable across versions — an unpinned CLI would turn oracle 3 into noise. A version mismatch SKIPs.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
G="$ROOT/tree-sitter-kama"
FIX="$ROOT/tests/syntax"
MANIFEST="$G/test/parse-errors.txt"

fails=0
note() { echo "FAIL treesitter: $1" >&2; fails=$((fails+1)); }

if [ ! -d "$G" ]; then echo "FAIL treesitter: missing $G" >&2; exit 1; fi
if [ ! -x "$KAMA" ]; then echo "FAIL treesitter: $KAMA not built" >&2; exit 1; fi

# ---- 1. KEYWORD DRIFT (no dependencies) --------------------------------------------------------------
# Source of truth is kama.l's `static struct name_value keywords` table. The target is the GENERATED
# src/grammar.json rather than grammar.js: it is structured, so a keyword cannot be faked by an identifier
# that merely contains it, and it is what the parser was actually built from.
# ⚠️ The character class MUST include digits. check-syntax-drift.sh:17-20 records that blind spot — a
# `[a-z_]+` class silently excluded every one of int8…uint64 and float32.
lexer_keywords() {
    sed -n '/static struct name_value keywords/,/};/p' "$ROOT/src/kama.l" |
        grep -oE '\{"[a-z0-9_]+"' | tr -d '{"' | sort -u
}
grammar_terminals() {
    grep -oE '"value": *"[^"]+"' "$G/src/grammar.json" |
        sed -E 's/.*"value": *"//; s/"$//' | sort -u
}

if [ -f "$G/src/grammar.json" ]; then
    tmpk=$(mktemp); tmpg=$(mktemp)
    lexer_keywords > "$tmpk"
    grammar_terminals > "$tmpg"
    missing=$(comm -23 "$tmpk" "$tmpg")
    if [ -n "$missing" ]; then
        note "the grammar is missing keywords that kama.l reserves:"
        echo "$missing" | sed 's/^/      - /' >&2
        echo "    Add them to grammar.js and re-run 'tree-sitter generate'." >&2
    fi

    # The numeric SUFFIX shape, transcribed from kama.l. The unsigned suffix is `ui`, never `u` — a bare
    # u8/u16/u32/u64 is the defect that started the TextMate numeric audit.
    grep -qF 'u?i(8|16|32|64)' "$G/grammar.js" ||
        note "grammar.js no longer carries kama.l's int_suffix 'u?i(8|16|32|64)' — 42ui32 is the spelling, 42u32 is NOT a literal."
    grep -qF 'f(32|64)' "$G/grammar.js" ||
        note "grammar.js no longer carries the float suffixes f32/f64."

    # THE INVERSE CHECK, and the one that protects the whole contextual-kind design: if `value` or
    # `resource` ever becomes a grammar TERMINAL, `word:` promotes it to a keyword and `int32 value = 1;`
    # stops parsing. A plain drift grep can only ever ask whether something is present; this asks whether
    # something is absent, which is what this design needs.
    for kw in value resource view contract intrinsic both; do
        if grep -qE "\"value\": *\"$kw\"" "$G/src/grammar.json"; then
            note "'$kw' is a grammar TERMINAL, but it is a contextual IDENTIFIER in kama.l — promoting it to a keyword breaks 'int32 $kw = 1;'. It must stay a (type_kind)/(kind_name) node over an identifier."
        fi
    done
    rm -f "$tmpk" "$tmpg"
else
    note "$G/src/grammar.json is missing — run 'tree-sitter generate' in tree-sitter-kama/ and commit src/."
fi

# ---- 2. THE SHARED FIXTURES vs THE COMPILER (needs only $KAMA) ---------------------------------------
# Identical in meaning to check-syntax.sh's oracle 2, deliberately: the two grammars are checked against
# the same four files, so they cannot come to disagree about what that corpus asserts.
for f in "$FIX"/*.kama; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    if [ "$base" = "bad.kama" ]; then
        if "$KAMA" check "$f" >/dev/null 2>&1; then
            note "bad.kama COMPILES — it exists to hold spellings the compiler rejects."
        fi
    else
        if ! "$KAMA" check "$f" >/dev/null 2>&1; then
            note "$base does not pass 'kama check' — a grammar assertion about code the compiler rejects proves nothing."
        fi
    fi
done

# ---- the skip gate -----------------------------------------------------------------------------------
# ⚠️ Do NOT probe with `[ -x "$TS" ]`. node_modules/.bin/tree-sitter is a symlink to a JS shim that is
# always present and always executable; the 20 MB binary beside it is fetched by the package's install
# script, so `npm ci --ignore-scripts` leaves a tree that passes an -x test and then dies with an
# unhandled 'error' event. Probe by RUNNING it.
TS="$G/node_modules/.bin/tree-sitter"
if ! "$TS" --version >/dev/null 2>&1; then
    TS=$(command -v tree-sitter 2>/dev/null || true)
fi
skip() {
    echo "SKIP treesitter: $1"
    echo "  (keyword drift + fixture agreement above still ran and PASSED)"
    [ "$fails" -eq 0 ] || { echo "FAIL treesitter ($fails problem(s))" >&2; exit 1; }
    exit 0
}
if [ -z "$TS" ] || ! "$TS" --version >/dev/null 2>&1; then
    skip "no working tree-sitter CLI (run: npm --prefix tree-sitter-kama ci)"
fi

PIN=$(sed -n 's/.*"tree-sitter-cli": *"\([^"]*\)".*/\1/p' "$G/package.json")
HAVE=$("$TS" --version 2>/dev/null | awk '{print $NF}')
if [ "$HAVE" != "$PIN" ]; then
    skip "CLI is $HAVE but src/ was generated by $PIN — a byte-diff against a different generator is meaningless. Use the pinned one: npm --prefix tree-sitter-kama ci"
fi
command -v cc >/dev/null 2>&1 || skip "no C compiler on PATH (the CLI compiles src/parser.c on demand)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- 3. GENERATE IS A NO-OP --------------------------------------------------------------------------
# Regenerate into a COPY, never in place: generating in the worktree would leave it dirty on failure and
# would make this oracle depend on git. Do not "simplify" this back.
cp -R "$G" "$tmp/g"
rm -rf "$tmp/g/node_modules" "$tmp/g/src"
if ! (cd "$tmp/g" && "$TS" generate >"$tmp/gen.log" 2>&1); then
    note "'tree-sitter generate' FAILED — grammar.js does not compile:"
    sed 's/^/      /' "$tmp/gen.log" >&2
elif ! diff -ru "$G/src" "$tmp/g/src" >"$tmp/gen.diff" 2>&1; then
    note "src/ differs from a fresh 'tree-sitter generate'. Regenerate and commit:"
    echo "      (cd tree-sitter-kama && node_modules/.bin/tree-sitter generate)" >&2
    head -40 "$tmp/gen.diff" | sed 's/^/      /' >&2
fi

# ---- 4. CORPUS TESTS + QUERY COMPILATION -------------------------------------------------------------
if ! (cd "$G" && "$TS" test) >"$tmp/test.log" 2>&1; then
    note "'tree-sitter test' failed over test/corpus/:"
    sed 's/^/      /' "$tmp/test.log" >&2
fi
ncorpus=$(awk '/^={70,}$/{c++} END{print int(c/2)}' "$G"/test/corpus/*.txt 2>/dev/null || echo 0)

# A query that names a node the grammar does not have is not an error anywhere — it is silently DEAD, and
# the editor just shows less colour. Compile every one of them, in BOTH vocabularies.
nq=0
for q in "$G"/queries/*.scm "$ROOT"/editor/zed/languages/kama/*.scm; do
    [ -e "$q" ] || continue
    nq=$((nq+1))
    if ! (cd "$G" && "$TS" query "$q" "$FIX/declarations.kama") >"$tmp/q.log" 2>&1; then
        note "$(basename "$(dirname "$q")")/$(basename "$q") does not compile against the grammar:"
        head -10 "$tmp/q.log" | sed 's/^/      /' >&2
    fi
done

# ---- 5. THE WHOLE-CORPUS ORACLE ----------------------------------------------------------------------
# ⚠️ `find` must be -type f: tests/pkg_path_dep.d/ contains a DIRECTORY literally named `.kama`, and
# feeding it to the CLI aborts the whole run with "Is a directory (os error 21)".
find "$ROOT/tests" "$ROOT/lib" "$ROOT/examples" "$ROOT/prelude" "$ROOT/seed" -type f -name '*.kama' 2>/dev/null |
    sed "s|^$ROOT/||" | sort > "$tmp/all"
ncorp=$(wc -l < "$tmp/all" | tr -d ' ')

# One CLI process over every file (~1 s), rather than 884 `kama check` calls (~43 s).
(cd "$ROOT" && sed "s|^|$ROOT/|" "$tmp/all" > "$tmp/all.abs"
 cd "$G" && "$TS" parse --quiet --paths "$tmp/all.abs" 2>/dev/null || true) > "$tmp/parse.out"
grep -E '\(ERROR|\(MISSING' "$tmp/parse.out" | awk '{print $1}' |
    sed "s|^$ROOT/||" | sort -u > "$tmp/ts-bad"

grep -v '^#' "$MANIFEST" | grep -v '^[[:space:]]*$' | sort > "$tmp/expected"

if ! diff -u "$tmp/expected" "$tmp/ts-bad" > "$tmp/delta" 2>&1; then
    # The two disagree. Ask the COMPILER about exactly the files in the symmetric difference — and only
    # those — so a stale manifest and a wrong grammar are told apart by name instead of guessed at.
    comm -3 "$tmp/expected" "$tmp/ts-bad" | tr -d '\t' | sort -u > "$tmp/disputed"
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if "$KAMA" check "$ROOT/$f" 2>&1 | grep -qE 'Parse error:|Lexical error:'; then
            compiler=reject
        else
            compiler=accept
        fi
        if grep -qxF "$f" "$tmp/ts-bad"; then ts=reject; else ts=accept; fi

        if [ "$compiler" = "$ts" ]; then
            note "test/parse-errors.txt is STALE for $f (the compiler and tree-sitter both $compiler it). Update the manifest."
        elif [ "$compiler" = accept ]; then
            note "THE GRAMMAR IS WRONG: the compiler ACCEPTS $f but tree-sitter produces an ERROR node. Run: (cd tree-sitter-kama && node_modules/.bin/tree-sitter parse ../$f | grep ERROR)"
        else
            note "THE GRAMMAR IS TOO PERMISSIVE: the compiler REJECTS $f with a parse/lexical error but tree-sitter parses it clean."
        fi
    done < "$tmp/disputed"
fi

nbad=$(wc -l < "$tmp/ts-bad" | tr -d ' ')
nok=$((ncorp - nbad))

if [ "$fails" -ne 0 ]; then
    echo "FAIL treesitter ($fails problem(s))" >&2
    exit 1
fi
echo "PASS treesitter (src/ regenerates byte-identically; $ncorpus corpus tests; $nq queries compile; $nok accepted .kama files parse clean, $nbad rejected files error — agreeing with the compiler exactly)"
