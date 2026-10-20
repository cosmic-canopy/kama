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
# Since 0.9.639 every trap carries one (KR-179) — a bounds miss, a narrowing cast, a divide — and a panic
# handler is handed the same `file()`/`line()`, so those are held here too, in both packages.
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
export { area, pick, ratio };
fn int32 area(int32 w, int32 h) {
    if (w < 0) { panic(msg: "negative width"); }
    return w * h;
}
fn int32 pick(isize i) { InlineArray<int32>#(2) xs = [1, 2]; return xs[i]; }
fn int32 ratio(int32 a, int32 b) { return a / b; }
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

expect project "fn uint8 main() {
    panic(msg: \"here\");
    return 0;
}" "game/main.kama:2"
expect assert "fn uint8 main() {
    int32 n = 0;
    assert(cond: n == 1, msg: \"\");
    return 0;
}" "game/main.kama:3"
expect dependency "import { geo::area };
fn uint8 main() { return cast<uint8>(area(w: -2, h: 3)); }" "geo/geo.kama:3"
expect bounds "fn int32 at(isize i) { InlineArray<int32>#(2) xs = [1, 2]; return xs[i]; }
fn uint8 main() { return cast<uint8>(at(i: 8)); }" "game/main.kama:1"
expect cast "fn int8 narrow(int32 v) { return cast<int8>(v); }
fn uint8 main() { return cast<uint8>(narrow(v: 300)); }" "game/main.kama:1"
expect divide "fn int32 div(int32 a, int32 b) { return a / b; }
fn uint8 main() { return cast<uint8>(div(a: 1, b: 0)); }" "game/main.kama:1"
expect dependency-bounds "import { geo::pick };
fn uint8 main() { return cast<uint8>(pick(i: 9)); }" "geo/geo.kama:6"
expect dependency-divide "import { geo::ratio };
fn uint8 main() { return cast<uint8>(ratio(a: 1, b: 0)); }" "geo/geo.kama:7"
# ...and the handler is handed the same location the line names.
expect hook "import { core::setPanicHandler, core::PanicInfo, core::eprintln, geo::pick };
unsafe fn void onPanic(const ref PanicInfo info) { string f = info.file(); int32 l = info.line(); eprintln(s: \"(hook \${f}:\${l})\"); }
unsafe fn void install() { setPanicHandler(handler: onPanic); }
fn uint8 main() { install(); return cast<uint8>(pick(i: 9)); }" "hook geo/geo.kama:6"

[ "$fail" = 0 ] || exit 1
echo "check-panic-locations: PASS (in a release build, a panic, an assert, a bounds miss, a narrowing cast and a divide name game/main.kama or a path dependency's geo/geo.kama, a handler is handed the same, and no binary carries the directory it was built in)"
