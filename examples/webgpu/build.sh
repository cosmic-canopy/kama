#!/usr/bin/env bash
# Build the Kama WebGPU example to WASM + an HTML/canvas harness.
# Run inside the container: tools/cdev exec examples/webgpu/build.sh
# Then open the output .html in a WebGPU-capable browser (Chrome/Edge).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/out/triangle.html}"
mkdir -p "$(dirname "$OUT")"

"$ROOT/kama" build "$HERE/triangle.kama" --target wasm --webgpu -o "$OUT"
echo "built $OUT (+ .js + .wasm) — open in a WebGPU browser"
