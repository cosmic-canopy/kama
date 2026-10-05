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
# `$furl`: a file:// URL for something under $tmp, spelled for a NATIVE tool. kama is a native program and runs
# `curl` through the host shell, so on msys2 the URL must not carry an msys path: `file:///tmp/x` opens only
# in an msys curl (/usr/bin), and the day a native one lands in /ucrt64/bin ahead of it — it arrives as a
# dependency of cmake, measured 2026-09-18 — the download fails with curl (37). `file:///C:/msys64/tmp/x`
# opens in both. (The `git` URLs below stay `file://$tmp/…`: git here is msys git, measured.)
furl="file://$tmp"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) furl="file:///$(cygpath -m "$tmp")" ;; esac
trap 'rm -rf "$tmp"' EXIT
# Native-spelled: KAMA_STORE reaches kama through the ENVIRONMENT, which msys2 does not path-convert the
# way it converts arguments (see kama_native_path in tools/kama-bin.sh). Left as `/tmp/…` the store landed
# on a different drive root than the one this guard writes its fixtures to.
#
# The `file://` URLs below stay POSIX on purpose. git here is msys2's own build, which resolves `/tmp/x`
# natively and reads a `file:///C:/x` URL as the path `/C:/x` — "does not appear to be a git repository".
export KAMA_STORE="$(kama_native_path "$tmp")/store"   # isolate the store; NOT KAMA_HOME (that selects the stdlib root)

# ---- a tiny package as a local git repo, tagged v1.0.0 -----------------------------------------------
geo="$tmp/geo-src"
mkdir -p "$geo/src"
cat > "$geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$geo/src/geo.kama" <<'KAMA'
export { area };
fn int32 area() { return 30; }
KAMA
git -C "$geo" init -q
git -C "$geo" -c user.email=t@t -c user.name=t add -A
git -C "$geo" -c user.email=t@t -c user.name=t commit -qm init
git -C "$geo" tag v1.0.0

# ---- a consumer project depending on it via a file:// git URL ----------------------------------------
proj="$tmp/proj"
mkdir -p "$proj/src"
cat > "$proj/kama.json" <<JSON
{
  "name": "consumer",
  "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": {
    "geo": { "git": "file://$geo", "rev": "v1.0.0" }
  },
  "modules": { ".": { "visibility": "internal" } }
}
JSON
cat > "$proj/src/main.kama" <<'KAMA'
import { geo::area };
fn int32 main() { return area(); }   // 30
KAMA

# 1. install → store + view + lock ---------------------------------------------------------------------
if ! "$KAMA" pkg install "$proj/kama.json" >"$tmp/install.out" 2>&1; then
    echo "check-packages: FAIL — git install errored:" >&2; sed 's/^/  /' "$tmp/install.out" >&2; exit 1
fi
lock="$proj/kama.lock"
if ! grep -q '"commit"' "$lock" || ! grep -q '"integrity": "sha256-' "$lock"; then
    echo "check-packages: FAIL — kama.lock missing commit/integrity for the git dep:" >&2
    sed 's/^/  /' "$lock" >&2; exit 1
fi
link="$proj/.kama/deps/geo"
# Normalized to the same spelling as $KAMA_STORE before comparing. On Windows the view entry is a junction
# (mklink /J) and readlink hands back the msys spelling `/tmp/…` of a store that kama created — correctly —
# at `C:/msys64/tmp/…`. Same directory, two names; without this the case below rejects a passing result.
target=$(readlink "$link" 2>/dev/null || true)
# `if`, not `[ -n … ] && …`: under `set -e` a trailing test that comes out false is the script's exit
# status, so an empty $target (readlink on a Windows junction can give one) killed this guard with no
# message at all rather than reaching the case below.
if [ -n "$target" ]; then target=$(kama_native_path "$target"); fi
case "$target" in
    "$KAMA_STORE"/geo-*) : ;;   # view symlink points into the content-addressed store
    *) echo "check-packages: FAIL — .kama/deps/geo does not link into the store (got '$target')" >&2; exit 1 ;;
esac

# ...and the built program actually resolves the dep through the view and returns 30.
if "$KAMA" build "$proj/kama.json" -o "$tmp/app" >"$tmp/build.out" 2>&1; then
    if "$tmp/app"; then rc=0; else rc=$?; fi
    if [ "$rc" != 30 ]; then echo "check-packages: FAIL — app returned $rc, expected 30" >&2; exit 1; fi
else
    echo "check-packages: FAIL — build of the consumer failed:" >&2; sed 's/^/  /' "$tmp/build.out" >&2; exit 1
fi

# 2. re-install → byte-identical lock (reproducibility + store dedup) -----------------------------------
cp "$lock" "$tmp/lock.first"
if ! "$KAMA" pkg install "$proj/kama.json" >/dev/null 2>&1; then
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
export { area2 };
fn int32 area2() { return 42; }
KAMA
tar -czf "$tmp/geo2.tgz" -C "$tmp" geo2       # wrapper dir geo2/ -> stripped by --strip-components=1

proj2="$tmp/proj2"
mkdir -p "$proj2/src"
cat > "$proj2/kama.json" <<JSON
{ "name": "c2", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "geo2": { "url": "$furl/geo2.tgz" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$proj2/src/main.kama" <<'KAMA'
import { geo2::area2 };
fn int32 main() { return area2(); }   // 42
KAMA
# 3. no integrity in the manifest -> TOFU: install succeeds and records the computed hash.
if ! "$KAMA" pkg install "$proj2/kama.json" >"$tmp/url.out" 2>&1; then
    echo "check-packages: FAIL — url (trust-on-first-use) install errored:" >&2; sed 's/^/  /' "$tmp/url.out" >&2; exit 1
fi
if ! grep -q '"integrity": "sha256-' "$proj2/kama.lock"; then
    echo "check-packages: FAIL — url TOFU did not record a computed integrity in kama.lock" >&2; exit 1
fi
# Nothing kama stages may outlive the command: the download beside the store entry, the signature beside a
# registry's tarball. Windows kept every one until 0.9.455 — its `rmdir /s /q` cannot delete a FILE (KR-99).
no_tmp_left() {
    if ls -A "$1" | grep -q '^\.tmp-'; then
        echo "check-packages: FAIL — $2 left a temp file behind in $1:" >&2; ls -A "$1" | sed 's/^/  /' >&2; exit 1
    fi
}
no_tmp_left "$KAMA_STORE" "a url install"

# 4. tamper: pin a WRONG integrity -> install must fail non-zero and not populate the store.
proj3="$tmp/proj3"
mkdir -p "$proj3/src"
cat > "$proj3/kama.json" <<JSON
{ "name": "c3", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "geo2": { "url": "$furl/geo2.tgz", "integrity": "sha256-0000000000000000000000000000000000000000000000000000000000000000" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$proj3/src/main.kama" <<'KAMA'
fn int32 main() { return 0; }
KAMA
if "$KAMA" pkg install "$proj3/kama.json" >"$tmp/tamper.out" 2>&1; then
    echo "check-packages: FAIL — install accepted a tampered tarball (wrong integrity)" >&2; exit 1
fi
if ! grep -qi "integrity mismatch" "$tmp/tamper.out"; then
    echo "check-packages: FAIL — integrity failure, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/tamper.out" >&2; exit 1
fi
no_tmp_left "$KAMA_STORE" "a refused url install"

# ---- M2.2: resolver + parseLock + dev-deps + pkg add/remove/update ------------------------------------
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
run() { if "$@"; then RC=0; else RC=$?; fi; }   # capture a program's exit code without tripping `set -e`

# a dev-only helper package, and a middle package that deps on geo (prod) + testkit (DEV).
tk="$tmp/testkit"; mkdir -p "$tk/src"
printf '{ "name": "testkit", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$tk/kama.json"
printf 'export { helper };\nfn int32 helper() { return 7; }\n' > "$tk/src/testkit.kama"
git -C "$tk" init -q; git -C "$tk" add -A; git -C "$tk" commit -qm init; git -C "$tk" tag v1.0.0

mid="$tmp/mid"; mkdir -p "$mid/src"
cat > "$mid/kama.json" <<J
{ "name": "mid", "version": "1.0.0", "kind": "library",
  "dependencies":     { "geo":     { "git": "file://$geo", "rev": "v1.0.0" } },
  "dev-dependencies": { "testkit": { "git": "file://$tk",  "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "public" } } }
J
printf 'import { geo::area };\nexport { boxed };\nfn int32 boxed() { return area() + 5; }\n' > "$mid/src/mid.kama"
git -C "$mid" init -q; git -C "$mid" add -A; git -C "$mid" commit -qm init; git -C "$mid" tag v1.0.0

# 5. transitive: consumer -> mid -> geo. mid's OWN dev-dep (testkit) must NOT propagate.
t5="$tmp/t5"; mkdir -p "$t5/src"
cat > "$t5/kama.json" <<J
{ "name": "t5", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "mid": { "git": "file://$mid", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { mid::boxed };\nfn int32 main() { return boxed(); }\n' > "$t5/src/main.kama"   # 35
if ! "$KAMA" pkg install "$t5/kama.json" >"$tmp/t5.out" 2>&1; then
    echo "check-packages: FAIL — transitive install errored:" >&2; sed 's/^/  /' "$tmp/t5.out" >&2; exit 1; fi
if ! grep -q '"mid"' "$t5/kama.lock" || ! grep -q '"geo"' "$t5/kama.lock" \
   || ! grep -q '"dependencies": \["geo"\]' "$t5/kama.lock"; then
    echo "check-packages: FAIL — transitive lock missing mid/geo or the mid->geo edge:" >&2; sed 's/^/  /' "$t5/kama.lock" >&2; exit 1; fi
if grep -q 'testkit' "$t5/kama.lock"; then
    echo "check-packages: FAIL — a fetched package's dev-dependency leaked transitively" >&2; exit 1; fi
if "$KAMA" build "$t5/kama.json" -o "$tmp/a5" >"$tmp/b5.out" 2>&1; then run "$tmp/a5"
    [ "$RC" = 35 ] || { echo "check-packages: FAIL — transitive app returned $RC, expected 35" >&2; exit 1; }
else echo "check-packages: FAIL — transitive build failed:" >&2; sed 's/^/  /' "$tmp/b5.out" >&2; exit 1; fi

# 6. raw-commit-sha pin: --depth 1 --branch can't ride a sha, so this exercises the init+fetch path.
sha=$(git -C "$geo" rev-parse 'v1.0.0^{commit}')
t6="$tmp/t6"; mkdir -p "$t6"
cat > "$t6/kama.json" <<J
{ "name": "t6", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "geo": { "git": "file://$geo", "rev": "$sha" } }, "modules": { ".": { "visibility": "internal" } } }
J
if ! "$KAMA" pkg install "$t6/kama.json" >"$tmp/t6.out" 2>&1; then
    echo "check-packages: FAIL — sha-pinned install errored:" >&2; sed 's/^/  /' "$tmp/t6.out" >&2; exit 1; fi
if ! grep -q "\"commit\": \"$sha\"" "$t6/kama.lock"; then
    echo "check-packages: FAIL — sha pin did not record commit $sha:" >&2; sed 's/^/  /' "$t6/kama.lock" >&2; exit 1; fi

# 7. lock-honoring: (a) source gone, warm store -> offline re-install, byte-identical lock; (b) cold store,
#    lock kept -> re-fetch by the pinned commit, same tree, byte-identical lock.
cp "$t6/kama.lock" "$tmp/t6.lock"
mv "$geo" "$geo.hidden"                       # source unreachable; the store still holds the tree
if ! "$KAMA" pkg install "$t6/kama.json" >/dev/null 2>&1 || ! cmp -s "$tmp/t6.lock" "$t6/kama.lock"; then
    echo "check-packages: FAIL — warm-store offline re-install not reproducible" >&2; mv "$geo.hidden" "$geo"; exit 1; fi
mv "$geo.hidden" "$geo"
rm -rf "$KAMA_STORE"                          # cold store, lock kept -> must re-fetch by the pinned commit
if ! "$KAMA" pkg install "$t6/kama.json" >"$tmp/t6b.out" 2>&1 || ! cmp -s "$tmp/t6.lock" "$t6/kama.lock"; then
    echo "check-packages: FAIL — cold-store re-fetch by pinned commit not reproducible:" >&2; sed 's/^/  /' "$tmp/t6b.out" >&2; exit 1; fi

# 8. dev-dependency boundary: dev view separate; prod build can't import it; --dev build can (any opt level).
t8="$tmp/t8"; mkdir -p "$t8/src"
cat > "$t8/kama.json" <<J
{ "name": "t8", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dev-dependencies": { "testkit": { "git": "file://$tk", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { testkit::helper };\nfn int32 main() { return helper(); }\n' > "$t8/src/main.kama"   # 7
if ! "$KAMA" pkg install "$t8/kama.json" >"$tmp/t8.out" 2>&1; then
    echo "check-packages: FAIL — dev-dep install errored:" >&2; sed 's/^/  /' "$tmp/t8.out" >&2; exit 1; fi
[ -e "$t8/.kama/dev-deps/testkit" ] || { echo "check-packages: FAIL — dev-dep not linked into .kama/dev-deps" >&2; exit 1; }
[ -e "$t8/.kama/deps/testkit" ]     && { echo "check-packages: FAIL — dev-dep leaked into the prod view" >&2; exit 1; }
grep -q '"dev": true' "$t8/kama.lock" || { echo "check-packages: FAIL — lock did not tag the dev-dep" >&2; sed 's/^/  /' "$t8/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t8/kama.json" -o "$tmp/a8" >"$tmp/e8" 2>&1; then
    echo "check-packages: FAIL — a prod build imported a dev-dependency" >&2; exit 1; fi
grep -qi "cannot resolve module" "$tmp/e8" || { echo "check-packages: FAIL — prod build failed with the wrong error:" >&2; sed 's/^/  /' "$tmp/e8" >&2; exit 1; }
if "$KAMA" build "$t8/kama.json" --dev -o "$tmp/a8" >"$tmp/e8b" 2>&1; then run "$tmp/a8"
    [ "$RC" = 7 ] || { echo "check-packages: FAIL — --dev app returned $RC, expected 7" >&2; exit 1; }
else echo "check-packages: FAIL — --dev build could not import the dev-dependency:" >&2; sed 's/^/  /' "$tmp/e8b" >&2; exit 1; fi

# 9. pkg add / remove round-trip: mutate kama.json while byte-preserving the rest (name/version/flags).
t9="$tmp/t9"; mkdir -p "$t9"
cat > "$t9/kama.json" <<'J'
{
  "name": "t9",
  "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "flags": { "FANCY": { "default": true } },
  "modules": { ".": { "visibility": "internal" } }
}
J
if ! ( cd "$t9" && "$KAMA" pkg add kama.json geo --git "file://$geo" --rev v1.0.0 ) >"$tmp/add.out" 2>&1; then
    echo "check-packages: FAIL — pkg add errored:" >&2; sed 's/^/  /' "$tmp/add.out" >&2; exit 1; fi
grep -q '"FANCY"' "$t9/kama.json" && grep -q '"geo"' "$t9/kama.json" && [ -f "$t9/kama.lock" ] \
    || { echo "check-packages: FAIL — pkg add did not preserve flags / write the dep / lock:" >&2; sed 's/^/  /' "$t9/kama.json" >&2; exit 1; }
if ! ( cd "$t9" && "$KAMA" pkg remove kama.json geo ) >"$tmp/rm.out" 2>&1; then
    echo "check-packages: FAIL — pkg remove errored:" >&2; sed 's/^/  /' "$tmp/rm.out" >&2; exit 1; fi
if grep -q '"geo"' "$t9/kama.json"; then echo "check-packages: FAIL — pkg remove left the dep behind" >&2; exit 1; fi
grep -q '"FANCY"' "$t9/kama.json" && grep -q '"name": "t9"' "$t9/kama.json" \
    || { echo "check-packages: FAIL — pkg remove damaged the manifest:" >&2; sed 's/^/  /' "$t9/kama.json" >&2; exit 1; }

# 10. conflict hard-fail: two packages require the same name at different revs (no reconciliation yet).
git -C "$geo" tag geo-alt v1.0.0
for m in midA midB; do
    d="$tmp/$m"; mkdir -p "$d/src"
    printf 'namespace %s;\nexport{v};\nfn int32 v(){return 1;}\n' "$m" > "$d/src/$m.kama"
done
cat > "$tmp/midA/kama.json" <<J
{ "name": "midA", "kind": "executable", "entry": "src/main.kama", "dependencies": { "geo": { "git": "file://$geo", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
cat > "$tmp/midB/kama.json" <<J
{ "name": "midB", "kind": "executable", "entry": "src/main.kama", "dependencies": { "geo": { "git": "file://$geo", "rev": "geo-alt" } }, "modules": { ".": { "visibility": "internal" } } }
J
for m in midA midB; do d="$tmp/$m"; git -C "$d" init -q; git -C "$d" add -A; git -C "$d" commit -qm i; git -C "$d" tag v1.0.0; done
t10="$tmp/t10"; mkdir -p "$t10"
cat > "$t10/kama.json" <<J
{ "name": "t10", "kind": "executable", "entry": "src/main.kama", "dependencies": { "midA": { "git": "file://$tmp/midA", "rev": "v1.0.0" }, "midB": { "git": "file://$tmp/midB", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
if "$KAMA" pkg install "$t10/kama.json" >"$tmp/e10" 2>&1; then
    echo "check-packages: FAIL — a dependency conflict was not detected" >&2; exit 1; fi
grep -qi "conflict" "$tmp/e10" || { echo "check-packages: FAIL — conflict not reported clearly:" >&2; sed 's/^/  /' "$tmp/e10" >&2; exit 1; }

# 11. kama run: a project with an `entry` + a git dep. `run <kama.json>` builds the project and execs it,
#     FORWARDING the exit code — from inside the project or by naming its manifest from anywhere else.
#
#     And the third assertion is the mode split itself: naming the SOURCE FILE is a loose run, which
#     applies no manifest and therefore has no dependency view, so the import cannot resolve. That is the
#     point of the spelling rather than a shortcoming of it — "just these files" has to mean it.
t11="$tmp/t11"; mkdir -p "$t11/src"
cat > "$t11/kama.json" <<J
{ "name": "t11", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "dependencies": { "geo": { "git": "file://$geo", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { geo::area };\nfn int32 main() { return area(); }\n' > "$t11/src/app.kama"   # 30
if ! "$KAMA" pkg install "$t11/kama.json" >"$tmp/t11.out" 2>&1; then
    echo "check-packages: FAIL — run project install errored:" >&2; sed 's/^/  /' "$tmp/t11.out" >&2; exit 1; fi
if ( cd "$t11" && "$KAMA" run kama.json ) >"$tmp/r11.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 30 ] || { echo "check-packages: FAIL — kama run kama.json returned $RC, expected 30" >&2; sed 's/^/  /' "$tmp/r11.out" >&2; exit 1; }
if "$KAMA" run "$t11/kama.json" >"$tmp/r11b.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 30 ] || { echo "check-packages: FAIL — kama run <path>/kama.json from outside returned $RC, expected 30" >&2; sed 's/^/  /' "$tmp/r11b.out" >&2; exit 1; }
if ( cd "$t11" && "$KAMA" run src/app.kama ) >"$tmp/r11c.out" 2>&1; then
    echo "check-packages: FAIL — a LOOSE run resolved a dependency; it applies no manifest" >&2
    sed 's/^/  /' "$tmp/r11c.out" >&2; exit 1; fi
grep -q "cannot resolve module 'geo'" "$tmp/r11c.out" || {
    echo "check-packages: FAIL — a loose run failed, but not for the reason claimed:" >&2
    sed 's/^/  /' "$tmp/r11c.out" >&2; exit 1; }

# 12. kama run + the --dev boundary: a dev-dep-importing entry runs under --dev and FAILS to resolve without.
t12="$tmp/t12"; mkdir -p "$t12/src"
cat > "$t12/kama.json" <<J
{ "name": "t12", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "dev-dependencies": { "testkit": { "git": "file://$tk", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { testkit::helper };\nfn int32 main() { return helper(); }\n' > "$t12/src/app.kama"   # 7
if ! "$KAMA" pkg install "$t12/kama.json" >"$tmp/t12.out" 2>&1; then
    echo "check-packages: FAIL — run --dev install errored:" >&2; sed 's/^/  /' "$tmp/t12.out" >&2; exit 1; fi
if ( cd "$t12" && "$KAMA" run kama.json --dev ) >"$tmp/r12.out" 2>&1; then RC=0; else RC=$?; fi
[ "$RC" = 7 ] || { echo "check-packages: FAIL — kama run --dev returned $RC, expected 7" >&2; sed 's/^/  /' "$tmp/r12.out" >&2; exit 1; }
if ( cd "$t12" && "$KAMA" run kama.json ) >"$tmp/r12b.out" 2>&1; then
    echo "check-packages: FAIL — kama run (no --dev) imported a dev-dependency" >&2; exit 1; fi
grep -qi "cannot resolve module" "$tmp/r12b.out" || { echo "check-packages: FAIL — run (no --dev) failed with the wrong error:" >&2; sed 's/^/  /' "$tmp/r12b.out" >&2; exit 1; }

# 13. kama run is native-only: --target wasm|embedded is a clean error, not a confusing downstream failure.
if ( cd "$t11" && "$KAMA" run kama.json --target wasm ) >"$tmp/r13.out" 2>&1; then
    echo "check-packages: FAIL — kama run --target wasm was not rejected" >&2; exit 1; fi
grep -qi "native-only" "$tmp/r13.out" || { echo "check-packages: FAIL — run --target wasm error unclear:" >&2; sed 's/^/  /' "$tmp/r13.out" >&2; exit 1; }

# 14. `kama run` naming a project that cannot answer → a clear error at each step of the way: no manifest
#     there at all, then a manifest whose `source` root is missing, then an executable that never said
#     which file holds its `main`. The last is what gives `entry` a consumer on the BUILD path: it used to
#     be discovered at the link, as `Undefined symbols: _main`.
t14="$tmp/t14"; mkdir -p "$t14"
if ( cd "$t14" && "$KAMA" run kama.json ) >"$tmp/r14a.out" 2>&1; then
    echo "check-packages: FAIL — kama run naming an absent manifest was not rejected" >&2; exit 1; fi
grep -qi "kama.json does not exist" "$tmp/r14a.out" || { echo "check-packages: FAIL — absent-manifest run error unclear:" >&2; sed 's/^/  /' "$tmp/r14a.out" >&2; exit 1; }
printf '{ "name": "t14", "version": "0.1.0", "kind": "executable", "modules": { ".": { "visibility": "internal" } } }\n' > "$t14/kama.json"
if ( cd "$t14" && "$KAMA" run kama.json ) >"$tmp/r14b.out" 2>&1; then
    echo "check-packages: FAIL — kama run with no source root was not rejected" >&2; exit 1; fi
grep -qF 'does not exist' "$tmp/r14b.out" || { echo "check-packages: FAIL — missing-source run error unclear:" >&2; sed 's/^/  /' "$tmp/r14b.out" >&2; exit 1; }
mkdir -p "$t14/src"; printf 'fn int32 main() { return 1; }\n' > "$t14/src/main.kama"
if ( cd "$t14" && "$KAMA" run kama.json ) >"$tmp/r14c.out" 2>&1; then
    echo "check-packages: FAIL — kama run with no \"entry\" was not rejected" >&2; exit 1; fi
grep -qF 'no "entry"' "$tmp/r14c.out" || { echo "check-packages: FAIL — no-entry run error unclear:" >&2; sed 's/^/  /' "$tmp/r14c.out" >&2; exit 1; }

# 14b. The pre-1.0 `main` spelling MOVED to tools/check-manifest.sh. It stopped being a `kama run` fact
#      when the manifest reader started rejecting the key by name — every command reports it now, so it
#      belongs with the rest of the schema rejections rather than here among the run-command cases.

# ---- M3.0: SemVer version ranges (git+version, no rev) -----------------------------------------------
# A single repo tagged across several versions; each tagged commit returns a version-distinguishable value
# from area() so the built program's EXIT CODE proves which tag the resolver selected. A pre-release
# (v1.3.0-rc1) and a non-SemVer alias (nightly) must be ignored — never selected.
gv="$tmp/gv-src"; mkdir -p "$gv/src"
printf '{ "name": "gv", "version": "0.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$gv/kama.json"
git -C "$gv" init -q
gvtag() {   # $1 = return value baked into area(); $2 = tag name
    printf 'export { area };\nfn int32 area() { return %s; }\n' "$1" > "$gv/src/gv.kama"
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
    d="$tmp/cr$3"; mkdir -p "$d/src"; CRDIR="$d"
    cat > "$d/kama.json" <<J
{ "name": "cr$3", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "gv": { "git": "file://$gv", "version": "$1" } }, "modules": { ".": { "visibility": "internal" } } }
J
    printf 'import { gv::area };\nfn int32 main() { return area(); }\n' > "$d/src/main.kama"
    if ! "$KAMA" pkg install "$d/kama.json" >"$tmp/cr$3.out" 2>&1; then
        echo "check-packages: FAIL — range '$1' install errored:" >&2; sed 's/^/  /' "$tmp/cr$3.out" >&2; exit 1; fi
    if "$KAMA" build "$d/kama.json" -o "$tmp/crapp$3" >"$tmp/crb$3.out" 2>&1; then run "$tmp/crapp$3"
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
midv="$tmp/midv"; mkdir -p "$midv/src"
cat > "$midv/kama.json" <<J
{ "name": "midv", "kind": "library", "dependencies": { "gv": { "git": "file://$gv", "version": "^1.0.0" } }, "modules": { ".": { "visibility": "public" } } }
J
printf 'import { gv::area };\nexport { mv };\nfn int32 mv() { return area(); }\n' > "$midv/src/midv.kama"
git -C "$midv" init -q; git -C "$midv" add -A; git -C "$midv" commit -qm init; git -C "$midv" tag v1.0.0
t16="$tmp/t16"; mkdir -p "$t16/src"
cat > "$t16/kama.json" <<J
{ "name": "t16", "kind": "executable", "entry": "src/main.kama", "dependencies": {
    "gv":   { "git": "file://$gv",   "version": ">=1.1.0" },
    "midv": { "git": "file://$midv", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { gv::area, midv::mv };\nfn int32 main() { return area() + mv()*0; }\n' > "$t16/src/main.kama"
if ! "$KAMA" pkg install "$t16/kama.json" >"$tmp/t16.out" 2>&1; then
    echo "check-packages: FAIL — intersection install errored:" >&2; sed 's/^/  /' "$tmp/t16.out" >&2; exit 1; fi
grep -q '"version": "1.2.0"' "$t16/kama.lock" \
    || { echo "check-packages: FAIL — range intersection did not resolve gv to 1.2.0:" >&2; sed 's/^/  /' "$t16/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t16/kama.json" -o "$tmp/a16" >"$tmp/b16.out" 2>&1; then run "$tmp/a16"
    [ "$RC" = 120 ] || { echo "check-packages: FAIL — intersection app returned $RC, expected 120" >&2; exit 1; }
else echo "check-packages: FAIL — intersection build failed:" >&2; sed 's/^/  /' "$tmp/b16.out" >&2; exit 1; fi

# 16b. intersection forcing a DOWNGRADE: root picks 1.2.0 for <=1.2.0, then midlo's <=1.1.0 tightens it ->
#      the resolver re-resolves to 1.1.0 (the restart-with-seeded-constraint path).
midlo="$tmp/midlo"; mkdir -p "$midlo/src"
cat > "$midlo/kama.json" <<J
{ "name": "midlo", "kind": "library", "dependencies": { "gv": { "git": "file://$gv", "version": "<=1.1.0" } }, "modules": { ".": { "visibility": "public" } } }
J
printf 'import { gv::area };\nexport { ml };\nfn int32 ml() { return area(); }\n' > "$midlo/src/midlo.kama"
git -C "$midlo" init -q; git -C "$midlo" add -A; git -C "$midlo" commit -qm init; git -C "$midlo" tag v1.0.0
t16b="$tmp/t16b"; mkdir -p "$t16b/src"
cat > "$t16b/kama.json" <<J
{ "name": "t16b", "kind": "executable", "entry": "src/main.kama", "dependencies": {
    "gv":    { "git": "file://$gv",    "version": "<=1.2.0" },
    "midlo": { "git": "file://$midlo", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { gv::area, midlo::ml };\nfn int32 main() { return area() + ml()*0; }\n' > "$t16b/src/main.kama"
if ! "$KAMA" pkg install "$t16b/kama.json" >"$tmp/t16b.out" 2>&1; then
    echo "check-packages: FAIL — downgrade install errored:" >&2; sed 's/^/  /' "$tmp/t16b.out" >&2; exit 1; fi
grep -q '"version": "1.1.0"' "$t16b/kama.lock" \
    || { echo "check-packages: FAIL — a tighter transitive range did not downgrade gv to 1.1.0:" >&2; sed 's/^/  /' "$t16b/kama.lock" >&2; exit 1; }
if "$KAMA" build "$t16b/kama.json" -o "$tmp/a16b" >"$tmp/b16b.out" 2>&1; then run "$tmp/a16b"
    [ "$RC" = 110 ] || { echo "check-packages: FAIL — downgrade app returned $RC, expected 110" >&2; exit 1; }
else echo "check-packages: FAIL — downgrade build failed:" >&2; sed 's/^/  /' "$tmp/b16b.out" >&2; exit 1; fi

# 17. DISJOINT ranges -> hard fail naming both requestors: midhi needs >=1.2.0, midlo2 needs <1.2.0.
for m in midhi:'>=1.2.0' midlo2:'<1.2.0'; do
    name=${m%%:*}; rng=${m#*:}; d="$tmp/$name"; mkdir -p "$d/src"
    cat > "$d/kama.json" <<J
{ "name": "$name", "kind": "executable", "entry": "src/main.kama", "dependencies": { "gv": { "git": "file://$gv", "version": "$rng" } }, "modules": { ".": { "visibility": "internal" } } }
J
    printf 'namespace %s;\nimport { gv::area };\nexport { v };\nfn int32 v() { return area(); }\n' "$name" > "$d/src/$name.kama"
    git -C "$d" init -q; git -C "$d" add -A; git -C "$d" commit -qm init; git -C "$d" tag v1.0.0
done
t17="$tmp/t17"; mkdir -p "$t17"
cat > "$t17/kama.json" <<J
{ "name": "t17", "kind": "executable", "entry": "src/main.kama", "dependencies": {
    "midhi":  { "git": "file://$tmp/midhi",  "rev": "v1.0.0" },
    "midlo2": { "git": "file://$tmp/midlo2", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
if "$KAMA" pkg install "$t17/kama.json" >"$tmp/e17" 2>&1; then
    echo "check-packages: FAIL — disjoint version ranges were not detected as a conflict" >&2; exit 1; fi
grep -qi "conflict" "$tmp/e17" || { echo "check-packages: FAIL — disjoint conflict not reported clearly:" >&2; sed 's/^/  /' "$tmp/e17" >&2; exit 1; }
grep -q '>=1.2.0' "$tmp/e17" && grep -q '<1.2.0' "$tmp/e17" \
    || { echo "check-packages: FAIL — disjoint conflict did not name both ranges:" >&2; sed 's/^/  /' "$tmp/e17" >&2; exit 1; }

# 18. offline byte-identical re-install: a range dep reuses the locked concrete version WITHOUT ls-remote.
off="$tmp/off"; mkdir -p "$off/src"
cat > "$off/kama.json" <<J
{ "name": "off", "kind": "executable", "entry": "src/main.kama", "dependencies": { "gv": { "git": "file://$gv", "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
J
printf 'import { gv::area };\nfn int32 main() { return area(); }\n' > "$off/src/main.kama"
if ! "$KAMA" pkg install "$off/kama.json" >"$tmp/off.out" 2>&1; then
    echo "check-packages: FAIL — range offline setup install errored:" >&2; sed 's/^/  /' "$tmp/off.out" >&2; exit 1; fi
cp "$off/kama.lock" "$tmp/off.lock"
mv "$gv" "$gv.hidden"                          # repo unreachable — a warm store + lock must resolve offline
if ! "$KAMA" pkg install "$off/kama.json" >"$tmp/offb.out" 2>&1 || ! cmp -s "$tmp/off.lock" "$off/kama.lock"; then
    echo "check-packages: FAIL — range offline re-install not reproducible (lock changed or ls-remote hit):" >&2
    sed 's/^/  /' "$tmp/offb.out" >&2; diff "$tmp/off.lock" "$off/kama.lock" >&2 || true; mv "$gv.hidden" "$gv"; exit 1; fi
mv "$gv.hidden" "$gv"

# ==== M3.1a: registry protocol + `kama publish` (network-free, a file:// dir registry) ================
# `kama publish` writes a static registry (<name>/index.json + <name>/<version>.tar.gz); a registry dep
# (a bare `version` range + a `registry` base) resolves the highest version FROM the index and reduces to
# a url dep for the fetch. Everything runs against a `file://` dir — nothing needs a live host.
# Native-spelled, unlike the git repos above, because a `file://` REGISTRY is a plain directory: kama
# strips the scheme and uses the rest as a path (registryDirFromArg), then hands that same path to tar.
# Left as `/tmp/…`, native kama resolved it against the current drive (`C:\tmp\…`) while msys2's tar read
# it as `C:\msys64\tmp\…`, so publish created the staging directory in one place and tar looked in another.
# The git URLs must stay POSIX for the opposite reason — msys2's git resolves `/tmp/…` itself.
reg="$(kama_native_path "$tmp")/reg"; mkdir -p "$reg"
# `kama publish` ships exactly the files git tracks, and refuses a directory that is not a git work tree or has
# uncommitted changes (KR-100) — so every package below is committed before it is published. Quiet, because
# `mkcf` returns its directory on stdout; `--allow-empty`, because a re-publish may change nothing.
commit_all() { git -C "$1" init -q && git -C "$1" add -A && git -C "$1" commit -qm pub --allow-empty >/dev/null; }

# publish two versions of `rg` (area() returns a version-distinguishing value) by dogfooding `kama publish`.
pub_rg() {   # pub_rg <version> <area-return>
    d="$tmp/rg-src-$1"; mkdir -p "$d/src"
    printf '{ "name": "rg", "version": "%s", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' "$1" > "$d/kama.json"
    printf 'export { area };\nfn int32 area() { return %s; }\n' "$2" > "$d/src/rg.kama"
    commit_all "$d"
    ( cd "$d" && "$KAMA" publish kama.json --registry "file://$reg" )
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
rc1="$tmp/rc1"; mkdir -p "$rc1/src"
cat > "$rc1/kama.json" <<JSON
{ "name": "rc1", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "rg": { "version": "^1.0.0", "registry": "file://$reg" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { rg::area };\nfn int32 main() { return area(); }\n' > "$rc1/src/main.kama"
if ! "$KAMA" pkg install "$rc1/kama.json" >"$tmp/rc1.out" 2>&1; then echo "check-packages: FAIL — registry install errored:" >&2; sed 's/^/  /' "$tmp/rc1.out" >&2; exit 1; fi
# a two-part range (`^1.0`) is refused, and the message names the three-part form — "an invalid version range"
# alone left the reader to guess what was wrong with `^1.0`.
rcr="$tmp/rc-range"; mkdir -p "$rcr/src"; cp "$rc1/src/main.kama" "$rcr/src/"
sed 's/"\^1\.0\.0"/"^1.0"/' "$rc1/kama.json" > "$rcr/kama.json"
if "$KAMA" pkg install "$rcr/kama.json" >"$tmp/rcr.out" 2>&1; then echo "check-packages: FAIL — a two-part range installed" >&2; exit 1; fi
grep -q "invalid version range \"\^1\.0\".*all three MAJOR.MINOR.PATCH parts" "$tmp/rcr.out" \
    || { echo "check-packages: FAIL — two-part range refusal does not name the three-part form:" >&2; sed 's/^/  /' "$tmp/rcr.out" >&2; exit 1; }
lock="$rc1/kama.lock"
grep -q '"source": "registry"' "$lock" && grep -q '"version": "1.2.0"' "$lock" && grep -q '"integrity": "sha256-' "$lock" \
    || { echo "check-packages: FAIL — registry lock missing source/version/integrity:" >&2; sed 's/^/  /' "$lock" >&2; exit 1; }
case "$(kama_native_path "$(readlink "$rc1/.kama/deps/rg" 2>/dev/null || echo /nonexistent)")" in
    "$KAMA_STORE"/rg-*) : ;;
    *) echo "check-packages: FAIL — .kama/deps/rg does not link into the store" >&2; exit 1 ;;
esac
if "$KAMA" build "$rc1/kama.json" -o "$tmp/rcapp" >"$tmp/rc1b.out" 2>&1; then
    if "$tmp/rcapp"; then rrc=0; else rrc=$?; rc=$rrc; fi
    [ "$rc" = 12 ] || { echo "check-packages: FAIL — registry consumer returned $rc, expected 12 (highest = 1.2.0)" >&2; exit 1; }
else echo "check-packages: FAIL — build of the registry consumer failed:" >&2; sed 's/^/  /' "$tmp/rc1b.out" >&2; exit 1; fi

# 20. transitive from a registry: `hi` (registry) depends on `rg` (registry); a consumer of `hi` resolves
# both from the fetched manifest (child deps read from the tarball, exactly like a url dep).
hi="$tmp/hi-src"; mkdir -p "$hi/src"
cat > "$hi/kama.json" <<JSON
{ "name": "hi", "version": "1.0.0", "kind": "library",
  "dependencies": { "rg": { "version": "^1.0.0", "registry": "file://$reg" } }, "modules": { ".": { "visibility": "public" } } }
JSON
printf 'import { rg::area };\nexport { total };\nfn int32 total() { return area() + 8; }\n' > "$hi/src/hi.kama"
commit_all "$hi"
if ! ( cd "$hi" && "$KAMA" publish kama.json --registry "file://$reg" ) >"$tmp/hipub.out" 2>&1; then echo "check-packages: FAIL — publish hi errored:" >&2; sed 's/^/  /' "$tmp/hipub.out" >&2; exit 1; fi
rc2="$tmp/rc2"; mkdir -p "$rc2/src"
cat > "$rc2/kama.json" <<JSON
{ "name": "rc2", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "hi": { "version": "^1.0.0", "registry": "file://$reg" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { hi::total };\nfn int32 main() { return total(); }\n' > "$rc2/src/main.kama"
if ! "$KAMA" pkg install "$rc2/kama.json" >"$tmp/rc2.out" 2>&1; then echo "check-packages: FAIL — transitive registry install errored:" >&2; sed 's/^/  /' "$tmp/rc2.out" >&2; exit 1; fi
grep -q '"hi"' "$rc2/kama.lock" && grep -q '"rg"' "$rc2/kama.lock" \
    || { echo "check-packages: FAIL — transitive registry install did not resolve both hi and rg:" >&2; sed 's/^/  /' "$rc2/kama.lock" >&2; exit 1; }
if "$KAMA" build "$rc2/kama.json" -o "$tmp/rc2app" >"$tmp/rc2b.out" 2>&1; then
    if "$tmp/rc2app"; then rc=0; else rc=$?; fi
    [ "$rc" = 20 ] || { echo "check-packages: FAIL — transitive consumer returned $rc, expected 20 (12 + 8)" >&2; exit 1; }
else echo "check-packages: FAIL — build of the transitive consumer failed:" >&2; sed 's/^/  /' "$tmp/rc2b.out" >&2; exit 1; fi

# 21. offline byte-identical re-install: remove the registry dir; a warm store + lock resolves with NO
# index fetch (the registry-dep analog of the git-range offline path).
cp "$rc2/kama.lock" "$tmp/rc2.lock"
mv "$reg" "$reg.hidden"                         # registry unreachable — the lock + warm store must suffice
if ! "$KAMA" pkg install "$rc2/kama.json" >"$tmp/rc2off.out" 2>&1 || ! cmp -s "$tmp/rc2.lock" "$rc2/kama.lock"; then
    echo "check-packages: FAIL — registry offline re-install not reproducible (lock changed or index hit):" >&2
    sed 's/^/  /' "$tmp/rc2off.out" >&2; diff "$tmp/rc2.lock" "$rc2/kama.lock" >&2 || true; mv "$reg.hidden" "$reg"; exit 1; fi
mv "$reg.hidden" "$reg"

# ==== M3.1b: scopes + `registries` config + dependency-confusion guard =================================
# A scoped `@acme/foo` imports under its BARE last segment (`foo`); the scope only selects which registry
# routes the fetch (via `registries` config). The lock pins content identity (the integrity), not the URI.

# 22. scoped routing + opt-out-of-default. Publish `@acme/sc` to a scope registry; a consumer routes
# `@acme` to it (and drops the default), imports it as `sc`, builds, and runs.
areg="$(kama_native_path "$tmp")/areg"; mkdir -p "$areg"
sc="$tmp/sc-src"; mkdir -p "$sc/src"
printf '{ "name": "@acme/sc", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$sc/kama.json"
printf 'export { val };\nfn int32 val() { return 7; }\n' > "$sc/src/sc.kama"
commit_all "$sc"
if ! ( cd "$sc" && "$KAMA" publish kama.json --registry "file://$areg" ) >"$tmp/scpub.out" 2>&1; then echo "check-packages: FAIL — publish @acme/sc errored:" >&2; sed 's/^/  /' "$tmp/scpub.out" >&2; exit 1; fi
[ -f "$areg/@acme/sc/index.json" ] || { echo "check-packages: FAIL — scoped publish path wrong (no @acme/sc/index.json)" >&2; find "$areg" >&2; exit 1; }
scp="$tmp/scp"; mkdir -p "$scp/src"
cat > "$scp/kama.json" <<JSON
{ "name": "scp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "registries": { "default": false, "@acme": "file://$areg" },
  "dependencies": { "@acme/sc": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { sc::val };\nfn int32 main() { return val(); }\n' > "$scp/src/main.kama"
if ! "$KAMA" pkg install "$scp/kama.json" >"$tmp/scp.out" 2>&1; then echo "check-packages: FAIL — scoped install errored:" >&2; sed 's/^/  /' "$tmp/scp.out" >&2; exit 1; fi
[ -L "$scp/.kama/deps/sc" ] || { echo "check-packages: FAIL — scoped dep did not import under its bare name (.kama/deps/sc)" >&2; ls "$scp/.kama/deps" >&2; exit 1; }
if "$KAMA" build "$scp/kama.json" -o "$tmp/scapp" >"$tmp/scb.out" 2>&1; then
    if "$tmp/scapp"; then rc=0; else rc=$?; fi
    [ "$rc" = 7 ] || { echo "check-packages: FAIL — scoped consumer returned $rc, expected 7" >&2; exit 1; }
else echo "check-packages: FAIL — build of the scoped consumer failed:" >&2; sed 's/^/  /' "$tmp/scb.out" >&2; exit 1; fi
# opt-out: an UNSCOPED name with `default:false` and no source is unresolvable (a clean hard error).
opo="$tmp/opo"; mkdir -p "$opo"
cat > "$opo/kama.json" <<JSON
{ "name": "opo", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "registries": { "default": false },
  "dependencies": { "sc": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
if "$KAMA" pkg install "$opo/kama.json" >"$tmp/opo.out" 2>&1; then echo "check-packages: FAIL — opt-out did not make an unscoped dep unresolvable" >&2; exit 1; fi
grep -qi "no registry configured" "$tmp/opo.out" || { echo "check-packages: FAIL — opt-out error message unclear:" >&2; sed 's/^/  /' "$tmp/opo.out" >&2; exit 1; }

# 23. re-pointable scope + the confusion guard. `cf` published with the SAME bytes to two registries and
# DIFFERENT bytes (same version) to a third. Re-pointing to the same-bytes mirror re-resolves with an
# unchanged integrity; re-pointing to the different-bytes mirror is a hard error.
# Native-spelled: these three are REGISTRIES (see the note at `reg=` above), not git repos.
ntmp=$(kama_native_path "$tmp")
ra="$ntmp/cf-a"; rb="$ntmp/cf-b"; rc_="$ntmp/cf-c"; mkdir -p "$ra" "$rb" "$rc_"
mkcf() { d="$tmp/cf-src-$1"; mkdir -p "$d/src"; printf '{ "name": "cf", "version": "1.0.0", "kind": "library" }\n' > "$d/kama.json"; printf 'export { val };\nfn int32 val() { return %s; }\n' "$2" > "$d/src/cf.kama"; commit_all "$d"; echo "$d"; }
csame=$(mkcf same 3); cdiff=$(mkcf diff 4)
( cd "$csame" && "$KAMA" publish kama.json --registry "file://$ra" ) >/dev/null 2>&1
( cd "$csame" && "$KAMA" publish kama.json --registry "file://$rb" ) >/dev/null 2>&1
( cd "$cdiff" && "$KAMA" publish kama.json --registry "file://$rc_" ) >/dev/null 2>&1
cfp="$tmp/cfp"; mkdir -p "$cfp"
cfjson() { cat > "$cfp/kama.json" <<JSON
{ "name": "cfp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "registries": { "default": "file://$1" },
  "dependencies": { "cf": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
}
cfjson "$ra"; "$KAMA" pkg install "$cfp/kama.json" >/dev/null 2>&1
int_a=$(grep -o 'sha256-[0-9a-f]*' "$cfp/kama.lock" | head -1)
cfjson "$rb"
if ! "$KAMA" pkg install "$cfp/kama.json" >"$tmp/cfb.out" 2>&1; then echo "check-packages: FAIL — re-point to same-bytes mirror errored:" >&2; sed 's/^/  /' "$tmp/cfb.out" >&2; exit 1; fi
int_b=$(grep -o 'sha256-[0-9a-f]*' "$cfp/kama.lock" | head -1)
[ "$int_a" = "$int_b" ] || { echo "check-packages: FAIL — re-point to same bytes changed the integrity ($int_a -> $int_b)" >&2; exit 1; }
cfjson "$rc_"
if "$KAMA" pkg install "$cfp/kama.json" >"$tmp/cfc.out" 2>&1; then echo "check-packages: FAIL — confusion guard let a different-bytes mirror install" >&2; exit 1; fi
grep -qi "confusion" "$tmp/cfc.out" || { echo "check-packages: FAIL — confusion-guard message unclear:" >&2; sed 's/^/  /' "$tmp/cfc.out" >&2; exit 1; }

# 24. import-name collision: two DIFFERENT scopes exposing the same bare name -> a hard error (alias one).
creg="$(kama_native_path "$tmp")/creg"; mkdir -p "$creg"
for scp2 in acme other; do d="$tmp/col-$scp2"; mkdir -p "$d/src"; printf '{ "name": "@%s/cn", "version": "1.0.0", "kind": "library" }\n' "$scp2" > "$d/kama.json"; printf 'export { val };\nfn int32 val() { return 1; }\n' > "$d/src/cn.kama"; commit_all "$d"; ( cd "$d" && "$KAMA" publish kama.json --registry "file://$creg" ) >/dev/null 2>&1; done
colp="$tmp/colp"; mkdir -p "$colp"
cat > "$colp/kama.json" <<JSON
{ "name": "colp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "registries": { "default": false, "@acme": "file://$creg", "@other": "file://$creg" },
  "dependencies": { "@acme/cn": { "version": "^1.0.0" }, "@other/cn": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
if "$KAMA" pkg install "$colp/kama.json" >"$tmp/colp.out" 2>&1; then echo "check-packages: FAIL — import-name collision was not rejected" >&2; exit 1; fi
grep -qi "collision" "$tmp/colp.out" || { echo "check-packages: FAIL — collision message unclear:" >&2; sed 's/^/  /' "$tmp/colp.out" >&2; exit 1; }

# ==== KR-100: publish ships exactly the files git tracks =================================================
# Publish used to tar the DIRECTORY, so a gitignored `.env` landed in a tarball the registry keeps forever — and
# on macOS every file gained a hidden AppleDouble `._` twin. Members are read with Python's tarfile, NEVER
# `tar -t`: macOS's tar hides the `._` entries it writes itself, which is exactly how they went unseen. Without
# python3 the exit-code cases still run and the member-set assertions are skipped.
kreg="$(kama_native_path "$tmp")/kreg"; mkdir -p "$kreg"
kfail() { echo "check-packages: FAIL — $1" >&2; if [ -n "${2:-}" ]; then sed 's/^/  /' "$2" >&2; fi; exit 1; }
kpub() { kd=$1; shift; ( cd "$kd" && "$KAMA" publish kama.json "$@" ); }
kpkg() {   # kpkg <dir> <name> <version>: a one-file library, not yet committed
    mkdir -p "$1/src"
    printf '{ "name": "%s", "version": "%s", "kind": "library" }\n' "$2" "$3" > "$1/kama.json"
    printf 'export { v };\nfn int32 v() { return 1; }\n' > "$1/src/$2.kama"
}
# kmembers <tgz> <expected, one per line>: the tarball's FILES, wrapper directory peeled, must be exactly these.
# Bytes, not print(): a native Windows python — mingw's, which UCRT64's PATH puts ahead of msys2's own — writes
# each "\n" as "\r\n", and the list then differs from the expected one by nothing anyone can see.
if command -v python3 >/dev/null 2>&1; then KPY=1; KRNOTE="exact member sets via tarfile"; else KPY=; KRNOTE="member sets skipped (no python3)"; fi
kmembers() {
    [ -n "$KPY" ] || return 0
    got=$(python3 -c 'import sys,tarfile; sys.stdout.buffer.write("\n".join(sorted(m.name.split("/",1)[1] for m in tarfile.open(sys.argv[1]) if not m.isdir())).encode())' "$1")
    [ "$got" = "$2" ] || { printf 'check-packages: FAIL — %s holds the wrong files\n  expected:\n%s\n  got:\n%s\n' "$1" "$2" "$got" >&2; exit 1; }
}

# 26a. outside a git work tree: refused, and nothing reaches the registry. The ceiling keeps an enclosing
# repository (a TMPDIR inside one) from turning this into a pass.
ng="$tmp/kr-nogit"; kpkg "$ng" ng 1.0.0
if GIT_CEILING_DIRECTORIES="$tmp" kpub "$ng" --registry "file://$kreg" >"$tmp/ng.out" 2>&1; then kfail "publish outside git was allowed" "$tmp/ng.out"; fi
grep -q "not in a git work tree" "$tmp/ng.out" || kfail "outside-git refusal message unclear" "$tmp/ng.out"
[ ! -e "$kreg/ng" ] || kfail "a refused publish still wrote into the registry"

# 26b. the leak as measured: a gitignored `.env` and an untracked (not ignored) file are NOT shipped, and the
# untracked file does not count as a dirty tree either. The index names the commit the tarball is.
lk="$tmp/kr-leak"; kpkg "$lk" lk 1.0.0
printf '.env\n.env.local\n' > "$lk/.gitignore"
echo 'API_TOKEN=supersecret' > "$lk/.env"; echo 'X=1' > "$lk/.env.local"
commit_all "$lk"
echo notes > "$lk/notes.txt"
kpub "$lk" --registry "file://$kreg" >"$tmp/lk.out" 2>&1 || kfail "publish of a clean git package errored" "$tmp/lk.out"
kmembers "$kreg/lk/1.0.0.tar.gz" ".gitignore
kama.json
src/lk.kama"
if gzip -dc "$kreg/lk/1.0.0.tar.gz" | grep -q supersecret; then kfail "the gitignored .env is in the tarball"; fi
grep -q "\"revision\": \"git:$(git -C "$lk" rev-parse HEAD)\"" "$kreg/lk/index.json" \
    || kfail "the index entry does not record the published commit" "$kreg/lk/index.json"

# 26c. a dirty tree is refused — a modified tracked file, and a staged-but-uncommitted one — and naming the
# file. The version is bumped so immutability cannot be the reason for the refusal.
sed 's/1\.0\.0/1.0.1/' "$lk/kama.json" > "$tmp/kj" && mv "$tmp/kj" "$lk/kama.json"
echo '// edit' >> "$lk/src/lk.kama"
if kpub "$lk" --registry "file://$kreg" >"$tmp/dirty.out" 2>&1; then kfail "publish of a modified tracked file was allowed" "$tmp/dirty.out"; fi
grep -q "uncommitted changes" "$tmp/dirty.out" && grep -q "src/lk.kama" "$tmp/dirty.out" || kfail "dirty-tree refusal message unclear" "$tmp/dirty.out"
git -C "$lk" add -A
if kpub "$lk" --registry "file://$kreg" >"$tmp/staged.out" 2>&1; then kfail "publish of a staged, uncommitted change was allowed" "$tmp/staged.out"; fi
grep -q "uncommitted changes" "$tmp/staged.out" || kfail "staged-change refusal message unclear" "$tmp/staged.out"
git -C "$lk" commit -qm edit
kpub "$lk" --registry "file://$kreg" >"$tmp/lk2.out" 2>&1 || kfail "publish after committing errored" "$tmp/lk2.out"

# 26d/e. a package in a SUBDIRECTORY of a larger repository. Before it is added its manifest is untracked:
# refused. Once committed it ships only its own files, rooted at itself — and not the files it tracks that no
# package ships (a root `kama.lock`, a build-output tree).
mono="$tmp/kr-mono"; mkdir -p "$mono/other"; echo top > "$mono/top.txt"; echo x > "$mono/other/x.kama"
git -C "$mono" init -q; git -C "$mono" add -A; git -C "$mono" commit -qm top
kpkg "$mono/pkgs/p" p 1.0.0
if kpub "$mono/pkgs/p" --registry "file://$kreg" >"$tmp/mono1.out" 2>&1; then kfail "publish with an untracked kama.json was allowed" "$tmp/mono1.out"; fi
grep -q "is not tracked by git" "$tmp/mono1.out" || kfail "untracked-manifest refusal message unclear" "$tmp/mono1.out"
mkdir -p "$mono/pkgs/p/out"; echo junk > "$mono/pkgs/p/out/junk.txt"; echo '{}' > "$mono/pkgs/p/kama.lock"
git -C "$mono" add -A; git -C "$mono" commit -qm p
kpub "$mono/pkgs/p" --registry "file://$kreg" >"$tmp/mono2.out" 2>&1 || kfail "publish of a subdirectory package errored" "$tmp/mono2.out"
kmembers "$kreg/p/1.0.0.tar.gz" "kama.json
src/p.kama"

# 26f. submodules: a checked-out one ships its tracked files (and not an untracked stray inside it). One that is
# not checked out is refused: `status` cannot see it, and it would ship as an empty directory — the package
# missing the files its commit names. `protocol.file.allow`: git refuses a local-path submodule by default.
subr="$tmp/kr-subrepo"; mkdir -p "$subr"; echo 'int vendored;' > "$subr/lib.c"; commit_all "$subr"
sp="$tmp/kr-sp"; kpkg "$sp" sp 1.0.0
git -C "$sp" init -q
git -C "$sp" -c protocol.file.allow=always submodule add -q "$subr" vendor/sub >"$tmp/spadd.out" 2>&1 || kfail "could not add a submodule" "$tmp/spadd.out"
git -C "$sp" add -A; git -C "$sp" commit -qm sp
echo stray > "$sp/vendor/sub/stray.txt"
kpub "$sp" --registry "file://$kreg" >"$tmp/sp.out" 2>&1 || kfail "publish with a checked-out submodule errored" "$tmp/sp.out"
kmembers "$kreg/sp/1.0.0.tar.gz" ".gitmodules
kama.json
src/sp.kama
vendor/sub/lib.c"
sp2="$tmp/kr-sp2"; git clone -q "$sp" "$sp2"   # no --recurse-submodules: vendor/sub is not checked out
sed 's/1\.0\.0/1.0.1/' "$sp2/kama.json" > "$tmp/kj" && mv "$tmp/kj" "$sp2/kama.json"; git -C "$sp2" commit -qam v
if kpub "$sp2" --registry "file://$kreg" >"$tmp/sp2.out" 2>&1; then kfail "publish with a submodule not checked out was allowed" "$tmp/sp2.out"; fi
grep -q "is not checked out" "$tmp/sp2.out" || kfail "submodule refusal message unclear" "$tmp/sp2.out"

# 26g. the COMMITTED bytes ship, not the checkout's: a clone with core.autocrlf=true has CRLF on disk (Git for
# Windows installs that way) and a clean status, and publishes the byte-identical tarball the LF checkout
# does. The `\r` count proves the clone really converted — otherwise this case would pass vacuously.
eo="$tmp/kr-eol"; kpkg "$eo" eo 1.0.0; commit_all "$eo"
eo2="$tmp/kr-eol-crlf"; git clone -q -c core.autocrlf=true "$eo" "$eo2"
[ "$(tr -cd '\r' < "$eo2/src/eo.kama" | wc -c | tr -d ' ')" -gt 0 ] || kfail "the autocrlf clone has no CRLF on disk — this case proves nothing"
kreg2="$(kama_native_path "$tmp")/kreg2"; mkdir -p "$kreg2"
kpub "$eo" --registry "file://$kreg" >"$tmp/eo.out" 2>&1 || kfail "publish of the LF checkout errored" "$tmp/eo.out"
kpub "$eo2" --registry "file://$kreg2" >"$tmp/eo2.out" 2>&1 || kfail "publish of the CRLF checkout errored" "$tmp/eo2.out"
cmp -s "$kreg/eo/1.0.0.tar.gz" "$kreg2/eo/1.0.0.tar.gz" || kfail "a CRLF checkout of the same commit published different bytes"

# 26h. a file stored through a git filter (Git LFS) is refused: its committed bytes are a pointer, not the
# file. The attribute alone decides — git-lfs need not be installed for the shape to be caught.
lf="$tmp/kr-lfs"; kpkg "$lf" lf 1.0.0
printf '*.bin filter=lfs diff=lfs merge=lfs -text\n' > "$lf/.gitattributes"; printf 'bytes' > "$lf/asset.bin"
commit_all "$lf"
if kpub "$lf" --registry "file://$kreg" >"$tmp/lf.out" 2>&1; then kfail "a file under the lfs filter was published" "$tmp/lf.out"; fi
grep -q "asset.bin. is stored through the git filter .lfs." "$tmp/lf.out" || kfail "filter refusal message unclear" "$tmp/lf.out"

# 26i. ONE commit, ONE sha256, on every machine and every leg. The archive is written by kama itself
# (src/kama.archive.cpp) — owner, time, modes, order and the deflate stream fixed — so this integrity is a
# constant. A change here means every rebuild of an already-published version stops matching its index
# entry: change the writer only on purpose, and then this number with it. Both the chmod and `--chmod=+x`:
# the index carries the mode where core.fileMode=false (Windows) ignores the disk, and the disk has to
# agree where it does not, or the tree is dirty.
gd="$tmp/kr-golden"; kpkg "$gd" golden 1.0.0
mkdir -p "$gd/tools" "$gd/docs/a_directory_name_long_enough_to_need/a_pax_path_record_in_the_tar"
printf '#!/bin/sh\necho golden\n' > "$gd/tools/run.sh"; chmod +x "$gd/tools/run.sh"
printf '# notes\n' > "$gd/docs/a_directory_name_long_enough_to_need/a_pax_path_record_in_the_tar/notes.md"
git -C "$gd" init -q; git -C "$gd" add -A; git -C "$gd" add --chmod=+x tools/run.sh; git -C "$gd" commit -qm golden
kpub "$gd" --registry "file://$kreg" >"$tmp/gd.out" 2>&1 || kfail "publish of the golden package errored" "$tmp/gd.out"
KR_GOLDEN=sha256-c810092dce694803902974ee8b297fc27e45f3a2259e7fea8fa7a2cdbc3122d4
grep -q "\"integrity\": \"$KR_GOLDEN\"" "$kreg/golden/index.json" \
    || kfail "the golden package's integrity moved — the archive writer's bytes changed (expected $KR_GOLDEN)" "$kreg/golden/index.json"

# 26j. a tracked symlink ships as a symlink, its target the bytes git recorded. POSIX only: msys2's `ln -s`
# copies, so git there would record a plain file.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) : ;; *)
    sl="$tmp/kr-link"; kpkg "$sl" sl 1.0.0; ln -s src/sl.kama "$sl/alias.kama"; commit_all "$sl"
    kpub "$sl" --registry "file://$kreg" >"$tmp/sl.out" 2>&1 || kfail "publish with a tracked symlink errored" "$tmp/sl.out"
    if [ -n "$KPY" ]; then
        python3 -c 'import sys,tarfile; m=tarfile.open(sys.argv[1]).getmember("sl/alias.kama"); sys.exit(0 if m.issym() and m.linkname=="src/sl.kama" else 1)' "$kreg/sl/1.0.0.tar.gz" \
            || kfail "the tracked symlink did not ship as a symlink to src/sl.kama"
    fi ;;
esac

# 26k. the secret backstop: a secret-shaped file git TRACKS is refused, every one named, with the advice to rotate
# it. The list is the registry's own (tools/check.py in cosmic-canopy/kama-registry). A template ships, and
# doing what the message says — `git rm --cached` — is enough to publish.
sk="$tmp/kr-secret"; kpkg "$sk" sk 1.0.0; mkdir -p "$sk/config" "$sk/certs"
for f in .env config/.env.production certs/server.pem id_ed25519; do echo 'SECRET=x' > "$sk/$f"; done
echo 'SECRET=' > "$sk/.env.example"
commit_all "$sk"
if kpub "$sk" --registry "file://$kreg" >"$tmp/sk.out" 2>&1; then kfail "tracked secret-shaped files were published" "$tmp/sk.out"; fi
for f in .env config/.env.production certs/server.pem id_ed25519; do
    grep -qx "    $f" "$tmp/sk.out" || kfail "the secret refusal does not name $f" "$tmp/sk.out"
done
grep -q "rotate" "$tmp/sk.out" || kfail "the secret refusal does not say to rotate" "$tmp/sk.out"
if grep -q "env.example" "$tmp/sk.out"; then kfail "the template .env.example was taken for a secret" "$tmp/sk.out"; fi
git -C "$sk" rm -q --cached .env config/.env.production certs/server.pem id_ed25519; git -C "$sk" commit -qm untrack
kpub "$sk" --registry "file://$kreg" >"$tmp/sk2.out" 2>&1 || kfail "publish after untracking the secrets errored" "$tmp/sk2.out"
kmembers "$kreg/sk/1.0.0.tar.gz" ".env.example
kama.json
src/sk.kama"

# 26l. publish.exclude narrows the tracked set. An entry naming nothing is refused (with the trailing-`/` hint
# when it named a directory), and so is naming kama.json. The backstop judges what SHIPS: with the fixture key
# still in, it refuses; with it excluded, the package publishes — only its sources.
ex="$tmp/kr-exclude"; kpkg "$ex" ex 1.0.0; mkdir -p "$ex/tools" "$ex/tests/tls"
echo gen > "$ex/tools/gen.sh"; echo notes > "$ex/NOTES.md"; echo fixture > "$ex/tests/tls/server.key"
exjson() {
    printf '{ "name": "ex", "version": "1.0.0", "kind": "library", "kama": ">=0.9.453", "publish": { "exclude": [%s] } }\n' "$1" > "$ex/kama.json"
    commit_all "$ex"
    if kpub "$ex" --registry "file://$kreg" >"$tmp/ex.out" 2>&1; then return 0; else return 1; fi
}
if exjson '"tool/"'; then kfail "an exclude entry naming nothing was accepted" "$tmp/ex.out"; fi
grep -q 'entry "tool/" matches no tracked file' "$tmp/ex.out" || kfail "no-match refusal unclear" "$tmp/ex.out"
if exjson '"tools"'; then kfail "a directory entry without its trailing / was accepted" "$tmp/ex.out"; fi
grep -q 'a directory needs a trailing `/`: "tools/"' "$tmp/ex.out" || kfail "directory hint missing" "$tmp/ex.out"
if exjson '"kama.json"'; then kfail "excluding kama.json was accepted" "$tmp/ex.out"; fi
grep -q "names kama.json" "$tmp/ex.out" || kfail "kama.json refusal unclear" "$tmp/ex.out"
if exjson '"tools/", "NOTES.md"'; then kfail "the unexcluded fixture key shipped" "$tmp/ex.out"; fi
grep -qx "    tests/tls/server.key" "$tmp/ex.out" || kfail "the backstop did not name the fixture key" "$tmp/ex.out"
exjson '"tools/", "NOTES.md", "tests/tls/"' || kfail "publish with every extra excluded errored" "$tmp/ex.out"
kmembers "$kreg/ex/1.0.0.tar.gz" "kama.json
src/ex.kama"

# 26m. --dry-run: every refusal and the archive, nothing written. It needs no --registry; given one, the registry
# is byte-for-byte unchanged, and an already-published version is refused just as a real publish would. The
# list is the shipped files, and the integrity it prints is the one the real publish then records.
dr="$tmp/kr-dry"; kpkg "$dr" dr 1.0.0; commit_all "$dr"
kpub "$dr" --dry-run >"$tmp/dr.out" 2>"$tmp/dr.err" || kfail "--dry-run without a registry errored" "$tmp/dr.err"
[ "$(cat "$tmp/dr.out")" = "kama.json
src/dr.kama" ] || kfail "--dry-run did not list exactly the files that ship" "$tmp/dr.out"
dint=$(grep -o 'sha256-[0-9a-f]*' "$tmp/dr.err") || kfail "--dry-run printed no integrity" "$tmp/dr.err"
kregsum() { ( cd "$kreg" && find . -type f -exec cksum {} + | sort ); }
before=$(kregsum)
kpub "$dr" --dry-run --registry "file://$kreg" >/dev/null 2>"$tmp/dr2.err" || kfail "--dry-run with a registry errored" "$tmp/dr2.err"
[ "$(kregsum)" = "$before" ] || kfail "--dry-run wrote into the registry"
kpub "$dr" --registry "file://$kreg" >/dev/null 2>&1 || kfail "the real publish after --dry-run errored"
grep -q "\"integrity\": \"$dint\"" "$kreg/dr/index.json" || kfail "the real publish recorded a different integrity than --dry-run printed ($dint)" "$kreg/dr/index.json"
if kpub "$dr" --dry-run --registry "file://$kreg" >/dev/null 2>"$tmp/dr3.err"; then kfail "--dry-run passed a version that is already published"; fi
grep -qi "immutable" "$tmp/dr3.err" || kfail "--dry-run immutability refusal unclear" "$tmp/dr3.err"
echo '// edit' >> "$dr/src/dr.kama"
if kpub "$dr" --dry-run >/dev/null 2>"$tmp/dr4.err"; then kfail "--dry-run hid the dirty-tree refusal"; fi
grep -q "uncommitted changes" "$tmp/dr4.err" || kfail "--dry-run dirty refusal unclear" "$tmp/dr4.err"

# 26m2. a `path` dependency is refused at publish. The resolver refuses one below a fetched package, so a
# version published with it could never be installed — and versions are permanent. Refused on --dry-run too, and
# named with the fix. A `path` DEV-dependency ships: nobody follows a fetched package's dev-dependencies.
pd="$tmp/kr-pathdep"; kpkg "$pd" pd 1.0.0
printf '{ "name": "pd", "version": "1.0.0", "kind": "library", "dependencies": { "depa": { "path": "../dep" } } }\n' > "$pd/kama.json"
commit_all "$pd"
if kpub "$pd" --registry "file://$kreg" >/dev/null 2>"$tmp/pd.err"; then kfail "a path dependency was published"; fi
grep -q "dependency 'depa' is a \`path\` dependency" "$tmp/pd.err" || kfail "path-dependency refusal unclear" "$tmp/pd.err"
grep -q "overrides" "$tmp/pd.err" || kfail "path-dependency refusal does not name the fix" "$tmp/pd.err"
[ ! -e "$kreg/pd" ] || kfail "a refused publish wrote into the registry"
if kpub "$pd" --dry-run >/dev/null 2>"$tmp/pd2.err"; then kfail "--dry-run passed a path dependency"; fi
printf '{ "name": "pd", "version": "1.0.0", "kind": "library", "dev-dependencies": { "depa": { "path": "../dep" } } }\n' > "$pd/kama.json"
git -C "$pd" commit -qam dev >/dev/null
kpub "$pd" --registry "file://$kreg" >/dev/null 2>"$tmp/pd3.err" || kfail "a path DEV-dependency was refused" "$tmp/pd3.err"

# 26n. the committed bytes ship EXACTLY, a CR before an LF and a Ctrl-Z (0x1A) included. Those are the two a
# text-mode read changes — Windows' CRT folds CRLF to LF and stops at 0x1A — so this, not the golden above (LF
# text only), holds publish's binary read of `git cat-file --batch`. `-text` keeps any git config from
# converting the file, and the blob is checked for the bytes, or this would prove nothing. The member is read
# with `tar -O` through the POSIX spelling of the path: GNU tar takes the `C:` of a native one for a host.
cz="$tmp/kr-cz"; kpkg "$cz" cz 1.0.0
printf 'raw.bin -text\n' > "$cz/.gitattributes"; printf 'a\r\nb\032c\r\n' > "$cz/raw.bin"; commit_all "$cz"
[ "$(git -C "$cz" cat-file blob HEAD:raw.bin | tr -cd '\r\032' | wc -c | tr -d ' ')" -eq 3 ] \
    || kfail "the committed raw.bin holds no CR/Ctrl-Z bytes — this case proves nothing"
kpub "$cz" --registry "file://$kreg" >"$tmp/cz.out" 2>&1 || kfail "publish of committed CR and Ctrl-Z bytes errored" "$tmp/cz.out"
tar -xzOf "$tmp/kreg/cz/1.0.0.tar.gz" cz/raw.bin > "$tmp/cz.got" || kfail "raw.bin could not be read back out of the tarball"
git -C "$cz" cat-file blob HEAD:raw.bin | cmp -s - "$tmp/cz.got" \
    || kfail "the published raw.bin is not the committed bytes — a CR or the Ctrl-Z did not survive the read"

# ==== M3.2a: sign-on-publish / verify-on-install (SSHSIG via ssh-keygen -Y) ============================
# `kama publish --key` signs the tarball; the index carries the signature + signer key. `--verify` on
# install enforces (a present signature must verify; a missing one is an error); the default is warn-only.
if command -v ssh-keygen >/dev/null 2>&1; then
    sreg="$(kama_native_path "$tmp")/sreg"; mkdir -p "$sreg"
    ssh-keygen -t ed25519 -f "$tmp/pubkey" -N "" -q
    sg="$tmp/sg-src"; mkdir -p "$sg/src"
    printf '{ "name": "sg", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$sg/kama.json"
    printf 'export { val };\nfn int32 val() { return 5; }\n' > "$sg/src/sg.kama"
    commit_all "$sg"
    if ! ( cd "$sg" && "$KAMA" publish kama.json --registry "file://$sreg" --key "$tmp/pubkey" ) >"$tmp/sgpub.out" 2>&1; then
        echo "check-packages: FAIL — signed publish errored:" >&2; sed 's/^/  /' "$tmp/sgpub.out" >&2; exit 1; fi
    grep -q '"signature"' "$sreg/sg/index.json" && grep -q '"key"' "$sreg/sg/index.json" \
        || { echo "check-packages: FAIL — signed publish did not record signature+key in the index" >&2; exit 1; }
    no_tmp_left "$sreg" "a signed publish"
    sgp="$tmp/sgp"; mkdir -p "$sgp/src"
    cat > "$sgp/kama.json" <<JSON
{ "name": "sgp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "sg": { "version": "^1.0.0", "registry": "file://$sreg" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
    printf 'import { sg::val };\nfn int32 main() { return val(); }\n' > "$sgp/src/main.kama"
    # 25a. --verify install of a signed package passes.
    if ! "$KAMA" pkg install "$sgp/kama.json" --verify >"$tmp/sv.out" 2>&1; then
        echo "check-packages: FAIL — --verify install of a signed package errored:" >&2; sed 's/^/  /' "$tmp/sv.out" >&2; exit 1; fi
    no_tmp_left "$KAMA_STORE" "a verified registry install"
    # 25b. tamper the signature blob in the index → --verify FAILS (cold store forces a re-fetch+re-verify).
    rm -rf "$KAMA_STORE" "$sgp/kama.lock" "$sgp/.kama"
    awk '{ if (!done && index($0,"BEGIN SSH SIGNATURE")>0) { done=1 } print }' "$sreg/sg/index.json" >/dev/null
    # flip a character inside the armored signature body (the line after the BEGIN marker)
    sed 's/\(BEGIN SSH SIGNATURE-----\\n\)./\1Z/' "$sreg/sg/index.json" > "$tmp/idx.bad" && mv "$tmp/idx.bad" "$sreg/sg/index.json"
    if "$KAMA" pkg install "$sgp/kama.json" --verify >"$tmp/svbad.out" 2>&1; then
        echo "check-packages: FAIL — --verify accepted a tampered signature:" >&2; sed 's/^/  /' "$tmp/svbad.out" >&2; exit 1; fi
    grep -qi "signature verification failed" "$tmp/svbad.out" || { echo "check-packages: FAIL — tampered-signature message unclear:" >&2; sed 's/^/  /' "$tmp/svbad.out" >&2; exit 1; }
    # 25c. the same tampered signature WITHOUT --verify is warn-only: install succeeds, a warning is printed.
    rm -rf "$KAMA_STORE" "$sgp/kama.lock" "$sgp/.kama"
    if ! "$KAMA" pkg install "$sgp/kama.json" >"$tmp/svwarn.out" 2>&1; then
        echo "check-packages: FAIL — warn-only install (bad sig, no --verify) errored:" >&2; sed 's/^/  /' "$tmp/svwarn.out" >&2; exit 1; fi
    grep -qi "signature check failed" "$tmp/svwarn.out" || { echo "check-packages: FAIL — warn-only did not warn on a bad signature:" >&2; sed 's/^/  /' "$tmp/svwarn.out" >&2; exit 1; }
    SIGNOTE="signed publish/verify/tamper/warn-only, no .sig left behind"
else
    echo "check-packages: NOTE — ssh-keygen absent, skipping M3.2a signing cases"
    SIGNOTE="signing skipped (no ssh-keygen)"
fi

# ---- M5.3: kama.local.json local overrides -----------------------------------------------------------
# 26. dep path-override (patch-style): a git dep is redirected to a LOCAL dir for this dev only. The
#     committed kama.lock stays BYTE-IDENTICAL (the override is never locked → CI-safe), the view relinks
#     to the local dir, and the build compiles the local code (77) rather than the published one (30).
ogeo="$tmp/ogeo-src"; mkdir -p "$ogeo/src"
printf '{ "name": "ogeo", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$ogeo/kama.json"
printf 'export { area };\nfn int32 area() { return 30; }\n' > "$ogeo/src/ogeo.kama"
git -C "$ogeo" init -q
git -C "$ogeo" -c user.email=t@t -c user.name=t add -A
git -C "$ogeo" -c user.email=t@t -c user.name=t commit -qm init
git -C "$ogeo" tag v1.0.0
ovp="$tmp/ovp"; mkdir -p "$ovp/src"
cat > "$ovp/kama.json" <<JSON
{ "name": "ovc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "ogeo": { "git": "file://$ogeo", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { ogeo::area };\nfn int32 main() { return area(); }\n' > "$ovp/src/main.kama"
if ! "$KAMA" pkg install "$ovp/kama.json" >"$tmp/ov1.out" 2>&1; then
    echo "check-packages: FAIL — override base install errored:" >&2; sed 's/^/  /' "$tmp/ov1.out" >&2; exit 1; fi
cp "$ovp/kama.lock" "$tmp/ov.lock.canon"
oloc="$tmp/ogeo-local"; mkdir -p "$oloc/src"
printf '{ "name": "ogeo", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$oloc/kama.json"
printf 'export { area };\nfn int32 area() { return 77; }\n' > "$oloc/src/ogeo.kama"
cat > "$ovp/kama.local.json" <<JSON
{ "overrides": { "ogeo": { "path": "../ogeo-local" } } }
JSON
if ! "$KAMA" pkg install "$ovp/kama.json" >"$tmp/ov2.out" 2>&1; then
    echo "check-packages: FAIL — override install errored:" >&2; sed 's/^/  /' "$tmp/ov2.out" >&2; exit 1; fi
if ! cmp -s "$tmp/ov.lock.canon" "$ovp/kama.lock"; then
    echo "check-packages: FAIL — a dep override perturbed kama.lock (must stay canonical):" >&2
    diff "$tmp/ov.lock.canon" "$ovp/kama.lock" >&2 || true; exit 1; fi
case "$(readlink "$ovp/.kama/deps/ogeo" 2>/dev/null || true)" in
    *ogeo-local) : ;;
    *) echo "check-packages: FAIL — override did not relink the view to the local dir" >&2; exit 1 ;;
esac
if "$KAMA" build "$ovp/kama.json" -o "$tmp/ovapp" >"$tmp/ovb.out" 2>&1; then
    if "$tmp/ovapp"; then orc=0; else orc=$?; fi
    [ "$orc" = 77 ] || { echo "check-packages: FAIL — override app returned $orc, expected 77 (local code)" >&2; exit 1; }
else echo "check-packages: FAIL — build against the override failed:" >&2; sed 's/^/  /' "$tmp/ovb.out" >&2; exit 1; fi
# override of a non-dependency → hard error
cat > "$ovp/kama.local.json" <<JSON
{ "overrides": { "nope": { "path": "../ogeo-local" } } }
JSON
if "$KAMA" pkg install "$ovp/kama.json" >"$tmp/ovbad.out" 2>&1; then
    echo "check-packages: FAIL — override of a non-dependency was accepted" >&2; exit 1; fi
grep -qi "not a dependency" "$tmp/ovbad.out" \
    || { echo "check-packages: FAIL — non-dependency override message unclear:" >&2; sed 's/^/  /' "$tmp/ovbad.out" >&2; exit 1; }

# 27. registries local override: kama.json drops every default (`"default": false`, the built-in official
#     registry included), so the registry dep does not resolve; a kama.local.json `registries.default`
#     supplies the base and it does — proving the local override is consulted, and replaces a `false`.
#     (Reuses the `rg` package published to $reg above.) Hermetic: nothing here may reach the built-in
#     default, which is a live host.
rp="$tmp/rp"; mkdir -p "$rp/src"
cat > "$rp/kama.json" <<JSON
{ "name": "rpc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "registries": { "default": false },
  "dependencies": { "rg": { "version": "^1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { rg::area };\nfn int32 main() { return area(); }\n' > "$rp/src/main.kama"
if "$KAMA" pkg install "$rp/kama.json" >"$tmp/rp0.out" 2>&1; then
    echo "check-packages: FAIL — registry dep resolved with every default dropped" >&2; exit 1; fi
grep -qi "no registry configured" "$tmp/rp0.out" \
    || { echo "check-packages: FAIL — dropped-default refusal unclear:" >&2; sed 's/^/  /' "$tmp/rp0.out" >&2; exit 1; }
cat > "$rp/kama.local.json" <<JSON
{ "registries": { "default": "file://$reg" } }
JSON
if ! "$KAMA" pkg install "$rp/kama.json" >"$tmp/rp1.out" 2>&1; then
    echo "check-packages: FAIL — kama.local.json registries override did not resolve the dep:" >&2; sed 's/^/  /' "$tmp/rp1.out" >&2; exit 1; fi
grep -q '"source": "registry"' "$rp/kama.lock" \
    || { echo "check-packages: FAIL — registries-override install did not lock a registry source:" >&2; sed 's/^/  /' "$rp/kama.lock" >&2; exit 1; }

# 27a. a DEPENDENCY written for a newer compiler: its manifest carries a key this one has never heard of,
#     under a `kama` range this one misses. The install says to update rather than naming the key alone —
#     the shape 0.9.440 met with a package that used `publish` (it said only "unknown key", at a store path).
fut="$tmp/fut"; mkdir -p "$fut/src"
printf '{ "name": "fut", "version": "1.0.0", "kind": "library", "kama": ">=99.0.0", "futurekey": true }\n' > "$fut/kama.json"
printf 'export { v };\nfn int32 v() { return 1; }\n' > "$fut/src/fut.kama"
tar -czf "$tmp/fut.tgz" -C "$tmp" fut
fc="$tmp/futc"; mkdir -p "$fc/src"
cat > "$fc/kama.json" <<JSON
{ "name": "futc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "fut": { "url": "$furl/fut.tgz" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'fn int32 main() { return 0; }\n' > "$fc/src/main.kama"
if "$KAMA" pkg install "$fc/kama.json" >"$tmp/fut.out" 2>&1; then echo "check-packages: FAIL — a dependency needing kama >=99 installed" >&2; exit 1; fi
grep -q "needs kama >=99.0.0" "$tmp/fut.out" && grep -q 'could not read: unknown key `futurekey`' "$tmp/fut.out" \
    || { echo "check-packages: FAIL — a too-new dependency's refusal does not say to update:" >&2; sed 's/^/  /' "$tmp/fut.out" >&2; exit 1; }

# 27b. with NOTHING configured, a registry dep resolves through the built-in official registry. Proven without
#     the network: a `curl` on PATH that fails at once, so the install reports which index it asked for. POSIX
#     only — on Windows the driver spawns through cmd.exe, which would not run a shell-script `curl`.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) : ;; *)
    shim="$tmp/nocurl"; mkdir -p "$shim"; printf '#!/bin/sh\nexit 22\n' > "$shim/curl"; chmod +x "$shim/curl"
    dd="$tmp/dflt"; mkdir -p "$dd/src"
    cat > "$dd/kama.json" <<JSON
{ "name": "dflt", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "@kama/sodium": { "version": "^0.4.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
    printf 'fn int32 main() { return 0; }\n' > "$dd/src/main.kama"
    if PATH="$shim:$PATH" "$KAMA" pkg install "$dd/kama.json" >"$tmp/dflt.out" 2>&1; then
        echo "check-packages: FAIL — an install with a failing curl succeeded" >&2; exit 1; fi
    grep -q "https://registry.kama-lang.org/@kama/sodium/index.json" "$tmp/dflt.out" \
        || { echo "check-packages: FAIL — an unconfigured registry dep did not go to the built-in official registry:" >&2; sed 's/^/  /' "$tmp/dflt.out" >&2; exit 1; } ;;
esac

# ---- workspace-internal dependencies -----------------------------------------------------------------
# The five-file workspace from docs/packages.md § Workspaces: a root `kama_workspace.json` composing two
# libraries and an app. `libs/net` declares the sibling it imports, which is what makes it extractable.
# The root file carries no name and no version: a workspace is not a project.
ws="$tmp/acme"
mkdir -p "$ws/libs/config/src" "$ws/libs/net/src" "$ws/apps/server/src"
cat > "$ws/kama_workspace.json" <<'JSON'
{ "projects": { "libs/*": { "optional": false }, "apps/server": { "optional": false } } }
JSON
cat > "$ws/libs/config/kama.json" <<'JSON'
{ "name": "config", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$ws/libs/config/src/config.kama" <<'KAMA'
export { Config };

type value Config {
    public int32 port;
    public ctor of(int32 port) { this.port = port; }
}
KAMA
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library",
  "dependencies": { "config": { "path": "../config" } }, "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$ws/libs/net/src/net.kama" <<'KAMA'
import { config::Config };
export { listenPort };

fn int32 listenPort() { Config c = Config.of(port: 8); return c.port; }
KAMA
# The app declares only what IT imports. `config` therefore reaches the resolver for the first time as a
# TRANSITIVE request from `net` — a non-root requestor, which is the case the top-level-only rule refused.
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "net": { "path": "../../libs/net" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { net::listenPort };\nfn int32 main() { return listenPort(); }\n' > "$ws/apps/server/src/main.kama"

# 28. a workspace member may declare the sibling it imports, and that path dep resolves transitively.
if ! "$KAMA" pkg install "$ws/apps/server/kama.json" >"$tmp/ws0.out" 2>&1; then
    echo "check-packages: FAIL — workspace-internal path dep rejected:" >&2; sed 's/^/  /' "$tmp/ws0.out" >&2; exit 1; fi
"$KAMA" run "$ws/apps/server/kama.json" >/dev/null 2>&1 && wsrc=0 || wsrc=$?
[ "$wsrc" = 8 ] || { echo "check-packages: FAIL — workspace app exited $wsrc, expected 8" >&2; exit 1; }

# 29. THE MILESTONE — the sub-project is EXTRACTABLE: it builds on its own, from its own directory, with
#     no ancestor manifest in play. Before workspace-internal path deps it could not declare `config` at
#     all, so it built where it sat and nowhere else.
if ! "$KAMA" pkg install "$ws/libs/net/kama.json" >"$tmp/ws1.out" 2>&1; then
    echo "check-packages: FAIL — sub-project could not install standalone:" >&2; sed 's/^/  /' "$tmp/ws1.out" >&2; exit 1; fi
if ! "$KAMA" check "$ws/libs/net/kama.json" >"$tmp/ws2.out" 2>&1; then
    echo "check-packages: FAIL — sub-project does not build standalone (not extractable):" >&2; sed 's/^/  /' "$tmp/ws2.out" >&2; exit 1; fi

# 30. the app may ALSO declare `config`, and it spells the same directory differently (`../../libs/config`
#     vs net's `../config`). Paths are relative to the manifest that declared them, so the two must
#     canonicalize to one package and dedup — comparing the spellings reports "require different sources".
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "config": { "path": "../../libs/config" },
                    "net":    { "path": "../../libs/net" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
if ! "$KAMA" pkg install "$ws/apps/server/kama.json" >"$tmp/ws5.out" 2>&1; then
    echo "check-packages: FAIL — two spellings of one sibling directory conflicted:" >&2; sed 's/^/  /' "$tmp/ws5.out" >&2; exit 1; fi

# 31. a member may NOT reach outside the workspace — that path is not carried by anything, so it is not
#     reproducible. The declaration is the gate, not adjacency.
mkdir -p "$tmp/stray/lib/src"
cat > "$tmp/stray/lib/kama.json" <<'JSON'
{ "name": "stray", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
printf 'export { v };\nfn int32 v() { return 1; }\n' > "$tmp/stray/lib/src/stray.kama"
cp "$ws/libs/net/kama.json" "$tmp/net-manifest.bak"
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library",
  "dependencies": { "config": { "path": "../config" },
                    "stray":  { "path": "../../../stray/lib" } }, "modules": { ".": { "visibility": "public" } } }
JSON
if "$KAMA" pkg install "$ws/apps/server/kama.json" >"$tmp/ws3.out" 2>&1; then
    echo "check-packages: FAIL — a path dep escaping the workspace was accepted" >&2; exit 1; fi
grep -q "only allowed at the top level" "$tmp/ws3.out" \
    || { echo "check-packages: FAIL — workspace-escape message unclear:" >&2; sed 's/^/  /' "$tmp/ws3.out" >&2; exit 1; }
cp "$tmp/net-manifest.bak" "$ws/libs/net/kama.json"

# 32. and without a workspace file DECLARING the members, the very same sibling dep is refused — proving
#     the gate is the declaration and not mere directory adjacency. (The app is back to declaring only
#     `net`, so `config` is again a first-encounter transitive request.)
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "net": { "path": "../../libs/net" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
mv "$ws/kama_workspace.json" "$tmp/ws-root.bak"
if "$KAMA" pkg install "$ws/apps/server/kama.json" >"$tmp/ws4.out" 2>&1; then
    echo "check-packages: FAIL — sibling path dep accepted with no declared workspace" >&2; exit 1; fi
mv "$tmp/ws-root.bak" "$ws/kama_workspace.json"

# 33. per-package import checking: `libs/net` imports `config` while declaring nothing, and only the app
#     declares it. That builds where it sits and nowhere else, so the BUILD FAILS and names the exact line
#     to add. A hard error — a guarantee nobody is forced to honor is not a guarantee.
cat > "$ws/apps/server/kama.json" <<'JSON'
{ "name": "server", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "config": { "path": "../../libs/config" },
                    "net":    { "path": "../../libs/net" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$ws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$ws/apps/server/kama.json" >/dev/null 2>&1
if "$KAMA" run "$ws/apps/server/kama.json" >"$tmp/ws6.out" 2>&1; then
    echo "check-packages: FAIL — a free-riding sub-project still built:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; fi
grep -q "error:.*does not declare it" "$tmp/ws6.out" \
    || { echo "check-packages: FAIL — no error for a free-riding sub-project:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; }
grep -q '"config": { "path": "../config" }' "$tmp/ws6.out" \
    || { echo "check-packages: FAIL — error did not name the line to add:" >&2; sed 's/^/  /' "$tmp/ws6.out" >&2; exit 1; }

# 33b. the editor must NOT be held to it. `kama query` mirrors the LSP, and refusing to analyze would
#      strip cross-module hover/definitions over a *manifest* problem when the code itself resolves fine.
if ! "$KAMA" query "$ws/libs/net/kama.json" "$ws/libs/net/src/net.kama" --symbols >"$tmp/ws6q.out" 2>&1; then
    echo "check-packages: FAIL — the query/LSP path must degrade to a warning, not refuse:" >&2; sed 's/^/  /' "$tmp/ws6q.out" >&2; exit 1; fi

# 34. and applying exactly the line it named makes it build — the remedy the diagnostic gives is one the
#     resolver actually accepts (which it did not, before workspace-internal path deps).
cp "$tmp/net-manifest.bak" "$ws/libs/net/kama.json"
"$KAMA" pkg install "$ws/apps/server/kama.json" >/dev/null 2>&1
"$KAMA" run "$ws/apps/server/kama.json" >"$tmp/ws7.out" 2>&1 && frrc=0 || frrc=$?
[ "$frrc" = 8 ] || { echo "check-packages: FAIL — declared workspace build exited $frrc, expected 8:" >&2; sed 's/^/  /' "$tmp/ws7.out" >&2; exit 1; }
if grep -q "does not declare it" "$tmp/ws7.out"; then
    echo "check-packages: FAIL — still reported after declaring the dependency:" >&2; sed 's/^/  /' "$tmp/ws7.out" >&2; exit 1; fi

# 35. a FETCHED package that free-rides must NOT be warned about. Its sources live in the
#     content-addressed store, where the manifest is not the user's to edit and editing it would break
#     the tree hash naming its store entry — and a path dep out of a fetched package is refused anyway.
#     So the diagnostic would instruct a destructive action nobody can act on. (`geo` is the git package
#     built at the top of this file; here a second copy imports `mathx` without declaring it.)
fr="$tmp/frdep"; mkdir -p "$fr/geosrc/src" "$fr/mathx/src" "$fr/app/src"
cat > "$fr/mathx/kama.json" <<'JSON'
{ "name": "mathx", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
printf 'export { two };\nfn int32 two() { return 2; }\n' > "$fr/mathx/src/mathx.kama"
cat > "$fr/geosrc/kama.json" <<'JSON'
{ "name": "geodep", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
printf 'import { mathx::two };\nexport { area };\nfn int32 area() { return two(); }\n' > "$fr/geosrc/src/geodep.kama"
( cd "$fr/geosrc" && git init -q . && git add -A \
  && git -c user.email=t@t -c user.name=t commit -qm x && git tag v1.0.0 ) >/dev/null 2>&1
cat > "$fr/app/kama.json" <<JSON
{ "name": "frapp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "geodep": { "git": "file://$fr/geosrc", "rev": "v1.0.0" },
                    "mathx":  { "path": "../mathx" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { geodep::area };\nfn int32 main() { return area(); }\n' > "$fr/app/src/main.kama"
if ! "$KAMA" pkg install "$fr/app/kama.json" >"$tmp/fr0.out" 2>&1; then
    echo "check-packages: FAIL — fetched free-rider fixture did not install:" >&2; sed 's/^/  /' "$tmp/fr0.out" >&2; exit 1; fi
"$KAMA" run "$fr/app/kama.json" >"$tmp/fr1.out" 2>&1 && frdrc=0 || frdrc=$?
[ "$frdrc" = 2 ] || { echo "check-packages: FAIL — fetched free-rider build exited $frdrc, expected 2:" >&2; sed 's/^/  /' "$tmp/fr1.out" >&2; exit 1; }
if grep -q "does not declare it" "$tmp/fr1.out"; then
    echo "check-packages: FAIL — told the user to edit a package inside the store:" >&2; sed 's/^/  /' "$tmp/fr1.out" >&2; exit 1; fi

# 36. ACCEPTANCE — every member of the workspace builds on its own, from its own directory, with no
#     workspace file in play. That is what "extractable" means, and it is mechanically checkable: walk
#     the members and install + check each where it stands. (`libs/config` has no
#     dependencies, `libs/net` has one sibling, `apps/server` has two — all three must stand alone.)
for member in "$ws/libs/config" "$ws/libs/net" "$ws/apps/server"; do
    if ! "$KAMA" pkg install "$member/kama.json" >"$tmp/acc.out" 2>&1; then
        echo "check-packages: FAIL — $member does not install standalone:" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
    # Checked BY ITS MANIFEST, which is also what makes this an extractability claim at all: naming the
    # source files would be a loose check, applying no manifest and resolving no dependency — it would
    # pass or fail for reasons having nothing to do with whether the member stands alone.
    if ! "$KAMA" check "$member/kama.json" >"$tmp/acc.out" 2>&1; then
        echo "check-packages: FAIL — $member does not build standalone (not extractable):" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
    if grep -q "does not declare it" "$tmp/acc.out"; then
        echo "check-packages: FAIL — $member free-rides on an ancestor manifest:" >&2; sed 's/^/  /' "$tmp/acc.out" >&2; exit 1; fi
done

# 37. two packages claiming the SAME (type, contract) conformance can no longer be written. A conformance is
#     program-wide, so a duplicate was visible under the whole-program view — but visible is not fixable: an
#     application owning neither package could not build at all. Since 0.9.511 an adapter lives only in its
#     contract's own module (the user's ruling, 2026-10-01), so the app's claim below is refused where it is
#     written, and the error says which module may adapt `int32` to `Marker`.
dc="$tmp/dupconf"; mkdir -p "$dc/lib/src" "$dc/app/src"
cat > "$dc/lib/kama.json" <<'JSON'
{ "name": "marklib", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$dc/lib/src/marklib.kama" <<'EOF'
export { Marker, viaMarker };
type contract Marker for value, intrinsic { fn int32 mark(); }
type adapter <int32> implements Marker { public fn int32 mark() { return 1; } }
fn int32 viaMarker<T: Marker>(ref T v) { return v.mark(); }
EOF
cat > "$dc/app/kama.json" <<JSON
{ "name": "dupapp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "marklib": { "path": "../lib" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$dc/app/src/main.kama" <<'EOF'
import { marklib::Marker, marklib::viaMarker };
type adapter <int32> implements Marker { public fn int32 mark() { return 2; } }
fn int32 main() { int32 x = 5; return viaMarker(v: ref x); }
EOF
"$KAMA" pkg install "$dc/app/kama.json" >/dev/null 2>&1
if "$KAMA" run "$dc/app/kama.json" >"$tmp/dup.out" 2>&1; then
    echo "check-packages: FAIL — a package adapted a type it does not own to a contract it does not own, and it built:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; fi
grep -q "belongs in module \`marklib\`" "$tmp/dup.out" \
    || { echo "check-packages: FAIL — a foreign adapter was not refused with the contract's home:" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; }
if grep -q "already implements" "$tmp/dup.out"; then
    echo "check-packages: FAIL — the refused adapter still registered (a second, duplicate-conformance error):" >&2; sed 's/^/  /' "$tmp/dup.out" >&2; exit 1; fi

# 37b. a duplicate WITHIN the contract's own module is still a duplicate.
cat > "$dc/app/src/main.kama" <<'EOF'
type contract Solo for value { fn int32 solo(); }
type adapter <int32> implements Solo { public fn int32 solo() { return 1; } }
type adapter <int32> implements Solo { public fn int32 solo() { return 2; } }
fn int32 main() { return 0; }
EOF
if "$KAMA" run "$dc/app/kama.json" >"$tmp/dup2.out" 2>&1; then
    echo "check-packages: FAIL — a duplicate conformance in one package still built:" >&2; sed 's/^/  /' "$tmp/dup2.out" >&2; exit 1; fi
grep -q "already implements" "$tmp/dup2.out" \
    || { echo "check-packages: FAIL — no duplicate-conformance error within one package:" >&2; sed 's/^/  /' "$tmp/dup2.out" >&2; exit 1; }

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
#
#     The OPERAND RULE is what settles this now, and it settles it by construction rather than by getting
#     a walk right: the manifest is named, so there is no directory to guess from and the answer cannot
#     depend on where the shell is standing. These three assertions are kept because they are the ones
#     that would notice if that ever stopped being true.
out="$tmp/outside"; mkdir -p "$out"
"$KAMA" seed "$out/dep" --kind library >/dev/null 2>&1 \
    || { echo "check-packages: FAIL — could not seed the outside-build library" >&2; exit 1; }
"$KAMA" seed "$out/app" >/dev/null 2>&1 \
    || { echo "check-packages: FAIL — could not seed the outside-build app" >&2; exit 1; }
cat > "$out/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "dependencies": { "dep": { "path": "../dep" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$out/app/src/app.kama" <<'EOF'
import { dep::answer };
fn int32 main() { return answer(); }
EOF
"$KAMA" pkg install "$out/app/kama.json" >"$tmp/out.out" 2>&1 \
    || { echo "check-packages: FAIL — outside-build fixture did not install:" >&2; sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

# (a) the dependency resolves when the build is driven from outside the project.
( cd "$out" && "$KAMA" build app/kama.json >"$tmp/out.out" 2>&1 ) \
    || { echo "check-packages: FAIL — a build from OUTSIDE the project cannot see its dependencies:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

# (b) the output went to the project's out/, not next to the source. src/ must hold sources only —
#     the same invariant tools/check-clean-tree.sh holds for a manifest-less build.
_stray=$(ls "$out/app/src" | grep -v '\.kama$' || true)
[ -z "$_stray" ] \
    || { echo "check-packages: FAIL — a build from outside wrote into the project's src/: $_stray" >&2; exit 1; }
# `app$EXE`: Windows names the binary app.exe, and `-name app` matches filenames, not stems — so the
# assertion looked like a build that had written its output somewhere else entirely. It was invisible
# until case 35 above stopped exiting first and this case became reachable at all.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
    *)                    EXE=""     ;;
esac
[ -n "$(find "$out/app/out" -name "app$EXE" -type f -print -quit 2>/dev/null)" ] \
    || { echo "check-packages: FAIL — a build from outside did not use the project's out/ root" >&2; exit 1; }

# (c) a file's OWN project wins over the directory the shell happens to be standing in. Sitting inside
#     `dep`, building app's entry must still use APP's manifest — otherwise the walk would have merely
#     traded one wrong answer for another.
( cd "$out/dep" && "$KAMA" build ../app/kama.json >"$tmp/out.out" 2>&1 ) \
    || { echo "check-packages: FAIL — building app from inside a SIBLING project failed:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }
grep -q "app/out/" "$tmp/out.out" \
    || { echo "check-packages: FAIL — used the CWD's manifest instead of the input file's own project:" >&2
         sed 's/^/  /' "$tmp/out.out" >&2; exit 1; }

# ---- a MULTI-MODULE library, consumed as a dependency ------------------------------------------------
# A library whose own files import across its own modules built standalone and then failed the moment
# anything depended on it. The free-ride rule below — a sibling must declare what it imports, or it
# builds where it sits and fails alone — never exempted a package importing ITSELF, and a file reaching
# across its own package's modules spells its own package name. It only fired when CONSUMED, because
# that is when the library's sources arrive through the dependency view rather than the file's own
# directory; standalone, the whole block is skipped. The suggested remedy was a package depending on
# itself, and the diagnostic's premise ("this package will not build on its own") was disproved by the
# command before it.
#
# ⚠️ Nothing could have caught it: `tests/mod_dir_module.d` has the multi-module shape but is an
# EXECUTABLE, so nothing ever consumes it. It takes two packages, which is why this lives here rather
# than in a `.d` fixture. Found porting a real project.
mm="$tmp/mm"
mkdir -p "$mm/lib/src/a" "$mm/lib/src/b" "$mm/app/src"
cat > "$mm/lib/kama.json" <<'JSON'
{ "name": "mmlib", "version": "0.1.0", "kind": "library",
  "modules": { ".": { "visibility": "public" }, "a": { "visibility": "public" },
               "b": { "visibility": "public" } } }
JSON
printf 'export { av };\nfn int32 av() { return 1; }\n'                                  > "$mm/lib/src/a/a.kama"
printf 'import { mmlib::a::av };\nexport { bv };\nfn int32 bv() { return av() + 1; }\n' > "$mm/lib/src/b/b.kama"
printf 'export { root };\nfn int32 root() { return 0; }\n'                              > "$mm/lib/src/mmlib.kama"
cat > "$mm/app/kama.json" <<'JSON'
{ "name": "mmapp", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "mmlib": { "path": "../lib" } } }
JSON
printf 'import { mmlib::b::bv };\nfn int32 main() { return bv(); }\n' > "$mm/app/src/app.kama"

"$KAMA" build "$mm/lib/kama.json" >"$tmp/mm.out" 2>&1     || { echo "check-packages: FAIL — the multi-module library does not build standalone:" >&2
         sed 's/^/  /' "$tmp/mm.out" >&2; exit 1; }
"$KAMA" pkg install "$mm/app/kama.json" >/dev/null 2>&1
"$KAMA" build "$mm/app/kama.json" >"$tmp/mm.out" 2>&1     || { echo "check-packages: FAIL — the SAME library fails when consumed as a dependency." >&2
         echo "                 A package importing its own modules is not a free-ride; the" >&2
         echo "                 undeclared-import rule must exempt a package's own name." >&2
         sed 's/^/  /' "$tmp/mm.out" >&2; exit 1; }
mmbin=$(sed -n 's/^kama: built //p' "$tmp/mm.out" | tail -1)
# ⚠️ `set -e` is on and this binary EXITS 2 on SUCCESS (it returns bv()). Capture the code in an `if`,
# never as a bare `cmd; [ "$?" -eq 2 ]` — that form kills the guard at the very assertion it is making.
mmrc=0
if [ -n "$mmbin" ]; then
    "$mmbin" >/dev/null 2>&1 || mmrc=$?
fi
[ "$mmrc" -eq 2 ] \
    || { echo "check-packages: FAIL — the consumed multi-module library computed the wrong answer" >&2
         echo "                 (expected 2 from bv() = av() + 1, got $mmrc)" >&2; exit 1; }

# ...and the rule it relaxes must STILL fire. `mmmid` imports `mmlib` without declaring it, while the
# app declares both — the genuine free-ride, which builds here and would fail standalone. Exempting a
# package's OWN name must not exempt anyone else's.
mkdir -p "$mm/mid/src"
cat > "$mm/mid/kama.json" <<'JSON'
{ "name": "mmmid", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
printf 'import { mmlib::b::bv };\nexport { mv };\nfn int32 mv() { return bv() + 1; }\n' > "$mm/mid/src/mmmid.kama"
cat > "$mm/app/kama.json" <<'JSON'
{ "name": "mmapp", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "mmlib": { "path": "../lib" }, "mmmid": { "path": "../mid" } } }
JSON
printf 'import { mmmid::mv };\nfn int32 main() { return mv(); }\n' > "$mm/app/src/app.kama"
"$KAMA" pkg install "$mm/app/kama.json" >/dev/null 2>&1
if "$KAMA" build "$mm/app/kama.json" >"$tmp/mm.out" 2>&1; then
    echo 'check-packages: FAIL — a genuine free-ride was ACCEPTED. mmmid imports mmlib without' >&2
    echo '                 declaring it, so it cannot build on its own; the self-import exemption must' >&2
    echo '                 cover a package OWN name and nothing else.' >&2; exit 1
fi
grep -q "does not declare it" "$tmp/mm.out" \
    || { echo "check-packages: FAIL — the free-ride was rejected for the wrong reason:" >&2
         sed 's/^/  /' "$tmp/mm.out" >&2; exit 1; }


# ---- the output is named for the PROJECT, and dep links are RELATIVE ---------------------------------
# The default output took the stem of the alphabetically FIRST source file, ignoring `name` and
# `entry` entirely. A project named `tests` with `entry: src/main.kama` built a binary called
# `engine_test`, and adding a file that sorted earlier silently RENAMED the shipped executable — a
# published artifact changing name because someone added `assets.kama`. Libraries had it too.
nm="$tmp/nm"
mkdir -p "$nm/src"
cat > "$nm/kama.json" <<'JSON'
{ "name": "namedproj", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'export { hv };\nfn int32 hv() { return 3; }\n'                    > "$nm/src/zz_last.kama"
printf 'import { namedproj::hv };\nfn int32 main() { return hv(); }\n'    > "$nm/src/main.kama"
"$KAMA" build "$nm/kama.json" >"$tmp/nm.out" 2>&1 \
    || { echo "check-packages: FAIL — the named project does not build:" >&2
         sed 's/^/  /' "$tmp/nm.out" >&2; exit 1; }
grep -q '/namedproj$' "$tmp/nm.out" \
    || { echo "check-packages: FAIL — the output is not named for the project (expected .../namedproj):" >&2
         sed 's/^/  /' "$tmp/nm.out" >&2; exit 1; }
# ...and a file that sorts BEFORE the entry must not rename it. This is the regression itself.
printf 'export { av };\nfn int32 av() { return 0; }\n' > "$nm/src/aaa_first.kama"
rm -rf "$nm/out"
"$KAMA" build "$nm/kama.json" >"$tmp/nm.out" 2>&1 \
    || { echo "check-packages: FAIL — rebuild after adding a source failed:" >&2
         sed 's/^/  /' "$tmp/nm.out" >&2; exit 1; }
grep -q '/namedproj$' "$tmp/nm.out" \
    || { echo "check-packages: FAIL — adding an alphabetically-EARLIER source renamed the output." >&2
         echo "                 The binary is named for the project, not for whichever file sorts first." >&2
         sed 's/^/  /' "$tmp/nm.out" >&2; exit 1; }

# A path dependency's view link must be RELATIVE, so one resolved tree is valid under every mount
# point at once (host and container share this repo). An absolute link dangles the moment the tree moves,
# and the resulting error blames the manifest, which is correct. The STORE stays absolute — it is
# machine-global and does not travel with the tree — which the git/registry cases above already cover.
rl="$tmp/rl"
mkdir -p "$rl/dep/src" "$rl/app/src"
cat > "$rl/dep/kama.json" <<'JSON'
{ "name": "rldep", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
printf 'export { dv };\nfn int32 dv() { return 4; }\n' > "$rl/dep/src/rldep.kama"
cat > "$rl/app/kama.json" <<'JSON'
{ "name": "rlapp", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "rldep": { "path": "../dep" } } }
JSON
printf 'import { rldep::dv };\nfn int32 main() { return dv(); }\n' > "$rl/app/src/app.kama"
"$KAMA" pkg install "$rl/app/kama.json" >/dev/null 2>&1
# ⚠️ Windows is exempt from the RELATIVE half, and the exemption is the platform's rather than kama's:
# `linkDir` materializes the view with `mklink /J`, a directory JUNCTION, and a junction's reparse point
# stores an absolute path by definition. The relative alternative is a directory SYMLINK, which needs
# SeCreateSymbolicLinkPrivilege — Developer Mode or elevation — and so cannot be what an ordinary
# `pkg install` depends on. A resolved tree therefore does not survive a move there, and this case
# asserts what does hold: the link is made, and the tree builds where it was resolved.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) relocatable=0 ;; *) relocatable=1 ;; esac
if [ "$relocatable" = 1 ]; then
    lnk=$(readlink "$rl/app/.kama/deps/rldep" || true)
    case "$lnk" in
        /*) echo "check-packages: FAIL — a path dependency was linked ABSOLUTELY ($lnk)." >&2
            echo "                 A resolved tree must be relocatable: relative links are correct under" >&2
            echo "                 every mount point at once, absolute ones dangle on the first move." >&2; exit 1 ;;
        "") echo "check-packages: FAIL — no dependency link at $rl/app/.kama/deps/rldep" >&2; exit 1 ;;
    esac
    # The real assertion is not the string — it is that the tree still builds somewhere else.
    mv "$rl" "$tmp/rl-moved"
    rlbuild="$tmp/rl-moved/app/kama.json"; rlwhere="after being MOVED"
else
    [ -d "$rl/app/.kama/deps/rldep" ] \
        || { echo "check-packages: FAIL — no dependency link at $rl/app/.kama/deps/rldep" >&2; exit 1; }
    rlbuild="$rl/app/kama.json"; rlwhere="through its path-dependency link"
fi
"$KAMA" build "$rlbuild" >"$tmp/rl.out" 2>&1 \
    || { echo "check-packages: FAIL — the resolved tree does not build $rlwhere:" >&2
         sed 's/^/  /' "$tmp/rl.out" >&2; exit 1; }


# ---- `kama`: the compiler-version range a package declares (docs/packages.md § What compiler a package needs)
# A PATH dependency whose manifest asks for a compiler that does not exist: `pkg install` must refuse it
# by name, naming both versions, and leave no view behind; the same package asking for a floor this
# compiler clears installs and builds. The manifest-shape half (a range that does not parse, a non-string)
# is check-manifest.sh's.
kr="$tmp/kreq-src"; mkdir -p "$kr/src"
printf 'export { one };\nfn int32 one() { return 1; }\n' > "$kr/src/kreq.kama"
kc="$tmp/kreq-app"; mkdir -p "$kc/src"
printf '{ "name": "kreqapp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "dependencies": { "kreq": { "path": "../kreq-src" } }, "modules": { ".": { "visibility": "internal" } } }\n' > "$kc/kama.json"
printf 'import { kreq::one };\nfn int32 main() { return one(); }\n' > "$kc/src/main.kama"
printf '{ "name": "kreq", "version": "0.1.0", "kind": "library", "kama": ">=99.0.0", "modules": { ".": { "visibility": "public" } } }\n' > "$kr/kama.json"
if "$KAMA" pkg install "$kc/kama.json" >"$tmp/kreq.out" 2>&1; then
    echo "check-packages: FAIL — a dependency needing kama >=99.0.0 was installed" >&2; exit 1; fi
grep -q "needs kama >=99.0.0" "$tmp/kreq.out" \
    || { echo "check-packages: FAIL — the refusal did not name the requirement:" >&2; sed 's/^/  /' "$tmp/kreq.out" >&2; exit 1; }
[ -e "$kc/.kama/deps/kreq" ] && { echo "check-packages: FAIL — the refused dependency was linked into the view anyway" >&2; exit 1; }
printf '{ "name": "kreq", "version": "0.1.0", "kind": "library", "kama": ">=0.0.1", "modules": { ".": { "visibility": "public" } } }\n' > "$kr/kama.json"
if ! "$KAMA" pkg install "$kc/kama.json" >"$tmp/kreq.out" 2>&1; then
    echo "check-packages: FAIL — a dependency needing kama >=0.0.1 did not install:" >&2; sed 's/^/  /' "$tmp/kreq.out" >&2; exit 1; fi
if "$KAMA" build "$kc/kama.json" -o "$tmp/kreqapp" >"$tmp/kreqb.out" 2>&1; then run "$tmp/kreqapp"
    [ "$RC" = "1" ] || { echo "check-packages: FAIL — the kama-range consumer returned $RC, expected 1" >&2; exit 1; }
else echo "check-packages: FAIL — the kama-range consumer did not build:" >&2; sed 's/^/  /' "$tmp/kreqb.out" >&2; exit 1; fi
# ...and the BUILD-time half: the view is installed, then the dependency's manifest is edited in place
# (a `path` dep is its own store) to need more than this compiler — the build must refuse, install did not run.
printf '{ "name": "kreq", "version": "0.1.0", "kind": "library", "kama": ">=99.0.0", "modules": { ".": { "visibility": "public" } } }\n' > "$kr/kama.json"
if "$KAMA" build "$kc/kama.json" -o "$tmp/kreqapp2" >"$tmp/kreqb2.out" 2>&1; then
    echo "check-packages: FAIL — a build against a dependency needing kama >=99.0.0 succeeded" >&2; exit 1; fi
grep -q "needs kama >=99.0.0" "$tmp/kreqb2.out" \
    || { echo "check-packages: FAIL — the build refusal did not name the requirement:" >&2; sed 's/^/  /' "$tmp/kreqb2.out" >&2; exit 1; }

# ---- registry metadata, the catalog and `kama pkg search` (docs/packages.md § What a registry shows, § Searching)
# 39a. `description`/`repository`/`keywords`/`license` reach the version's index entry; `catalog.json` lists every
# package's HIGHEST version, sorted by name, and is rebuilt on each publish (a second version moves the entry).
mreg="$(kama_native_path "$tmp")/mreg"; mkdir -p "$mreg"
pub_meta() {   # pub_meta <dir> <manifest-json>
    mkdir -p "$1/src"; printf '%s\n' "$2" > "$1/kama.json"
    printf 'export { v };\nfn int32 v() { return 1; }\n' > "$1/src/$(basename "$1").kama"
    commit_all "$1"
    ( cd "$1" && "$KAMA" publish kama.json --registry "file://$mreg" )
}
geo1='{ "name": "@m/geo", "version": "1.0.0", "kind": "library", "kama": ">=0.9.523", "license": "MIT", "description": "Points and polygons", "repository": "https://example.com/geo", "keywords": ["geometry", "math"], "modules": { ".": { "visibility": "public" } } }'
pub_meta "$tmp/m-geo" "$geo1" >"$tmp/meta.out" 2>&1 \
    || { echo "check-packages: FAIL — publishing with registry metadata errored:" >&2; sed 's/^/  /' "$tmp/meta.out" >&2; exit 1; }
grep -q '"description": "Points and polygons", "repository": "https://example.com/geo", "keywords": \["geometry", "math"\]' "$mreg/@m/geo/index.json" \
    || { echo "check-packages: FAIL — the index entry does not carry the metadata:" >&2; sed 's/^/  /' "$mreg/@m/geo/index.json" >&2; exit 1; }
grep -q '"license": "MIT"' "$mreg/@m/geo/index.json" || { echo "check-packages: FAIL — the index entry lost \`license\`" >&2; exit 1; }
pub_meta "$tmp/m-plain" '{ "name": "plain", "version": "0.1.0", "kind": "library", "description": "a helper for geo things", "kama": ">=0.9.523", "modules": { ".": { "visibility": "public" } } }' >>"$tmp/meta.out" 2>&1 \
    || { echo "check-packages: FAIL — publishing \`plain\` errored:" >&2; sed 's/^/  /' "$tmp/meta.out" >&2; exit 1; }
sed -i.bak 's/"version": "1.0.0"/"version": "1.1.0"/; s/Points and polygons/Points, polygons and areas/' "$tmp/m-geo/kama.json" && rm -f "$tmp/m-geo/kama.json.bak"
git -C "$tmp/m-geo" commit -qam v11 >/dev/null
( cd "$tmp/m-geo" && "$KAMA" publish kama.json --registry "file://$mreg" ) >>"$tmp/meta.out" 2>&1 \
    || { echo "check-packages: FAIL — publishing @m/geo 1.1.0 errored:" >&2; sed 's/^/  /' "$tmp/meta.out" >&2; exit 1; }
cat > "$tmp/catalog.want" <<'CAT'
{
  "packages": [
    { "name": "@m/geo", "version": "1.1.0", "license": "MIT", "description": "Points, polygons and areas", "repository": "https://example.com/geo", "keywords": ["geometry", "math"] },
    { "name": "plain", "version": "0.1.0", "description": "a helper for geo things" }
  ]
}
CAT
cmp -s "$tmp/catalog.want" "$mreg/catalog.json" \
    || { echo "check-packages: FAIL — catalog.json is not the highest version of each package, sorted:" >&2; diff "$tmp/catalog.want" "$mreg/catalog.json" >&2; exit 1; }

# 39b. a key newer than the package's `kama` floor is refused at publish, and at --dry-run, writing nothing.
cp "$mreg/@m/geo/index.json" "$tmp/geo.index.before"
sed -i.bak 's/"kama": ">=0.9.523"/"kama": ">=0.9.500"/; s/"version": "1.1.0"/"version": "1.2.0"/' "$tmp/m-geo/kama.json" && rm -f "$tmp/m-geo/kama.json.bak"
git -C "$tmp/m-geo" commit -qam v12 >/dev/null
for mode in "--registry file://$mreg" "--dry-run"; do
    if ( cd "$tmp/m-geo" && "$KAMA" publish kama.json $mode ) >"$tmp/floor.out" 2>&1; then
        echo "check-packages: FAIL — publish ($mode) accepted \`description\` under a \`kama\` floor of >=0.9.500" >&2; exit 1; fi
    grep -q 'is new in kama 0.9.523' "$tmp/floor.out" && grep -q '">=0.9.523"' "$tmp/floor.out" \
        || { echo "check-packages: FAIL — the floor refusal ($mode) did not name the key's release and the fix:" >&2; sed 's/^/  /' "$tmp/floor.out" >&2; exit 1; }
done
cmp -s "$tmp/geo.index.before" "$mreg/@m/geo/index.json" || { echo "check-packages: FAIL — a refused publish changed the index" >&2; exit 1; }

# 39c. each key is held to its shape wherever the manifest is read (`kama check` here, before any publish).
badmeta() {   # badmeta <json-fragment> <expected-message-part>
    d="$tmp/m-bad"; mkdir -p "$d/src"
    printf '{ "name": "bad", "version": "0.1.0", "kind": "library", "kama": ">=0.9.523", %s, "modules": { ".": { "visibility": "public" } } }\n' "$1" > "$d/kama.json"
    printf 'export { v };\nfn int32 v() { return 1; }\n' > "$d/src/bad.kama"
    if "$KAMA" check "$d/kama.json" >"$tmp/bad.out" 2>&1; then echo "check-packages: FAIL — accepted $1" >&2; exit 1; fi
    grep -qF -- "$2" "$tmp/bad.out" || { echo "check-packages: FAIL — refusing $1 did not say \"$2\":" >&2; sed 's/^/  /' "$tmp/bad.out" >&2; exit 1; }
}
long=$(printf 'x%.0s' $(seq 1 201))
badmeta '"description": "two\nlines"'                         'no line breaks'
badmeta '"description": " padded"'                            'a space at its start or end'
badmeta "\"description\": \"$long\""                          'the limit is 200'
badmeta '"repository": "http://example.com/x"'                'must be an `https://` URL'
badmeta '"repository": "javascript:alert(1)"'                 'must be an `https://` URL'
badmeta '"keywords": ["SQL"]'                                 'starting with a letter'
badmeta '"keywords": ["a", "b", "c", "d", "e", "f"]'          'the limit is 5'
badmeta '"keywords": ["sql", "sql"]'                          'listed twice'
badmeta '"keywords": []'                                      '`keywords` is empty'

# 39d. `kama pkg search`: every word must match (name, description or keyword, ignoring case); the name itself
# ranks above a description mention; nothing matching exits 1; a registry with no catalog says so.
if ! "$KAMA" pkg search GEO --registry "file://$mreg" >"$tmp/search.out" 2>&1; then
    echo "check-packages: FAIL — pkg search geo found nothing:" >&2; sed 's/^/  /' "$tmp/search.out" >&2; exit 1; fi
[ "$(sed -n 1p "$tmp/search.out" | awk '{print $1, $2}')" = "@m/geo 1.1.0" ] && [ "$(sed -n 2p "$tmp/search.out" | awk '{print $1}')" = "plain" ] \
    || { echo "check-packages: FAIL — pkg search geo: the name match must rank above the description match:" >&2; sed 's/^/  /' "$tmp/search.out" >&2; exit 1; }
"$KAMA" pkg search math --registry "file://$mreg" 2>&1 | grep -q '^@m/geo ' || { echo "check-packages: FAIL — pkg search did not match a keyword" >&2; exit 1; }
if "$KAMA" pkg search geometry helper --registry "file://$mreg" >"$tmp/search.out" 2>&1; then
    echo "check-packages: FAIL — pkg search matched a package missing one of the words:" >&2; sed 's/^/  /' "$tmp/search.out" >&2; exit 1; fi
grep -q 'no package matches "geometry helper"' "$tmp/search.out" || { echo "check-packages: FAIL — an empty search did not say so:" >&2; sed 's/^/  /' "$tmp/search.out" >&2; exit 1; }
nocat="$(kama_native_path "$tmp")/nocat"; mkdir -p "$nocat"
if "$KAMA" pkg search geo --registry "file://$nocat" >"$tmp/search.out" 2>&1; then
    echo "check-packages: FAIL — pkg search against a registry with no catalog succeeded" >&2; exit 1; fi
grep -q 'keeps no catalog' "$tmp/search.out" || { echo "check-packages: FAIL — a catalog-less registry was not explained:" >&2; sed 's/^/  /' "$tmp/search.out" >&2; exit 1; }

# 40. a lock that pins a registry package BELOW its newest match, reached
# both directly and through a path dependency, is kept. The second requestor saw the newer version as the highest in
# the merged range and restarted resolution; every restart took the lock's version again — "did not converge".
kreg="$(kama_native_path "$tmp")/kreg"; mkdir -p "$kreg"
pub_sod() {   # pub_sod <version> <ver()-return>
    d="$tmp/sod-$1"; mkdir -p "$d/src"
    printf '{ "name": "sod", "version": "%s", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' "$1" > "$d/kama.json"
    printf 'export { ver };\nfn int32 ver() { return %s; }\n' "$2" > "$d/src/sod.kama"
    commit_all "$d"
    ( cd "$d" && "$KAMA" publish kama.json --registry "file://$kreg" )
}
pub_sod 0.5.0 50 >"$tmp/kb39.out" 2>&1 || { echo "check-packages: FAIL — publishing sod 0.5.0 errored:" >&2; sed 's/^/  /' "$tmp/kb39.out" >&2; exit 1; }
kb="$tmp/kb39b"; mkdir -p "$kb/src"
cat > "$kb/kama.json" <<JSON
{ "name": "kb", "version": "0.1.0", "kind": "library",
  "dependencies": { "sod": { "version": "^0.5.0", "registry": "file://$kreg" } }, "modules": { ".": { "visibility": "public" } } }
JSON
printf 'import { sod::ver };\nexport { viaB };\nfn int32 viaB() { return ver(); }\n' > "$kb/src/kb.kama"
ka="$tmp/kb39a"; mkdir -p "$ka/src"
cat > "$ka/kama.json" <<JSON
{ "name": "ka", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "kb": { "path": "../kb39b" }, "sod": { "version": "^0.5.0", "registry": "file://$kreg" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { kb::viaB, sod::ver };\nfn int32 main() { return viaB() + ver(); }\n' > "$ka/src/main.kama"
"$KAMA" pkg install "$ka/kama.json" >"$tmp/kb39.out" 2>&1 \
    || { echo "check-packages: FAIL — the first install (sod 0.5.0 only) errored:" >&2; sed 's/^/  /' "$tmp/kb39.out" >&2; exit 1; }
grep -q '"version": "0.5.0"' "$ka/kama.lock" || { echo "check-packages: FAIL — the lock did not pin sod 0.5.0:" >&2; sed 's/^/  /' "$ka/kama.lock" >&2; exit 1; }
pub_sod 0.5.1 51 >"$tmp/kb39.out" 2>&1 || { echo "check-packages: FAIL — publishing sod 0.5.1 errored:" >&2; sed 's/^/  /' "$tmp/kb39.out" >&2; exit 1; }
"$KAMA" pkg install "$ka/kama.json" >"$tmp/kb39.out" 2>&1 \
    || { echo "check-packages: FAIL — re-install with the lock below the newest match errored:" >&2; sed 's/^/  /' "$tmp/kb39.out" >&2; exit 1; }
grep -q '"version": "0.5.0"' "$ka/kama.lock" || { echo "check-packages: FAIL — the lock's sod 0.5.0 was not kept:" >&2; sed 's/^/  /' "$ka/kama.lock" >&2; exit 1; }
"$KAMA" build "$ka/kama.json" -o "$tmp/kb39app" >"$tmp/kb39b.out" 2>&1 \
    || { echo "check-packages: FAIL — the resolved tree did not build:" >&2; sed 's/^/  /' "$tmp/kb39b.out" >&2; exit 1; }
run "$tmp/kb39app"; [ "$RC" = 100 ] || { echo "check-packages: FAIL — the app returned $RC, expected 100 (both paths see sod 0.5.0)" >&2; exit 1; }

# 41. a path dependency between two workspace members is judged by the workspace holding the package that
#     DECLARED it, not by the one being built. A project outside the workspace path-depends on member `net`,
#     and `net`'s own `../util` comes with it — the workspace carries both. It used to be judged against the
#     outside project's workspace (none) and refused, which forced a consumer to copy the sources instead.
xws="$tmp/xws"; mkdir -p "$xws/libs/util/src" "$xws/libs/net/src" "$tmp/xout/src"
printf '{ "projects": { "libs/*": { "optional": false } } }\n' > "$xws/kama_workspace.json"
printf '{ "name": "util", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }\n' > "$xws/libs/util/kama.json"
printf 'export { v };\nfn int32 v() { return 7; }\n' > "$xws/libs/util/src/util.kama"
cat > "$xws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } },
  "dependencies": { "util": { "path": "../util" } } }
JSON
printf 'import { util::v };\nexport { u };\nfn int32 u() { return v(); }\n' > "$xws/libs/net/src/net.kama"
cat > "$tmp/xout/kama.json" <<'JSON'
{ "name": "xout", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "net": { "path": "../xws/libs/net" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'import { net::u };\nfn int32 main() { return u(); }\n' > "$tmp/xout/src/main.kama"
"$KAMA" pkg install "$tmp/xout/kama.json" >"$tmp/xws.out" 2>&1 \
    || { echo "check-packages: FAIL — a member's sibling dep, consumed from outside its workspace, was refused:" >&2; sed 's/^/  /' "$tmp/xws.out" >&2; exit 1; }
"$KAMA" build "$tmp/xout/kama.json" -o "$tmp/xoutapp" >"$tmp/xws.out" 2>&1 \
    || { echo "check-packages: FAIL — the outside consumer did not build:" >&2; sed 's/^/  /' "$tmp/xws.out" >&2; exit 1; }
run "$tmp/xoutapp"; [ "$RC" = 7 ] || { echo "check-packages: FAIL — the outside consumer returned $RC, expected 7" >&2; exit 1; }
# ...and with no workspace over `net`, its `../util` is a bare path, and the refusal says which two were not listed.
mv "$xws/kama_workspace.json" "$tmp/xws-root.bak"
if "$KAMA" pkg install "$tmp/xout/kama.json" >"$tmp/xws.out" 2>&1; then
    echo "check-packages: FAIL — a transitive path dep with no workspace over it was accepted" >&2; exit 1; fi
grep -qF 'no kama_workspace.json lists both `net` and its `../util`' "$tmp/xws.out" \
    || { echo "check-packages: FAIL — the refusal did not name the unlisted pair:" >&2; sed 's/^/  /' "$tmp/xws.out" >&2; exit 1; }
mv "$tmp/xws-root.bak" "$xws/kama_workspace.json"
# ...and a FETCHED package's path dep is refused whatever surrounds it: the store carries only the package.
xg="$tmp/xws-git"; mkdir -p "$xg/src"; cp "$xws/libs/net/kama.json" "$xg/kama.json"; cp "$xws/libs/net/src/net.kama" "$xg/src/"
( cd "$xg" && git init -q . && git add -A && git -c user.email=t@t -c user.name=t commit -qm x && git tag v1.0.0 ) >/dev/null 2>&1
cat > "$tmp/xout/kama.json" <<JSON
{ "name": "xout", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "dependencies": { "net": { "git": "file://$xg", "rev": "v1.0.0" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
if "$KAMA" pkg install "$tmp/xout/kama.json" >"$tmp/xws.out" 2>&1; then
    echo "check-packages: FAIL — a fetched package's path dep was accepted" >&2; exit 1; fi
grep -qF '`net` was fetched, and a fetched package cannot reference a local path' "$tmp/xws.out" \
    || { echo "check-packages: FAIL — the fetched refusal did not say so:" >&2; sed 's/^/  /' "$tmp/xws.out" >&2; exit 1; }

echo "check-packages: PASS (store+integrity+tamper, no download left behind; transitive BFS; sha pin; lock-honoring offline/cold re-fetch; dev-dep --dev boundary; pkg add/remove round-trip; conflict rejected; kama run entry/forward-exit/native-only; SemVer range select/intersect/downgrade/disjoint/offline; registry publish/immutability/resolve/transitive/offline/two-part-range-names-the-form; KR-100 publish ships the git-tracked files (outside-git/dirty/staged/untracked-manifest refused, ignored .env absent, subdirectory package, submodule shipped/not-checked-out refused, CRLF checkout = identical bytes, lfs filter refused, golden sha256 pinned, committed CR/Ctrl-Z bytes exact, symlink kept, revision recorded; secret backstop names each + template ships; publish.exclude file/dir/no-match/dir-hint/kama.json/fixture-key; --dry-run lists/prints-the-real-integrity/writes-nothing/still-refuses; a path dependency refused at publish and --dry-run, a path dev-dependency ships; $KRNOTE); scopes/registries-config/opt-out/re-point/confusion-guard/collision; kama.local.json dep-override/lock-canonical/registries-override(replaces default:false); built-in default = registry.kama-lang.org (curl shim, no network) + unreachable index says so; too-new dependency says update; workspace sibling-dep/extractable/spelling-dedup/escape-refused/undeclared-tree-refused/free-ride-is-an-ERROR(lenient in the query/LSP path)-then-fixed; store-package-not-blamed; MULTI-MODULE library consumed as a dep (self-import is not a free-ride) + the free-ride still caught; output named for the PROJECT (not the first source file); path-dep links RELATIVE so a resolved tree survives a move (Windows junctions exempt — see the case); ACCEPTANCE every member builds standalone; build-from-OUTSIDE resolves deps + uses the project out/ + prefers the input file's own project; a foreign adapter is refused for its contract's home (no cross-package duplicate can be written); $SIGNOTE; \`kama\` range refused at install and at build, satisfied one builds; registry metadata in the index entry + catalog rebuilt per publish, newer-than-floor keys refused at publish/--dry-run, each key's shape, \`pkg search\` all-words/rank/none/no-catalog; a lock below the newest match, reached directly and through a path dep, kept; a member's sibling dep consumed from OUTSIDE its workspace, refused without one, refused from a fetched package)"
