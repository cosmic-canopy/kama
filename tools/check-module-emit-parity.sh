#!/bin/sh
# check-module-emit-parity.sh — the same source emits the same C wherever it is written.
#
# A member's declared type is a fact about its CLASS. When the emitter re-resolved one at a READ site, the
# answer depended on the READER: inside the declaring module the name resolved (and the reader was told to
# import a type it never spells — see check-diag-file.sh and def_assign_sibling_field.d), while one module
# over the same resolution MISSED, the lookup came up empty, and the code path took its silent `continue`.
#
# ⚠️ WHY A GUARD AND NOT A FIXTURE. The divergence measured here has no kama-level observable: the field a
# slot loses its `default` fill for is filled again by the `out` argument that is the only legal way to fill
# a slot, so both spellings return the same value. What differs is the EMITTED C, and the only oracle is to
# write one program twice — once beside the type, once a module away — and diff the two bodies. A fixture
# compares exit codes and cannot see it. Measured before the fix: the same `slot Box b;` emitted
# `b.c = Counter__start();` inside `Box`'s module and nothing at all outside it.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"          # exports $KAMA (absolute; never the ./kama symlink — see AGENTS.md)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-module-emit-parity: FAIL — $1" >&2; fail=1; }

# One project, two modules. `geo` declares `Box`, whose `Counter` field has a NON-ZERO `default` ctor —
# so the fill is visible in the C as a call rather than as a zero. `geo/inside.kama` and the root
# `main.kama` then declare the identical `slot Box b;`. Same statement, two modules.
mkdir -p "$tmp/proj/src/geo"
cat > "$tmp/proj/kama.json" <<'JEOF'
{ "name": "parity", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" }, "geo": { "visibility": "internal" } } }
JEOF
cat > "$tmp/proj/src/geo/box.kama" <<'KEOF'
export { Box, Counter };
type value Counter {
    int32 n;
    public default ctor start() { this.n = 7; }
    public fn int32 value() { return this.n; }
}
type value Box {
    Counter c;
    public ctor make() { }
    public fn int32 read() { return this.c.value(); }
    public static fn void into(out Box dst) { dst = Box.make(); }
}
KEOF
cat > "$tmp/proj/src/geo/inside.kama" <<'KEOF'
import { Box };
export { inside };
fn int32 inside() { slot Box b; Box::into(dst: out b); return b.read(); }
KEOF
cat > "$tmp/proj/src/main.kama" <<'KEOF'
import { parity::geo::Box, parity::geo::inside };
fn int32 main() { slot Box b; Box::into(dst: out b); return b.read() + inside(); }
KEOF

( cd "$tmp/proj" && "$KAMA" build kama.json -o "$tmp/proj/out.bin" --keep-c ) >"$tmp/build.log" 2>&1 || {
    note "the probe project did not build"; sed 's/^/    /' "$tmp/build.log" >&2; exit 1; }

# The fill, per module. `grep -c` over each module's own C, so a miss in one is visible against the other.
inside_c=$(find "$tmp/proj" -name '*geo__inside.c' | head -1)
main_c=$(find "$tmp/proj" -name '*main.c' | head -1)
[ -n "$inside_c" ] && [ -n "$main_c" ] || { note "the probe emitted no C to read (--keep-c)"; exit 1; }

n_inside=$(LC_ALL=C grep -c 'Counter__start()' "$inside_c" || true)
n_main=$(LC_ALL=C grep -c 'Counter__start()' "$main_c" || true)

# A probe that produced nothing must not read as a pass — see check-diag-line.sh, which learned this the
# hard way. The fill must be PRESENT in the declaring module before its absence elsewhere means anything.
[ "$n_inside" -ge 1 ] || note "the field-default fill is missing even INSIDE the declaring module \
(\`Counter__start()\` not in $(basename "$inside_c")) — the probe no longer exercises the fill at all"

[ "$n_main" = "$n_inside" ] || note "the same \`slot Box b;\` emits the field-default fill \
$n_inside time(s) inside \`geo\` and $n_main time(s) one module over — a member's type is being resolved \
in the READER's scope"

# ── Case 2: the same rule at the ARGUMENT path (the KB-13 residual, 0.9.189) ───────────────────────────
#
# The declaration path was baked in 0.9.176; PASSING a field along was still resolved at the read site,
# through three more resolvers (lvalueCType / exprClass / receiverScalarCType, all reached from
# typeOfExpr). Same divergence, worse consequence: `takesOther(o: h.k)` — a `Kind` handed to an `Other`
# parameter — was REFUSED inside the declaring module and ACCEPTED one module over. Measured on the
# unfixed compiler, this exact probe: 2 errors inside (one of them the spurious import), 0 outside.
#
# ⚠️ Not a fixture, for a sharper reason than case 1: both enums lower to the same C integer type, so the
# C compiler does not catch it either. The wrong program simply builds and runs, and the only oracle is
# that the two spellings must AGREE.
mkdir -p "$tmp/arg/src/geo"
cat > "$tmp/arg/kama.json" <<'JEOF'
{ "name": "argp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" }, "geo": { "visibility": "internal" } } }
JEOF
cat > "$tmp/arg/src/geo/decl.kama" <<'KEOF'
export { Kind, Other, Holder, makeHolder, takesOther };
type enum Kind  { A, B }
type enum Other { X, Y }
type value Holder {
    public int32 n;
    public Kind k;
    public ctor make(int32 n, Kind k) { this.n = n; this.k = k; }
}
fn Holder makeHolder() { return Holder.make(n: 20, k: Kind::A); }
fn int32 takesOther(Other o) { match (o) { case X: { return 1; } case Y: { return 2; } }; }
KEOF
# The violation, written INSIDE the declaring module...
cat > "$tmp/arg/src/geo/inside.kama" <<'KEOF'
import { Holder, makeHolder, takesOther };
export { inside };
fn int32 inside() { Holder h = makeHolder(); return takesOther(o: h.k); }
KEOF
cat > "$tmp/arg/src/main.kama" <<'KEOF'
import { argp::geo::inside };
fn int32 main() { return inside(); }
KEOF
( cd "$tmp/arg" && "$KAMA" check kama.json ) >"$tmp/inside.log" 2>&1 && in_ok=1 || in_ok=0

# ...and one module over, byte-for-byte the same two statements.
cat > "$tmp/arg/src/geo/inside.kama" <<'KEOF'
export { inside };
fn int32 inside() { return 0; }
KEOF
cat > "$tmp/arg/src/main.kama" <<'KEOF'
import { argp::geo::Holder, argp::geo::makeHolder, argp::geo::takesOther };
fn int32 main() { Holder h = makeHolder(); return takesOther(o: h.k); }
KEOF
( cd "$tmp/arg" && "$KAMA" check kama.json ) >"$tmp/outside.log" 2>&1 && out_ok=1 || out_ok=0

# The probe must BE a violation — if `kama check` ever accepts it from inside the declaring module, the
# enum-identity rule moved and this guard is measuring nothing (case 1's lesson, restated).
[ "$in_ok" = 0 ] || note "\`takesOther(o: h.k)\` is accepted INSIDE the declaring module — the enum-identity \
rule no longer fires here, so this probe cannot see the read-site divergence any more"

[ "$in_ok" = "$out_ok" ] || note "handing a field to a mismatched parameter is refused inside the declaring \
module and ACCEPTED one module over — a member's type is being resolved in the READER's scope (the KB-13 \
residual). Route the read site through fieldCType(), never cTypeInInstance(owner, f.type)"

exit $fail
