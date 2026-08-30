#!/bin/sh
# check-site.sh — the static site still builds from the docs it is generated out of.
#
# Why this guard exists, stated precisely — because the obvious version of this story is wrong.
#
# The site is assembled FROM docs/ (tools/site/pages.mjs lists the pages; tools/site/render.mjs
# renders them under rules the docs must obey — no raw HTML, every internal #fragment resolves). CI
# ALREADY covers it: ci.yml's `grammar` job runs `sh tools/build-site` on every push to dev/main and
# every PR, and its comment says exactly why ("a docs edit that breaks a cross-reference fails the
# PR, not the deploy on main"). That net works. It is how the 151 `<!-- xfail: name -->` /
# `<!-- test: name -->` markers — which tools/check-doc-claims.sh requires beside every negative claim,
# and which marked sees as raw HTML — were caught on a dev push rather than on the deploy.
#
# What was missing is LOCAL. `./dev check` and `./dev matrix` never built the site, so the whole
# corpus of guards could pass on your machine while a one-word docs edit was already broken; you found
# out minutes later from CI. Every other doc invariant in this repo (claims, spelling, diagnostics,
# roadmap) is checkable before pushing. This one now is too.
#
# ⚠️ So this guard is a FAST LOCAL ECHO of a check CI already owns, not the authority. It DEGRADES TO
# SKIP without node or the pinned `marked` dependency rather than running `npm ci` itself — a guard
# that reaches the network is a guard that fails on someone's plane — which is exactly why it must not
# be mistaken for the authoritative run. That one is in ci.yml, and it must stay there.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

command -v node >/dev/null 2>&1 || { echo "check-site: OK (no node, skipped)"; exit 0; }
[ -d "$ROOT/node_modules/marked" ] || {
    echo "check-site: OK (site deps not installed, skipped — 'npm ci' or './dev site' once to enable)"
    exit 0
}

# Its own mktemp: guards run in parallel and may not write into the worktree. KAMA_SITE_OUT is read by
# tools/site/build.mjs; without it the build would land in the repo's own _site/.
tmp=$(mktemp -d) || { echo "check-site: FAIL — mktemp" >&2; exit 1; }
trap 'rm -rf "$tmp"' EXIT

if ! KAMA_SITE_OUT="$tmp/site" node "$ROOT/tools/site/build.mjs" >"$tmp/log" 2>&1; then
    echo "check-site: FAIL — the site no longer builds from docs/." >&2
    sed 's/^/    /' "$tmp/log" | head -20 >&2
    echo "  Reproduce with: ./dev site" >&2
    exit 1
fi

pages=$(sed -n 's/.*— \([0-9]*\) pages.*/\1/p' "$tmp/log" | head -1)
echo "check-site: OK (${pages:-?} pages rendered from docs/)"
