#!/bin/sh
# check-runtime-dir.sh — resolveRuntimeDir() finds kama_runtime.h from every shape of tree kama is
# shipped or built in.
#
# kama.driver.cpp has cited "tools/check-runtime-dir.sh" as the thing that pins this since the function
# was written. The guard did not exist. That is the exact failure AGENTS.md is about — a comment is not
# a test — and it cost the Windows leg: the root `./kama` is a real symlink on POSIX, so realpath()
# resolves it back to out/<platform>/kama and the dev-tree probe fires. On msys2 `ln -s` degrades to a
# COPY, the binary sits at the repo root beside include/, none of the three probes matched, and
# resolveRuntimeDir fell through to ".". The failure is silent at that point and surfaces much later as
#
#     p.c:2:10: fatal error: 'kama_runtime.h' file not found
#
# so `./kama build` was unusable on Windows while `out/<platform>/kama build` (what run_tests.sh uses,
# via $KAMA) worked — which is why the whole suite could pass without anyone noticing.
#
# Each layout below is built out of SYMLINKS OR COPIES to the real binary and the real include/, so this
# asserts the resolution rule and nothing about how the tree was produced.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-runtime-dir: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
say_ok()   { echo "  ok: $1"; }
say_fail() { echo "  FAIL: $1" >&2; fail=1; }

# A program that needs the runtime header but nothing else — so the ONLY thing under test is whether the
# C compile can find kama_runtime.h.
cat > "$tmp/p.kama" <<'KAMA'
fn int32 main() {
    return 0;
}
KAMA

# Lay out one tree shape and build through the kama binary sitting in it. `$1` is the label, `$2` the
# path (relative to the tree root) the binary is placed at; include/ always goes where that layout says.
try_layout() {
    label=$1; binrel=$2; increl=$3
    t="$tmp/t$$-$(echo "$label" | tr -c 'a-zA-Z0-9' '_')"
    mkdir -p "$t/$(dirname "$binrel")" "$t/$increl"
    cp "$KAMA" "$t/$binrel"
    # The real headers, so the compile is a real compile.
    cp "$ROOT"/include/*.h "$t/$increl/"
    # Build from a cwd that is NOT the tree, so a "." fallback cannot accidentally succeed.
    if ( cd "$tmp" && "$t/$binrel" build "$tmp/p.kama" -o "$t/out.exe" >"$t/err" 2>&1 ); then
        say_ok "$label"
    else
        say_fail "$label — resolveRuntimeDir did not find the headers"
        sed 's/^/      /' "$t/err" >&2 | head -5
    fi
}

echo "check-runtime-dir: every shipped and built tree shape"
# 1. An INSTALLED tree: <prefix>/bin/kama, headers at <prefix>/include.
try_layout "installed: bin/kama -> ../include"   "bin/kama"            "include"
# 2. A FLAT tree: the binary beside its headers (what a --no-std unpack looks like).
try_layout "flat: kama beside its headers"       "kama"                "."
# 3. The repo root: `make` leaves ./kama here, a symlink on POSIX and a COPY on msys2, with include/
#    beside it. This is the one that was broken.
try_layout "dev root: ./kama beside include/"    "kama"                "include"
# 4. The dev BUILD dir: out/<platform>/kama, headers two levels up at include/.
try_layout "dev build: out/<plat>/kama"          "out/plat/kama"       "include"

# And the negative: no headers anywhere near the binary must FAIL LOUDLY at the C compile rather than
# resolving to something ambient. (resolveRuntimeDir returns "." there; the point is that it does not
# silently pick up a stray copy.)
t="$tmp/none"; mkdir -p "$t"; cp "$KAMA" "$t/kama"
if ( cd "$tmp" && "$t/kama" build "$tmp/p.kama" -o "$t/out.exe" >"$t/err" 2>&1 ); then
    say_fail "a tree with NO include/ built anyway — the headers came from somewhere unintended"
else
    say_ok "no include/ anywhere: the build fails instead of finding an ambient copy"
fi

[ "$fail" = 0 ] || { echo "check-runtime-dir: FAILED" >&2; exit 1; }
echo "check-runtime-dir: PASS (installed, flat, dev root, dev build; and no ambient fallback)"
