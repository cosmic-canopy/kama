#!/usr/bin/env bash
# Build the WebGPU toolchain smoke test with Emscripten's WebGPU port.
# Run inside the container: tools/cdev exec tests/webgpu/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../out/webgpu_smoke.html}"
mkdir -p "$(dirname "$OUT")"

emcc -std=c11 --use-port=emdawnwebgpu "$HERE/smoke.c" -o "$OUT"
echo "built $OUT (+ .js + .wasm)"
