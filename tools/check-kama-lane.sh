#!/bin/sh
# check-kama-lane.sh — every name kama chooses on a type is spelled by one helper, `kamaMember` (KR-318), and every
# generic instance's name by another, `instanceName` (KR-319; section 2 below).
#
# The defect this guards. kama joins two kinds of name onto a type's C name: the members a user declares
# (`T__area`) and what kama writes for itself (a destructor, a generated copy, a vtable, a graph-serde helper). Both
# shared the one `__` lane, so a member named after one of kama's met it: a method `dtor` beside `~R()` was
# "conflicting types for `R__dtor`", a variant `dtor` of an owning enum met the enum's destructor, a method `vdrop`
# on a virtual class met its virtual drop, `op_add` beside `operator+` was refused as a duplicate operator. The fix
# moves every name kama chooses to `<Type>__kama_<what>`, which no declared name may enter (a joined name may not
# begin `kama_`, SPEC § C names), and spells it in one place: `CEmitter::kamaMember` (src/kama.cemit.h).
#
# Two checks over the C the emitter writes (the string literals of src/kama.cemit*.cpp) and the runtime headers:
#   A. no name the helper chooses is joined by hand. The list is read from the helper's own calls, so a name moved
#      into the lane later is held without editing this file. A hand-written `R__dtor(&x)` would compile again the
#      day a type declares a member `dtor`, and call it.
#   B. a function the emitter DEFINES by joining a suffix by hand (`<< T << "__name(" << T << "* self`) is one of
#      the members a program calls: a derived one, or an intrinsic handle's. Anything else is a name kama chose
#      outside the lane, which is how the first ~20 were written. A new derived member joins MEMBERS below.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-kama-lane: FAIL — $*" >&2; exit 1; }

MEMBERS='bytes|compareTo|equals|expired|format|hash|mergeFrom|serialize|tryUpgrade|valid'

# Every `what` handed to kamaMember/preludeMember: the quoted words of the call's LAST argument, which is `"dtor"`,
# `"as_" + C`, or `isSize ? "vsize" : "valign"`. A word holds a letter: the `"__"` of `contract + "__" + n` is a separator,
# not a name, and read as one it made every `____mergeStep` literal look hand-joined.
whats() {
    awk '{
        s = $0
        while (match(s, /(kamaMember|preludeMember)\(/)) {
            s = substr(s, RSTART + RLENGTH); depth = 1; last = 0
            for (i = 1; i <= length(s) && depth > 0; i++) {
                c = substr(s, i, 1)
                if (c == "(") depth++
                else if (c == ")") depth--
                else if (c == "," && depth == 1) last = i
            }
            arg = substr(s, last + 1, i - last - 2)
            while (match(arg, /"_*[A-Za-z][A-Za-z_]*"/)) { print substr(arg, RSTART + 1, RLENGTH - 2); arg = substr(arg, RSTART + RLENGTH) }
        }
    }' "$@" | sort -u
}

# `file:line: text` for every string literal (whole-line `//` comments skipped) that joins a name in $1 by hand,
# or that defines a function by a hand-joined suffix outside $MEMBERS.
scan_emitter() {
    w=$1; shift
    for f in "$@"; do
        awk -v file="$f" -v w="$w" -v members="^($MEMBERS)$" '
            BEGIN { pre = "(^|[A-Za-z0-9\"])__(" w ")" }
            /^[[:space:]]*\/\// { next }
            {
                lits = ""; s = $0
                while (match(s, /"([^"\\]|\\.)*"/)) { lits = lits " \"" substr(s, RSTART + 1, RLENGTH - 2); s = substr(s, RSTART + RLENGTH) }
                hit = 0; t = lits
                while (match(t, pre)) {                          # a prefix (`as_`) ends anywhere; a word at a boundary
                    rest = substr(t, RSTART + RLENGTH); m = substr(t, RSTART, RLENGTH)
                    if (m ~ /_$/ || rest !~ /^[A-Za-z0-9_]/) { hit = 1; break }
                    t = rest
                }
                gsub(/kama_vtbl|__kama_[A-Za-z0-9_]*/, "", lits)   # a handle field, and globals kama names itself
                if (lits ~ /("|[A-Za-z0-9])_vt(bl|able)([^A-Za-z0-9_]|$)/) hit = 1
                d = $0
                while (match(d, /"__[a-z][A-Za-z0-9_]*\(" *(<<|\+) *[A-Za-z_][A-Za-z0-9_.>-]* *(<<|\+) *"\*/)) {
                    name = substr(d, RSTART + 3); sub(/\(.*/, "", name)
                    if (name !~ members) hit = 1
                    d = substr(d, RSTART + RLENGTH)
                }
                if (hit) printf "%s:%d: %s\n", file, NR, $0
            }' "$f"
    done
}

# The runtime headers are C, not literals: a joined name outside a comment.
scan_header() {
    w=$1; shift
    for f in "$@"; do
        awk -v file="$f" -v re="[A-Za-z0-9]__($w)([^A-Za-z0-9_]|$)" '
            { line = $0; sub(/\/\/.*/, "", line); if (line ~ re) printf "%s:%d: %s\n", file, NR, $0 }' "$f"
    done
}

# --- 0. the scanners catch what they are for (a guard that never fires proves nothing) ----------------------
printf '    *_out << kamaMember(ci.name, "dtor") << "(self);\\n";\n    return kamaMember(cls, "as_" + C);\n    mi.cName = kamaMember(tkey, contract + "__" + n);\n' > "$tmp/helper.cpp"
[ "$(whats "$tmp/helper.cpp" | tr '\n' ' ')" = "as_ dtor " ] || fail "the helper scan did not read exactly \`dtor\` and \`as_\` from planted calls (a \`\"__\"\` separator is no name)"
W='dtor|as_'
printf '    *_out << ci.name << "__dtor(self);\\n";\n' > "$tmp/join.cpp"
printf '    return cls + "__as_" + C;\n' > "$tmp/prefix.cpp"
printf '    *_out << "struct " << C << "_vtbl {\\n";\n' > "$tmp/vtbl.cpp"
printf '    *_out << "static inline void " << N << "__fresh(" << N << "* self);\\n";\n' > "$tmp/define.cpp"
printf '    *_out << "x.kama_vtbl = &" << kamaMember(K, "as_" + C) << ";\\n";\n    *_out << "static inline bool " << N << "__valid(" << N << "* self);\\n";\n    *_out << "case " << id << "u: " << K << "__area(self);\\n";   // T__dtor in a comment\n    *_out << "static const " << S << " __kama_null_ser_vtbl = {\\n";\n' > "$tmp/clean.cpp"
printf 'static inline void kama_string__dtor(kama_string* s) { }\n' > "$tmp/planted.h"
printf '// the `__dtor` slot\nstatic inline void kama_string__kama_dtor(kama_string* s) { }\n' > "$tmp/clean.h"
for p in join prefix vtbl define; do
    [ -n "$(scan_emitter "$W" "$tmp/$p.cpp")" ] || fail "the emitter scan missed a planted $p ($(cat "$tmp/$p.cpp"))"
done
[ -z "$(scan_emitter "$W" "$tmp/clean.cpp")" ] || fail "the emitter scan fired on a helper call, a handle field, a member or a comment"
[ -n "$(scan_header "$W" "$tmp/planted.h")" ]  || fail "the header scan missed a planted kama_string__dtor"
[ -z "$(scan_header "$W" "$tmp/clean.h")" ]    || fail "the header scan fired on a comment or the lane's own spelling"

# --- 1. the tree ------------------------------------------------------------------------------------------------
cd "$ROOT"
W=$(whats src/kama.cemit*.cpp src/kama.cemit.h | tr '\n' '|' | sed 's/|$//')
[ -n "$W" ] && [ "$(printf '%s' "$W" | tr '|' '\n' | grep -c .)" -ge 10 ] \
    || fail "read only '$W' from kamaMember/preludeMember — the helper's calls moved, so check A holds nothing"
{
    scan_emitter "$W" src/kama.cemit*.cpp
    scan_header "$W" include/*.h
} > "$tmp/hits"
if [ -s "$tmp/hits" ]; then
    sed 's/^/  /' "$tmp/hits" >&2
    fail "a name kama chooses on a type is joined by hand — spell it \`kamaMember(T, \"<what>\")\` (src/kama.cemit.h), which \
puts it in the \`<Type>__kama_<what>\` lane no declared name can enter. If it is a member a program calls (a derived \
method), add it to MEMBERS here"
fi

# --- 2. instance names (KR-319) -----------------------------------------------------------------------------------
# A generic instance is `<template>__kama_of__<arg>__kama_and__<arg>__kama_end`, spelled by `instanceName` and read
# back by `instanceTemplate`/`splitInstance`, all in src/kama.cemit.h. Through 0.9.849 it was a `_` join of the
# template and each argument's mangle, which met what a user can spell: a plain `Pair_int32` WAS `Pair<int32>`.
#   C. a marker is written only in that header: never in an emitter literal, never in a runtime header;
#   D. no name kamaMember chooses begins `of`, `and` or `end`, so `__kama_<what>` can never read as a marker;
#   E. no argument's mangle is joined on with a bare `_` by hand, which is the spelling that was the defect.
MARK='__kama_(of__|and__|end)'
scan_marks() {
    for f in "$@"; do
        awk -v file="$f" -v re="$MARK" '
            /^[[:space:]]*\/\// { next }
            { lits = ""; s = $0
              while (match(s, /"([^"\\]|\\.)*"/)) { lits = lits substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH) }
              if (lits ~ re) printf "%s:%d: %s\n", file, NR, $0 }' "$f"
    done
}
scan_marks_c() {
    for f in "$@"; do
        awk -v file="$f" -v re="$MARK" '{ line = $0; sub(/\/\/.*/, "", line); if (line ~ re) printf "%s:%d: %s\n", file, NR, $0 }' "$f"
    done
}
scan_underscore_join() { grep -nE '"_" *\+ *mangleElem\(' "$@" || true; }

printf '    std::string n = tmpl + "__kama_of__" + a + "__kama_end";\n' > "$tmp/mark.cpp"
printf '    return instanceName(tmpl, { a });   // `Pair__kama_of__int32__kama_end`\n' > "$tmp/markclean.cpp"
printf '    m += "_" + mangleElem(a);\n' > "$tmp/ujoin.cpp"
[ -n "$(scan_marks "$tmp/mark.cpp")" ]          || fail "the marker scan missed a planted hand-written instance name"
[ -z "$(scan_marks "$tmp/markclean.cpp")" ]     || fail "the marker scan fired on a helper call or a comment"
[ -n "$(scan_underscore_join "$tmp/ujoin.cpp")" ] || fail "the join scan missed a planted \`\"_\" + mangleElem(\`"

{
    scan_marks src/*.cpp
    scan_marks_c include/*.h
    scan_underscore_join src/*.cpp src/*.h
    printf '%s\n' "$W" | tr '|' '\n' | grep -E '^(of|and|end)' | sed 's/^/kamaMember what: /' || true
} > "$tmp/ihits"
if [ -s "$tmp/ihits" ]; then
    sed 's/^/  /' "$tmp/ihits" >&2
    fail "an instance name is spelled by hand — build it with \`instanceName(tmpl, args)\` or \`instanceOf\` (src/kama.cemit.h), \
and read one with \`instanceTemplate\`/\`splitInstance\`; a name kama chooses on a type may not begin \`of\`, \`and\` or \`end\`"
fi
echo "check-kama-lane: OK ($(printf '%s' "$W" | tr '|' '\n' | grep -c .) names kama chooses, each spelled by kamaMember;"
echo "                 every instance name spelled by instanceName)"
