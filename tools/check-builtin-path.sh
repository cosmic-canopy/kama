#!/bin/sh
# check-builtin-path.sh — the compiler offers an on-disk path for a compiler-owned source only when that
# file is still the one it COMPILED.
#
# The prelude and the smart-pointer triad are compiled from text EMBEDDED in the binary
# (tools/embed_prelude.sh), while `builtinSourcePath` resolves a file beside the install. Those two drift
# the moment either moves — a stale binary against an edited prelude is the everyday case in this repo —
# and a path handed out across that gap points confidently at the wrong text. Measured before the check:
# insert five lines at the top of a copy's `prelude/global.kama` without rebuilding, and
# go-to-definition on `Optional` still answered `global.kama:8`, where line 8 had become a comment and
# `Optional` had moved to 13.
#
# ⚠️ WHY A GUARD AND NOT A FIXTURE. The fault needs a compiler whose embedded source disagrees with the
# file on disk, which no single-tree fixture can arrange: the harness builds the compiler it tests, so in
# the worktree the two ALWAYS agree. This copies the built compiler into a tree of its own and edits that
# tree's prelude, which is the only way to make the two disagree without rebuilding.
#
# Cheap on purpose: it copies a binary and one directory, and never builds anything.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"          # exports $KAMA (absolute; never the ./kama symlink — see AGENTS.md)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-builtin-path: FAIL — $1" >&2; fail=1; }

# A tree shaped the way `resolveStdlibDir` reads a dev tree: <copy>/out/<platform>/kama beside
# <copy>/lib/std, with <copy>/prelude as lib's sibling. Only `lib/std` needs to EXIST (it is what the
# resolver probes for); the prelude file is the one this guard actually reads.
plat=$(basename "$(dirname "$KAMA")")
mkdir -p "$tmp/copy/out/$plat" "$tmp/copy/lib/std" "$tmp/copy/prelude"
cp "$KAMA" "$tmp/copy/out/$plat/kama"
cp -R "$ROOT/lib/std/memory" "$tmp/copy/lib/std/memory"
cp "$ROOT/prelude/global.kama" "$tmp/copy/prelude/global.kama"
COPY="$tmp/copy/out/$plat/kama"

# `Optional` is declared in the prelude and used here, so `--def` on it must land in prelude/global.kama.
cat > "$tmp/probe.kama" <<'KEOF'
fn Optional<int32> pick() { return Optional::None; }
fn int32 main() { return 0; }
KEOF

# 1. UNTOUCHED COPY — the path is offered, and it is right. Without this the guard would pass for the
# wrong reason: a compiler that never answers at all satisfies case 2 trivially.
before=$("$COPY" query "$tmp/probe.kama" --def 1:3 2>&1 || true)
case "$before" in
    *prelude/global.kama:*) ;;
    *) note "an untouched copy does not resolve \`Optional\` into its prelude — got: $before"; exit 1 ;;
esac
line=$(printf '%s' "$before" | LC_ALL=C sed 's/.*global\.kama:\([0-9]*\).*/\1/')
decl=$(LC_ALL=C grep -n 'type enum Optional' "$tmp/copy/prelude/global.kama" | head -1 | cut -d: -f1)
[ "$line" = "$decl" ] || note "the copy resolves \`Optional\` to global.kama:$line, but it is declared at \
line $decl — the embedded source and the file on disk disagree in a tree where they must agree"

# 2. EDITED COPY, NOT REBUILT — every declaration has moved five lines down while the binary still holds
# the old text. The answer must now be NOTHING, not a stale position.
printf '// drift\n// drift\n// drift\n// drift\n// drift\n' > "$tmp/hdr"
cat "$tmp/hdr" "$tmp/copy/prelude/global.kama" > "$tmp/g" && mv "$tmp/g" "$tmp/copy/prelude/global.kama"
after=$("$COPY" query "$tmp/probe.kama" --def 1:3 2>&1 || true)
case "$after" in
    *global.kama:*) note "after five lines were inserted into the copy's prelude WITHOUT a rebuild, \
go-to-definition still answers \`$after\` — a path into a file the binary did not compile" ;;
esac

exit $fail
