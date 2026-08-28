#!/bin/sh
# check-builtin-doc.sh — hold `prelude/builtin.kama` against the C++ that actually registers the built-ins.
#
# `int32`, `string`, `isize` and `string`'s methods are registered in C++ — reserved words, not
# declarations — so there is no source anywhere for go-to-definition to open. The fix is the shape Go and
# Rust both use: a documentation-only file the tooling points at (`builtin.go`, `primitive_docs.rs`).
#
# ⚠️ THAT SHAPE HAS EXACTLY ONE FAILURE MODE, and this file is it. A hand-written doc and the C++
# registrations are TWO STATEMENTS OF ONE TRUTH, so without a guard the doc rots the way an unguarded
# prose claim does — silently, and in the direction that matters most, because a reader who jumps to a
# stale file believes it. So both directions are asserted:
#
#   1. Every name the compiler registers is DOCUMENTED     (else a real built-in still lands nowhere).
#   2. Every name documented is REGISTERED                 (else the file describes a language kama
#                                                           does not have, which is worse than no file).
#   3. The POSITIONS are right                             (the file's layout is the scanner's contract:
#                                                           reformatting it moves where the jump lands).
#
# 1 and 2 are set comparisons against the two places the compiler states the truth machine-readably:
# `IDENTIFIER_*_VAL` in kama.ast.h (the reserved words) and `addMethod("…")` inside registerCollection's
# string block (the intrinsics). 3 is behavioural — it compiles a probe and asks the compiler.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-builtin-doc: $KAMA not built" >&2; exit 1; fi

DOC="$ROOT/prelude/builtin.kama"
[ -f "$DOC" ] || { echo "check-builtin-doc: missing $DOC" >&2; exit 1; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail=0
say_ok()   { echo "  ok: $1"; }
say_fail() { echo "  FAIL: $1" >&2; fail=1; }

# cmp_sets <label> <expected-file> <actual-file> <hint>
cmp_sets() {
    _missing=$(comm -23 "$2" "$3")
    _extra=$(comm -13 "$2" "$3")
    if [ -z "$_missing" ] && [ -z "$_extra" ]; then
        say_ok "$1"
    else
        [ -n "$_missing" ] && printf '  FAIL: %s — registered but NOT documented: %s\n' \
            "$1" "$(printf '%s' "$_missing" | tr '\n' ' ')" >&2
        [ -n "$_extra" ] && printf '  FAIL: %s — documented but NOT registered: %s\n' \
            "$1" "$(printf '%s' "$_extra" | tr '\n' ' ')" >&2
        echo "        ($4)" >&2
        fail=1
    fi
}

echo "check-builtin-doc: prelude/builtin.kama vs the C++ registrations"

# ---------------------------------------------------------------------------------------------------
# 1+2a. The reserved-word types. `IDENTIFIER_<NAME>_VAL` in kama.ast.h IS the list — the grammar builds
# every primitive IdentifierNode with one of these — and the spelling is the name lowercased.
grep -oE '^#define IDENTIFIER_[A-Z0-9]+_VAL' "$ROOT/src/kama.ast.h" \
    | sed 's/#define IDENTIFIER_//; s/_VAL//' | tr 'A-Z' 'a-z' \
    | grep -v '^none$' | sort -u > "$tmp/registered"

# The three that are plain IDENTIFIERS rather than reserved words: `cType` special-cases them by spelling
# instead of by `builtInVal`, so they carry no `IDENTIFIER_*_VAL` and have to be named here. Each is
# checked below to still BE special-cased, so this list cannot quietly outlive the code it stands for.
IDENT_BUILTINS='isize usize UnsafePtr InlineArray BindableFunctionPtr'
for n in $IDENT_BUILTINS; do echo "$n"; done | sort -u >> "$tmp/registered"
sort -u -o "$tmp/registered" "$tmp/registered"

# What the doc declares: `type <kind> <Name>`, one per line — the scanner's own rule.
grep -oE '^type +[a-z]+ +[A-Za-z_][A-Za-z0-9_]*' "$DOC" | awk '{print $3}' | sort -u > "$tmp/documented"

cmp_sets "every built-in TYPE the compiler registers is documented, and vice versa" \
    "$tmp/registered" "$tmp/documented" \
    "add or remove a \`type <kind> <Name>\` line in prelude/builtin.kama"

# The identifier-spelled three are only built-in because kama.cemit.cpp says so by NAME. If that
# special-casing goes away the name stops being a built-in, and the doc entry becomes a lie that
# assertion 1 above cannot see — it would still be in this script's own list.
for n in $IDENT_BUILTINS; do
    if grep -qF "\"$n\"" "$ROOT/src/kama.cemit.cpp"; then :; else
        say_fail "\`$n\` is documented and listed here as a built-in, but kama.cemit.cpp no longer names it"
    fi
done
say_ok "...and the identifier-spelled built-ins are still special-cased in kama.cemit.cpp"

# ---------------------------------------------------------------------------------------------------
# 1+2b. `string`'s intrinsic METHODS. Scoped to registerCollection's own block: `addMethod` is also called
# by registerFixed and registerBindable, whose `get`/`set` belong to InlineArray and a bound fn pointer,
# not to `string`. An unscoped grep would demand `set` be documented as a string method.
awk "/string\`'s intrinsic method set/,/_classes\[cName\] = ci;/" "$ROOT/src/kama.cemit.cpp" \
    | grep -oE 'addMethod\("[a-zA-Z]+"' | sed 's/addMethod("//; s/"//' | sort -u > "$tmp/reg_methods"

sed -n '/^type value string {/,/^}/p' "$DOC" \
    | grep -oE 'fn +[A-Za-z0-9_<>]+ +[a-zA-Z]+\(' | sed 's/.* //; s/(//' | sort -u > "$tmp/doc_methods"

cmp_sets "every \`string\` intrinsic is documented, and vice versa" \
    "$tmp/reg_methods" "$tmp/doc_methods" \
    "add or remove a \`public fn <ret> <name>(…)\` line inside \`type value string\` in prelude/builtin.kama"

# ---------------------------------------------------------------------------------------------------
# 3. The POSITIONS, asked of the compiler rather than of this script. The file's layout is the scanner's
# contract (one declaration per line, `type <kind> <Name>`, `fn <ret> <name>(` indented inside its type),
# so a reformat that keeps every name would pass both set comparisons above and still send every jump to
# the wrong line. This is the assertion that catches it.
#
# One probe per KIND rather than per name — a reserved word, an identifier-spelled type, and an intrinsic
# method each reach the index by a different road, and it is the roads that break, not the individual
# names. (A reserved word in a SIGNATURE and one in a BODY are the same road; the body is the one that
# needed `recordBuiltinRef`, so it is the one probed.)
cat > "$tmp/p.kama" <<'KAMA'
fn int32 main() {
    string s = "hi";
    isize n = s.length();
    int32 k = 1;
    return k + cast<int32>(n);
}
KAMA

# docline <name>            -> the 1-based line of `type <kind> <name>`
# docmethodline <name>      -> the 1-based line of `fn … <name>(` inside `type value string`
docline()       { grep -nE "^type +[a-z]+ +$1( |<|\{)" "$DOC" | head -1 | cut -d: -f1; }
docmethodline() { grep -nE "^ +public fn +[A-Za-z0-9_<>]+ +$1\(" "$DOC" | head -1 | cut -d: -f1; }

probe() {   # probe <label> <L:C> <expected-line>
    _got=$( cd "$tmp" && "$KAMA" query "$tmp/p.kama" --def "$2" 2>/dev/null | tail -1 )
    case "$_got" in
        */prelude/builtin.kama:"$3":*) say_ok "$1 -> builtin.kama:$3" ;;
        *) say_fail "$1 should open builtin.kama line $3, got '$_got'" ;;
    esac
}
probe "a reserved word in a body (\`string\`)"      2:4  "$(docline string)"
probe "a reserved word in a body (\`int32\`)"       4:4  "$(docline int32)"
probe "an identifier-spelled built-in (\`isize\`)"  3:4  "$(docline isize)"
probe "an intrinsic method (\`s.length()\`)"        3:16 "$(docmethodline length)"

# ...and the one thing that must NOT happen: a doc entry is a place to READ, never a symbol of the
# program. If these leaked, `builtin.kama`'s 20 types would flood every outline and every Ctrl+T.
if "$KAMA" query "$tmp/p.kama" --symbols 2>/dev/null | grep -qE '\b(int32|string|isize)\b'; then
    say_fail "a built-in leaked into the document outline — it is a location, not a declaration"
else
    say_ok "...and no built-in leaks into the outline or the symbol search"
fi

if [ "$fail" != 0 ]; then echo "check-builtin-doc: FAILED" >&2; exit 1; fi
echo "check-builtin-doc: OK ($(wc -l < "$tmp/documented" | tr -d ' ') types, $(wc -l < "$tmp/doc_methods" | tr -d ' ') string intrinsics, positions verified)"
