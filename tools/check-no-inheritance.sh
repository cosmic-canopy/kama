#!/bin/sh
# check-no-inheritance.sh — build kama WITHOUT inheritance and prove the variant still works.
#
# `KAMA_INHERITANCE=0` exists for two reasons (docs/design/inheritance.md decision C):
#   1. isolate the size delta — what inheritance costs the compiler, answerable only by building both ways
#   2. be the EXTRACTION POINT — if inheritance is dropped, the `#if KAMA_INHERITANCE` blocks are the
#      deletion list, already proven to compile without their contents
#
# Purpose 2 is why this guard exists at all: an untested build variant rots within weeks, and a rotted
# extraction point is worse than none, because it looks like an option and isn't.
#
# Asserts:
#   1. BUILDS   — the KAMA_INHERITANCE=0 compiler compiles clean.
#   2. REJECTS  — `extends`, a `virtual class` and a `virtual` method each fail with the build's own
#                 diagnostic (a real message, not a syntax error — the grammar is deliberately NOT gated).
#   3. WORKS    — a non-inheriting program using contracts + generics still compiles AND RUNS correctly,
#                 so the gate removed inheritance and not something load-bearing next to it.
#   4. MEASURES — prints the `.text` delta between the two compilers.
#
# SLOW (a full compiler build), so it is NOT in run_tests.sh. Run it before shipping a change that touches
# the inheritance paths, and in CI.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
cd "$ROOT"

if [ ! -x "$KAMA" ]; then echo "check-no-inheritance: $KAMA not built (run make first)" >&2; exit 1; fi

PLATFORM=$(uname -s)-$(uname -m)
OUT="build/$PLATFORM-noinherit"      # the Makefile picks this itself when KAMA_INHERITANCE=0
NOINH="$OUT/kama"

# 1. BUILDS. `make` also repoints the root ./kama symlink at whatever built last, so put it back after.
echo "check-no-inheritance: building with KAMA_INHERITANCE=0 ..."
if ! make KAMA_INHERITANCE=0 >/tmp/noinh.build.log 2>&1; then
    echo "check-no-inheritance: FAIL — the KAMA_INHERITANCE=0 build does not compile:" >&2
    tail -30 /tmp/noinh.build.log >&2
    ln -sf "$KAMA" kama 2>/dev/null || true
    exit 1
fi
ln -sf "$KAMA" kama 2>/dev/null || true
if grep -q ' error:\| warning:' /tmp/noinh.build.log; then
    echo "check-no-inheritance: FAIL — the KAMA_INHERITANCE=0 build is not warning-clean:" >&2
    grep ' error:\| warning:' /tmp/noinh.build.log | head -20 >&2; exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
GATE='needs inheritance, and this kama was built without it'

# 2. REJECTS — one program per surface. `extends` alone is not enough: a `virtual class` with no subclass
#    still carries a vtable, which is exactly the machinery this build exists to remove.
cat > "$tmp/ext.kama" <<'EOF'
type virtual resource B { protected virtual fn int32 t() { return 0; } }
type final resource D extends B {
    public ctor make() { }
    protected override fn int32 t() { return 1; } }
fn int main() { D d = D.make(); return 0; }
EOF
if "$NOINH" check "$tmp/ext.kama" >/dev/null 2>"$tmp/ext.err"; then
    echo "check-no-inheritance: FAIL — the KAMA_INHERITANCE=0 compiler accepted 'extends'" >&2; exit 1
fi
for want in '`extends`' 'virtual class' 'virtual` method'; do
    if ! grep -qF "$want" "$tmp/ext.err"; then
        echo "check-no-inheritance: FAIL — no rejection naming \"$want\":" >&2
        sed 's/^/  /' "$tmp/ext.err" >&2; exit 1
    fi
done
if ! grep -qF "$GATE" "$tmp/ext.err"; then
    echo "check-no-inheritance: FAIL — rejected, but not with the build's own diagnostic:" >&2
    sed 's/^/  /' "$tmp/ext.err" >&2; exit 1
fi
# A syntax error would mean the grammar got gated too, which is deliberately NOT the design.
if grep -q 'Parse error' "$tmp/ext.err"; then
    echo "check-no-inheritance: FAIL — 'extends' produced a PARSE error. The grammar must keep parsing" >&2
    echo "  it so the emitter can answer with a real message; see KAMA_INHERITANCE in kama.cemit.h." >&2
    exit 1
fi

# 3. WORKS — contracts, generics and a collection all survive, and the program produces the right answer.
#    Compiling is not enough: the gate sits next to the contract vtable machinery, which must be untouched.
cat > "$tmp/ok.kama" <<'EOF'
import std::collections::{DynamicArray};
type contract Shape for value { fn int32 area(); }
type value Sq implements Shape { public int32 s;
    public ctor make(int32 s) { this.s = s; }
    public fn int32 area() { return this.s * this.s; } }
fn int32 total(Shape a, Shape b) { return a.area() + b.area(); }
fn int main() {
    DynamicArray<Sq> xs = DynamicArray.withCapacity(capacity: 2);
    xs.add(item: Sq.make(s: 3));
    return total(a: Sq.make(s: 4), b: Sq.make(s: 5)) + 0;   // 16 + 25 = 41
}
EOF
if ! "$NOINH" build "$tmp/ok.kama" -o "$tmp/ok.bin" >/dev/null 2>"$tmp/ok.err"; then
    echo "check-no-inheritance: FAIL — a contract/generic program does not build without inheritance:" >&2
    sed 's/^/  /' "$tmp/ok.err" >&2; exit 1
fi
"$tmp/ok.bin" || rc=$?; rc=${rc:-0}
if [ "$rc" != 41 ]; then
    echo "check-no-inheritance: FAIL — that program RAN wrong without inheritance (got $rc, want 41)." >&2
    echo "  The gate removed something load-bearing next to inheritance, not just inheritance." >&2
    exit 1
fi

# 4. MEASURES.
on=$(size "$KAMA"   | awk 'NR==2{print $1}')
off=$(size "$NOINH" | awk 'NR==2{print $1}')
echo "PASS no-inheritance (KAMA_INHERITANCE=0 builds, rejects, and runs contracts correctly)"
echo "  .text with inheritance:    $on"
echo "  .text without:             $off"
echo "  delta:                     $((on - off)) bytes"
