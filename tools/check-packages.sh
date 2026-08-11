#!/bin/sh
# check-packages.sh — M2.1 package-store guard. The `.d/` fixture harness only runs `kama build`, never
# `kama install`, so fetch/store/integrity live here. Everything is NETWORK-FREE: a `file://` git repo and
# a local `file://` tarball stand in for real remotes (curl/git both speak file://), mirroring how the net
# tests gate off live I/O. KAMA_STORE points at a throwaway dir so the real ~/.kama/store is never touched.
# It proves:
#   1. git dep → fetched into the content-addressed store, `.kama/deps/<name>` links into it, kama.lock
#      records commit + integrity, and the built program runs (end-to-end).
#   2. re-install → BYTE-IDENTICAL kama.lock (reproducibility + store dedup), exit 0.
#   3. url tarball dep with no `integrity` → trust-on-first-use records the computed sha256.
#   4. url dep with a WRONG `integrity` → install FAILS non-zero and nothing enters the store (tamper).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-packages: $KAMA not built" >&2; exit 1; fi
# The store hash and git fetch shell out; skip gracefully where those tools are absent (they ship on the
# base image, but a bare host may lack them — a skip must not fail the suite).
if ! command -v git >/dev/null 2>&1; then echo "check-packages: SKIP (git not available)"; exit 0; fi
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "check-packages: SKIP (no sha256sum/shasum)"; exit 0
fi
if ! command -v curl >/dev/null 2>&1; then echo "check-packages: SKIP (curl not available)"; exit 0; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export KAMA_STORE="$tmp/store"          # isolate the store; NOT KAMA_HOME (that selects the stdlib root)

# ---- a tiny package as a local git repo, tagged v1.0.0 -----------------------------------------------
geo="$tmp/geo-src"
mkdir -p "$geo"
cat > "$geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0" }
JSON
cat > "$geo/geo.kama" <<'KAMA'
namespace geo;
export { area };
fn int32 area() { return 30; }
KAMA
git -C "$geo" init -q
git -C "$geo" -c user.email=t@t -c user.name=t add -A
git -C "$geo" -c user.email=t@t -c user.name=t commit -qm init
git -C "$geo" tag v1.0.0

# ---- a consumer project depending on it via a file:// git URL ----------------------------------------
proj="$tmp/proj"
mkdir -p "$proj"
cat > "$proj/kama.json" <<JSON
{
  "name": "consumer",
  "version": "0.1.0",
  "dependencies": {
    "geo": { "git": "file://$geo", "rev": "v1.0.0" }
  }
}
JSON
cat > "$proj/main.kama" <<'KAMA'
import geo::{area};
fn int32 main() { return area(); }   // 30
KAMA

# 1. install → store + view + lock ---------------------------------------------------------------------
if ! "$KAMA" pkg install "$proj" >"$tmp/install.out" 2>&1; then
    echo "check-packages: FAIL — git install errored:" >&2; sed 's/^/  /' "$tmp/install.out" >&2; exit 1
fi
lock="$proj/kama.lock"
if ! grep -q '"commit"' "$lock" || ! grep -q '"integrity": "sha256-' "$lock"; then
    echo "check-packages: FAIL — kama.lock missing commit/integrity for the git dep:" >&2
    sed 's/^/  /' "$lock" >&2; exit 1
fi
link="$proj/.kama/deps/geo"
target=$(readlink "$link" 2>/dev/null || true)
case "$target" in
    "$KAMA_STORE"/geo-*) : ;;   # view symlink points into the content-addressed store
    *) echo "check-packages: FAIL — .kama/deps/geo does not link into the store (got '$target')" >&2; exit 1 ;;
esac

# ...and the built program actually resolves the dep through the view and returns 30.
if "$KAMA" build "$proj/main.kama" -o "$tmp/app" >"$tmp/build.out" 2>&1; then
    if "$tmp/app"; then rc=0; else rc=$?; fi
    if [ "$rc" != 30 ]; then echo "check-packages: FAIL — app returned $rc, expected 30" >&2; exit 1; fi
else
    echo "check-packages: FAIL — build of the consumer failed:" >&2; sed 's/^/  /' "$tmp/build.out" >&2; exit 1
fi

# 2. re-install → byte-identical lock (reproducibility + store dedup) -----------------------------------
cp "$lock" "$tmp/lock.first"
if ! "$KAMA" pkg install "$proj" >/dev/null 2>&1; then
    echo "check-packages: FAIL — second install (store populated) errored" >&2; exit 1
fi
if ! cmp -s "$tmp/lock.first" "$lock"; then
    echo "check-packages: FAIL — kama.lock is not byte-identical across installs (non-reproducible)" >&2
    diff "$tmp/lock.first" "$lock" >&2 || true; exit 1
fi

# 3 & 4. url tarball dep: trust-on-first-use, then a tamper (wrong integrity) hard-fails ----------------
pkgsrc="$tmp/geo2"
mkdir -p "$pkgsrc"
cat > "$pkgsrc/geo2.kama" <<'KAMA'
namespace geo2;
export { area2 };
fn int32 area2() { return 42; }
KAMA
tar -czf "$tmp/geo2.tgz" -C "$tmp" geo2       # wrapper dir geo2/ -> stripped by --strip-components=1

proj2="$tmp/proj2"
mkdir -p "$proj2"
cat > "$proj2/kama.json" <<JSON
{ "name": "c2", "version": "0.1.0", "dependencies": { "geo2": { "url": "file://$tmp/geo2.tgz" } } }
JSON
cat > "$proj2/main.kama" <<'KAMA'
import geo2::{area2};
fn int32 main() { return area2(); }   // 42
KAMA
# 3. no integrity in the manifest -> TOFU: install succeeds and records the computed hash.
if ! "$KAMA" pkg install "$proj2" >"$tmp/url.out" 2>&1; then
    echo "check-packages: FAIL — url (trust-on-first-use) install errored:" >&2; sed 's/^/  /' "$tmp/url.out" >&2; exit 1
fi
if ! grep -q '"integrity": "sha256-' "$proj2/kama.lock"; then
    echo "check-packages: FAIL — url TOFU did not record a computed integrity in kama.lock" >&2; exit 1
fi

# 4. tamper: pin a WRONG integrity -> install must fail non-zero and not populate the store.
proj3="$tmp/proj3"
mkdir -p "$proj3"
cat > "$proj3/kama.json" <<JSON
{ "name": "c3", "version": "0.1.0",
  "dependencies": { "geo2": { "url": "file://$tmp/geo2.tgz", "integrity": "sha256-0000000000000000000000000000000000000000000000000000000000000000" } } }
JSON
cat > "$proj3/main.kama" <<'KAMA'
fn int32 main() { return 0; }
KAMA
if "$KAMA" pkg install "$proj3" >"$tmp/tamper.out" 2>&1; then
    echo "check-packages: FAIL — install accepted a tampered tarball (wrong integrity)" >&2; exit 1
fi
if ! grep -qi "integrity mismatch" "$tmp/tamper.out"; then
    echo "check-packages: FAIL — integrity failure, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/tamper.out" >&2; exit 1
fi

# ---- M2.2: resolver + parseLock + dev-deps + pkg add/remove/update ------------------------------------
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
run() { if "$@"; then RC=0; else RC=$?; fi; }   # capture a program's exit code without tripping `set -e`

# a dev-only helper package, and a middle package that deps on geo (prod) + testkit (DEV).
tk="$tmp/testkit"; mkdir -p "$tk"
printf '{ "name": "testkit", "version": "1.0.0" }\n' > "$tk/kama.json"
printf 'namespace testkit;\nexport { helper };\nfn int32 helper() { return 7; }\n' > "$tk/testkit.kama"
git -C "$tk" init -q; git -C "$tk" add -A; git -C "$tk" commit -qm init; git -C "$tk" tag v1.0.0

mid="$tmp/mid"; mkdir -p "$mid"
cat > "$mid/kama.json" <<J
{ "name": "mid", "version": "1.0.0",
  "dependencies":     { "geo":     { "git": "file://$geo", "rev": "v1.0.0" } },
  "dev-dependencies": { "testkit": { "git": "file://$tk",  "rev": "v1.0.0" } } }
J
printf 'namespace mid;\nimport geo::{area};\nexport { boxed };\nfn int32 boxed() { return area() + 5; }\n' > "$mid/mid.kama"
git -C "$mid" init -q; git -C "$mid" add -A; git -C "$mid" commit -qm init; git -C "$mid" tag v1.0.0

# 5. transitive: consumer -> mid -> geo. mid's OWN dev-dep (testkit) must NOT propagate.
t5="$tmp/t5"; mkdir -p "$t5"
cat > "$t5/kama.json" <<J
{ "name": "t5", "version": "0.1.0", "dependencies": { "mid": { "git": "file://$mid", "rev": "v1.0.0" } } }
J
printf 'import mid::{boxed};\nfn int32 main() { return boxed(); }\n' > "$t5/main.kama"   # 35
if ! "$KAMA" pkg install "$t5" >"$tmp/t5.out" 2>&1; then
    echo "check-packages: FAIL — transitive install errored:" >&2; sed 's/^/  /' "$tmp/t5.out" >&2; exit 1; fi
if ! grep -q '"mid"' "$t5/kama.lock" || ! grep -q '"geo"' "$t5/kama.lock" \
   || ! grep -q '"dependencies": \["geo"\]' "$t5/kama.lock"; then
    echo "check-packages: FAIL — transitive lock missing mid/geo or the mid->geo edge:" >&2; sed 's/^/  /' "$t5/kama.lock" >&2; exit 1; fi
if grep -q 'testkit' "$t5/kama.lock"; then
    echo "check-packages: FAIL — a fetched package's dev-dependency leaked transitively" >&2; exit 1; fi
if "$KAMA" build "$t5/main.kama" -o "$tmp/a5" >"$tmp/b5.out" 2>&1; then run "$tmp/a5"
    [ "$RC" = 35 ] || { echo "check-packages: FAIL — transitive app returned $RC, expected 35" >&2; exit 1; }
else echo "check-packages: FAIL — transitive build failed:" >&2; sed 's/^/  /' "$tmp/b5.out" >&2; exit 1; fi

# 6. raw-commit-sha pin: --depth 1 --branch can't ride a sha, so this exercises the init+fetch path.
sha=$(git -C "$geo" rev-parse 'v1.0.0^{commit}')
t6="$tmp/t6"; mkdir -p "$t6"
cat > "$t6/kama.json" <<J
{ "name": "t6", "version": "0.1.0", "dependencies": { "geo": { "git": "file://$geo", "rev": "$sha" } } }
J
if ! "$KAMA" pkg install "$t6" >"$tmp/t6.out" 2>&1; then
    echo "check-packages: FAIL — sha-pinned install errored:" >&2; sed 's/^/  /' "$tmp/t6.out" >&2; exit 1; fi
if ! grep -q "\"commit\": \"$sha\"" "$t6/kama.lock"; then
    echo "check-packages: FAIL — sha pin did not record commit $sha:" >&2; sed 's/^/  /' "$t6/kama.lock" >&2; exit 1; fi

# 7. lock-honoring: (a) source gone, warm store -> offline re-install, byte-identical lock; (b) cold store,
#    lock kept -> re-fetch by the pinned commit, same tree, byte-identical lock.
cp "$t6/kama.lock" "$tmp/t6.lock"
mv "$geo" "$geo.hidden"                       # source unreachable; the store still holds the tree
if ! "$KAMA" pkg install "$t6" >/dev/null 2>&1 || ! cmp -s "$tmp/t6.lock" "$t6/kama.lock"; then
    echo "check-packages: FAIL — warm-store offline re-install not reproducible" >&2; mv "$geo.hidden" "$geo"; exit 1; fi
mv "$geo.hidden" "$geo"
rm -rf "$KAMA_STORE"                          # cold store, lock kept -> must re-fetch by the pinned commit
if ! "$KAMA" pkg install "$t6" >"$tmp/t6b.out" 2>&1 || ! cmp -s "$tmp/t6.lock" "$t6/kama.lock"; then
    echo "check-packages: FAIL — cold-store re-fetch by pinned commit not reproducible:" >&2; sed 's/^/  /' "$tmp/t6b.out" >&2; exit 1; fi

# 8. dev-dependency boundary: dev view separate; prod build can't import it; --dev build can (any opt level).
t8="$tmp/t8"; mkdir -p "$t8"
cat > "$t8/kama.json" <<J
{ "name": "t8", "version": "0.1.0", "dev-dependencies": { "testkit": { "git": "file://$tk", "rev": "v1.0.0" } } }
J
printf 'import testkit::{helper};\nfn int32 main() { return helper(); }\n' > "$t8/main.kama"   # 7
if ! "$KAMA" pkg install "$t8" >"$tmp/t8.out" 2>&1; then
    echo "check-packages: FAIL — dev-dep install errored:" >&2; sed 's/^/  /' "$tmp/t8.out" >&2; exit 1; fi
[ -e "$t8/.kama/dev-deps/testkit" ] || { echo "check-packages: FAIL — dev-dep not linked into .kama/dev-deps" >&2; exit 1; }
[ -e "$t8/.kama/deps/testkit" ]     && { echo "check-packages: FAIL — dev-dep leaked into the prod view" >&2; exit 1; }
grep -q '"dev": true' "$t8/kama.lock" || { echo "check-packages: FAIL — lock did not tag the dev-dep" >&2; sed 's/^/  /' "$t8/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t8/main.kama" -o "$tmp/a8" >"$tmp/e8" 2>&1; then
    echo "check-packages: FAIL — a prod build imported a dev-dependency" >&2; exit 1; fi
grep -qi "cannot resolve module" "$tmp/e8" || { echo "check-packages: FAIL — prod build failed with the wrong error:" >&2; sed 's/^/  /' "$tmp/e8" >&2; exit 1; }
if "$KAMA" build "$t8/main.kama" --dev -o "$tmp/a8" >"$tmp/e8b" 2>&1; then run "$tmp/a8"
    [ "$RC" = 7 ] || { echo "check-packages: FAIL — --dev app returned $RC, expected 7" >&2; exit 1; }
else echo "check-packages: FAIL — --dev build could not import the dev-dependency:" >&2; sed 's/^/  /' "$tmp/e8b" >&2; exit 1; fi

# 9. pkg add / remove round-trip: mutate kama.json while byte-preserving the rest (name/version/flags).
t9="$tmp/t9"; mkdir -p "$t9"
cat > "$t9/kama.json" <<'J'
{
  "name": "t9",
  "version": "0.1.0",
  "flags": { "FANCY": { "default": true } }
}
J
if ! ( cd "$t9" && "$KAMA" pkg add geo --git "file://$geo" --rev v1.0.0 ) >"$tmp/add.out" 2>&1; then
    echo "check-packages: FAIL — pkg add errored:" >&2; sed 's/^/  /' "$tmp/add.out" >&2; exit 1; fi
grep -q '"FANCY"' "$t9/kama.json" && grep -q '"geo"' "$t9/kama.json" && [ -f "$t9/kama.lock" ] \
    || { echo "check-packages: FAIL — pkg add did not preserve flags / write the dep / lock:" >&2; sed 's/^/  /' "$t9/kama.json" >&2; exit 1; }
if ! ( cd "$t9" && "$KAMA" pkg remove geo ) >"$tmp/rm.out" 2>&1; then
    echo "check-packages: FAIL — pkg remove errored:" >&2; sed 's/^/  /' "$tmp/rm.out" >&2; exit 1; fi
if grep -q '"geo"' "$t9/kama.json"; then echo "check-packages: FAIL — pkg remove left the dep behind" >&2; exit 1; fi
grep -q '"FANCY"' "$t9/kama.json" && grep -q '"name": "t9"' "$t9/kama.json" \
    || { echo "check-packages: FAIL — pkg remove damaged the manifest:" >&2; sed 's/^/  /' "$t9/kama.json" >&2; exit 1; }

# 10. conflict hard-fail: two packages require the same name at different revs (no reconciliation yet).
git -C "$geo" tag geo-alt v1.0.0
for m in midA midB; do
    d="$tmp/$m"; mkdir -p "$d"
    printf 'namespace %s;\nexport{v};\nfn int32 v(){return 1;}\n' "$m" > "$d/$m.kama"
done
cat > "$tmp/midA/kama.json" <<J
{ "name": "midA", "dependencies": { "geo": { "git": "file://$geo", "rev": "v1.0.0" } } }
J
cat > "$tmp/midB/kama.json" <<J
{ "name": "midB", "dependencies": { "geo": { "git": "file://$geo", "rev": "geo-alt" } } }
J
for m in midA midB; do d="$tmp/$m"; git -C "$d" init -q; git -C "$d" add -A; git -C "$d" commit -qm i; git -C "$d" tag v1.0.0; done
t10="$tmp/t10"; mkdir -p "$t10"
cat > "$t10/kama.json" <<J
{ "name": "t10", "dependencies": { "midA": { "git": "file://$tmp/midA", "rev": "v1.0.0" }, "midB": { "git": "file://$tmp/midB", "rev": "v1.0.0" } } }
J
if "$KAMA" pkg install "$t10" >"$tmp/e10" 2>&1; then
    echo "check-packages: FAIL — a dependency conflict was not detected" >&2; exit 1; fi
grep -qi "conflict" "$tmp/e10" || { echo "check-packages: FAIL — conflict not reported clearly:" >&2; sed 's/^/  /' "$tmp/e10" >&2; exit 1; }

# 11. kama run: a project with a `main` field + a git dep. `run` (no file) discovers kama.json in CWD, reads
#     `main`, builds + execs it, and FORWARDS the exit code. The explicit `run <file>` form does the same.
t11="$tmp/t11"; mkdir -p "$t11/src"
cat > "$t11/kama.json" <<J
{ "name": "t11", "version": "0.1.0", "entry": "src/app.kama",
  "dependencies": { "geo": { "git": "file://$geo", "rev": "v1.0.0" } } }
J
printf 'import geo::{area};\nfn int32 main() { return area(); }\n' > "$t11/src/app.kama"   # 30
if ! "$KAMA" pkg install "$t11" >"$tmp/t11.out" 2>&1; then
    echo "check-packages: FAIL — run project install errored:" >&2; sed 's/^/  /' "$tmp/t11.out" >&2; exit 1; fi
if ( cd "$t11" && "$KAMA" run ) >"$tmp/r11.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 30 ] || { echo "check-packages: FAIL — kama run (no file) returned $RC, expected 30" >&2; sed 's/^/  /' "$tmp/r11.out" >&2; exit 1; }
if ( cd "$t11" && "$KAMA" run src/app.kama ) >"$tmp/r11b.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 30 ] || { echo "check-packages: FAIL — kama run <file> returned $RC, expected 30" >&2; sed 's/^/  /' "$tmp/r11b.out" >&2; exit 1; }

# 12. kama run + the --dev boundary: a dev-dep-importing entry runs under --dev and FAILS to resolve without.
t12="$tmp/t12"; mkdir -p "$t12/src"
cat > "$t12/kama.json" <<J
{ "name": "t12", "version": "0.1.0", "entry": "src/app.kama",
  "dev-dependencies": { "testkit": { "git": "file://$tk", "rev": "v1.0.0" } } }
J
printf 'import testkit::{helper};\nfn int32 main() { return helper(); }\n' > "$t12/src/app.kama"   # 7
if ! "$KAMA" pkg install "$t12" >"$tmp/t12.out" 2>&1; then
    echo "check-packages: FAIL — run --dev install errored:" >&2; sed 's/^/  /' "$tmp/t12.out" >&2; exit 1; fi
if ( cd "$t12" && "$KAMA" run --dev ) >"$tmp/r12.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 7 ] || { echo "check-packages: FAIL — kama run --dev returned $RC, expected 7" >&2; sed 's/^/  /' "$tmp/r12.out" >&2; exit 1; }
if ( cd "$t12" && "$KAMA" run ) >"$tmp/r12b.out" 2>&1; then
    echo "check-packages: FAIL — kama run (no --dev) imported a dev-dependency" >&2; exit 1; fi
grep -qi "cannot resolve module" "$tmp/r12b.out" || { echo "check-packages: FAIL — run (no --dev) failed with the wrong error:" >&2; sed 's/^/  /' "$tmp/r12b.out" >&2; exit 1; }

# 13. kama run is native-only: --target wasm|embedded is a clean error, not a confusing downstream failure.
if ( cd "$t11" && "$KAMA" run --target wasm ) >"$tmp/r13.out" 2>&1; then
    echo "check-packages: FAIL — kama run --target wasm was not rejected" >&2; exit 1; fi
grep -qi "native-only" "$tmp/r13.out" || { echo "check-packages: FAIL — run --target wasm error unclear:" >&2; sed 's/^/  /' "$tmp/r13.out" >&2; exit 1; }

# 14. kama run with no file and no resolvable entry → a clear error (no kama.json; and kama.json without `entry`).
t14="$tmp/t14"; mkdir -p "$t14"
if ( cd "$t14" && "$KAMA" run ) >"$tmp/r14a.out" 2>&1; then
    echo "check-packages: FAIL — kama run in an empty dir was not rejected" >&2; exit 1; fi
grep -qi "no kama.json" "$tmp/r14a.out" || { echo "check-packages: FAIL — no-manifest run error unclear:" >&2; sed 's/^/  /' "$tmp/r14a.out" >&2; exit 1; }
printf '{ "name": "t14", "version": "0.1.0" }\n' > "$t14/kama.json"
if ( cd "$t14" && "$KAMA" run ) >"$tmp/r14b.out" 2>&1; then
    echo "check-packages: FAIL — kama run with no \"entry\" was not rejected" >&2; exit 1; fi
grep -qi 'no "entry"' "$tmp/r14b.out" || { echo "check-packages: FAIL — no-entry run error unclear:" >&2; sed 's/^/  /' "$tmp/r14b.out" >&2; exit 1; }

# 14b. The pre-1.0 spelling. `main` is NOT accepted (one way to do a thing), but a manifest that visibly
#      names an entry must not be told it has none — the error has to name the rename. A prose claim that
#      something is rejected has no guard unless a case proves it, so this is that case.
t14c="$tmp/t14c"; mkdir -p "$t14c/src"
printf 'fn int32 main() { return 0; }\n' > "$t14c/src/app.kama"
printf '{ "name": "t14c", "version": "0.1.0", "main": "src/app.kama" }\n' > "$t14c/kama.json"
if ( cd "$t14c" && "$KAMA" run ) >"$tmp/r14c.out" 2>&1; then
    echo "check-packages: FAIL — kama run accepted the legacy \"main\" key" >&2; exit 1; fi
grep -q 'now "entry"' "$tmp/r14c.out" || { echo "check-packages: FAIL — legacy-main error does not name the rename:" >&2; sed 's/^/  /' "$tmp/r14c.out" >&2; exit 1; }

# ---- M3.0: SemVer version ranges (git+version, no rev) -----------------------------------------------
# A single repo tagged across several versions; each tagged commit returns a version-distinguishable value
# from area() so the built program's EXIT CODE proves which tag the resolver selected. A pre-release
# (v1.3.0-rc1) and a non-SemVer alias (nightly) must be ignored — never selected.
gv="$tmp/gv-src"; mkdir -p "$gv"
printf '{ "name": "gv", "version": "0.0.0" }\n' > "$gv/kama.json"
git -C "$gv" init -q
gvtag() {   # $1 = return value baked into area(); $2 = tag name
    printf 'namespace gv;\nexport { area };\nfn int32 area() { return %s; }\n' "$1" > "$gv/gv.kama"
    git -C "$gv" add -A; git -C "$gv" commit -qm "$2"; git -C "$gv" tag "$2"
}
gvtag 100 v1.0.0
gvtag 110 v1.1.0
gvtag 120 v1.2.0
gvtag 130 v1.3.0-rc1        # pre-release: not a candidate
gvtag 200 v2.0.0
git -C "$gv" tag nightly    # non-SemVer alias on the newest commit: not a candidate

CRDIR=""
check_range() {   # $1 = range, $2 = expected exit code, $3 = unique suffix
    d="$tmp/cr$3"; mkdir -p "$d"; CRDIR="$d"
    cat > "$d/kama.json" <<J
{ "name": "cr$3", "version": "0.1.0", "dependencies": { "gv": { "git": "file://$gv", "version": "$1" } } }
J
    printf 'import gv::{area};\nfn int32 main() { return area(); }\n' > "$d/main.kama"
    if ! "$KAMA" pkg install "$d" >"$tmp/cr$3.out" 2>&1; then
        echo "check-packages: FAIL — range '$1' install errored:" >&2; sed 's/^/  /' "$tmp/cr$3.out" >&2; exit 1; fi
    if "$KAMA" build "$d/main.kama" -o "$tmp/crapp$3" >"$tmp/crb$3.out" 2>&1; then run "$tmp/crapp$3"
        [ "$RC" = "$2" ] || { echo "check-packages: FAIL — range '$1' selected the wrong version (app returned $RC, expected $2)" >&2; exit 1; }
    else echo "check-packages: FAIL — range '$1' consumer build failed:" >&2; sed 's/^/  /' "$tmp/crb$3.out" >&2; exit 1; fi
}

# 15. highest-satisfying selection across caret / tilde / comparator / wildcard.
check_range "^1.0.0" 120 15a     # >=1.0.0 <2.0.0 -> 1.2.0 (NOT the 1.3.0-rc1 pre-release, NOT 2.0.0)
grep -q '"version": "1.2.0"' "$CRDIR/kama.lock" \
    || { echo "check-packages: FAIL — ^1.0.0 did not record the resolved concrete version in the lock:" >&2; sed 's/^/  /' "$CRDIR/kama.lock" >&2; exit 1; }
grep -q '"rev": "v1.2.0"' "$CRDIR/kama.lock" \
    || { echo "check-packages: FAIL — ^1.0.0 lock did not pin the selected tag v1.2.0:" >&2; sed 's/^/  /' "$CRDIR/kama.lock" >&2; exit 1; }
check_range "~1.1.0" 110 15b     # >=1.1.0 <1.2.0 -> 1.1.0
check_range "<2.0.0" 120 15c     # comparator upper bound -> 1.2.0
check_range "*"      200 15d     # wildcard -> the absolute highest, 2.0.0

# 16. range INTERSECTION across two requestors (compatible): root wants >=1.1.0, midv wants ^1.0.0 ->
#     >=1.1.0 <2.0.0 -> a single flat gv at 1.2.0.
midv="$tmp/midv"; mkdir -p "$midv"
cat > "$midv/kama.json" <<J
{ "name": "midv", "dependencies": { "gv": { "git": "file://$gv", "version": "^1.0.0" } } }
J
printf 'namespace midv;\nimport gv::{area};\nexport { mv };\nfn int32 mv() { return area(); }\n' > "$midv/midv.kama"
git -C "$midv" init -q; git -C "$midv" add -A; git -C "$midv" commit -qm init; git -C "$midv" tag v1.0.0
t16="$tmp/t16"; mkdir -p "$t16"
cat > "$t16/kama.json" <<J
{ "name": "t16", "dependencies": {
    "gv":   { "git": "file://$gv",   "version": ">=1.1.0" },
    "midv": { "git": "file://$midv", "rev": "v1.0.0" } } }
J
printf 'import gv::{area};\nimport midv::{mv};\nfn int32 main() { return area() + mv()*0; }\n' > "$t16/main.kama"
if ! "$KAMA" pkg install "$t16" >"$tmp/t16.out" 2>&1; then
    echo "check-packages: FAIL — intersection install errored:" >&2; sed 's/^/  /' "$tmp/t16.out" >&2; exit 1; fi
grep -q '"version": "1.2.0"' "$t16/kama.lock" \
    || { echo "check-packages: FAIL — range intersection did not resolve gv to 1.2.0:" >&2; sed 's/^/  /' "$t16/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t16/main.kama" -o "$tmp/a16" >"$tmp/b16.out" 2>&1; then run "$tmp/a16"
    [ "$RC" = 120 ] || { echo "check-packages: FAIL — intersection app returned $RC, expected 120" >&2; exit 1; }
else echo "check-packages: FAIL — intersection build failed:" >&2; sed 's/^/  /' "$tmp/b16.out" >&2; exit 1; fi

# 16b. intersection forcing a DOWNGRADE: root picks 1.2.0 for <=1.2.0, then midlo's <=1.1.0 tightens it ->
#      the resolver re-resolves to 1.1.0 (the restart-with-seeded-constraint path).
midlo="$tmp/midlo"; mkdir -p "$midlo"
cat > "$midlo/kama.json" <<J
{ "name": "midlo", "dependencies": { "gv": { "git": "file://$gv", "version": "<=1.1.0" } } }
J
printf 'namespace midlo;\nimport gv::{area};\nexport { ml };\nfn int32 ml() { return area(); }\n' > "$midlo/midlo.kama"
git -C "$midlo" init -q; git -C "$midlo" add -A; git -C "$midlo" commit -qm init; git -C "$midlo" tag v1.0.0
t16b="$tmp/t16b"; mkdir -p "$t16b"
cat > "$t16b/kama.json" <<J
{ "name": "t16b", "dependencies": {
    "gv":    { "git": "file://$gv",    "version": "<=1.2.0" },
    "midlo": { "git": "file://$midlo", "rev": "v1.0.0" } } }
J
printf 'import gv::{area};\nimport midlo::{ml};\nfn int32 main() { return area() + ml()*0; }\n' > "$t16b/main.kama"
if ! "$KAMA" pkg install "$t16b" >"$tmp/t16b.out" 2>&1; then
    echo "check-packages: FAIL — downgrade install errored:" >&2; sed 's/^/  /' "$tmp/t16b.out" >&2; exit 1; fi
grep -q '"version": "1.1.0"' "$t16b/kama.lock" \
    || { echo "check-packages: FAIL — a tighter transitive range did not downgrade gv to 1.1.0:" >&2; sed 's/^/  /' "$t16b/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t16b/main.kama" -o "$tmp/a16b" >"$tmp/b16b.out" 2>&1; then run "$tmp/a16b"
    [ "$RC" = 110 ] || { echo "check-packages: FAIL — downgrade app returned $RC, expected 110" >&2; exit 1; }
else echo "check-packages: FAIL — downgrade build failed:" >&2; sed 's/^/  /' "$tmp/b16b.out" >&2; exit 1; fi

# 17. DISJOINT ranges -> hard fail naming both requestors: midhi needs >=1.2.0, midlo2 needs <1.2.0.
for m in midhi:'>=1.2.0' midlo2:'<1.2.0'; do
    name=${m%%:*}; rng=${m#*:}; d="$tmp/$name"; mkdir -p "$d"
    cat > "$d/kama.json" <<J
{ "name": "$name", "dependencies": { "gv": { "git": "file://$gv", "version": "$rng" } } }
J
    printf 'namespace %s;\nimport gv::{area};\nexport { v };\nfn int32 v() { return area(); }\n' "$name" > "$d/$name.kama"
    git -C "$d" init -q; git -C "$d" add -A; git -C "$d" commit -qm init; git -C "$d" tag v1.0.0
done
t17="$tmp/t17"; mkdir -p "$t17"
cat > "$t17/kama.json" <<J
{ "name": "t17", "dependencies": {
    "midhi":  { "git": "file://$tmp/midhi",  "rev": "v1.0.0" },
    "midlo2": { "git": "file://$tmp/midlo2", "rev": "v1.0.0" } } }
J
if "$KAMA" pkg install "$t17" >"$tmp/e17" 2>&1; then
    echo "check-packages: FAIL — disjoint version ranges were not detected as a conflict" >&2; exit 1; fi
grep -qi "conflict" "$tmp/e17" || { echo "check-packages: FAIL — disjoint conflict not reported clearly:" >&2; sed 's/^/  /' "$tmp/e17" >&2; exit 1; }
grep -q '>=1.2.0' "$tmp/e17" && grep -q '<1.2.0' "$tmp/e17" \
    || { echo "check-packages: FAIL — disjoint conflict did not name both ranges:" >&2; sed 's/^/  /' "$tmp/e17" >&2; exit 1; }

# 18. offline byte-identical re-install: a range dep reuses the locked concrete version WITHOUT ls-remote.
off="$tmp/off"; mkdir -p "$off"
cat > "$off/kama.json" <<J
{ "name": "off", "dependencies": { "gv": { "git": "file://$gv", "version": "^1.0.0" } } }
J
printf 'import gv::{area};\nfn int32 main() { return area(); }\n' > "$off/main.kama"
if ! "$KAMA" pkg install "$off" >"$tmp/off.out" 2>&1; then
    echo "check-packages: FAIL — range offline setup install errored:" >&2; sed 's/^/  /' "$tmp/off.out" >&2; exit 1; fi
cp "$off/kama.lock" "$tmp/off.lock"
mv "$gv" "$gv.hidden"                          # repo unreachable — a warm store + lock must resolve offline
if ! "$KAMA" pkg install "$off" >"$tmp/offb.out" 2>&1 || ! cmp -s "$tmp/off.lock" "$off/kama.lock"; then
    echo "check-packages: FAIL — range offline re-install not reproducible (lock changed or ls-remote hit):" >&2
    sed 's/^/  /' "$tmp/offb.out" >&2; diff "$tmp/off.lock" "$off/kama.lock" >&2 || true; mv "$gv.hidden" "$gv"; exit 1; fi
mv "$gv.hidden" "$gv"

# ==== M3.1a: registry protocol + `kama publish` (network-free, a file:// dir registry) ================
# `kama publish` writes a static registry (<name>/index.json + <name>/<version>.tar.gz); a registry dep
# (a bare `version` range + a `registry` base) resolves the highest version FROM the index and reduces to
# a url dep for the fetch. Everything runs against a `file://` dir — nothing needs a live host.
reg="$tmp/reg"; mkdir -p "$reg"

# publish two versions of `rg` (area() returns a version-distinguishing value) by dogfooding `kama publish`.
pub_rg() {   # pub_rg <version> <area-return>
    d="$tmp/rg-src-$1"; mkdir -p "$d"
    printf '{ "name": "rg", "version": "%s" }\n' "$1" > "$d/kama.json"
    printf 'namespace rg;\nexport { area };\nfn int32 area() { return %s; }\n' "$2" > "$d/rg.kama"
    ( cd "$d" && "$KAMA" publish --registry "file://$reg" )
}
# 19. publish → index + tarball + integrity; a second publish of the same version FAILS (immutability).
if ! pub_rg 1.0.0 10 >"$tmp/pub.out" 2>&1; then echo "check-packages: FAIL — publish 1.0.0 errored:" >&2; sed 's/^/  /' "$tmp/pub.out" >&2; exit 1; fi
if ! pub_rg 1.2.0 12 >>"$tmp/pub.out" 2>&1; then echo "check-packages: FAIL — publish 1.2.0 errored:" >&2; sed 's/^/  /' "$tmp/pub.out" >&2; exit 1; fi
[ -f "$reg/rg/index.json" ] && [ -f "$reg/rg/1.0.0.tar.gz" ] && [ -f "$reg/rg/1.2.0.tar.gz" ] \
    || { echo "check-packages: FAIL — publish did not populate the registry (index/tarballs)" >&2; ls -R "$reg" >&2; exit 1; }
grep -q '"integrity": "sha256-' "$reg/rg/index.json" \
    || { echo "check-packages: FAIL — index.json has no integrity" >&2; cat "$reg/rg/index.json" >&2; exit 1; }
if pub_rg 1.2.0 99 >"$tmp/repub.out" 2>&1; then echo "check-packages: FAIL — republishing 1.2.0 was allowed (immutability):" >&2; sed 's/^/  /' "$tmp/repub.out" >&2; exit 1; fi
grep -qi "immutable" "$tmp/repub.out" || { echo "check-packages: FAIL — republish rejection message unclear:" >&2; sed 's/^/  /' "$tmp/repub.out" >&2; exit 1; }

# a consumer with a registry dep + a caret range resolves the HIGHEST version (1.2.0), fetches into the
# store, links the view, records source:"registry" + version + integrity, and the built program runs (=12).
rc1="$tmp/rc1"; mkdir -p "$rc1"
cat > "$rc1/kama.json" <<JSON
{ "name": "rc1", "version": "0.1.0",
  "dependencies": { "rg": { "version": "^1.0.0", "registry": "file://$reg" } } }
JSON
printf 'import rg::{area};\nfn int32 main() { return area(); }\n' > "$rc1/main.kama"
if ! "$KAMA" pkg install "$rc1" >"$tmp/rc1.out" 2>&1; then echo "check-packages: FAIL — registry install errored:" >&2; sed 's/^/  /' "$tmp/rc1.out" >&2; exit 1; fi
lock="$rc1/kama.lock"
grep -q '"source": "registry"' "$lock" && grep -q '"version": "1.2.0"' "$lock" && grep -q '"integrity": "sha256-' "$lock" \
    || { echo "check-packages: FAIL — registry lock missing source/version/integrity:" >&2; sed 's/^/  /' "$lock" >&2; exit 1; }
case "$(readlink "$rc1/.kama/deps/rg" 2>/dev/null || true)" in
    "$KAMA_STORE"/rg-*) : ;;
    *) echo "check-packages: FAIL — .kama/deps/rg does not link into the store" >&2; exit 1 ;;
esac
if "$KAMA" build "$rc1/main.kama" -o "$tmp/rcapp" >"$tmp/rc1b.out" 2>&1; then
    if "$tmp/rcapp"; then rrc=0; else rrc=$?; rc=$rrc; fi
    [ "$rc" = 12 ] || { echo "check-packages: FAIL — registry consumer returned $rc, expected 12 (highest = 1.2.0)" >&2; exit 1; }
else echo "check-packages: FAIL — build of the registry consumer failed:" >&2; sed 's/^/  /' "$tmp/rc1b.out" >&2; exit 1; fi

# 20. transitive from a registry: `hi` (registry) depends on `rg` (registry); a consumer of `hi` resolves
# both from the fetched manifest (child deps read from the tarball, exactly like a url dep).
hi="$tmp/hi-src"; mkdir -p "$hi"
cat > "$hi/kama.json" <<JSON
{ "name": "hi", "version": "1.0.0",
  "dependencies": { "rg": { "version": "^1.0.0", "registry": "file://$reg" } } }
JSON
printf 'namespace hi;\nimport rg::{area};\nexport { total };\nfn int32 total() { return area() + 8; }\n' > "$hi/hi.kama"
if ! ( cd "$hi" && "$KAMA" publish --registry "file://$reg" ) >"$tmp/hipub.out" 2>&1; then echo "check-packages: FAIL — publish hi errored:" >&2; sed 's/^/  /' "$tmp/hipub.out" >&2; exit 1; fi
rc2="$tmp/rc2"; mkdir -p "$rc2"
cat > "$rc2/kama.json" <<JSON
{ "name": "rc2", "version": "0.1.0",
  "dependencies": { "hi": { "version": "^1.0.0", "registry": "file://$reg" } } }
JSON
printf 'import hi::{total};\nfn int32 main() { return total(); }\n' > "$rc2/main.kama"
if ! "$KAMA" pkg install "$rc2" >"$tmp/rc2.out" 2>&1; then echo "check-packages: FAIL — transitive registry install errored:" >&2; sed 's/^/  /' "$tmp/rc2.out" >&2; exit 1; fi
grep -q '"hi"' "$rc2/kama.lock" && grep -q '"rg"' "$rc2/kama.lock" \
    || { echo "check-packages: FAIL — transitive registry install did not resolve both hi and rg:" >&2; sed 's/^/  /' "$rc2/kama.lock" >&2; exit 1; }
if "$KAMA" build "$rc2/main.kama" -o "$tmp/rc2app" >"$tmp/rc2b.out" 2>&1; then
    if "$tmp/rc2app"; then rc=0; else rc=$?; fi
    [ "$rc" = 20 ] || { echo "check-packages: FAIL — transitive consumer returned $rc, expected 20 (12 + 8)" >&2; exit 1; }
else echo "check-packages: FAIL — build of the transitive consumer failed:" >&2; sed 's/^/  /' "$tmp/rc2b.out" >&2; exit 1; fi

# 21. offline byte-identical re-install: remove the registry dir; a warm store + lock resolves with NO
# index fetch (the registry-dep analog of the git-range offline path).
cp "$rc2/kama.lock" "$tmp/rc2.lock"
mv "$reg" "$reg.hidden"                         # registry unreachable — the lock + warm store must suffice
if ! "$KAMA" pkg install "$rc2" >"$tmp/rc2off.out" 2>&1 || ! cmp -s "$tmp/rc2.lock" "$rc2/kama.lock"; then
    echo "check-packages: FAIL — registry offline re-install not reproducible (lock changed or index hit):" >&2
    sed 's/^/  /' "$tmp/rc2off.out" >&2; diff "$tmp/rc2.lock" "$rc2/kama.lock" >&2 || true; mv "$reg.hidden" "$reg"; exit 1; fi
mv "$reg.hidden" "$reg"

# ==== M3.1b: scopes + `registries` config + dependency-confusion guard =================================
# A scoped `@acme/foo` imports under its BARE last segment (`foo`); the scope only selects which registry
# routes the fetch (via `registries` config). The lock pins content identity (the integrity), not the URI.

# 22. scoped routing + opt-out-of-default. Publish `@acme/sc` to a scope registry; a consumer routes
# `@acme` to it (and drops the default), imports it as `sc`, builds, and runs.
areg="$tmp/areg"; mkdir -p "$areg"
sc="$tmp/sc-src"; mkdir -p "$sc"
printf '{ "name": "@acme/sc", "version": "1.0.0" }\n' > "$sc/kama.json"
printf 'namespace sc;\nexport { val };\nfn int32 val() { return 7; }\n' > "$sc/sc.kama"
if ! ( cd "$sc" && "$KAMA" publish --registry "file://$areg" ) >"$tmp/scpub.out" 2>&1; then echo "check-packages: FAIL — publish @acme/sc errored:" >&2; sed 's/^/  /' "$tmp/scpub.out" >&2; exit 1; fi
[ -f "$areg/@acme/sc/index.json" ] || { echo "check-packages: FAIL — scoped publish path wrong (no @acme/sc/index.json)" >&2; find "$areg" >&2; exit 1; }
scp="$tmp/scp"; mkdir -p "$scp"
cat > "$scp/kama.json" <<JSON
{ "name": "scp", "version": "0.1.0",
  "registries": { "default": false, "@acme": "file://$areg" },
  "dependencies": { "@acme/sc": { "version": "^1.0.0" } } }
JSON
printf 'import sc::{val};\nfn int32 main() { return val(); }\n' > "$scp/main.kama"
if ! "$KAMA" pkg install "$scp" >"$tmp/scp.out" 2>&1; then echo "check-packages: FAIL — scoped install errored:" >&2; sed 's/^/  /' "$tmp/scp.out" >&2; exit 1; fi
[ -L "$scp/.kama/deps/sc" ] || { echo "check-packages: FAIL — scoped dep did not import under its bare name (.kama/deps/sc)" >&2; ls "$scp/.kama/deps" >&2; exit 1; }
if "$KAMA" build "$scp/main.kama" -o "$tmp/scapp" >"$tmp/scb.out" 2>&1; then
    if "$tmp/scapp"; then rc=0; else rc=$?; fi
    [ "$rc" = 7 ] || { echo "check-packages: FAIL — scoped consumer returned $rc, expected 7" >&2; exit 1; }
else echo "check-packages: FAIL — build of the scoped consumer failed:" >&2; sed 's/^/  /' "$tmp/scb.out" >&2; exit 1; fi
# opt-out: an UNSCOPED name with `default:false` and no source is unresolvable (a clean hard error).
opo="$tmp/opo"; mkdir -p "$opo"
cat > "$opo/kama.json" <<JSON
{ "name": "opo", "version": "0.1.0", "registries": { "default": false },
  "dependencies": { "sc": { "version": "^1.0.0" } } }
JSON
if "$KAMA" pkg install "$opo" >"$tmp/opo.out" 2>&1; then echo "check-packages: FAIL — opt-out did not make an unscoped dep unresolvable" >&2; exit 1; fi
grep -qi "no registry configured" "$tmp/opo.out" || { echo "check-packages: FAIL — opt-out error message unclear:" >&2; sed 's/^/  /' "$tmp/opo.out" >&2; exit 1; }

# 23. re-pointable scope + the confusion guard. `cf` published with the SAME bytes to two registries and
# DIFFERENT bytes (same version) to a third. Re-pointing to the same-bytes mirror re-resolves with an
# unchanged integrity; re-pointing to the different-bytes mirror is a hard error.
ra="$tmp/cf-a"; rb="$tmp/cf-b"; rc_="$tmp/cf-c"; mkdir -p "$ra" "$rb" "$rc_"
mkcf() { d="$tmp/cf-src-$1"; mkdir -p "$d"; printf '{ "name": "cf", "version": "1.0.0" }\n' > "$d/kama.json"; printf 'namespace cf;\nexport { val };\nfn int32 val() { return %s; }\n' "$2" > "$d/cf.kama"; echo "$d"; }
csame=$(mkcf same 3); cdiff=$(mkcf diff 4)
( cd "$csame" && "$KAMA" publish --registry "file://$ra" ) >/dev/null 2>&1
( cd "$csame" && "$KAMA" publish --registry "file://$rb" ) >/dev/null 2>&1
( cd "$cdiff" && "$KAMA" publish --registry "file://$rc_" ) >/dev/null 2>&1
cfp="$tmp/cfp"; mkdir -p "$cfp"
cfjson() { cat > "$cfp/kama.json" <<JSON
{ "name": "cfp", "version": "0.1.0", "registries": { "default": "file://$1" },
  "dependencies": { "cf": { "version": "^1.0.0" } } }
JSON
}
cfjson "$ra"; "$KAMA" pkg install "$cfp" >/dev/null 2>&1
int_a=$(grep -o 'sha256-[0-9a-f]*' "$cfp/kama.lock" | head -1)
cfjson "$rb"
if ! "$KAMA" pkg install "$cfp" >"$tmp/cfb.out" 2>&1; then echo "check-packages: FAIL — re-point to same-bytes mirror errored:" >&2; sed 's/^/  /' "$tmp/cfb.out" >&2; exit 1; fi
int_b=$(grep -o 'sha256-[0-9a-f]*' "$cfp/kama.lock" | head -1)
[ "$int_a" = "$int_b" ] || { echo "check-packages: FAIL — re-point to same bytes changed the integrity ($int_a -> $int_b)" >&2; exit 1; }
cfjson "$rc_"
if "$KAMA" pkg install "$cfp" >"$tmp/cfc.out" 2>&1; then echo "check-packages: FAIL — confusion guard let a different-bytes mirror install" >&2; exit 1; fi
grep -qi "confusion" "$tmp/cfc.out" || { echo "check-packages: FAIL — confusion-guard message unclear:" >&2; sed 's/^/  /' "$tmp/cfc.out" >&2; exit 1; }

# 24. import-name collision: two DIFFERENT scopes exposing the same bare name -> a hard error (alias one).
creg="$tmp/creg"; mkdir -p "$creg"
for scp2 in acme other; do d="$tmp/col-$scp2"; mkdir -p "$d"; printf '{ "name": "@%s/cn", "version": "1.0.0" }\n' "$scp2" > "$d/kama.json"; printf 'namespace cn;\nexport { val };\nfn int32 val() { return 1; }\n' > "$d/cn.kama"; ( cd "$d" && "$KAMA" publish --registry "file://$creg" ) >/dev/null 2>&1; done
colp="$tmp/colp"; mkdir -p "$colp"
cat > "$colp/kama.json" <<JSON
{ "name": "colp", "version": "0.1.0",
  "registries": { "default": false, "@acme": "file://$creg", "@other": "file://$creg" },
  "dependencies": { "@acme/cn": { "version": "^1.0.0" }, "@other/cn": { "version": "^1.0.0" } } }
JSON
if "$KAMA" pkg install "$colp" >"$tmp/colp.out" 2>&1; then echo "check-packages: FAIL — import-name collision was not rejected" >&2; exit 1; fi
grep -qi "collision" "$tmp/colp.out" || { echo "check-packages: FAIL — collision message unclear:" >&2; sed 's/^/  /' "$tmp/colp.out" >&2; exit 1; }

# ==== M3.2a: sign-on-publish / verify-on-install (SSHSIG via ssh-keygen -Y) ============================
# `kama publish --key` signs the tarball; the index carries the signature + signer key. `--verify` on
# install enforces (a present signature must verify; a missing one is an error); the default is warn-only.
if command -v ssh-keygen >/dev/null 2>&1; then
    sreg="$tmp/sreg"; mkdir -p "$sreg"
    ssh-keygen -t ed25519 -f "$tmp/pubkey" -N "" -q
    sg="$tmp/sg-src"; mkdir -p "$sg"
    printf '{ "name": "sg", "version": "1.0.0" }\n' > "$sg/kama.json"
    printf 'namespace sg;\nexport { val };\nfn int32 val() { return 5; }\n' > "$sg/sg.kama"
    if ! ( cd "$sg" && "$KAMA" publish --registry "file://$sreg" --key "$tmp/pubkey" ) >"$tmp/sgpub.out" 2>&1; then
        echo "check-packages: FAIL — signed publish errored:" >&2; sed 's/^/  /' "$tmp/sgpub.out" >&2; exit 1; fi
    grep -q '"signature"' "$sreg/sg/index.json" && grep -q '"key"' "$sreg/sg/index.json" \
        || { echo "check-packages: FAIL — signed publish did not record signature+key in the index" >&2; exit 1; }
    sgp="$tmp/sgp"; mkdir -p "$sgp"
    cat > "$sgp/kama.json" <<JSON
{ "name": "sgp", "version": "0.1.0",
  "dependencies": { "sg": { "version": "^1.0.0", "registry": "file://$sreg" } } }
JSON
    printf 'import sg::{val};\nfn int32 main() { return val(); }\n' > "$sgp/main.kama"
    # 25a. --verify install of a signed package passes.
    if ! "$KAMA" pkg install "$sgp" --verify >"$tmp/sv.out" 2>&1; then
        echo "check-packages: FAIL — --verify install of a signed package errored:" >&2; sed 's/^/  /' "$tmp/sv.out" >&2; exit 1; fi
    # 25b. tamper the signature blob in the index → --verify FAILS (cold store forces a re-fetch+re-verify).
    rm -rf "$KAMA_STORE" "$sgp/kama.lock" "$sgp/.kama"
    awk '{ if (!done && index($0,"BEGIN SSH SIGNATURE")>0) { done=1 } print }' "$sreg/sg/index.json" >/dev/null
    # flip a character inside the armored signature body (the line after the BEGIN marker)
    sed 's/\(BEGIN SSH SIGNATURE-----\\n\)./\1Z/' "$sreg/sg/index.json" > "$tmp/idx.bad" && mv "$tmp/idx.bad" "$sreg/sg/index.json"
    if "$KAMA" pkg install "$sgp" --verify >"$tmp/svbad.out" 2>&1; then
        echo "check-packages: FAIL — --verify accepted a tampered signature:" >&2; sed 's/^/  /' "$tmp/svbad.out" >&2; exit 1; fi
    grep -qi "signature verification failed" "$tmp/svbad.out" || { echo "check-packages: FAIL — tampered-signature message unclear:" >&2; sed 's/^/  /' "$tmp/svbad.out" >&2; exit 1; }
    # 25c. the same tampered signature WITHOUT --verify is warn-only: install succeeds, a warning is printed.
    rm -rf "$KAMA_STORE" "$sgp/kama.lock" "$sgp/.kama"
    if ! "$KAMA" pkg install "$sgp" >"$tmp/svwarn.out" 2>&1; then
        echo "check-packages: FAIL — warn-only install (bad sig, no --verify) errored:" >&2; sed 's/^/  /' "$tmp/svwarn.out" >&2; exit 1; fi
    grep -qi "signature check failed" "$tmp/svwarn.out" || { echo "check-packages: FAIL — warn-only did not warn on a bad signature:" >&2; sed 's/^/  /' "$tmp/svwarn.out" >&2; exit 1; }
    SIGNOTE="signed publish/verify/tamper/warn-only"
else
    echo "check-packages: NOTE — ssh-keygen absent, skipping M3.2a signing cases"
    SIGNOTE="signing skipped (no ssh-keygen)"
fi

# ---- M5.3: kama.local.json local overrides -----------------------------------------------------------
# 26. dep path-override (patch-style): a git dep is redirected to a LOCAL dir for this dev only. The
#     committed kama.lock stays BYTE-IDENTICAL (the override is never locked → CI-safe), the view relinks
#     to the local dir, and the build compiles the local code (77) rather than the published one (30).
ogeo="$tmp/ogeo-src"; mkdir -p "$ogeo"
printf '{ "name": "ogeo", "version": "1.0.0" }\n' > "$ogeo/kama.json"
printf 'namespace ogeo;\nexport { area };\nfn int32 area() { return 30; }\n' > "$ogeo/ogeo.kama"
git -C "$ogeo" init -q
git -C "$ogeo" -c user.email=t@t -c user.name=t add -A
git -C "$ogeo" -c user.email=t@t -c user.name=t commit -qm init
git -C "$ogeo" tag v1.0.0
ovp="$tmp/ovp"; mkdir -p "$ovp"
cat > "$ovp/kama.json" <<JSON
{ "name": "ovc", "version": "0.1.0",
  "dependencies": { "ogeo": { "git": "file://$ogeo", "rev": "v1.0.0" } } }
JSON
printf 'import ogeo::{area};\nfn int32 main() { return area(); }\n' > "$ovp/main.kama"
if ! "$KAMA" pkg install "$ovp" >"$tmp/ov1.out" 2>&1; then
    echo "check-packages: FAIL — override base install errored:" >&2; sed 's/^/  /' "$tmp/ov1.out" >&2; exit 1; fi
cp "$ovp/kama.lock" "$tmp/ov.lock.canon"
oloc="$tmp/ogeo-local"; mkdir -p "$oloc"
printf '{ "name": "ogeo", "version": "1.0.0" }\n' > "$oloc/kama.json"
printf 'namespace ogeo;\nexport { area };\nfn int32 area() { return 77; }\n' > "$oloc/ogeo.kama"
cat > "$ovp/kama.local.json" <<JSON
{ "overrides": { "ogeo": { "path": "../ogeo-local" } } }
JSON
if ! "$KAMA" pkg install "$ovp" >"$tmp/ov2.out" 2>&1; then
    echo "check-packages: FAIL — override install errored:" >&2; sed 's/^/  /' "$tmp/ov2.out" >&2; exit 1; fi
if ! cmp -s "$tmp/ov.lock.canon" "$ovp/kama.lock"; then
    echo "check-packages: FAIL — a dep override perturbed kama.lock (must stay canonical):" >&2
    diff "$tmp/ov.lock.canon" "$ovp/kama.lock" >&2 || true; exit 1; fi
case "$(readlink "$ovp/.kama/deps/ogeo" 2>/dev/null || true)" in
    *ogeo-local) : ;;
    *) echo "check-packages: FAIL — override did not relink the view to the local dir" >&2; exit 1 ;;
esac
if "$KAMA" build "$ovp/main.kama" -o "$tmp/ovapp" >"$tmp/ovb.out" 2>&1; then
    if "$tmp/ovapp"; then orc=0; else orc=$?; fi
    [ "$orc" = 77 ] || { echo "check-packages: FAIL — override app returned $orc, expected 77 (local code)" >&2; exit 1; }
else echo "check-packages: FAIL — build against the override failed:" >&2; sed 's/^/  /' "$tmp/ovb.out" >&2; exit 1; fi
# override of a non-dependency → hard error
cat > "$ovp/kama.local.json" <<JSON
{ "overrides": { "nope": { "path": "../ogeo-local" } } }
JSON
if "$KAMA" pkg install "$ovp" >"$tmp/ovbad.out" 2>&1; then
    echo "check-packages: FAIL — override of a non-dependency was accepted" >&2; exit 1; fi
grep -qi "not a dependency" "$tmp/ovbad.out" \
    || { echo "check-packages: FAIL — non-dependency override message unclear:" >&2; sed 's/^/  /' "$tmp/ovbad.out" >&2; exit 1; }

# 27. registries local override: a registry dep with NO `registry`/`registries` configured in kama.json
#     fails to resolve (no built-in default in a dev build); a kama.local.json `registries.default`
#     supplies the base and the dep resolves — proving the local registries override is consulted. (Reuses
#     the `rg` package published to $reg above.)
rp="$tmp/rp"; mkdir -p "$rp"
cat > "$rp/kama.json" <<JSON
{ "name": "rpc", "version": "0.1.0",
  "dependencies": { "rg": { "version": "^1.0.0" } } }
JSON
printf 'import rg::{area};\nfn int32 main() { return area(); }\n' > "$rp/main.kama"
if "$KAMA" pkg install "$rp" >"$tmp/rp0.out" 2>&1; then
    echo "check-packages: FAIL — registry dep resolved with no registry configured" >&2; exit 1; fi
cat > "$rp/kama.local.json" <<JSON
{ "registries": { "default": "file://$reg" } }
JSON
if ! "$KAMA" pkg install "$rp" >"$tmp/rp1.out" 2>&1; then
    echo "check-packages: FAIL — kama.local.json registries override did not resolve the dep:" >&2; sed 's/^/  /' "$tmp/rp1.out" >&2; exit 1; fi
grep -q '"source": "registry"' "$rp/kama.lock" \
    || { echo "check-packages: FAIL — registries-override install did not lock a registry source:" >&2; sed 's/^/  /' "$rp/kama.lock" >&2; exit 1; }

# ---- workspace-internal dependencies -----------------------------------------------------------------
# The five-file monorepo from docs/packages.md § Workspaces: a root that composes `projects`, two
# libraries, and an app. `libs/net` declares the sibling it imports, which is what makes it extractable.
ws="$tmp/acme"
mkdir -p "$ws/libs/config" "$ws/libs/net" "$ws/apps/server"
cat > "$ws/kama.json" <<'JSON'
{ "name": "acme", "version": "0.1.0", "projects": ["libs/*", "apps/server"] }
JSON
cat > "$ws/libs/config/kama.json" <<'JSON'
{ "name": "config", "version": "0.1.0", "sources": ["."] }
JSON
cat > "$ws/libs/config/config.kama" <<'KAMA'
namespace config;
export { Config };

type value Config {
    public int32 port;
    public ctor of(int32 port) { this.port = port; }
}
KAMA
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "sources": ["."],
  "dependencies": { "config": { "path": "../config" } } }
JSON
cat > "$ws/libs/net/net.kama" <<'KAMA'
namespace net;
import config::{Config};
export { listenPort };

fn int32 listenPort() { Config c = Config.of(port: 8); return c.port; }
KAMA
# The app declares only what IT imports. `config` therefore reaches the resolver for the first time as a
# TRANSITIVE request from `net` — a non-root requestor, which is the case the top-level-only rule refused.
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "net": { "path": "../../libs/net" } } }
JSON
printf 'import net::{listenPort};\nfn int32 main() { return listenPort(); }\n' > "$ws/apps/server/main.kama"

# 28. a workspace member may declare the sibling it imports, and that path dep resolves transitively.
if ! "$KAMA" pkg install "$ws/apps/server" >"$tmp/ws0.out" 2>&1; then
    echo "check-packages: FAIL — workspace-internal path dep rejected:" >&2; sed 's/^/  /' "$tmp/ws0.out" >&2; exit 1; fi
"$KAMA" run "$ws/apps/server/main.kama" >/dev/null 2>&1 && wsrc=0 || wsrc=$?
[ "$wsrc" = 8 ] || { echo "check-packages: FAIL — workspace app exited $wsrc, expected 8" >&2; exit 1; }

# 29. THE MILESTONE — the sub-project is EXTRACTABLE: it builds on its own, from its own directory, with
#     no ancestor manifest in play. Before workspace-internal path deps it could not declare `config` at
#     all, so it built where it sat and nowhere else.
if ! "$KAMA" pkg install "$ws/libs/net" >"$tmp/ws1.out" 2>&1; then
    echo "check-packages: FAIL — sub-project could not install standalone:" >&2; sed 's/^/  /' "$tmp/ws1.out" >&2; exit 1; fi
if ! "$KAMA" check "$ws/libs/net/net.kama" >"$tmp/ws2.out" 2>&1; then
    echo "check-packages: FAIL — sub-project does not build standalone (not extractable):" >&2; sed 's/^/  /' "$tmp/ws2.out" >&2; exit 1; fi

# 30. the app may ALSO declare `config`, and it spells the same directory differently (`../../libs/config`
#     vs net's `../config`). Paths are relative to the manifest that declared them, so the two must
#     canonicalize to one package and dedup — comparing the spellings reports "require different sources".
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "config": { "path": "../../libs/config" },
                    "net":    { "path": "../../libs/net" } } }
JSON
if ! "$KAMA" pkg install "$ws/apps/server" >"$tmp/ws5.out" 2>&1; then
    echo "check-packages: FAIL — two spellings of one sibling directory conflicted:" >&2; sed 's/^/  /' "$tmp/ws5.out" >&2; exit 1; fi

# 31. a member may NOT reach outside the workspace — that path is not carried by anything, so it is not
#     reproducible. The declaration is the gate, not adjacency.
mkdir -p "$tmp/stray/lib"
cat > "$tmp/stray/lib/kama.json" <<'JSON'
{ "name": "stray", "version": "0.1.0", "sources": ["."] }
JSON
printf 'namespace stray;\nexport { v };\nfn int32 v() { return 1; }\n' > "$tmp/stray/lib/stray.kama"
cp "$ws/libs/net/kama.json" "$tmp/net-manifest.bak"
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "sources": ["."],
  "dependencies": { "config": { "path": "../config" },
                    "stray":  { "path": "../../../stray/lib" } } }
JSON
if "$KAMA" pkg install "$ws/apps/server" >"$tmp/ws3.out" 2>&1; then
    echo "check-packages: FAIL — a path dep escaping the workspace was accepted" >&2; exit 1; fi
grep -q "only allowed at the top level" "$tmp/ws3.out" \
    || { echo "check-packages: FAIL — workspace-escape message unclear:" >&2; sed 's/^/  /' "$tmp/ws3.out" >&2; exit 1; }
cp "$tmp/net-manifest.bak" "$ws/libs/net/kama.json"

# 32. and without a root manifest DECLARING the tree, the very same sibling dep is refused — proving the
#     gate is the `projects` declaration and not mere directory adjacency. (The app is back to declaring
#     only `net`, so `config` is again a first-encounter transitive request.)
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "net": { "path": "../../libs/net" } } }
JSON
mv "$ws/kama.json" "$tmp/ws-root.bak"
if "$KAMA" pkg install "$ws/apps/server" >"$tmp/ws4.out" 2>&1; then
    echo "check-packages: FAIL — sibling path dep accepted with no declared workspace" >&2; exit 1; fi
mv "$tmp/ws-root.bak" "$ws/kama.json"

# 33. per-package import checking: `libs/net` imports `config` while declaring nothing, and only the app
#     declares it. That builds where it sits and nowhere else, so the BUILD FAILS and names the exact line
#     to add. A hard error — a guarantee nobody is forced to honor is not a guarantee.
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "config": { "path": "../../libs/config" },
                    "net":    { "path": "../../libs/net" } } }
JSON
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "sources": ["."] }
JSON
"$KAMA" pkg install "$ws/apps/server" >/dev/null 2>&1
if "$KAMA" run "$ws/apps/server/main.kama" >"$tmp/ws6.out" 2>&1; then
    echo "check-packages: FAIL — a free-riding sub-project still built:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; fi
grep -q "error:.*does not declare it" "$tmp/ws6.out" \
    || { echo "check-packages: FAIL — no error for a free-riding sub-project:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; }
grep -q '"config": { "path": "../config" }' "$tmp/ws6.out" \
    || { echo "check-packages: FAIL — error did not name the line to add:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; }

# 33b. the editor must NOT be held to it. `kama query` mirrors the LSP, and refusing to analyze would
#      strip cross-module hover/definitions over a *manifest* problem when the code itself resolves fine.
if ! "$KAMA" query "$ws/libs/net/net.kama" --symbols >"$tmp/ws6q.out" 2>&1; then
    echo "check-packages: FAIL — the query/LSP path must degrade to a warning, not refuse:" >&2; sed 's/^/  /' "$tmp/ws6q.out" >&2; exit 1; fi

# 34. and applying exactly the line it named makes it build — the remedy the diagnostic gives is one the
#     resolver actually accepts (which it did not, before workspace-internal path deps).
cp "$tmp/net-manifest.bak" "$ws/libs/net/kama.json"
"$KAMA" pkg install "$ws/apps/server" >/dev/null 2>&1
"$KAMA" run "$ws/apps/server/main.kama" >"$tmp/ws7.out" 2>&1 && frrc=0 || frrc=$?
[ "$frrc" = 8 ] || { echo "check-packages: FAIL — declared workspace build exited $frrc, expected 8:" >&2; sed 's/^/  /' "$tmp/ws7.out" >&2; exit 1; }
if grep -q "does not declare it" "$tmp/ws7.out"; then
    echo "check-packages: FAIL — still reported after declaring the dependency:" >&2; sed 's/^/  /' "$tmp/ws7.out" >&2; exit 1; fi

# 35. a FETCHED package that free-rides must NOT be warned about. Its sources live in the
#     content-addressed store, where the manifest is not the user's to edit and editing it would break
#     the tree hash naming its store entry — and a path dep out of a fetched package is refused anyway.
#     So the diagnostic would instruct a destructive action nobody can act on. (`geo` is the git package
#     built at the top of this file; here a second copy imports `mathx` without declaring it.)
fr="$tmp/frdep"; mkdir -p "$fr/geosrc" "$fr/mathx" "$fr/app"
cat > "$fr/mathx/kama.json" <<'JSON'
{ "name": "mathx", "version": "1.0.0" }
JSON
printf 'namespace mathx;\nexport { two };\nfn int32 two() { return 2; }\n' > "$fr/mathx/mathx.kama"
cat > "$fr/geosrc/kama.json" <<'JSON'
{ "name": "geodep", "version": "1.0.0" }
JSON
printf 'namespace geodep;\nimport mathx::{two};\nexport { area };\nfn int32 area() { return two(); }\n' > "$fr/geosrc/geodep.kama"
( cd "$fr/geosrc" && git init -q . && git add -A \
  && git -c user.email=t@t -c user.name=t commit -qm x && git tag v1.0.0 ) >/dev/null 2>&1
cat > "$fr/app/kama.json" <<JSON
{ "name": "frapp", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "geodep": { "git": "file://$fr/geosrc", "rev": "v1.0.0" },
                    "mathx":  { "path": "../mathx" } } }
JSON
printf 'import geodep::{area};\nfn int32 main() { return area(); }\n' > "$fr/app/main.kama"
if ! "$KAMA" pkg install "$fr/app" >"$tmp/fr0.out" 2>&1; then
    echo "check-packages: FAIL — fetched free-rider fixture did not install:" >&2; sed 's/^/  /' "$tmp/fr0.out" >&2; exit 1; fi
"$KAMA" run "$fr/app/main.kama" >"$tmp/fr1.out" 2>&1 && frdrc=0 || frdrc=$?
[ "$frdrc" = 2 ] || { echo "check-packages: FAIL — fetched free-rider build exited $frdrc, expected 2:" >&2; sed 's/^/  /' "$tmp/fr1.out" >&2; exit 1; }
if grep -q "does not declare it" "$tmp/fr1.out"; then
    echo "check-packages: FAIL — told the user to edit a package inside the store:" >&2; sed 's/^/  /' "$tmp/fr1.out" >&2; exit 1; fi

# 36. ACCEPTANCE — every member of the workspace builds on its own, from its own directory, with no
#     ancestor manifest in play. That is what "extractable" means, and it is mechanically checkable: walk
#     the `projects` tree and install + check each member where it stands. (`libs/config` has no
#     dependencies, `libs/net` has one sibling, `apps/server` has two — all three must stand alone.)
for member in "$ws/libs/config" "$ws/libs/net" "$ws/apps/server"; do
    if ! "$KAMA" pkg install "$member" >"$tmp/acc.out" 2>&1; then
        echo "check-packages: FAIL — $member does not install standalone:" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
    for src in "$member"/*.kama; do
        [ -e "$src" ] || continue
        if ! "$KAMA" check "$src" >"$tmp/acc.out" 2>&1; then
            echo "check-packages: FAIL — $src does not build standalone (not extractable):" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
        if grep -q "does not declare it" "$tmp/acc.out"; then
            echo "check-packages: FAIL — $src free-rides on an ancestor manifest:" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
    done
done

# 37. two packages claiming the SAME (type, contract) conformance. A conformance is program-wide, so a
#     duplicate is visible under the whole-program view and already rejected — but the message named only
#     the type and the contract, which is useless here: neither package's author can see the other, and
#     the user cannot fix either from one side. It must name BOTH packages. (This is the reason kama needs
#     no orphan rule: the hazard an orphan rule prevents is detectable directly.)
dc="$tmp/dupconf"; mkdir -p "$dc/lib" "$dc/app"
cat > "$dc/lib/kama.json" <<'JSON'
{ "name": "marklib", "version": "1.0.0" }
JSON
cat > "$dc/lib/marklib.kama" <<'EOF'
namespace marklib;
export { Marker, viaMarker };
type contract Marker for value { fn int32 mark(); }
type intrinsic <int32> implements Marker { public fn int32 mark() { return 1; } }
fn int32 viaMarker<T: Marker>(ref T v) { return v.mark(); }
EOF
cat > "$dc/app/kama.json" <<JSON
{ "name": "dupapp", "version": "0.1.0", "entry": "main.kama",
  "dependencies": { "marklib": { "path": "../lib" } } }
JSON
cat > "$dc/app/main.kama" <<'EOF'
import marklib::{Marker, viaMarker};
type intrinsic <int32> implements Marker { public fn int32 mark() { return 2; } }
fn int32 main() { int32 x = 5; return viaMarker(v: ref x); }
EOF
"$KAMA" pkg install "$dc/app" >/dev/null 2>&1
if "$KAMA" run "$dc/app/main.kama" >"$tmp/dup.out" 2>&1; then
    echo "check-packages: FAIL — two packages claimed one conformance and it still built:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; fi
grep -q "already implements" "$tmp/dup.out" \
    || { echo "check-packages: FAIL — no duplicate-conformance error:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; }
grep -q "$dc/lib/kama.json" "$tmp/dup.out" \
    || { echo "check-packages: FAIL — duplicate-conformance error did not name the FIRST package:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; }
grep -q "$dc/app/kama.json" "$tmp/dup.out" \
    || { echo "check-packages: FAIL — duplicate-conformance error did not name the SECOND package:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; }

# 37b. the same duplicate WITHIN one package keeps the plain message — there is no second package to name,
#      and the author can see both declarations.
cat > "$dc/app/main.kama" <<'EOF'
type contract Solo for value { fn int32 solo(); }
type intrinsic <int32> implements Solo { public fn int32 solo() { return 1; } }
type intrinsic <int32> implements Solo { public fn int32 solo() { return 2; } }
fn int32 main() { return 0; }
EOF
if "$KAMA" run "$dc/app/main.kama" >"$tmp/dup2.out" 2>&1; then
    echo "check-packages: FAIL — a duplicate conformance in one package still built:" >&2; sed 's/^/  /' "$tmp/dup2.out" >&2; exit 1; fi
grep -q "already implements" "$tmp/dup2.out" \
    || { echo "check-packages: FAIL — no duplicate-conformance error within one package:" >&2; sed 's/^/  /' "$tmp/dup2.out" >&2; exit 1; }
if grep -q "and by package" "$tmp/dup2.out"; then
    echo "check-packages: FAIL — named two packages for a duplicate inside ONE package:" >&2; sed 's/^/  /' "$tmp/dup2.out" >&2; exit 1; fi

# 38. BUILD FROM OUTSIDE THE PROJECT. Manifest discovery used to check the input file's own directory and
#     then the CWD, with no walk up — while owningPackageDir, twenty lines below it, had walked all along.
#     That was survivable while a project's .kama files sat beside its kama.json, and stopped being
#     survivable when `kama seed` put every project's code in src/:
#
#         kama build proj/src/app.kama        # from proj/'s PARENT
#
#     found no manifest, so (a) dependencies did not resolve, and (b) — silently, which is worse — the
#     project was treated as manifest-LESS and the binary was written next to the source in proj/src/
#     instead of under proj/out/. Both halves are checked here, because fixing only the first would leave
#     a build that resolves its dependencies and then litters the source tree anyway.
out="$tmp/outside"; mkdir -p "$out"
"$KAMA" seed "$out/dep" --kind library >/dev/null 2>&1 \
    || { echo "check-packages: FAIL — could not seed the outside-build library" >&2; exit 1; }
"$KAMA" seed "$out/app" >/dev/null 2>&1 \
    || { echo "check-packages: FAIL — could not seed the outside-build app" >&2; exit 1; }
cat > "$out/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "entry": "src/app.kama",
  "dependencies": { "dep": { "path": "../dep" } } }
JSON
cat > "$out/app/src/app.kama" <<'EOF'
import dep::{ answer };
fn int32 main() { return answer(); }
EOF
"$KAMA" pkg install "$out/app" >"$tmp/out.out" 2>&1 \
    || { echo "check-packages: FAIL — outside-build fixture did not install:" >&2; sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

# (a) the dependency resolves when the build is driven from outside the project.
( cd "$out" && "$KAMA" build app/src/app.kama >"$tmp/out.out" 2>&1 ) \
    || { echo "check-packages: FAIL — a build from OUTSIDE the project cannot see its dependencies:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

# (b) the output went to the project's out/, not next to the source. src/ must hold sources only —
#     the same invariant tools/check-clean-tree.sh holds for a manifest-less build.
_stray=$(ls "$out/app/src" | grep -v '\.kama$' || true)
[ -z "$_stray" ] \
    || { echo "check-packages: FAIL — a build from outside wrote into the project's src/: $_stray" >&2; exit 1; }
[ -n "$(find "$out/app/out" -name app -type f -print -quit 2>/dev/null)" ] \
    || { echo "check-packages: FAIL — a build from outside did not use the project's out/ root" >&2; exit 1; }

# (c) a file's OWN project wins over the directory the shell happens to be standing in. Sitting inside
#     `dep`, building app's entry must still use APP's manifest — otherwise the walk would have merely
#     traded one wrong answer for another.
( cd "$out/dep" && "$KAMA" build ../app/src/app.kama >"$tmp/out.out" 2>&1 ) \
    || { echo "check-packages: FAIL — building app from inside a SIBLING project failed:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }
grep -q "app/out/" "$tmp/out.out" \
    || { echo "check-packages: FAIL — used the CWD's manifest instead of the input file's own project:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

echo "check-packages: PASS (store+integrity+tamper; transitive BFS; sha pin; lock-honoring offline/cold re-fetch; dev-dep --dev boundary; pkg add/remove round-trip; conflict rejected; kama run entry/forward-exit/native-only; SemVer range select/intersect/downgrade/disjoint/offline; registry publish/immutability/resolve/transitive/offline; scopes/registries-config/opt-out/re-point/confusion-guard/collision; kama.local.json dep-override/lock-canonical/registries-override; workspace sibling-dep/extractable/spelling-dedup/escape-refused/undeclared-tree-refused/free-ride-is-an-ERROR(lenient in the query/LSP path)-then-fixed; store-package-not-blamed; ACCEPTANCE every member builds standalone; build-from-OUTSIDE resolves deps + uses the project out/ + prefers the input file's own project; duplicate conformance names BOTH packages (and neither, within one); $SIGNOTE)"
