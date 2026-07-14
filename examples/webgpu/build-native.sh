#!/usr/bin/env bash
# Build the Kama WebGPU triangle as a NATIVE desktop app (Mac / Linux / Windows).
# The SAME triangle.kama the wasm build uses — the surface/present/loop differences live behind the
# std::gpu seam (lib/std/gpu/kama_gpu.*).
#
# Prereqs (once):
#   tools/fetch-webgpu.sh              # fetch the wgpu-native SDK into third_party/wgpu (gitignored)
#   brew install glfw                  # macOS   (Linux: apt install libglfw3-dev)
#
#   examples/webgpu/build-native.sh    # -> out/triangle ; run it to see the window
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/out/triangle}"
mkdir -p "$(dirname "$OUT")"

"$ROOT/kama" build "$HERE/triangle.kama" --webgpu -o "$OUT"
echo "built $OUT — run it to see the spinning triangle in a native window"
