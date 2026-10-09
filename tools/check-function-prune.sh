#!/bin/sh
# check-function-prune.sh — a build compiles only the functions its program reaches (KR-130).
#
# Closure pruning (check-closure-pruning.sh) keeps or drops whole FILES. A dependency file the program reaches kept
# every function in it, so its uncalled `unused` was compiled with its 64 KB frame, and a consumer that sets
# `-Wframe-larger-than` was failed by a function it never calls. Until 0.9.561 this project did not build.
#
# What must survive the prune is everything reached some way other than a call: an `expose fn` the host calls by
# symbol, a function handed out as a `fnptr` value, a contract method reached through its vtable. `kama transpile`
# writes the whole translation — asking for the C means all of it.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-function-prune: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-function-prune: FAIL — $1" >&2; exit 1; }

mkdir -p "$tmp/heavy/src" "$tmp/app/src"
cat > "$tmp/heavy/kama.json" <<'JSON'
{ "name": "heavy", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$tmp/heavy/src/heavy.kama" <<'K'
export { used, unused, Shape, Square, area };
fn int64 used(int64 v) { return v + 1i64; }
fn int64 unused(int64 v) {                          // called by nothing: a 64 KB frame no one needs compiled
    InlineArray<int64>#(8192) big = [v; (8192)];
    return big[8191];
}
type contract Shape for value { const fn int64 area(); }
type value Square implements Shape {
    int64 side;
    public ctor make(int64 side) { this.side = side; }
    public const fn int64 area() { return this.side * this.side; }   // reached only through Shape's vtable
}
fn int64 area(Shape s) { return s.area(); }
K
cat > "$tmp/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "heavy": { "path": "../heavy" } },
  "cflags": ["-Wframe-larger-than=16384", "-Werror=frame-larger-than"] }
JSON
cat > "$tmp/app/src/main.kama" <<'K'
import { heavy::used, heavy::Square, heavy::area };
fnptr int64 Step(int64 v);
fn int64 twice(int64 v) { return v * 2i64; }        // reached only as a `fnptr` value
fn int64 apply(Step f, int64 v) { return f(v: v); }
expose fn int32 hostEntry() { return 7; }           // called by no kama code: the host's way in
fn int32 main() {
    Square s = Square.make(side: 3i64);
    return cast<int32>(used(v: 1i64) + apply(f: twice, v: 5i64) + area(s: s));   // 2 + 10 + 9
}
K
"$KAMA" pkg install "$tmp/app/kama.json" >"$tmp/i.log" 2>&1 || { cat "$tmp/i.log" >&2; fail "the project did not install"; }
"$KAMA" build "$tmp/app/kama.json" -o "$tmp/out/app" --keep-c >"$tmp/b.log" 2>&1 \
    || { sed 's/^/  /' "$tmp/b.log" >&2; fail "a build failed on a dependency's uncalled function (or on something else above)"; }
rc=0; "$tmp/out/app" || rc=$?
[ "$rc" = 21 ] || fail "the program returned $rc, expected 21 — something reached was pruned"
grep -q 'heavy__unused' "$tmp/out"/*.c && fail "\`heavy::unused\`, called by nothing, is still in the C the build compiled"
grep -q 'hostEntry' "$tmp/out"/*.c || fail "\`expose fn hostEntry\` was pruned — the host calls it by symbol"
# The release build folds every module into one translation unit; it prunes the same.
"$KAMA" build "$tmp/app/kama.json" --release -o "$tmp/rel/app" >"$tmp/r.log" 2>&1 \
    || { sed 's/^/  /' "$tmp/r.log" >&2; fail "the release build failed"; }
rc=0; "$tmp/rel/app" || rc=$?
[ "$rc" = 21 ] || fail "the release program returned $rc, expected 21"
# `transpile` writes everything.
"$KAMA" transpile "$tmp/app/kama.json" -o "$tmp/all.c" >"$tmp/t.log" 2>&1 || { sed 's/^/  /' "$tmp/t.log" >&2; fail "transpile failed"; }
grep -q 'heavy__unused' "$tmp/all.c" || fail "\`kama transpile\` left out \`heavy::unused\` — asking for the C means all of it"
echo "check-function-prune: PASS (a dependency's uncalled function is not compiled, so a consumer's -Wframe-larger-than holds; an expose fn, a fnptr value and a vtable method survive, debug and release; transpile keeps everything)"
