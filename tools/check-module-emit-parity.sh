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

exit $fail
