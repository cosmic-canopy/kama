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
import { std::collections::DynamicArray };
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
cp -R "$ROOT"/prelude     "$t/lib/kama/prelude"
if ( cd "$tmp" && "$t/bin/kama" build "$tmp/s.kama" -o "$t/out.exe" >"$t/err" 2>&1 ); then
    say_ok "an installed payload resolves \`import std::…\`"
else
    say_fail "the staged payload cannot resolve the stdlib — is release.yml still copying everything lib/ needs?"
    head -5 "$t/err" | sed 's/^/      /' >&2
fi

# The PRELUDE half of the same claim, and it is a separate assertion because it fails separately: the
# prelude is EMBEDDED, so every `Optional` in the world resolves whether or not the source ships. What
# does not work without it is opening the declaration — an installed toolchain would answer "no
# definition" for the most-navigated names in the language while every local leg stayed green, which is
# precisely the failure shape this file exists for.
cat > "$tmp/p.kama" <<'KAMA'
fn Ordering pick() {
    return Ordering::Less;
}
KAMA
pq=$( cd "$tmp" && "$t/bin/kama" query "$tmp/p.kama" --def 1:3 2>/dev/null | tail -1 )
# Matched on the payload-relative TAIL, not on "$t/...": the answer is normalized through realpath, and
# on macOS the temp dir is reached as /var/... but resolves to /private/var/..., so an exact-prefix test
# fails on a correct answer. `lib/kama/prelude/` cannot be the repo's own copy either way.
case "$pq" in
    */lib/kama/prelude/global.kama:*)
        say_ok "...and go-to-definition on a prelude name opens the payload's own prelude/global.kama" ;;
    *)
        say_fail "an installed payload cannot open a prelude declaration (got '$pq') — is release.yml still copying prelude/?" ;;
esac
# The negative that gives it meaning: WITHOUT the source there is no location, and the compiler says so
# rather than inventing a path into a tree that does not exist.
mv "$t/lib/kama/prelude" "$t/prelude-away"
pq2=$( cd "$tmp" && "$t/bin/kama" query "$tmp/p.kama" --def 1:3 2>/dev/null | tail -1 )
mv "$t/prelude-away" "$t/lib/kama/prelude"
if [ "$pq2" = "no definition" ]; then
    say_ok "...and a payload staged without it answers \`no definition\` rather than a path to nowhere"
else
    say_fail "a payload with no prelude/ still claimed a definition at '$pq2'"
fi

# A build SUCCEEDING is not enough on its own to say the manifest was READ, so assert what the manifest
# is FOR: the module the stdlib's files are compiled into. Read it out of the EMITTED C, where a symbol
# is `project · module · name` — nothing else in the payload can produce `std__collections__`.
#
# ⚠️ This used to read `--probe-modules`, deleted with phase 2e along with the `namespace` declaration it
# existed to measure against. The symbol is the better instrument anyway, and for the reason the probe
# version had already been bitten by once: its first cut grepped the whole probe row for
# `std::collections` and matched the DECLARED column, reporting the manifest as read when it had been
# deleted. A C symbol has no second column to match by accident.
mkdir -p "$t/c"
( cd "$tmp" && "$t/bin/kama" build "$tmp/s.kama" -o "$t/c/out2.exe" --keep-c >/dev/null 2>&1 )
if grep -rqE '\bstd__collections__' "$t/c" 2>/dev/null; then
    say_ok "...and the payload's kama.json is what gives the stdlib its module identity"
else
    say_fail "no \`std__collections__\` symbol in the payload's emitted C — is lib/kama.json in the payload?"
fi

# ...and with the manifest removed the stdlib does not resolve AT ALL, which is a stronger statement than
# the one this assertion used to make. It used to check that the file merely derived no module (`-`),
# because resolution found `lib/kama/std/collections/` by walking the filesystem. §2i deleted that walk:
# `import std::…` names the module of a PROJECT, and with no `kama.json` there is no project called `std`
# to name. So a payload staged without it does not ship a nameless stdlib — it ships one nothing can
# import, and the build says so.
rm -f "$t/lib/kama/kama.json"
if ( cd "$tmp" && "$t/bin/kama" build "$tmp/s.kama" -o "$t/out3.exe" >"$t/err3" 2>&1 ); then
    say_fail "the payload's stdlib still resolved with no kama.json present — what is naming it?"
elif grep -qF "cannot resolve module 'std::collections'" "$t/err3"; then
    say_ok "...and with the manifest removed nothing can import it, so the check above is not vacuous"
else
    say_fail "the manifest-less payload failed for the wrong reason:"
    head -3 "$t/err3" | sed 's/^/      /' >&2
fi

[ "$fail" = 0 ] || { echo "check-runtime-dir: FAILED" >&2; exit 1; }
echo "check-runtime-dir: PASS (installed, flat, dev root, dev build; no ambient fallback; stdlib payload)"
