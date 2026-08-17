#!/bin/sh
# check-version.sh — the VERSION file is well-formed, and a source change bumped it.
#
# Why this guard exists. `kama --version` is the first thing a bug report carries, and until now it
# could not distinguish two compilers: VERSION sat at 0.9.5 for weeks across dozens of commits that
# changed the emitter, so every one of those binaries reported the same number. The rule is now that a
# commit touching the compiler, the shipped headers, the prelude or the stdlib bumps VERSION.
#
# Two halves, and only the first is absolute:
#   1. VERSION is well-formed SemVer. Always checked — a malformed one silently becomes part of every
#      binary's identity, and `make VERSION=` interpolates it into a C string literal.
#   2. VERSION differs from origin/dev's, when this branch changes source relative to origin/dev.
#      Advisory by nature: it needs a fetched origin/dev to compare against, and there are legitimate
#      states where there is none (a fresh clone, a shallow CI checkout, a detached build from a
#      tarball). Those SKIP rather than fail — a guard that fails open on infrastructure it does not
#      control would just get disabled, and then neither half runs.
#
# The Makefile appends `+g<short-sha>` to whatever is here (see its VERSION block), so a forgotten
# bump still yields an identifiable binary. That is the backstop, not the rule.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VF="$ROOT/VERSION"

# ---- 1. well-formed ---------------------------------------------------------------------------
[ -f "$VF" ] || { echo "check-version: FAIL — no VERSION file at $VF" >&2; exit 1; }

ver=$(tr -d ' \t\n\r' < "$VF")
[ -n "$ver" ] || { echo "check-version: FAIL — VERSION is empty" >&2; exit 1; }

# MAJOR.MINOR.PATCH, with an optional -prerelease and +build. Deliberately not the full SemVer
# grammar: this file is hand-edited, and the shapes worth catching are a stray comment, a `v` prefix
# and a two-part number.
if ! printf '%s' "$ver" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$'; then
    echo "check-version: FAIL — VERSION is not SemVer: '$ver'" >&2
    echo "  want MAJOR.MINOR.PATCH (e.g. 0.9.6), no leading 'v', no trailing comment." >&2
    exit 1
fi

# The Makefile adds the build metadata; carrying it in the file too would double it.
case "$ver" in
    *+*) echo "check-version: FAIL — VERSION carries build metadata ('$ver')." >&2
         echo "  The Makefile appends +g<sha> itself; keep the file to the release number." >&2
         exit 1 ;;
esac

# ---- 2. the BINARY agrees with the file --------------------------------------------------------
# The one that actually bit. `-DKAMA_VERSION` bakes the string into kama.driver.o, and nothing in the
# Makefile's dependency graph mentioned VERSION — so a bump (or just a commit, which moves the +g<sha>
# suffix) left the .o up to date and `kama --version` reported whatever it had been built with last.
# A version that lies is worse than none, so assert the two agree rather than trusting the build rule.
if [ -n "${KAMA:-}" ] && [ -x "${KAMA:-}" ]; then
    got=$("$KAMA" --version 2>/dev/null | head -1)
    case "$got" in
        *"$ver"*) ;;
        *) echo "check-version: FAIL — the binary does not report VERSION." >&2
           echo "    VERSION file : $ver" >&2
           echo "    $KAMA --version: $got" >&2
           echo "  The version is baked into kama.driver.o; it rebuilds via \$(BUILD)/kama.version.stamp." >&2
           echo "  If that stamp rule was removed or the binary is stale, rebuild: ./dev build" >&2
           exit 1 ;;
    esac
fi

# ---- 3. bumped, when this branch changed source ------------------------------------------------
# Everything below degrades to SKIP. See the header.
if ! command -v git >/dev/null 2>&1; then
    echo "check-version: OK ($ver; no git, bump check skipped)"; exit 0
fi
if ! git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    echo "check-version: OK ($ver; not a git worktree, bump check skipped)"; exit 0
fi

base=origin/dev
if ! git -C "$ROOT" rev-parse --verify --quiet "$base" >/dev/null 2>&1; then
    echo "check-version: OK ($ver; no $base to compare against, bump check skipped)"; exit 0
fi

# Did anything that ends up IN the binary change? Docs-only and test-only branches need no bump —
# the point is to distinguish compilers, and those two produce an identical one.
#
# `agents/` and `seed/` are on this list because they are EMBEDDED (Makefile: kama.agents.gen.cpp,
# kama.seed.gen.cpp), so `kama agents` and `kama seed` write different bytes after a change to them.
# They read as documentation and are not, which is precisely the case this guard exists to catch —
# two different compilers claiming the same version.
changed=$(git -C "$ROOT" diff --name-only "$base"...HEAD -- src include prelude lib agents seed Makefile 2>/dev/null)
if [ -z "$changed" ]; then
    echo "check-version: OK ($ver; no source change vs $base)"; exit 0
fi

basever=$(git -C "$ROOT" show "$base:VERSION" 2>/dev/null | tr -d ' \t\n\r')
if [ -z "$basever" ]; then
    echo "check-version: OK ($ver; $base has no VERSION, bump check skipped)"; exit 0
fi

if [ "$ver" = "$basever" ]; then
    echo "check-version: FAIL — source changed vs $base but VERSION is still $ver." >&2
    echo "  Changed under src/ include/ prelude/ lib/ agents/ seed/ Makefile:" >&2
    printf '    %s\n' $changed >&2
    echo "  Bump the patch in ./VERSION (see AGENTS.md)." >&2
    exit 1
fi

echo "check-version: OK ($basever -> $ver, source changed vs $base)"
