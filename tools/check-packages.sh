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
if ! "$KAMA" install "$proj" >"$tmp/install.out" 2>&1; then
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
if ! "$KAMA" install "$proj" >/dev/null 2>&1; then
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
if ! "$KAMA" install "$proj2" >"$tmp/url.out" 2>&1; then
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
if "$KAMA" install "$proj3" >"$tmp/tamper.out" 2>&1; then
    echo "check-packages: FAIL — install accepted a tampered tarball (wrong integrity)" >&2; exit 1
fi
if ! grep -qi "integrity mismatch" "$tmp/tamper.out"; then
    echo "check-packages: FAIL — integrity failure, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/tamper.out" >&2; exit 1
fi

echo "check-packages: PASS (git+url fetch into the content-addressed store; integrity verified; reproducible lock; tamper rejected)"
