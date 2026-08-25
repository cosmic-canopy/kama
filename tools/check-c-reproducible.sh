#!/bin/sh
# check-c-reproducible.sh — `--keep-c` must not depend on the order the operands were typed.
#
# What this is about. The README's pitch is that kama "drops into an existing C codebase": you keep the
# generated C, read it, diff it, check it in. None of that survives output that moves when the argument
# list is permuted, and until §2e.26 it did. Generated `.c` files were named `<basename>_<index>`, where
# the index was the unit's POSITION in the compilation, so one three-file program emitted
#
#     order 1:  main_0.c  x_1.c  x_2.c
#     order 2:  x_0.c     x_1.c  main_2.c
#
# The `_<index>` was not decoration — it was the only thing keeping two source files that share a basename
# from writing to the same `.c`. So it could not simply be dropped; it had to be replaced by something
# derived from the unit's IDENTITY, which is what the module system spent phases 1-3 building. A generated
# file is now named `<module>__<basename>` (`lib/std/collections/vec.kama` -> `std__collections__vec.c`),
# and a unit with no module at all — the loose ROOT, whose symbols are unimportable anyway (§2e.27) —
# keeps its bare basename.
#
# Three assertions. Each was checked by BREAKING the mechanism, not by reading it:
#
#   1. the two builds emit the SAME SET OF FILENAMES
#        — REAL. With the positional suffix restored this fails immediately and prints both listings.
#   2. no generated name carries a positional suffix
#        — REAL, and it is the assertion that survives a partial revert: a build could round-trip to the
#          same names while still being positional if the permutation happened to be an identity, which
#          (1) alone would not catch. This one reads the FORM, so it cannot be satisfied by luck.
#   3. every generated file is BYTE-IDENTICAL across the two orders
#        — NOT YET ASSERTED, and named here rather than left silent because a guard that stops at
#          filenames reads as if it proved more than it did. Two things still move: the file-private
#          scope is `_F<index>`, and the shared header declares each unit in the order the units were
#          loaded. Both land in phase 4b, and this assertion turns on with them.
#
# The fixture is deliberately the hard case: three loose files, TWO OF THEM SHARING THE BASENAME `x.kama`,
# which is exactly the collision the positional suffix existed to absorb.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-c-reproducible: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/src/a" "$tmp/src/b" "$tmp/one" "$tmp/two"

# The root file: no module, so it is the population that used to be named positionally. It also declares a
# file-private helper, so its SCOPE lands in the emitted C and assertion 3 can see it move.
cat > "$tmp/src/main.kama" <<'EOF'
import { a::helper };
fn int32 priv() { return 7; }
fn int32 main() { return helper() + priv() - 7; }
EOF

# Two siblings sharing a basename, in different modules. Under the old rule these were `x_1.c` and `x_2.c`;
# under the new one they are `a__x.c` and `b__x.c`, which is why the suffix is no longer load-bearing.
cat > "$tmp/src/a/x.kama" <<'EOF'
export { helper };
fn int32 helper() { return 0; }
EOF
cat > "$tmp/src/b/x.kama" <<'EOF'
export { other };
fn int32 other() { return 1; }
EOF

build_into() {
    dir="$1"; shift
    if ! "$KAMA" build "$@" -o "$dir/app" --keep-c > "$dir/build.log" 2>&1; then
        echo "check-c-reproducible: FAIL — the fixture does not build:" >&2
        cat "$dir/build.log" >&2
        exit 1
    fi
}

build_into "$tmp/one" "$tmp/src/main.kama" "$tmp/src/a/x.kama" "$tmp/src/b/x.kama"
build_into "$tmp/two" "$tmp/src/b/x.kama" "$tmp/src/a/x.kama" "$tmp/src/main.kama"

# ---- 1. the same set of filenames ---------------------------------------------------------------------
( cd "$tmp/one" && ls *.c *.h ) | LC_ALL=C sort > "$tmp/names.one"
( cd "$tmp/two" && ls *.c *.h ) | LC_ALL=C sort > "$tmp/names.two"

if ! cmp -s "$tmp/names.one" "$tmp/names.two"; then
    echo "check-c-reproducible: FAIL — the generated filenames depend on the operand order." >&2
    echo "  order 1: $(tr '\n' ' ' < "$tmp/names.one")" >&2
    echo "  order 2: $(tr '\n' ' ' < "$tmp/names.two")" >&2
    echo "  A generated .c is named from its unit's module (§2e.26), never from its position." >&2
    exit 1
fi

# ---- 2. no positional suffix survives -----------------------------------------------------------------
if LC_ALL=C grep -E '_[0-9]+\.c$' "$tmp/names.one" > "$tmp/positional" 2>/dev/null; then
    echo "check-c-reproducible: FAIL — a generated file still carries a positional suffix:" >&2
    sed 's/^/  /' "$tmp/positional" >&2
    echo "  That is the '<basename>_<index>' form §2e.26 replaced with the unit's module." >&2
    exit 1
fi

# ---- 3. byte-identical contents — PHASE 4b, see the header ---------------------------------------------

echo "check-c-reproducible: OK — generated filenames are identical across operand orders"
