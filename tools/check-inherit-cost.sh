#!/bin/sh
# check-inherit-cost.sh — guard for `--inherit-depth` / kama.json `"inheritDepth"`, the knob that decides
# how deep `extends` may go and, at 0, bans inheritance outright.
#
# The `tests/xfail/*` loop builds every fixture with NO extra flags, so a flag-driven rejection can't be
# tested there (the depth-CAP rejection can, and is — tests/xfail/inherit_depth_exceeded). This drives the
# real flag and asserts:
#   1. REJECT   — at depth 0, `extends`, a `virtual class` and a `virtual` method each fail to build with
#                 the gate's own diagnostic (the right error, not any failure).
#   2. CONTROL  — the SAME program builds fine at the default depth (so the flag is what rejects it).
#   3. NO BLOAT — a program that uses no inheritance emits ZERO class-vtable machinery, and its binary is
#                 the same size at depth 0 as at the default. This is the claim behind "turning
#                 inheritance off costs the output nothing", and it must be measured, not asserted.
#                 ⚠️ CONTRACTS KEEP THEIR OWN VTABLES — a fat pointer needs one — so the same program must
#                 still emit `_vtbl`. Checking only for absence would pass on an empty file.
#   4. MANIFEST — kama.json `"inheritDepth": 0` has the same effect, and an explicit `--inherit-depth`
#                 still overrides it (a one-off "does this build with inheritance off?" must not need an
#                 edit to a committed file).
# Fails (exit 1) with a diagnostic if any property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-inherit-cost: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

GATE='requires inheritance, which is disabled for this build'

# ---- 1. REJECT + 2. CONTROL -------------------------------------------------------------------------
# One program per surface, so a gate that quietly stopped covering one of them is visible. `extends` is
# not enough on its own: a `virtual class` with NO subclass still emits a vtable, which is exactly how
# "inheritance is off" could otherwise still cost the output something.
mk_case() {
    name=$1; shift
    cat > "$tmp/$name.kama"
}

mk_case extends <<'EOF'
type virtual resource B { int32 x;
    protected fn int32 get() { return this.x; }
    protected virtual fn int32 tag() { return 0; } }
type final resource D extends B {
    public ctor make() { slot D r; return give r; }
    protected override fn int32 tag() { return 1; } }
fn int main() { D d = D.make(); return 0; }
EOF

mk_case virtualclass <<'EOF'
type virtual resource B {
    public ctor make() { slot B r; return give r; }
    public fn int32 rank() { return this.tag(); }
    protected virtual fn int32 tag() { return 0; } }
fn int main() { B b = B.make(); return b.rank(); }
EOF

for case in extends virtualclass; do
    if "$KAMA" build --inherit-depth=0 "$tmp/$case.kama" -o "$tmp/$case.out" >/dev/null 2>"$tmp/$case.err"; then
        echo "check-inherit-cost: FAIL — '--inherit-depth=0' accepted $case" >&2; exit 1
    fi
    if ! grep -qF "$GATE" "$tmp/$case.err"; then
        echo "check-inherit-cost: FAIL — $case rejected at depth 0, but not by the inheritance gate:" >&2
        sed 's/^/  /' "$tmp/$case.err" >&2; exit 1
    fi
    # CONTROL: the identical program must build at the default depth (proves the flag is the cause).
    if ! "$KAMA" build "$tmp/$case.kama" -o "$tmp/$case.ctrl" >/dev/null 2>"$tmp/$case.ctrl.err"; then
        echo "check-inherit-cost: FAIL — $case does not build even at the DEFAULT depth:" >&2
        sed 's/^/  /' "$tmp/$case.ctrl.err" >&2; exit 1
    fi
done

# ---- 3. NO BLOAT ------------------------------------------------------------------------------------
# Contracts + a generic collection: everything a non-inheriting program actually uses for polymorphism.
cat > "$tmp/pure.kama" <<'EOF'
import std::collections::{DynamicArray};
type contract Shape for value { fn int32 area(); }
type value Sq implements Shape { public int32 s;
    public ctor make(int32 s) { slot Sq r; r.s = s; return give r; }
    public fn int32 area() { return this.s * this.s; } }
fn int32 total(Shape a, Shape b) { return a.area() + b.area(); }
fn int main() {
    DynamicArray<Sq> xs = DynamicArray.withCapacity(capacity: 2);
    xs.add(item: Sq.make(s: 3));
    return total(a: Sq.make(s: 4), b: Sq.make(s: 5)) + 0;
}
EOF

mkdir -p "$tmp/c0"
cp "$tmp/pure.kama" "$tmp/c0/pure.kama"
if ! "$KAMA" build --inherit-depth=0 --keep-c "$tmp/c0/pure.kama" -o "$tmp/c0/pure.out" >/dev/null 2>"$tmp/pure.err"; then
    echo "check-inherit-cost: FAIL — a contract/collection program does not build at depth 0:" >&2
    sed 's/^/  /' "$tmp/pure.err" >&2; exit 1
fi

# CLASS vtable machinery must be entirely absent...
if grep -l '__vptr\|_vtable\|__base' "$tmp"/c0/*.h "$tmp"/c0/*.c >/dev/null 2>&1; then
    echo "check-inherit-cost: FAIL — a program with no inheritance still emits class-vtable machinery:" >&2
    grep -ho '__vptr\|_vtable\|__base' "$tmp"/c0/*.h "$tmp"/c0/*.c 2>/dev/null | sort | uniq -c | sed 's/^/  /' >&2
    exit 1
fi
# ...and the CONTRACT vtables must still be there, or the check above is proving nothing.
if ! grep -l '_vtbl' "$tmp"/c0/*.h "$tmp"/c0/*.c >/dev/null 2>&1; then
    echo "check-inherit-cost: FAIL — no contract vtable (`_vtbl`) in the emitted C, so the" >&2
    echo "  class-vtable check above cannot distinguish 'clean' from 'nothing was emitted'." >&2
    exit 1
fi

# Same program, same flags, only the depth differs -> identical binary size. This is the measurement:
# turning inheritance off must cost the OUTPUT nothing, because a program that never used it never paid.
"$KAMA" build --release --inherit-depth=0 "$tmp/pure.kama" -o "$tmp/off.bin" >/dev/null 2>&1
"$KAMA" build --release                   "$tmp/pure.kama" -o "$tmp/on.bin"  >/dev/null 2>&1
off=$(wc -c < "$tmp/off.bin"); on=$(wc -c < "$tmp/on.bin")
if [ "$off" != "$on" ]; then
    echo "check-inherit-cost: FAIL — a non-inheriting program changes size with the depth knob" >&2
    echo "  (--inherit-depth=0: $off bytes, default: $on bytes). The knob must gate the LANGUAGE," >&2
    echo "  never the codegen of a program that does not use the feature." >&2
    exit 1
fi

# ---- 4. MANIFEST ------------------------------------------------------------------------------------
mkdir -p "$tmp/proj"
cp "$tmp/extends.kama" "$tmp/proj/app.kama"
cat > "$tmp/proj/kama.json" <<'EOF'
{ "name": "depthproj", "version": "0.1.0", "inheritDepth": 0 }
EOF
if "$KAMA" build "$tmp/proj/app.kama" -o "$tmp/proj/app.out" >/dev/null 2>"$tmp/m.err"; then
    echo "check-inherit-cost: FAIL — kama.json \"inheritDepth\": 0 did not disable inheritance" >&2; exit 1
fi
if ! grep -qF "$GATE" "$tmp/m.err"; then
    echo "check-inherit-cost: FAIL — the manifest rejected, but not via the inheritance gate:" >&2
    sed 's/^/  /' "$tmp/m.err" >&2; exit 1
fi
# An explicit flag outranks the manifest, in both directions.
if ! "$KAMA" build --inherit-depth=1 "$tmp/proj/app.kama" -o "$tmp/proj/app.out" >/dev/null 2>"$tmp/mo.err"; then
    echo "check-inherit-cost: FAIL — '--inherit-depth=1' did not override kama.json \"inheritDepth\": 0:" >&2
    sed 's/^/  /' "$tmp/mo.err" >&2; exit 1
fi
# A malformed value is a manifest error, not a silent fall-back to the default.
cat > "$tmp/proj/kama.json" <<'EOF'
{ "name": "depthproj", "version": "0.1.0", "inheritDepth": "1" }
EOF
if "$KAMA" build "$tmp/proj/app.kama" -o "$tmp/proj/app.out" >/dev/null 2>"$tmp/mb.err"; then
    echo "check-inherit-cost: FAIL — a string \"inheritDepth\" was silently tolerated" >&2; exit 1
fi

echo "PASS inherit-depth (depth 0 rejects extends/virtual, contracts unaffected, output size unchanged, kama.json honoured)"
