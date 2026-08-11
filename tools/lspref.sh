#!/usr/bin/env bash
# lspref.sh — LSP-campaign regression oracle.
#
# Transpiles every single-file fixture (tests/*.kama) with `--no-line` and records a sha256 of the
# generated C, one `<sha>  <name>` line per fixture, sorted. The M0 query-index work (T4/T5) is purely
# ADDITIVE — it must not change a single byte of emitted C — so this hash set must stay IDENTICAL before
# and after. Usage:
#   mkdir -p .scratch                             # gitignored; not present in a fresh checkout
#   tools/lspref.sh > .scratch/lspref-after.txt   # regenerate
#   diff .scratch/lspref-before.txt .scratch/lspref-after.txt && echo IDENTICAL
# Run inside the container: tools/cdev exec tools/lspref.sh > .scratch/lspref-after.txt
set -u
ROOT="."                        # run from the repo root (see the usage lines above)
. tools/kama-bin.sh             # sets $KAMA — this platform's build, else the root ./kama symlink
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for src in tests/*.kama; do
    name="$(basename "$src" .kama)"
    out="$TMP/$name.c"
    if "$KAMA" transpile "$src" -o "$out" --no-line >/dev/null 2>&1; then
        sha="$(sha256sum "$out" | cut -d' ' -f1)"
    else
        sha="TRANSPILE_FAILED"
    fi
    printf '%s  %s\n' "$sha" "$name"
done | sort
