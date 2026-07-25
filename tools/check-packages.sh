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
KAMA="$ROOT/kama"

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
{ "name": "t11", "version": "0.1.0", "main": "src/app.kama",
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
{ "name": "t12", "version": "0.1.0", "main": "src/app.kama",
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

# 14. kama run with no file and no resolvable entry → a clear error (no kama.json; and kama.json without `main`).
t14="$tmp/t14"; mkdir -p "$t14"
if ( cd "$t14" && "$KAMA" run ) >"$tmp/r14a.out" 2>&1; then
    echo "check-packages: FAIL — kama run in an empty dir was not rejected" >&2; exit 1; fi
grep -qi "no kama.json" "$tmp/r14a.out" || { echo "check-packages: FAIL — no-manifest run error unclear:" >&2; sed 's/^/  /' "$tmp/r14a.out" >&2; exit 1; }
printf '{ "name": "t14", "version": "0.1.0" }\n' > "$t14/kama.json"
if ( cd "$t14" && "$KAMA" run ) >"$tmp/r14b.out" 2>&1; then
    echo "check-packages: FAIL — kama run with no \"main\" was not rejected" >&2; exit 1; fi
grep -qi 'no "main"' "$tmp/r14b.out" || { echo "check-packages: FAIL — no-main run error unclear:" >&2; sed 's/^/  /' "$tmp/r14b.out" >&2; exit 1; }

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

echo "check-packages: PASS (store+integrity+tamper; transitive BFS; sha pin; lock-honoring offline/cold re-fetch; dev-dep --dev boundary; pkg add/remove round-trip; conflict rejected; kama run entry/forward-exit/native-only; SemVer range select/intersect/downgrade/disjoint/offline)"
