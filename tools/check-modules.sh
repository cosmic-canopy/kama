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
# Three sections, and the second is the one that changes as the campaign runs:
#
#   §1  the derivation itself, against a purpose-built tree whose answers are known by construction.
#       These are ordinary assertions and they hold forever.
#   §2  the CORPUS sweep. Every file whose declared namespace disagrees with its derived module is a
#       migration item, so they were listed by name below and anything NOT on that list failed. The list
#       is now EMPTY: every file in the corpus that declares a namespace derives exactly that namespace.
#       That is the fact the identity cutover rests on — swapping ctxOf from the declaration to the
#       derivation cannot move a symbol, because the two already agree everywhere.
#   §3  the CONSUMPTION, added with the 2c cutover. §1 and §2 would read exactly the same with the
#       emitter still scoping files by their `namespace` declaration — which is what it did through 2a
#       and 2b. §3 reads the derived module back out of the emitted C, so the cutover has a guard.
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
# `Owned` is here for §3: the embedded triad is the one unit no path→module derivation can reach, and
# instantiating it is what puts its symbol in the emitted C where the guard can read it.
cat > "$p/src/app.kama" <<'KAMA'
type value Cell {
    int32 v;
    public ctor make(int32 x) { this.v = x; }
    public fn int32 get() { return this.v; }
}
fn int32 rootHelper() {
    Owned<Cell> boxed = new Cell.make(x: 7);
    return boxed.get();
}
fn int32 main() { return rootHelper(); }
KAMA
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
echo "check-modules: the derived module is what the C symbol is built from"

# §1 and §2 measure the DERIVATION. Nothing above them says the emitter USES it — `--probe-modules` would
# report the same rows with ctxOf still reading the `namespace` declaration, which is exactly what it did
# through 2a and 2b. So build the §1 fixture, whose answers are known by construction, and read the
# symbols back out of the emitted C. This is the assertion that phase 2c actually happened.
#
# Note what `deriv__rootHelper` pins: app.kama declares NO namespace and never could have been public
# before, because a file's identity came from a declaration it does not carry. It is `deriv`'s root module
# now because of where it sits. (`main` is exempt from scoping in both directions — it is `kama_main`.)
mkdir -p "$tmp/c"
if ! "$KAMA" build "$p/kama.json" -o "$tmp/c/app" --keep-c > "$tmp/build.log" 2>&1; then
    echo "check-modules: FAIL — the derivation fixture does not build:" >&2
    head -5 "$tmp/build.log" >&2
    exit 1
fi

syms=$(grep -ohE '\b(deriv[A-Za-z_]*|_F[0-9]+)__[A-Za-z_]+' "$tmp/c"/*.c "$tmp/c"/*.h 2>/dev/null | sort -u)
emitted() {
    printf '%s\n' "$syms" | grep -Fxq "$1" && ok "$2" || bad "$2 — no \`$1\` in the emitted C"
}
emitted deriv__rootHelper   "a file at the source root is scoped by the PROJECT name, with no declaration to say so"
emitted deriv__net__n       "a listed folder's file is scoped by its module"
emitted deriv__net__web__w  "a nested module composes both segments in the symbol"
emitted deriv__network__k   "\`network\` does not collapse into \`net\`"
emitted deriv__tidy__o      "a \`name\` override reaches the symbol, not the folder's spelling"
emitted deriv__net__d       "an unlisted folder's file carries its nearest listed ancestor's scope"
emitted deriv__vendored__v  "...at any depth"

# The blind spot §2 reports but cannot enter: the smart-pointer triad reaches the emitter as
# `<prelude>/std/memory/*.kama` — synthetic units with NO PATH, so no path→module derivation can reach
# them and their module is stated instead, in KamaPreludeModule::module.
#
# ⚠️ Be precise about what this assertion is worth TODAY, because it is easy to over-claim and I did:
# breaking the driver's synthetic arm does NOT fail this, since lib/std/memory/*.kama still declare
# `namespace std::memory` and ctxOf's declaration rung catches them. (Measured — a compiler was built
# with that arm returning "" and the whole guard stayed green.) What this pins is the SYMBOL: the triad
# emits under its module and not under a file-private scope. It becomes the guard on the stated module
# in 2e, when the declaration rung is deleted and nothing else can produce this name.
if printf '%s\n' "$syms" | grep -qE '^_F[0-9]+__(Owned|Shared|Weak)'; then
    bad "the embedded triad lost its module and fell back to a file-private scope"
elif grep -qE '\bstd__memory__Owned' "$tmp/c"/*.c "$tmp/c"/*.h 2>/dev/null; then
    ok "the embedded prelude module keeps the identity it states, having no path to derive one from"
else
    bad "the embedded triad emitted no \`std__memory__Owned\` — did app.kama stop instantiating it?"
fi

# The positional file scope is what identity REPLACES. It survives only for a loose file (§2e.27) — a
# project's files must never land in it, and a stray `_F<n>` here means a unit missed the derivation.
if printf '%s\n' "$syms" | grep -q '^_F[0-9]'; then
    bad "a file in a project still carries the positional \`_F<n>\` scope:"
    printf '%s\n' "$syms" | grep '^_F[0-9]' | head -5 | sed 's/^/        /' >&2
else
    ok "no positional \`_F<n>\` scope survives anywhere in a project build"
fi

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-modules: PASS" || echo "check-modules: FAIL" >&2
exit "$fail"
