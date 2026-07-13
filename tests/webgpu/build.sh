#!/usr/bin/env bash
# Build the WebGPU toolchain smoke test — Kama bindings through Emscripten's WebGPU port.
# Run inside the container: tools/cdev exec tests/webgpu/build.sh
# Linking successfully is the test (a real device needs a browser).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/out/webgpu_smoke.html}"
mkdir -p "$(dirname "$OUT")"

"$ROOT/kama" build "$HERE/smoke.kama" --target wasm --webgpu -o "$OUT"
echo "built $OUT (+ .js + .wasm)"
