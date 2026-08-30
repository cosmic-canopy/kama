#!/bin/sh
# check-ext-version.sh — the VS Code extension's version is well-formed, and a change to it bumped.
#
# Why this guard exists. `release.yml` runs a plain `vsce package`, so the `.vsix` takes its version
# from editor/vscode/package.json and NOT from the git tag — the tag only names the output FILE. So a
# tag of v0.9.117 produced `kama-vscode-0.9.117.vsix` whose manifest said 0.2.0, and the Marketplace
# listing would show 0.2.0.
#
# The failure that actually matters is the SILENT one. Publishing a version that is already on the
# Marketplace is rejected, and both publish steps were `continue-on-error: true` — so a release that
# changed the extension without bumping this file went green and simply did not publish it. Same shape
# as the test-infra holes: infrastructure that exists, is believed, and asserts nothing.
#
# Two halves, mirroring check-version.sh:
#   1. The version is well-formed. Always checked. The Marketplace requires a strict `major.minor.patch`
#      — it rejects the SemVer pre-release/build suffixes that ./VERSION is allowed to carry, so this is
#      a STRICTER grammar than check-version.sh's, deliberately.
#   2. It differs from origin/dev's, when this branch changes anything under editor/vscode/.
#      Advisory by nature — needs a fetched origin/dev — so every missing input SKIPs rather than fails.
#
# The extension version is deliberately NOT tied to ./VERSION. They ship on different clocks: bumping
# the extension on every kama patch would push an auto-update to every user for a compiler change that
# did not touch the editor at all. The rule is "bump it when you change it", which is what this checks.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG="$ROOT/editor/vscode/package.json"

[ -f "$PKG" ] || { echo "check-ext-version: FAIL — no $PKG" >&2; exit 1; }

# The version field, read without a JSON parser (no node/jq dependency in the guard set): the first
# top-level `"version": "…"`. `name`/`displayName` sit above it and carry no digits-and-dots value, so
# the first match is the right one.
ver=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$PKG" | head -1)
[ -n "$ver" ] || { echo "check-ext-version: FAIL — no \"version\" in editor/vscode/package.json" >&2; exit 1; }

# ---- 1. well-formed, by the MARKETPLACE's rule --------------------------------------------------
if ! printf '%s' "$ver" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "check-ext-version: FAIL — extension version is not major.minor.patch: '$ver'" >&2
    echo "  The VS Code Marketplace rejects pre-release/build suffixes here (unlike ./VERSION)." >&2
    exit 1
fi

# ---- 2. bumped, when this branch changed the extension ------------------------------------------
if ! command -v git >/dev/null 2>&1; then
    echo "check-ext-version: OK ($ver; no git, bump check skipped)"; exit 0
fi
if ! git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    echo "check-ext-version: OK ($ver; not a git worktree, bump check skipped)"; exit 0
fi

base=origin/dev
if ! git -C "$ROOT" rev-parse --verify --quiet "$base" >/dev/null 2>&1; then
    echo "check-ext-version: OK ($ver; no $base to compare against, bump check skipped)"; exit 0
fi

# What ships INSIDE the .vsix. `.vscodeignore` keeps the packaged set to the manifest, the entry point,
# the grammar, the language configuration, the icons, the README and LICENSE — so a change to any of
# them is a change a user would receive, and needs a version to arrive under. package-lock.json is on
# the list because it pins the language client that IS packaged (node_modules ships: the extension is
# not bundled), so a lockfile bump changes the shipped bytes.
changed=$(git -C "$ROOT" diff --name-only "$base"...HEAD -- editor/vscode 2>/dev/null)
if [ -z "$changed" ]; then
    echo "check-ext-version: OK ($ver; no editor/vscode change vs $base)"; exit 0
fi

basever=$(git -C "$ROOT" show "$base:editor/vscode/package.json" 2>/dev/null \
          | sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
if [ -z "$basever" ]; then
    echo "check-ext-version: OK ($ver; $base has no extension manifest, bump check skipped)"; exit 0
fi

if [ "$ver" = "$basever" ]; then
    echo "check-ext-version: FAIL — editor/vscode changed vs $base but its version is still $ver." >&2
    echo "  Changed under editor/vscode/:" >&2
    printf '    %s\n' $changed >&2
    echo "  Bump \"version\" in editor/vscode/package.json — the Marketplace refuses a republished" >&2
    echo "  version, and release.yml would skip the publish rather than fail it." >&2
    exit 1
fi

echo "check-ext-version: OK ($basever -> $ver, editor/vscode changed vs $base)"
