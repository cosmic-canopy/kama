#!/usr/bin/env bash
# Native link-gate for the WebGPU bindings — the same smoke.kama the wasm gate uses, built against
# wgpu-native instead of the emscripten port. Linking (+ a defensive instance create) is the test; a
# window/render is verified by hand via examples/webgpu/build-native.sh. Windowless, so it needs only
# the wgpu-native SDK (tools/fetch-webgpu.sh) — no GLFW. Not wired into CI (the container is headless).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/out/webgpu_smoke}"
mkdir -p "$(dirname "$OUT")"

"$ROOT/kama" build "$HERE/smoke.kama" --webgpu -o "$OUT"
echo "built $OUT — run it: prints whether a WGPUInstance was created"
