#!/bin/sh
# check-scan-parity.sh — the two discovery walks over the AST must visit the same node kinds.
#
# The defect this guards. `CEmitter` has two pre-passes that walk a function body before emission, asking
# the same question — what must be REGISTERED before the emitter needs it:
#
#     scanExprForCollections / scanStmtForCollections   -> collection + generic-TYPE instances
#     scanExprForGenerics    / scanStmtForGenerics      -> generic-FUNCTION instantiations
#
# They are separate `dynamic_cast` ladders over the same tree, so they drift, and drift is invisible:
# whichever pass is missing an arm simply never learns about anything written inside that shape, and the
# consequence surfaces far away, as a diagnostic naming the wrong cause or as generated C that only the C
# compiler refuses. Measured on 0.9.99, the generics half was missing three kinds its sibling had —
# `MatchNode`, `ArrayLiteralNode`, `AsDowncastNode` — so NO generic call written inside a `match`, an array
# literal or an `expr.as<T>()` operand was ever discovered. Nobody could write one, so nothing caught it:
# a corpus grep found zero such calls across lib/, tests/, prelude/, examples/ and bench/.
#
# The rule is symmetric parity per pair, derived from the sources and never listed here — hardcoding the
# node set is what would let this guard rot the same way the walks did. Same idiom as
# `tools/check-syntax-drift.sh` and `tools/check-doc-spelling.sh`.
#
# ⚠️ A deliberate asymmetry is DECLARED, in EXEMPT below, with the reason. Silence is not a way to pass.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/kama.cemit.cpp"

[ -f "$SRC" ] || { echo "check-scan-parity: FAIL — no $SRC" >&2; exit 1; }

# Node kinds one side may legitimately visit alone. One `pair:kind` per line, with the reason beside it.
# The collections walk registers types and the generics walk registers instantiations, but both must still
# ENTER every shape that can contain either.
#
# - IdentifierNode: a LEAF — it contains no shape, so neither walk enters one. The generics walk casts to it
#   only to read a method call's receiver. The collections walk used to name it for the same kind of peek
#   (the primitive-widening recorder, deleted with KR-37), which is all that kept the two sets equal.
# - SizeofNode: its operand is a TYPE, never an expression, so it can hold no call for the generics walk to
#   find. The collections walk enters it because the C is `sizeof(<the instance's struct>)`, and an instance
#   named nowhere else must still be registered (KR-62).
EXEMPT="scanExprForCollections:IdentifierNode
scanExprForGenerics:SizeofNode"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0

# The node kinds one function's `dynamic_cast<X*>` ladder tests. Read from the function body only — awk
# stops at the closing brace in column 1, which is this file's style throughout.
kinds() {
    awk "/^void CEmitter::$1\\(/,/^}/" "$SRC" | grep -oE 'dynamic_cast<[A-Za-z_]+\*>' | sort -u
}

# ⚠️ The derivation is this guard's only oracle, so prove it reads something before trusting a clean
# comparison. Two empty sets compare equal, and a renamed function would make every pair empty — which is
# exactly the shape of "passes because it cannot read its inputs".
check_pair() {
    a="$1"; b="$2"
    kinds "$a" > "$tmp/a"
    kinds "$b" > "$tmp/b"
    if [ ! -s "$tmp/a" ] || [ ! -s "$tmp/b" ]; then
        echo "check-scan-parity: FAIL — derived an EMPTY node set for $a or $b." >&2
        echo "  A walk was renamed or reshaped; a guard that reads nothing passes everything." >&2
        fail=1
        return
    fi
    # Both directions. Spelled out rather than looped: an `[ … ] && x=y` that tests false is a non-zero
    # statement, and under `set -e` that ends the script — silently, half-checked, reporting nothing.
    report_only "$(comm -23 "$tmp/a" "$tmp/b")" "$a" "$b"
    report_only "$(comm -13 "$tmp/a" "$tmp/b")" "$b" "$a"
}

# $1 = newline-separated `dynamic_cast<X*>` spellings only `$2` visits; `$3` is the walk that lacks them.
# One `for` over the word-split list: no pipe, so no subshell, so `fail=1` set here is the one the caller
# reads. (A `while read` on the right of a pipe cannot set it — that is how a guard reports and still
# exits 0.) `if`, not `&&`, for the same reason the caller spells its two directions out: a false `&&`
# list is a non-zero statement and `set -e` ends the script on it.
report_only() {
    for kind in $1; do
        k=$(printf '%s' "$kind" | sed -E 's/dynamic_cast<([A-Za-z_]+)\*>/\1/')
        if printf '%s\n' "$EXEMPT" | grep -qx "$3:$k"; then continue; fi
        echo "check-scan-parity: FAIL — $2 visits $k and $3 does not." >&2
        echo "  The two walks answer the same question over the same tree; a shape only one enters is" >&2
        echo "  a discovery hole. Add the arm to $3, or declare the asymmetry in EXEMPT with why." >&2
        fail=1
    done
}

check_pair scanExprForCollections scanExprForGenerics
check_pair scanStmtForCollections scanStmtForGenerics

# A kind BOTH walks miss is one parity cannot see: two walks lacking the same arm agree. That is how the generic
# call in `bitcast<uint64>(sin(x: x))` and in `copy xs[pick(i: 1)]` was never instantiated (to 0.9.852): neither
# walk entered a BitcastNode or a HandoffNode, and the first refused as an untyped operand, the second as "no
# instantiation was resolved". The oracle is the emitter's own ladder, `emitExpressionNode`: every kind it lowers
# is entered by both walks, is declared one-sided in EXEMPT, or is a LEAF below, which holds no expression.
#
# - BooleanNode, CharNode, NullNode, StringNode, ZeroValueNode: literals.
# - ThisAccessNode: `this`.
# - InterpolatedStringNode: a hole is lexed as a place (a name with `.field`/`[index]`), so no call can be
#   written in one (tests/xfail/interp_hole_operator), and a place is registered where it was declared.
# - BaseAccessNode: only `base.field` lowers; `base[…]` is refused at emission, so its list never reaches C.
LEAVES="BooleanNode CharNode NullNode StringNode ZeroValueNode ThisAccessNode InterpolatedStringNode BaseAccessNode"
awk '/^std::string CEmitter::emitExpressionNode\(/,/^}/' "$SRC" | grep -oE 'dynamic_cast<[A-Za-z_]+\*>\(n\)' \
    | sed -E 's/dynamic_cast<([A-Za-z_]+)\*>.*/\1/' | sort -u > "$tmp/emitted"
if [ ! -s "$tmp/emitted" ]; then
    echo "check-scan-parity: FAIL — derived an EMPTY node set for emitExpressionNode; it was renamed or reshaped." >&2
    fail=1
fi
for walk in scanExprForCollections scanExprForGenerics; do
    kinds "$walk" | sed -E 's/dynamic_cast<([A-Za-z_]+)\*>/\1/' > "$tmp/walk"
    for k in $(comm -23 "$tmp/emitted" "$tmp/walk"); do
        if printf '%s\n' "$EXEMPT" | grep -qx "$walk:$k"; then continue; fi
        case " $LEAVES " in *" $k "*) continue ;; esac
        echo "check-scan-parity: FAIL — emitExpressionNode lowers $k and $walk never enters it." >&2
        echo "  A call or a type written inside one is never discovered. Add the arm to both walks, or declare" >&2
        echo "  $k a LEAF with why." >&2
        fail=1
    done
done

[ "$fail" -eq 0 ] || exit 1
echo "check-scan-parity: PASS (the collections and generics walks visit the same node kinds, and every kind the emitter lowers)"
