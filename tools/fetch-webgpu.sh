#!/usr/bin/env bash
# Fetch the native WebGPU SDK (wgpu-native prebuilt) into a gitignored third_party/wgpu/, so
# `kama build ... --webgpu` (native) can compile + link. Nothing multi-MB lands in git — this
# mirrors how emcc fetches its emdawnwebgpu port on demand for the wasm target.
#
#   tools/fetch-webgpu.sh                 # latest release, host OS/arch
#   WGPU_NATIVE_VERSION=v25.0.2.1 tools/fetch-webgpu.sh   # pin a tag for reproducibility
#
# License note: wgpu-native is dual-licensed MIT OR Apache-2.0, at the user's choice. It is fetched rather
# than vendored only because it is several MB; a program that redistributes it ships its license notice.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DEST="${KAMA_WGPU_DIR:-$ROOT/third_party/wgpu}"

os="$(uname -s)"; arch="$(uname -m)"
case "$os" in
  Darwin) plat=macos ;;
  Linux)  plat=linux ;;
  MINGW*|MSYS*|CYGWIN*) plat=windows ;;
  *) echo "fetch-webgpu: unsupported OS '$os'" >&2; exit 1 ;;
esac
case "$arch" in
  arm64|aarch64) cpu=aarch64 ;;
  x86_64|amd64)  cpu=x86_64 ;;
  *) echo "fetch-webgpu: unsupported arch '$arch'" >&2; exit 1 ;;
esac

# Windows prebuilts come in msvc/gnu flavors, and the choice is not a preference — an import library
# has to match the toolchain doing the link. kama on Windows is built with mingw-w64 clang under msys2
# (see the CI job and the Makefile's -static), so `gnu` is the one that links; `msvc` was the default
# here and produces an archive mingw's linker cannot read. Overridable for anyone building with MSVC.
if [ "$plat" = windows ]; then
  ASSET="wgpu-${plat}-${cpu}-${WGPU_WINDOWS_ABI:-gnu}-release.zip"
else
  ASSET="wgpu-${plat}-${cpu}-release.zip"
fi

VER="${WGPU_NATIVE_VERSION:-latest}"
if [ "$VER" = latest ]; then
  URL="https://github.com/gfx-rs/wgpu-native/releases/latest/download/$ASSET"
else
  URL="https://github.com/gfx-rs/wgpu-native/releases/download/$VER/$ASSET"
fi

echo "fetch-webgpu: $ASSET ($VER)"
echo "           -> $DEST"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$URL" -o "$tmp/wgpu.zip"

rm -rf "$DEST"
mkdir -p "$DEST"
unzip -q "$tmp/wgpu.zip" -d "$DEST"

# Recent releases lay out include/ + lib/ at the archive root. If a release ever flattens the
# headers to the root instead, tuck them under include/webgpu/ so the -I path is uniform.
if [ ! -f "$DEST/include/webgpu/webgpu.h" ] && [ -f "$DEST/webgpu.h" ]; then
  mkdir -p "$DEST/include/webgpu"; mv "$DEST"/*.h "$DEST/include/webgpu/"
fi
if [ ! -f "$DEST/include/webgpu/webgpu.h" ]; then
  echo "fetch-webgpu: extracted archive has no include/webgpu/webgpu.h — layout changed?" >&2
  exit 1
fi

echo "fetch-webgpu: done."
echo "  If you set a custom location, export it: export KAMA_WGPU_DIR=\"$DEST\""
echo "  Native windowing also needs GLFW: 'brew install glfw' (macOS) / 'apt install libglfw3-dev' (Linux)."
