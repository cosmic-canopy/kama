#!/bin/sh
# check-registry-site.sh — tools/site/registry.mjs builds a registry's human pages without touching what the
# toolchain reads, without letting a package's README run anything, and with the same search `kama pkg search` has.
#
# Why this guard exists. registry.kama-lang.org serves two audiences from one tree: the toolchain reads
# `<name>/index.json` and `catalog.json`, and people read the pages registry.mjs writes beside them. The pages
# are the part that is easy to get wrong silently:
#   - a generator that rewrote an index (pretty-printing, say) would change what every install reads;
#   - a README is a stranger's markdown, and marked passes raw HTML through by default;
#   - the page's search is JavaScript re-implementing the rule `kama pkg search` applies in C++, and two
#     implementations of one rule drift unless something compares them.
# So it publishes a small registry with `kama publish` itself (two packages, one depending on the other, one
# README written to be hostile), builds the pages, and checks each of those, plus that a registry the
# generator cannot trust (no catalog, or one disagreeing with an index) fails the build instead of deploying.
#
# ⚠️ Like check-site.sh it SKIPS without node or the pinned `marked` dependency rather than reaching the network.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

command -v node >/dev/null 2>&1 || { echo "check-registry-site: OK (no node, skipped)"; exit 0; }
[ -d "$ROOT/node_modules/marked" ] || {
    echo "check-registry-site: OK (site deps not installed, skipped — 'npm ci' or './dev site' once to enable)"; exit 0; }
command -v git >/dev/null 2>&1 || { echo "check-registry-site: OK (no git, skipped)"; exit 0; }

tmp=$(mktemp -d) || { echo "check-registry-site: FAIL — mktemp" >&2; exit 1; }
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-registry-site: FAIL — $*" >&2; exit 1; }

reg="$(kama_native_path "$tmp")/reg"; mkdir -p "$reg"
publish() {   # publish <dir> <manifest-json> [readme]
    mkdir -p "$1/src"; printf '%s\n' "$2" > "$1/kama.json"
    printf 'export { v };\nfn int32 v() { return 1; }\n' > "$1/src/$(basename "$1").kama"
    [ -n "${3:-}" ] && printf '%s' "$3" > "$1/README.md"
    git -C "$1" init -q && git -C "$1" add -A && git -C "$1" -c user.name=t -c user.email=t@t commit -qm pub >/dev/null
    ( cd "$1" && "$KAMA" publish kama.json --registry "file://$reg" ) >"$tmp/pub.out" 2>&1 \
        || { sed 's/^/    /' "$tmp/pub.out" >&2; fail "publishing $(basename "$1") errored"; }
}
publish "$tmp/geo" '{ "name": "@t/geo", "version": "1.0.0", "kind": "library", "kama": ">=0.9.523", "license": "MIT", "description": "Points and polygons", "repository": "https://github.com/t/geo", "keywords": ["geometry"], "modules": { ".": { "visibility": "public" } } }' \
'# geo

Hostile text: <script>alert(1)</script> and <b onmouseover="x()">bold</b>.

<img src=x onerror=alert(1)>

[relative](CHANGELOG.md) [script](javascript:alert(1)) [kept](https://example.com/ok)

```kama
fn int32 area() { return 1; }
```
'
# One package per rung of the ranking, all matching the query `geo` — the name itself (@t/geo), part of a name
# (geodesy), a keyword (map), part of a keyword (atlas), the description (charts) — so a page whose weights put
# any two rungs in another order fails the comparison in step 4, not only one whose matching differs.
publish "$tmp/map" '{ "name": "map", "version": "0.2.0", "kind": "library", "kama": ">=0.9.523", "description": "Tiles drawn over shapes", "keywords": ["tiles", "geo"], "dependencies": { "@t/geo": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "public" } } }'
publish "$tmp/geodesy" '{ "name": "geodesy", "version": "0.1.0", "kind": "library", "kama": ">=0.9.523", "description": "Distances on an ellipsoid", "modules": { ".": { "visibility": "public" } } }'
publish "$tmp/atlas" '{ "name": "atlas", "version": "0.1.0", "kind": "library", "kama": ">=0.9.523", "description": "Bound sheets of maps", "keywords": ["geography"], "modules": { ".": { "visibility": "public" } } }'
publish "$tmp/charts" '{ "name": "charts", "version": "0.1.0", "kind": "library", "kama": ">=0.9.523", "description": "Plots of geo data", "modules": { ".": { "visibility": "public" } } }'

out="$tmp/site"
node "$ROOT/tools/site/registry.mjs" --registry "$reg" --out "$out" >"$tmp/build.out" 2>&1 \
    || { sed 's/^/    /' "$tmp/build.out" >&2; fail "registry.mjs failed on a registry kama publish wrote"; }

# 1. What the toolchain reads is copied byte for byte.
for f in catalog.json @t/geo/index.json map/index.json @t/geo/1.0.0.tar.gz; do
    cmp -s "$reg/$f" "$out/$f" || fail "$f was changed on its way into the site (the toolchain reads it)"
done

# 2. The pages exist, and a dependency in the same registry links to its page.
[ -f "$out/index.html" ] && [ -f "$out/@t/geo/index.html" ] && [ -f "$out/map/index.html" ] || fail "a page is missing"
grep -q 'data-name="@t/geo" data-description="Points and polygons" data-keywords="geometry"' "$out/index.html" \
    || fail "the index page does not list @t/geo with its catalog data"
grep -q '<a href="/@t/geo/">@t/geo</a> <code>^1.0.0</code>' "$out/map/index.html" || fail "map's dependency on @t/geo is not linked to its page"
grep -q 'pkg add kama.json @t/geo --version \^1.0.0' "$out/@t/geo/index.html" || fail "the install line is missing"
grep -q 'https://github.com/t/geo/commit/' "$out/@t/geo/index.html" || fail "a GitHub repository's revision is not linked to its commit"
grep -q '<loc>https://registry.kama-lang.org/map/</loc>' "$out/sitemap.xml" || fail "sitemap.xml does not list map's page"

# 3. A README is shown, never run: no tag or handler it wrote survives, no script URL, no link to a file the host
# does not serve — and the parts that are fine are kept.
page="$out/@t/geo/index.html"
grep -q '&lt;script&gt;alert(1)&lt;/script&gt;' "$page" || fail "the README's <script> was not shown as text"
# Escaped text may spell `onerror=` (`&lt;img … onerror=…&gt;`); what must never appear is a real tag carrying it,
# a real <script> from the README, or a script URL in an href.
if grep -Eq '<script>alert|<[a-z][^>]*(onmouseover|onerror)=|href="javascript:' "$page"; then fail "README markup reached the page as markup"; fi
if grep -q 'href="CHANGELOG.md"' "$page"; then fail "a relative README link was kept (this host does not serve the package's files)"; fi
grep -q '<a href="https://example.com/ok" rel="noopener nofollow">kept</a>' "$page" || fail "an https README link was dropped"
grep -q '<span class="t-kw">fn</span>' "$page" || fail "a kama code block in the README was not highlighted"

# 4. The page's search ranks exactly as `kama pkg search` does, for every query below.
sed -n '/^<script>$/,/^<\/script>$/p' "$out/index.html" | sed '1d;$d' > "$tmp/search.js"
node --check "$tmp/search.js" 2>"$tmp/check.out" || { sed 's/^/    /' "$tmp/check.out" >&2; fail "the page's search script does not parse"; }
"$KAMA" pkg search geo --registry "file://$reg" 2>/dev/null | awk '{print $1}' | tr '\n' ' ' > "$tmp/rungs"
[ "$(cat "$tmp/rungs")" = "@t/geo geodesy map atlas charts " ] || fail "kama pkg search geo ranked the rungs as: $(cat "$tmp/rungs")"
for q in "geo" "geometry" "tiles geo" "GEO" "shapes" "maps" "nothing-here"; do
    "$KAMA" pkg search $q --registry "file://$reg" 2>/dev/null | awk '{print $1}' > "$tmp/cli"
    node -e '
      const src = require("fs").readFileSync(process.argv[1], "utf8");
      const lower = s => s.replace(/[A-Z]/g, c => c.toLowerCase());
      const score = eval("(" + /const score = ([\s\S]*?\n  \});\n/.exec(src)[1].replace(/;$/, "") + ")");
      const cat = JSON.parse(require("fs").readFileSync(process.argv[2], "utf8")).packages;
      const words = lower(process.argv[3]).split(/\s+/).filter(Boolean);
      const items = cat.map(p => ({ dataset: { name: p.name, description: p.description || "", keywords: (p.keywords || []).join(" ") } }));
      items.map(li => [score(li, words), li]).filter(([s]) => s > 0)
           .sort((a, b) => b[0] - a[0] || (a[1].dataset.name < b[1].dataset.name ? -1 : 1))
           .forEach(([, li]) => console.log(li.dataset.name));' "$tmp/search.js" "$reg/catalog.json" "$q" > "$tmp/web"
    cmp -s "$tmp/cli" "$tmp/web" || { echo "  query \"$q\" — kama pkg search:" >&2; sed 's/^/    /' "$tmp/cli" >&2
        echo "  the page:" >&2; sed 's/^/    /' "$tmp/web" >&2; fail "the page and \`kama pkg search\` disagree"; }
done

# 5. A registry the pages cannot trust fails the build rather than deploying: no catalog, or one that names a
# version its index does not have as the highest.
mv "$reg/catalog.json" "$tmp/catalog.json"
if node "$ROOT/tools/site/registry.mjs" --registry "$reg" --out "$tmp/s2" >"$tmp/b2.out" 2>&1; then fail "a registry with no catalog.json built"; fi
grep -q 'catalog.json is missing' "$tmp/b2.out" || { sed 's/^/    /' "$tmp/b2.out" >&2; fail "the missing catalog was not named"; }
sed 's/"version": "0.2.0"/"version": "0.1.0"/' "$tmp/catalog.json" > "$reg/catalog.json"
if node "$ROOT/tools/site/registry.mjs" --registry "$reg" --out "$tmp/s3" >"$tmp/b3.out" 2>&1; then fail "a catalog disagreeing with an index built"; fi
grep -q "says map is at 0.1.0, but its index's highest version is 0.2.0" "$tmp/b3.out" \
    || { sed 's/^/    /' "$tmp/b3.out" >&2; fail "the disagreement was not named"; }

echo "check-registry-site: OK (protocol files byte-identical; pages, dependency links, install line, commit links, sitemap; README shown never run; search ranks name > part of name > keyword > part of keyword > description, and agrees with kama pkg search on 7 queries; untrusted registry refused)"
