#!/bin/sh
# check-modules.sh — a file's module is WHERE IT SITS, and that is what reaches the C symbol.
#
# design/module-system.md §2b/§2i. A file's identity used to be the `namespace` it declared; phase 2
# replaced it with the module its path puts it in, read against its project's `modules` map — or, with
# no manifest in play, against the operand set's own root. This guard holds both halves down.
#
# Two sections, and they are deliberately end-to-end rather than a reading of an internal value:
#
#   §1  a PROJECT, whose answers are known by construction, checked through the emitted C. Seven shapes:
#       the source root, a listed folder, a nested one, a `name` override, a folder that is a string
#       prefix of another, an UNLISTED folder, and one nested deep below an unlisted one.
#   §2  a LOOSE build, where no manifest names anything and the operand set does it instead — folders
#       become modules, the root does not, and the operand ORDER cannot reach the symbols.
#
# ⚠️ It reads the emitted C, never a probe. Through phases 2a-2b this guard drove a hidden
# `--probe-modules` TSV and compared the DERIVED module against the DECLARED one, because until the
# emitter consumed the derivation the two could not be told apart by any other means. 2c made the emitter
# consume it and 2e deleted the declaration, so both the comparison and the flag are gone: the symbol in
# the `.c` is the derivation, arrived at the only place it matters.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-modules: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=$((fail+1)); }

# ---------------------------------------------------------------------------------------------------
echo "check-modules: a file's module is the nearest LISTED folder above it, and the symbol says so"

# One project, and every shape the derivation has to get right. `network/` is not an oversight: `net` is
# listed, and a string-prefix test would hand it `network/`'s files.
p="$tmp/deriv"
mkdir -p "$p/src/net/web" "$p/src/net/detail" "$p/src/network" "$p/src/vendored/deep/deeper" "$p/src/oddly-named"
# `Owned` is here for §3: the embedded triad is the one unit no path→module derivation can reach, and
# instantiating it is what puts its symbol in the emitted C where the guard can read it.
cat > "$p/src/app.kama" <<'KAMA'
type value Cell {
    int32 v;
    public ctor make(int32 x) { this.v = x; }
    public fn int32 get() { return this.v; }
}
fn int32 rootHelper() {
    Owned<Cell> boxed = new Cell.make(x: 7);
    return boxed.get();
}
fn int32 main() { return rootHelper(); }
KAMA
printf 'fn int32 n() { return 1; }\n'          > "$p/src/net/net.kama"
printf 'fn int32 w() { return 1; }\n'          > "$p/src/net/web/web.kama"
printf 'fn int32 d() { return 1; }\n'          > "$p/src/net/detail/d.kama"
printf 'fn int32 k() { return 1; }\n'          > "$p/src/network/k.kama"
printf 'fn int32 v() { return 1; }\n'          > "$p/src/vendored/deep/deeper/v.kama"
printf 'fn int32 o() { return 1; }\n'          > "$p/src/oddly-named/o.kama"
cat > "$p/kama.json" <<'JSON'
{
  "name": "deriv", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": {
    ".":           { "visibility": "internal" },
    "net":         { "visibility": "public",
                     "modules": { "web": { "visibility": "public" } } },
    "network":     { "visibility": "public" },
    "vendored":    { "visibility": "internal" },
    "oddly-named": { "visibility": "public", "name": "tidy" }
  }
}
JSON

# Build it, and read the answers back out of the emitted C. Asserting the derivation any other way would
# be asserting an internal value: what has to be true is that the module reaches the SYMBOL, since that is
# what an importer links against and what §2e.25 defines as `project · module · name`.
#
# Note what `deriv__rootHelper` pins: app.kama says nothing about where it lives and could not have been
# public at all before phase 2, because a file's identity came from a declaration it does not carry. It is
# `deriv`'s root module now because of where it SITS. (`main` is exempt from scoping in both directions —
# it is `kama_main`.)
mkdir -p "$tmp/c"
if ! "$KAMA" build "$p/kama.json" -o "$tmp/c/app" --keep-c > "$tmp/build.log" 2>&1; then
    echo "check-modules: FAIL — the derivation fixture does not build:" >&2
    head -5 "$tmp/build.log" >&2
    exit 1
fi

syms=$(grep -ohE '\b(deriv[A-Za-z_]*|_F[0-9]+)__[A-Za-z_]+' "$tmp/c"/*.c "$tmp/c"/*.h 2>/dev/null | sort -u)
emitted() {
    printf '%s\n' "$syms" | grep -Fxq "$1" && ok "$2" || bad "$2 — no \`$1\` in the emitted C"
}
emitted deriv__rootHelper   "a file at the source root is scoped by the PROJECT name, with no declaration to say so"
emitted deriv__net__n       "a listed folder's file is scoped by its module"
emitted deriv__net__web__w  "a nested module composes both segments in the symbol"
emitted deriv__network__k   "\`network\` does not collapse into \`net\`"
emitted deriv__tidy__o      "a \`name\` override reaches the symbol, not the folder's spelling"
emitted deriv__net__d       "an unlisted folder's file carries its nearest listed ancestor's scope"
emitted deriv__vendored__v  "...at any depth"

# The one file set no path→module derivation can reach: the smart-pointer triad arrives as
# `<prelude>/std/memory/*.kama`, synthetic units with NO PATH, so their module is STATED instead, in
# KamaPreludeModule::module.
#
# ⚠️ This assertion only became load-bearing in 2e, and the difference was measured both times. Through
# 2c/2d, breaking the driver's synthetic arm did NOT fail it: lib/std/memory/*.kama still declared
# `namespace std::memory` and ctxOf's declaration rung caught them, so a compiler built with that arm
# returning "" stayed green. With the declaration deleted, nothing else can produce this name — a
# compiler built that way now fails to build a one-line `new int32()` program at all.
if printf '%s\n' "$syms" | grep -qE '^_F[0-9]+__(Owned|Shared|Weak)'; then
    bad "the embedded triad lost its module and fell back to a file-private scope"
elif grep -qE '\bstd__memory__Owned' "$tmp/c"/*.c "$tmp/c"/*.h 2>/dev/null; then
    ok "the embedded prelude module keeps the identity it states, having no path to derive one from"
else
    bad "the embedded triad emitted no \`std__memory__Owned\` — did app.kama stop instantiating it?"
fi

# The positional file scope is what identity REPLACES. It survives only for a loose file (§2e.27) — a
# project's files must never land in it, and a stray `_F<n>` here means a unit missed the derivation.
if printf '%s\n' "$syms" | grep -q '^_F[0-9]'; then
    bad "a file in a project still carries the positional \`_F<n>\` scope:"
    printf '%s\n' "$syms" | grep '^_F[0-9]' | head -5 | sed 's/^/        /' >&2
else
    ok "no positional \`_F<n>\` scope survives anywhere in a project build"
fi

# ---------------------------------------------------------------------------------------------------
echo "check-modules: with no manifest, a module is still a folder"

# §2i: a loose build has no manifest to name anything, so the operand set does it — the deepest common
# ancestor of the operands' directories is the root, and a file's module is its own directory below it.
# NOT ONE FILE HERE DECLARES A NAMESPACE, which is the whole point: this is the shape the corpus takes
# after 2e, and until the loose arm existed a program like it could not be built at all. (Run it against
# a compiler without that arm: `area` emits as `_F<n>__area` while `app.kama`'s import resolves the CALL
# to `geo__area`, so the C compiler is handed a call to a function nobody defined.)
l="$tmp/loose-prog"
mkdir -p "$l/geo/deep" "$l/oddly-named" "$tmp/lc"
cat > "$l/app.kama" <<'KAMA'
import {
    geo::area,
    geo::deep::nested,
};
fn int32 helper() { return 1; }
fn int32 main() { return area() + nested() + helper(); }
KAMA
printf 'export { area };\nfn int32 area() { return 2; }\n'       > "$l/geo/area.kama"
printf 'export { nested };\nfn int32 nested() { return 3; }\n'   > "$l/geo/deep/nested.kama"
printf 'fn int32 unreachable() { return 9; }\n'                  > "$l/oddly-named/x.kama"

if ! "$KAMA" build "$l/app.kama" "$l/geo/area.kama" "$l/geo/deep/nested.kama" "$l/oddly-named/x.kama" \
        -o "$tmp/lc/app" --keep-c > "$tmp/loose.log" 2>&1; then
    bad "a loose program whose modules are folders does not build:"
    head -5 "$tmp/loose.log" >&2
else
    lsyms=$(grep -ohE '\b(geo__[A-Za-z_]+|_F[0-9]+__[A-Za-z_]+)' "$tmp/lc"/*.c "$tmp/lc"/*.h 2>/dev/null | sort -u)
    lemitted() { printf '%s\n' "$lsyms" | grep -Fxq "$1" && ok "$2" || bad "$2 — no \`$1\` in the emitted C"; }
    lemitted geo__area        "a loose file's folder is its module, with no manifest and no declaration"
    lemitted geo__deep__nested "...composing every folder down from the root"
    # §2e.27, and the one rung the loose derivation deliberately does NOT reach: a file in the root
    # itself has no folder below the root to be named by, so it stays file-private. That is what makes it
    # unimportable, which is the rule 2e turns into a diagnostic.
    printf '%s\n' "$lsyms" | grep -qE '^_F[0-9]+__helper' \
        && ok "a file in the loose ROOT has no module, so it keeps the file-private scope (§2e.27)" \
        || bad "a file in the loose root gained a module — the root is not a folder below itself"
    # A folder whose name is not a legal kama identifier is not a module: nothing could write
    # `import oddly-named::{ … }`. It must not become one ANYWAY and put a hyphen in a C symbol.
    grep -qE '[A-Za-z0-9_]-[A-Za-z0-9_]*__' "$tmp/lc"/*.c "$tmp/lc"/*.h 2>/dev/null \
        && bad "a folder that is not a legal identifier reached a C symbol" \
        || ok "...while a folder no \`import\` could spell is not a module at all"
    rc=0; "$tmp/lc/app" || rc=$?
    [ "$rc" = 6 ] && ok "and it runs: 2 + 3 + 1" || bad "the loose program exited $rc, expected 6"
fi

# The invariant that makes the derivation an IDENTITY and not a position (§1b): it reads a SET, so the
# order the operands are written in cannot reach the emitted C. `_F<n>` still numbers by load order —
# that is exactly what phase 4 replaces — so compare only the module-scoped symbols.
mkdir -p "$tmp/lc2"
if "$KAMA" build "$l/oddly-named/x.kama" "$l/geo/deep/nested.kama" "$l/geo/area.kama" "$l/app.kama" \
        -o "$tmp/lc2/app" --keep-c > "$tmp/loose2.log" 2>&1; then
    a=$(grep -ohE '\bgeo__[A-Za-z_]+' "$tmp/lc"/*.c  "$tmp/lc"/*.h  2>/dev/null | sort -u)
    b=$(grep -ohE '\bgeo__[A-Za-z_]+' "$tmp/lc2"/*.c "$tmp/lc2"/*.h 2>/dev/null | sort -u)
    [ -n "$a" ] && [ "$a" = "$b" ] \
        && ok "the operand ORDER does not reach the symbols — the derivation reads a set" \
        || bad "reordering the operands changed the module symbols"
else
    bad "the same program does not build with its operands in another order"
fi

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-modules: PASS" || echo "check-modules: FAIL" >&2
exit "$fail"
