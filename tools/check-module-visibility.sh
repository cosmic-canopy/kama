#!/bin/sh
# check-module-visibility.sh — the CROSS-PROJECT rungs of `visibility` (SPEC.md §Modules).
#
# The within-a-project rungs are fixtures, because a `.d` fixture can carry a `kama.json` and the xfail leg
# now builds by it: `tests/xfail/mod_vis_not_listed.d` (a list that does not name the importer),
# `tests/xfail/mod_vis_children.d` (`"children"` from outside the subtree), and `tests/mod_vis_ok.d`, the
# passing twin that pins the surprising half — a narrow PARENT does not confine a `public` CHILD.
#
# What cannot live there is the fourth row of §2c's table: `public` is the only form that reaches a
# DEPENDENT PROJECT, and proving it needs two projects and an installed path dependency. The xfail leg
# builds a fixture and does not run `kama pkg install` first, so a dependency fixture would fail on the
# missing `.kama/deps` view rather than on the rule. That is the same reason `check-manifest.sh` exists.
#
# ⚠️ BOTH HALVES ARE ASSERTED, and the positive one is not decoration: `visibility` was parsed, validated
# for self-consistency, and consulted by NO ACCESS DECISION for two phases. A guard that only checked
# rejections would have passed against that compiler for one of its two cases, so each rejection here is
# paired with the build that must still succeed.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-module-visibility: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# A dependency project with a `public` root and one `inner` module whose visibility is given per case.
#
# ⚠️ `inner` is given a SIBLING and a CHILD unconditionally, and that is not scaffolding — it is what makes
# cases 3 and 4 test the rung at all. A list may only name a module of its own project, so without
# `sibling` the spelling `["sibling"]` is refused by MANIFEST VALIDATION and never reaches visibility;
# likewise `"children"` on a LEAF is refused by `composeModules`. Both were written that way first and
# both passed with the rung disabled — canaries, not assertions. The fail-check is what said so.
dep() {
    rm -rf "$tmp/geo"; mkdir -p "$tmp/geo/src/inner/nested" "$tmp/geo/src/sibling"
    cat > "$tmp/geo/kama.json" <<JSON
{ "name": "geo", "version": "0.1.0", "kind": "library",
  "modules": { ".": { "visibility": "public" },
               "sibling": { "visibility": "internal" },
               "inner": { "visibility": $1,
                          "modules": { "nested": { "visibility": "internal" } } } } }
JSON
    printf 'export { pub };\nfn int32 pub() { return 4; }\n'      > "$tmp/geo/src/geo.kama"
    printf 'export { priv };\nfn int32 priv() { return 7; }\n'    > "$tmp/geo/src/inner/inner.kama"
    printf 'export { sib };\nfn int32 sib() { return 1; }\n'      > "$tmp/geo/src/sibling/sibling.kama"
    printf 'import { geo::inner::priv };\nexport { nes };\nfn int32 nes() { return priv(); }\n' \
        > "$tmp/geo/src/inner/nested/nested.kama"
}

# The consuming project, with `$1` as its import block body and `$2` as main's expression.
app() {
    rm -rf "$tmp/app"; mkdir -p "$tmp/app/src"
    cat > "$tmp/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "geo": { "path": "../geo" } } }
JSON
    printf 'import { %s };\n\nfn int32 main() { return %s; }\n' "$1" "$2" > "$tmp/app/src/main.kama"
    "$KAMA" pkg install "$tmp/app/kama.json" >/dev/null 2>&1 || true
}

build() { rm -f "$tmp/out.bin"; "$KAMA" build "$tmp/app/kama.json" -o "$tmp/out.bin" >"$tmp/o" 2>"$tmp/e"; }

# --- 1. `public` reaches a dependent project -------------------------------------------------------
dep '"internal"'
app 'geo::pub' 'pub() - 4'
if build; then ok "a dependency's \`public\` module is importable"
else bad "a dependency's \`public\` module was refused"; head -3 "$tmp/e" >&2; fi

# --- 2. `internal` does NOT ------------------------------------------------------------------------
app 'geo::inner::priv' 'priv() - 7'
if build; then
    bad "a dependency's \`internal\` module was imported — \`public\` is the only form that crosses"
elif ! grep -q "is not visible from" "$tmp/e"; then
    bad "the \`internal\` import was refused, but not by the visibility rung"; head -2 "$tmp/e" >&2
else ok "a dependency's \`internal\` module is refused"; fi

# --- 3. ...nor does a LIST -------------------------------------------------------------------------
# `["sibling"]` is a VALID list — `geo::sibling` exists — so the manifest is accepted and the only thing
# that can refuse this import is the rung. A list is scoped to its own project, so no spelling of one can
# reach a dependent: that is what makes `public` the single answer for crossing a project boundary.
dep '["sibling"]'
app 'geo::inner::priv' 'priv() - 7'
if build; then
    bad "a list let a DEPENDENT project in — a list is scoped to its own project"
elif ! grep -q "is not visible from" "$tmp/e"; then
    bad "refused, but not by the visibility rung"; head -2 "$tmp/e" >&2
else ok "a list cannot reach a dependent; crossing a project is \`public\`, full stop"; fi

# --- 4. and `children` does not cross either --------------------------------------------------------
# `inner` has a real child, so `"children"` is a valid spelling here and the manifest is accepted. The
# consumer is not in `inner`'s subtree — it is not even in the same project — so the rung refuses it.
dep '"children"'
app 'geo::inner::priv' 'priv() - 7'
if build; then bad "\`children\` reached a dependent project"
elif ! grep -q "is not visible from" "$tmp/e"; then
    bad "refused, but not by the visibility rung"; head -2 "$tmp/e" >&2
else ok "\`children\` is bounded by its own project too"; fi

[ "$fail" -eq 0 ] && echo "check-module-visibility: PASS (public crosses a project boundary; internal, a list and children do not)" \
                  || echo "check-module-visibility: FAIL" >&2
exit "$fail"
