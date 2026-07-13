#!/usr/bin/env bash
# Serve the built example over localhost so the browser gets a secure context for WebGPU.
# Build first:  tools/cdev exec examples/webgpu/build.sh
# Then open:    http://localhost:8000/triangle.html   (Chrome/Edge 113+ or Safari Tech Preview)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PORT="${1:-8000}"

cd "$ROOT/out"
echo "serving $ROOT/out at http://localhost:$PORT/triangle.html  (Ctrl-C to stop)"
python3 -m http.server "$PORT"
