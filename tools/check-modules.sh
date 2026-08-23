#!/bin/sh
# check-modules.sh — the file→module derivation, measured against the declarations still in the tree.
#
# TEMPORARY, and deliberately so: this guard exists for the length of the module-system campaign
# (docs/design/module-system.md phase 2) and is DELETED with `--probe-modules` when it closes.
#
# Why it exists. Phase 2 swaps the emitter's source of truth for a file's identity from the `namespace`
# it declares to the module its path and its project's `modules` map derive. That is the one step in the
# campaign that cannot be checked by reading: 91 files declare a namespace today, and the derivation has
# to reproduce every one of them before any of them is deleted. So the derivation ships FIRST as a
# measurement — `--probe-modules`, a hidden TSV on stdout — and this guard reads it.
#
# Two halves, and the second is the one that changes as the campaign runs:
#
#   §1  the derivation itself, against a purpose-built tree whose answers are known by construction.
#       These are ordinary assertions and they hold forever.
#   §2  the CORPUS sweep. Every file whose declared namespace disagrees with its derived module is a
#       migration item, so they were listed by name below and anything NOT on that list failed. The list
#       is now EMPTY: every file in the corpus that declares a namespace derives exactly that namespace.
#       That is the fact the identity cutover rests on — swapping ctxOf from the declaration to the
#       derivation cannot move a symbol, because the two already agree everywhere.
#
# The blind spot is reported rather than hidden (design §7: a measurement that hides its own blind spot
# is worse than no measurement). `no-project` is a loose file, whose identity §2i derives from the
# operand set rather than a manifest; `synthetic` is the embedded prelude, which has no path at all.
# Neither can this probe speak to, and the counts say how much that is.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-modules: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=$((fail+1)); }

# ---------------------------------------------------------------------------------------------------
echo "check-modules: a file's module is the nearest LISTED folder above it"

# One project, and every shape the derivation has to get right. `network/` is not an oversight: `net` is
# listed, and a string-prefix test would hand it `network/`'s files.
p="$tmp/deriv"
mkdir -p "$p/src/net/web" "$p/src/net/detail" "$p/src/network" "$p/src/vendored/deep/deeper" "$p/src/oddly-named"
printf 'fn int32 main() { return 7; }\n'      > "$p/src/app.kama"
printf 'fn int32 n() { return 1; }\n'          > "$p/src/net/net.kama"
printf 'fn int32 w() { return 1; }\n'          > "$p/src/net/web/web.kama"
printf 'fn int32 d() { return 1; }\n'          > "$p/src/net/detail/d.kama"
printf 'fn int32 k() { return 1; }\n'          > "$p/src/network/k.kama"
printf 'fn int32 v() { return 1; }\n'          > "$p/src/vendored/deep/deeper/v.kama"
printf 'fn int32 o() { return 1; }\n'          > "$p/src/oddly-named/o.kama"
cat > "$p/kama.json" <<'JSON'
{
  "name": "deriv", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": {
    ".":           { "visibility": "internal" },
    "net":         { "visibility": "public",
                     "modules": { "web": { "visibility": "public" } } },
    "network":     { "visibility": "public" },
    "vendored":    { "visibility": "internal" },
    "oddly-named": { "visibility": "public", "name": "tidy" }
  }
}
JSON

"$KAMA" check "$p/kama.json" --probe-modules > "$tmp/deriv.tsv" 2>"$tmp/deriv.err" \
    || { echo "check-modules: the derivation fixture does not check" >&2; head -3 "$tmp/deriv.err" >&2; exit 1; }

# derived <file-basename> <expected-module-name> <claim>
derived() {
    got=$(awk -F'\t' -v f="/$1" '$1=="kama-module" && index($2, f) == length($2) - length(f) + 1 { print $4 }' \
          "$tmp/deriv.tsv" | head -1)
    [ "$got" = "$2" ] && ok "$3" || bad "$3 — derived \"$got\", expected \"$2\""
}

derived app.kama    deriv                  "a file at the source root is in the project root module"
derived net.kama    deriv::net             "a file in a listed folder takes that folder's name"
derived web.kama    deriv::net::web        "a nested listed folder composes both segments"
derived k.kama      deriv::network         "\`network\` is its own module, not a prefix match on \`net\`"
derived o.kama      deriv::tidy            "a \`name\` override replaces the folder's segment"
# The rule that keeps "list a folder" an API decision rather than a compilation one: an unlisted folder
# is not a module, but its files are not homeless — they belong to the closest folder above that is one.
derived d.kama      deriv::net             "an UNLISTED folder's files belong to the nearest listed ancestor"
derived v.kama      deriv::vendored        "...at any depth below it"

# ---------------------------------------------------------------------------------------------------
echo "check-modules: the corpus agrees, or says exactly where it does not"

# A file whose declared `namespace` disagrees with its derived module is a migration item, not a defect
# in the derivation — so they were listed here by name and anything NOT on the list failed.
#
# **The list is now EMPTY, and that is the strongest form of the check, not the absence of one.** Every
# file in the corpus that declares a namespace derives exactly that namespace from its path and its
# project's module map. That is the fact the identity cutover rests on: swapping ctxOf from the
# declaration to the derivation cannot change a single symbol, because the two already agree everywhere.
#
# ⚠️ Do not delete this block because it looks vacuous. A new fixture that declares a namespace its
# layout does not support will land here, which is exactly when someone needs to be told.
: > "$tmp/known"   # EMPTY, and that is the assertion — see above.

cd "$ROOT"
# xfail fixtures are excluded because they are MEANT not to compile; a probe row from one says nothing.
git ls-files '*.kama' | grep -v '^tests/xfail/' > "$tmp/corpus"
rc=0
"$KAMA" check --each $(cat "$tmp/corpus") --probe-modules > "$tmp/all.tsv" 2>"$tmp/all.err" || rc=$?
[ -s "$tmp/all.tsv" ] || { echo "check-modules: the corpus sweep produced no rows" >&2; head -3 "$tmp/all.err" >&2; exit 1; }

awk -F'\t' '$1=="kama-module" && $5=="mismatch" { print $2 }' "$tmp/all.tsv" \
    | sed "s|^$ROOT/||" | sort > "$tmp/seen"
sort "$tmp/known" > "$tmp/known.sorted"

# LC_ALL=C and grep -Fxv rather than comm: comm is locale-broken on macOS and has silently reported
# nonsense for a set diff in this repo before.
new=$(LC_ALL=C grep -Fxv -f "$tmp/known.sorted" "$tmp/seen" || true)
gone=$(LC_ALL=C grep -Fxv -f "$tmp/seen" "$tmp/known.sorted" || true)

if [ -n "$new" ]; then
    bad "a file's derived module disagrees with its declaration, and it is not a known migration item:"
    echo "$new" | sed 's/^/        /' >&2
else
    n=$(wc -l < "$tmp/seen" | tr -d ' ')
    if [ "$n" = 0 ]; then
        ok "every file that declares a namespace derives exactly that namespace"
    else
        ok "every declared/derived disagreement is a listed migration item ($n of them)"
    fi
fi

# A stale entry is not fatal — a fixture migrating is the POINT — but it must be said, or the list rots
# into a set of paths nobody has checked in months and the guard quietly weakens.
if [ -n "$gone" ]; then
    echo "  note: these no longer mismatch and can leave the list in tools/check-modules.sh:" >&2
    echo "$gone" | sed 's/^/        /' >&2
fi

# The blind spot, stated out loud. These are not failures and not passes — they are the measure of what
# this probe cannot answer, and they only shrink when the corpus gains manifests.
#
# ⚠️ And the part a count cannot show: THE EMBEDDED PRELUDE IS NOT IN THIS POPULATION AT ALL. It reaches
# the emitter through setPrelude/addPreludeModule at setup, never through loadProgramUnits, so no probe
# row is ever emitted for it — `<prelude>` and `<prelude>/std/memory/*` are unmeasured here, and their
# identity is the open question §2f.30 flags. `synthetic` below is a TRIPWIRE, not coverage: it should
# read 0 forever, and a non-zero would mean the population moved under this guard.
echo "  blind spot: $(awk -F'\t' '$5=="no-project"{n++} END{print n+0}' "$tmp/all.tsv") loose (no project above them), \
$(awk -F'\t' '$5=="declared-only"{n++} END{print n+0}' "$tmp/all.tsv") declaring a namespace with no project yet; \
the embedded prelude is outside this probe entirely (synthetic tripwire: \
$(awk -F'\t' '$5=="synthetic"{n++} END{print n+0}' "$tmp/all.tsv"))"

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-modules: PASS" || echo "check-modules: FAIL" >&2
exit "$fail"
