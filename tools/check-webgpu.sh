#!/bin/sh
# check-webgpu.sh — the WebGPU example and the `--webgpu` link gate stay buildable (KR-188).
#
# Nothing else reaches either. The example is an ordinary project (examples/webgpu/kama.json: its platform layer is
# its own `csources`, gated by fact, and nothing in the compiler knows about it) and the smoke is a loose file;
# neither is a fixture, because a real build needs the wgpu-native SDK (tools/fetch-webgpu.sh) and, for the
# window, GLFW. So this does what every host can — `kama check` both, natively and for wasm, which needs neither —
# and, where the SDK is present, builds and runs the smoke (a windowless instance create), and where GLFW is too,
# builds the example natively. It does not run the example: that opens a window.
#
# check-legs: native
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA=${KAMA:-$ROOT/kama}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-webgpu: FAIL — $1" >&2; sed -n '1,20p' "$tmp/log" >&2; exit 1; }

for t in host wasm; do
    if [ "$t" = wasm ]; then set -- --target wasm; else set --; fi
    "$KAMA" check "$ROOT/examples/webgpu/kama.json" "$@" > "$tmp/log" 2>&1 || fail "examples/webgpu does not check ($t)"
    "$KAMA" check "$ROOT/tests/webgpu/smoke.kama" "$@" > "$tmp/log" 2>&1 || fail "tests/webgpu/smoke.kama does not check ($t)"
done
said="example and smoke check for native and wasm"

wgpu=${KAMA_WGPU_DIR:-$ROOT/third_party/wgpu/$(sh "$ROOT/tools/platform.sh")}   # per platform, as the driver reads it
if [ ! -f "$wgpu/include/webgpu/webgpu.h" ]; then
    echo "check-webgpu: PASS ($said; no wgpu-native SDK — builds skipped, tools/fetch-webgpu.sh fetches it)"
    exit 0
fi
"$KAMA" build "$ROOT/tests/webgpu/smoke.kama" --webgpu -o "$tmp/smoke" > "$tmp/log" 2>&1 || fail "the smoke does not build against the SDK"
"$tmp/smoke" > "$tmp/log" 2>&1 || fail "the smoke exited non-zero"
grep -q 'kama webgpu smoke:' "$tmp/log" || fail "the smoke printed nothing it promises"
said="$said; smoke built and ran"

glfw=""
for d in /opt/homebrew/include /usr/local/include /usr/include /ucrt64/include; do
    [ -f "$d/GLFW/glfw3.h" ] && glfw=$d && break
done
if [ -z "$glfw" ]; then
    echo "check-webgpu: PASS ($said; no GLFW — the example's native build skipped)"
    exit 0
fi
"$KAMA" build "$ROOT/examples/webgpu/kama.json" -o "$tmp/triangle" > "$tmp/log" 2>&1 || fail "examples/webgpu does not build natively"
[ -x "$tmp/triangle" ] || [ -x "$tmp/triangle.exe" ] || fail "examples/webgpu built no executable"
echo "check-webgpu: PASS ($said; example built natively against $glfw)"
