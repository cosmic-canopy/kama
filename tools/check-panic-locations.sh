#!/bin/sh
# check-panic-locations.sh — every source location kama writes INTO a program is package-relative (KR-121).
#
# A `panic`/`assert` site carries `file:line` into the binary, printed when it fires. Until 0.9.558 that was the
# path of the unit being emitted:
#   - absolute, so every binary embedded the build machine's home directory and folder layout (in a container,
#     the mount and store paths) and printed them to whoever saw the panic;
#   - the USER's file for a prelude or stdlib body instantiated there, at the prelude's line, and in a release
#     build a stdlib generic's site named no file at all (`(:33)`).
# Now: `<package>/<path under its source root>`. The trap fixtures cover a loose file, the prelude and the stdlib;
# a loose build never shows the PROJECT or DEPENDENCY spelling, so those live here, in a release build.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-panic-locations: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# A distinctive folder the build happens to sit in. Searched for as a COMPONENT rather than as $tmp, which msys2
# spells `/tmp/…` while the binary would carry `C:/msys64/tmp/…`.
where="$tmp/kr121-build-dir"
mkdir -p "$where/game/src" "$where/geo/src"
cat > "$where/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$where/geo/src/geo.kama" <<'K'
export { area };
fn int32 area(int32 w, int32 h) {
    if (w < 0) { panic(msg: "negative width"); }
    return w * h;
}
K
cat > "$where/game/kama.json" <<'JSON'
{ "name": "game", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "geo": { "path": "../geo" } } }
JSON
if ! "$KAMA" pkg install "$where/game/kama.json" >"$tmp/install.log" 2>&1; then
    echo "check-panic-locations: FAIL — the project did not install:" >&2; sed 's/^/  /' "$tmp/install.log" >&2; exit 1
fi

# expect <case> <main.kama body> <the location the panic must name>
expect() {
    printf '%s\n' "$2" > "$where/game/src/main.kama"
    bin="$tmp/game-$1"
    if ! "$KAMA" build "$where/game/kama.json" --release -o "$bin" >"$tmp/build.log" 2>&1; then
        echo "check-panic-locations: FAIL — $1: the release build failed:" >&2; sed 's/^/  /' "$tmp/build.log" >&2
        fail=1; return
    fi
    # Run under a shell that cannot exec it away (the trailing `:`), so the "Abort trap" job notice is that
    # shell's to print, into /dev/null.
    sh -c '"$1" >/dev/null 2>"$2"; :' _ "$bin" "$tmp/run.err" 2>/dev/null
    if ! grep -qF "($3)" "$tmp/run.err"; then
        echo "check-panic-locations: FAIL — $1: the panic does not name ($3):" >&2; sed 's/^/  /' "$tmp/run.err" >&2
        fail=1
    fi
    if LC_ALL=C grep -aqF "kr121-build-dir" "$bin"; then
        echo "check-panic-locations: FAIL — $1: the binary embeds the directory it was built in" >&2
        fail=1
    fi
}

expect project "fn int32 main() {
    panic(msg: \"here\");
    return 0;
}" "game/main.kama:2"
expect assert "fn int32 main() {
    int32 n = 0;
    assert(cond: n == 1, msg: \"\");
    return 0;
}" "game/main.kama:3"
expect dependency "import { geo::area };
fn int32 main() { return area(w: -2, h: 3); }" "geo/geo.kama:3"

[ "$fail" = 0 ] || exit 1
echo "check-panic-locations: PASS (a release build's panic and assert name game/main.kama and a path dependency's geo/geo.kama, and no binary carries the directory it was built in)"
