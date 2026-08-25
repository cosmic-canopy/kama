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
#   2. no generated name carries a positional form — not a `_<index>.c` filename, not an `_F<digits>`
#      symbol
#        — REAL, and it is the assertion that survives a partial revert: a build could round-trip to the
#          same names while still being positional if the permutation happened to be an identity, which
#          (1) alone would not catch. This one reads the FORM, so it cannot be satisfied by luck.
#   3. every generated file is BYTE-IDENTICAL across the two orders
#        — REAL for the emission ORDER, which was the second half of the defect: the shared header
#          declared each unit in LOAD order, so one program's three declarations came out in two different
#          sequences. ⚠️ It is NOT a test of the file-private scope, and believing it was is a mistake this
#          guard made for one revision. Units are sorted canonically before emission, so a POSITIONAL
#          scope is handed out in canonical order too and comes out identical either way. Measured, by
#          restoring the positional scope: (3) passed.
#   4. adding a file to a program does not move the symbols of the files already in it
#        — REAL, and it is what actually pins the scope. Positional numbering is stable only within a
#          FIXED unit set: insert one file near the front and every later file's private symbols shift,
#          so a diff of yesterday's generated C against today's is noise in files nobody edited. Sorting
#          cannot fix that and a name-derived scope does. This is the assertion (3) was wrongly credited
#          with.
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

# ---- 2. no positional FORM survives, in a filename or in a symbol -------------------------------------
if LC_ALL=C grep -E '_[0-9]+\.c$' "$tmp/names.one" > "$tmp/positional" 2>/dev/null; then
    echo "check-c-reproducible: FAIL — a generated file still carries a positional suffix:" >&2
    sed 's/^/  /' "$tmp/positional" >&2
    echo "  That is the '<basename>_<index>' form §2e.26 replaced with the unit's module." >&2
    exit 1
fi

# A file-private scope is `_F<file>`, never `_F<digits>`. `main.kama` is in the loose root, so it has no
# module and `priv` is exactly this population.
if LC_ALL=C grep -ohE '_F[0-9]+__[A-Za-z_]+' "$tmp/one"/*.c "$tmp/one"/*.h 2>/dev/null \
        | sort -u > "$tmp/numbered" && [ -s "$tmp/numbered" ]; then
    echo "check-c-reproducible: FAIL — a file-private scope is still numbered by load position:" >&2
    head -5 "$tmp/numbered" | sed 's/^/  /' >&2
    exit 1
fi
if ! LC_ALL=C grep -q '_Fmain__priv' "$tmp/one"/*.c "$tmp/one"/*.h 2>/dev/null; then
    echo "check-c-reproducible: FAIL — the loose-root file's private scope is not named after it." >&2
    echo "  Expected '_Fmain__priv' from main.kama; found:" >&2
    LC_ALL=C grep -ohE '_F[A-Za-z0-9_]*__priv' "$tmp/one"/*.c "$tmp/one"/*.h 2>/dev/null \
        | sort -u | head -3 | sed 's/^/    /' >&2
    exit 1
fi

# ---- 3. byte-identical contents -----------------------------------------------------------------------
# `#line` directives name the SOURCE, which is the same file in both builds, so there is nothing to
# normalize away — a difference here is a real difference in the emitted C.
while read -r f; do
    if ! cmp -s "$tmp/one/$f" "$tmp/two/$f"; then
        echo "check-c-reproducible: FAIL — '$f' differs between the two operand orders:" >&2
        diff "$tmp/one/$f" "$tmp/two/$f" | head -12 | sed 's/^/  /' >&2
        echo "  A file-private scope is derived from the unit's name, and units are emitted in a" >&2
        echo "  canonical order — neither may depend on the position of an operand." >&2
        exit 1
    fi
done < "$tmp/names.one"

# ---- 4. adding a file does not move the symbols of the files already there -----------------------------
# The property a positional scope violates even when nothing is permuted. `aaa/` sorts BEFORE everything
# already in the program, so under load-order numbering it takes an index the other units used to hold and
# pushes each of them along — every private symbol in files nobody edited moves.
mkdir -p "$tmp/src/aaa" "$tmp/three"
cat > "$tmp/src/aaa/extra.kama" <<'EOF'
export { extra };
fn int32 extra() { return 0; }
EOF
build_into "$tmp/three" "$tmp/src/main.kama" "$tmp/src/a/x.kama" "$tmp/src/b/x.kama" "$tmp/src/aaa/extra.kama"

# Into files, not process substitution: this is `#!/bin/sh`. `grep -Fxv -f` rather than `comm`, which is
# locale-broken on macOS and silently reports nonsense.
syms_of() { LC_ALL=C grep -ohE '\b_F[A-Za-z0-9_]+__[A-Za-z_]+' "$1"/*.c "$1"/*.h 2>/dev/null | sort -u; }
syms_of "$tmp/one"   > "$tmp/syms.before"
syms_of "$tmp/three" > "$tmp/syms.after"
if [ ! -s "$tmp/syms.before" ]; then
    echo "check-c-reproducible: FAIL — no file-private symbol found; the fixture stopped testing this." >&2
    exit 1
fi
missing=$(LC_ALL=C grep -Fxv -f "$tmp/syms.after" "$tmp/syms.before" || true)
if [ -n "$missing" ]; then
    echo "check-c-reproducible: FAIL — adding one file renamed symbols in the files already present:" >&2
    printf '%s\n' "$missing" | head -5 | sed 's/^/  was: /' >&2
    head -5 "$tmp/syms.after" | sed 's/^/  now: /' >&2
    echo "  A file-private scope is named after its FILE, so an unrelated file cannot move it." >&2
    exit 1
fi

# ---- 5. the collision the positional suffix used to absorb ---------------------------------------------
# Deriving a name from identity only works while identity is unique, and for a module-less file identity is
# just its NAME. Two rules cover that, and they are NOT the same rule — different populations, different
# consequences, different commands:
#
#   build: two units, one generated `.c` filename. Reachable between units that DO have modules, too
#          (`b__c.kama` in module `a` collides with `c.kama` in `a::b`), so it cannot be folded into the
#          scope rule below.
#   check: two module-less units, one file-private scope — their private symbols would collide in the
#          emitted C with nothing to notice, since neither is importable.
#
# Both are asserted here rather than as an `xfail` fixture because the xfail leg only ever runs `kama
# build`, where the filename rule fires first — so the scope rule could not be reached from there at all,
# and an xfail for it would pass while testing the other rule's message.
#
# `my-prog` and `my_prog` are the reachable shape: two files CAN live in one directory under those names,
# and a `-` is not legal in a C identifier, so both fold to the same stem.
mkdir -p "$tmp/clash"
printf 'fn int32 main() { return 0; }\n'   > "$tmp/clash/my-prog.kama"
printf 'fn int32 helper() { return 2; }\n' > "$tmp/clash/my_prog.kama"

if "$KAMA" build "$tmp/clash/my-prog.kama" "$tmp/clash/my_prog.kama" -o "$tmp/clash/app" \
        > "$tmp/clash.log" 2>&1; then
    echo "check-c-reproducible: FAIL — two units that fold onto one generated filename built clean." >&2
    exit 1
fi
if ! grep -qF "would both generate 'my_prog.c'" "$tmp/clash.log"; then
    echo "check-c-reproducible: FAIL — the filename collision is not reported by name:" >&2
    head -4 "$tmp/clash.log" | sed 's/^/  /' >&2
    exit 1
fi

if "$KAMA" check "$tmp/clash/my-prog.kama" "$tmp/clash/my_prog.kama" > "$tmp/clash2.log" 2>&1; then
    echo "check-c-reproducible: FAIL — two units sharing a file-private scope checked clean." >&2
    exit 1
fi
if ! grep -qF "the same file-private scope" "$tmp/clash2.log"; then
    echo "check-c-reproducible: FAIL — the scope collision is not reported as one:" >&2
    head -4 "$tmp/clash2.log" | sed 's/^/  /' >&2
    exit 1
fi

echo "check-c-reproducible: OK — filenames and emitted C identical across operand orders; no positional form; adding a file moves nothing; both identity collisions reported"
