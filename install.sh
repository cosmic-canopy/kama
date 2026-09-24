#!/bin/sh
# kama installer:  curl -fsSL https://kama-lang.org/install.sh | sh
#
# Detects OS/arch and whether a C compiler is present, then installs the matching
# release into ~/.kama/versions/<version>/. The `kama` on PATH is a thin selector.
#   --no-std / KAMA_NO_STD=1   skip the bundled standard library
#   KAMA_VERSION=vX.Y.Z        install a specific release (default: latest)
#   KAMA_SET_DEFAULT=1         make this version the global default (else set only on first install)
#   KAMA_HOME=<dir>            install prefix (default: ~/.kama)
set -eu
REPO="cosmic-canopy/kama"
PREFIX="${KAMA_HOME:-$HOME/.kama}"
VERSION="${KAMA_VERSION:-latest}"
NOSTD="${KAMA_NO_STD:-}"
for a in "$@"; do [ "$a" = "--no-std" ] && NOSTD=1; done

say() { printf 'kama-install: %s\n' "$1"; }
err() { printf 'kama-install: error: %s\n' "$1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1; }

case "$(uname -s)" in
  Linux)  OS=linux ;;
  Darwin) OS=macos ;;
  *) err "unsupported OS $(uname -s) — on Windows use install.ps1" ;;
esac
case "$(uname -m)" in
  x86_64|amd64)  ARCH=x64 ;;
  arm64|aarch64) ARCH=arm64 ;;
  *) err "unsupported arch $(uname -m)" ;;
esac
[ "$OS" = macos ] && ARCH=universal        # macOS ships a universal2 binary

if need cc || need clang || need gcc; then
  FLAVOR=""; say "C compiler found — installing the slim build"
else
  FLAVOR="-bundled"; say "no C compiler found — installing the self-contained build (bundled zig cc)"
fi

if [ "$VERSION" = latest ]; then
  VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
            | sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p')
  [ -n "$VERSION" ] || err "could not resolve the latest version"
fi

ASSET="kama-$OS-$ARCH$FLAVOR-$VERSION.tar.gz"
URL="https://github.com/$REPO/releases/download/$VERSION/$ASSET"

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
say "downloading $ASSET"
curl -fsSL "$URL" -o "$tmp/$ASSET" || err "download failed: $URL"
if curl -fsSL "$URL.sha256" -o "$tmp/sum" 2>/dev/null; then
  got=$({ shasum -a 256 "$tmp/$ASSET" 2>/dev/null || sha256sum "$tmp/$ASSET"; } | awk '{print $1}')
  [ "$got" = "$(awk '{print $1}' "$tmp/sum")" ] && say "checksum ok" || err "checksum mismatch"
else
  say "warning: no checksum published — skipping verification"
fi

# Versioned store: each toolchain lives in its own dir; the shared package store ($PREFIX/store) is untouched.
VDIR="$PREFIX/versions/$VERSION"
mkdir -p "$VDIR"
tar xzf "$tmp/$ASSET" -C "$VDIR" --strip-components=1
[ -n "$NOSTD" ] && { rm -rf "$VDIR/lib/kama"; say "skipped stdlib (--no-std)"; }
say "installed kama $VERSION to $VDIR"

# The PATH selector + global default: refreshed when explicitly requested (`kama update`), or on the
# first-ever install (so there is always a default to resolve to). Otherwise this is an add-alongside.
BIN="$PREFIX/bin"
if [ "${KAMA_SET_DEFAULT:-}" = 1 ] || [ ! -f "$PREFIX/default" ]; then
  mkdir -p "$BIN"
  # A SYMLINK, never a copy. The compiler resolves its runtime headers and its stdlib RELATIVE TO ITS
  # OWN EXECUTABLE (`<exeDir>/../include`, `<exeDir>/../lib/kama`) and does so through realpath — so a
  # symlink resolves into the versioned toolchain and finds that version's own include/ and lib/.
  # A COPY at $PREFIX/bin/kama instead looks for $PREFIX/include, which does not exist: `kama check`
  # worked and `kama build` died in the C compiler with "'kama_runtime.h' file not found", for every
  # user who followed the PATH line printed below. Measured against a real install of v0.9.440.
  ln -sfn "$VDIR/bin/kama" "$BIN/kama"
  printf '%s\n' "$VERSION" > "$PREFIX/default"
  say "default is now kama $VERSION"
fi

case ":$PATH:" in
  *":$BIN:"*) : ;;
  *) say "add to your shell profile:  export PATH=\"$BIN:\$PATH\"" ;;
esac
"$BIN/kama" --version 2>/dev/null || true
