#!/bin/sh
# check-repro-math.sh — a `reproducible-float` program's std::math gives the same bits built `--release`.
#
# tests/repro_math.d holds the bits of std::math's 14 not-correctly-rounded functions under the key, on every leg the
# suite runs. But the suite builds every fixture in debug, and the divergence this exists for showed only in a
# release build: at `-Oz` emscripten links table-free `pow`, `log` and `log2`, so through 0.9.856 a release wasm build
# disagreed with a debug one on 21,407 of the fixture's inputs, while the debug build passed. So this builds the same
# fixture `--release` and runs it: natively always, and for wasm where emcc and node are installed (the wasm leg's
# image, CI's wasm job), since that is the build that diverged.
#
# check-legs: native wasm
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fix="$ROOT/tests/repro_math.d/kama.json"
fail() { echo "check-repro-math: FAIL — $1" >&2; sed -n '1,32p' "$tmp/log" >&2; exit 1; }

"$KAMA" build "$fix" --release -o "$tmp/native" > "$tmp/log" 2>&1 || fail "tests/repro_math.d does not build --release"
"$tmp/native" > "$tmp/log" 2>&1 || fail "a native --release build gives other bits than tests/repro_math.d expects"
said="native"

if command -v "${EMCC:-emcc}" >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
    "$KAMA" build "$fix" --release --target wasm --cc "${EMCC:-emcc}" -o "$tmp/wasm.js" > "$tmp/log" 2>&1 \
        || fail "tests/repro_math.d does not build --release for wasm"
    node "$tmp/wasm.js" > "$tmp/log" 2>&1 || fail "a wasm --release build gives other bits than tests/repro_math.d expects"
    said="native and wasm"
fi
echo "check-repro-math: PASS ($said --release builds give every row's bits)"
