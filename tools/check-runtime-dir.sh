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

# ---------------------------------------------------------------------------------------------------
# The STDLIB half. resolveStdlibDir is resolveRuntimeDir's sibling — same three-arm shape, same silent
# failure mode — and it has the same gap this file was written about: nothing local ever built the tree
# the release workflow actually stages, so a file the tarball forgets to carry breaks every
# `import std::…` in the shipped toolchain while the whole suite stays green.
#
# That is not hypothetical twice over. The header copy a few lines up says `include/kama_*.h` as a GLOB
# because naming them individually had already dropped eight. And lib/ now holds a kama.json beside
# std/ — the manifest that names the project `std` and its module map — which is exactly the kind of
# single file a `cp -R lib/std` staging step does not notice it is missing.
#
# So: stage a payload the way .github/workflows/release.yml does, and compile something that needs the
# stdlib through it.
cat > "$tmp/s.kama" <<'KAMA'
import std::collections::{DynamicArray};
fn int32 main() {
    DynamicArray<int32> xs = DynamicArray.empty();
    xs.add(item: 7);
    return xs[0];
}
KAMA

# The installed prefix, exactly the tarball tree: bin/ include/ lib/kama/.
t="$tmp/payload"
mkdir -p "$t/bin" "$t/include" "$t/lib/kama"
cp "$KAMA" "$t/bin/kama"
cp "$ROOT"/include/*.h "$t/include/"
cp "$ROOT"/lib/kama.json "$t/lib/kama/kama.json"
cp -R "$ROOT"/lib/std     "$t/lib/kama/std"
if ( cd "$tmp" && "$t/bin/kama" build "$tmp/s.kama" -o "$t/out.exe" >"$t/err" 2>&1 ); then
    say_ok "an installed payload resolves \`import std::…\`"
else
    say_fail "the staged payload cannot resolve the stdlib — is release.yml still copying everything lib/ needs?"
    head -5 "$t/err" | sed 's/^/      /' >&2
fi

# A build SUCCEEDING is not enough on its own: the stdlib resolves by path today, so it would go on
# working with the manifest missing and this section would pass while shipping a tarball whose `std` has
# no identity. So assert what the manifest is FOR — read the module a stdlib file derives, which is the
# probe's fourth column, and require it present with the file and absent without it.
#
# ⚠️ Column-precise on purpose. The first cut grepped the whole line for `std::collections`, which
# matched the DECLARED column (the `namespace` those files still carry) and reported the manifest as
# read when it had been deleted — a passing assertion that tested nothing.
derived_module() {   # derived_module <path-fragment> -> the DERIVED module, or "-"
    ( cd "$tmp" && "$t/bin/kama" build "$tmp/s.kama" -o "$1" --probe-modules 2>/dev/null ) \
        | awk -F'\t' -v f="$2" '$1=="kama-module" && index($2,f) { print $4; exit }'
}
got=$(derived_module "$t/out2.exe" "lib/kama/std/collections/")
[ "$got" = "std::collections" ] \
    && say_ok "...and the payload's kama.json is what gives the stdlib its module identity" \
    || say_fail "a stdlib file derived \"$got\", expected \"std::collections\" — is lib/kama.json in the payload?"

rm -f "$t/lib/kama/kama.json"
got=$(derived_module "$t/out3.exe" "lib/kama/std/collections/")
[ "$got" = "-" ] \
    && say_ok "...and with the manifest removed it has none, so the check above is not vacuous" \
    || say_fail "a stdlib file still derived \"$got\" with no kama.json present"

[ "$fail" = 0 ] || { echo "check-runtime-dir: FAILED" >&2; exit 1; }
echo "check-runtime-dir: PASS (installed, flat, dev root, dev build; no ambient fallback; stdlib payload)"
