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

# ⚠️ DRIVEN BY MANIFEST, ONE PROCESS PER PROJECT — and that is not a performance choice, it is the only
# way this section measures anything at all. Two blind spots make it so, and both were found by being
# bitten:
#
#   * a probe row exists only for a unit that LOADS, so a project's `src/main.kama` handed over ALONE
#     emits none — it cannot resolve its own imports (2c: the 12 files that cutover moved were invisible
#     to the instrument measuring it);
#   * and since §2i, a `.kama` operand IS a loose build, where no manifest participates in anything —
#     including identity. So `kama check --each <every file>` measures the LOOSE derivation, and would
#     report all 52 stdlib files as `declared-only` while asserting a clean sweep. That is precisely how
#     this section went vacuous the day the loose arm landed.
#
# So: every project, named by its `kama.json`; then everything no project owns, as the loose programs
# they are. A project that emits NO rows is named out loud rather than silently skipped.
cd "$ROOT"
: > "$tmp/all.tsv"
: > "$tmp/silent"
for m in $(git ls-files '*kama.json' | grep -v 'kama_workspace.json'); do
    rows=$("$KAMA" check "$m" --probe-modules 2>"$tmp/one.err" | grep '^kama-module' || true)
    if [ -z "$rows" ]; then
        echo "$m ($(head -1 "$tmp/one.err"))" >> "$tmp/silent"
    else
        printf '%s\n' "$rows" >> "$tmp/all.tsv"
    fi
done

# The remainder: tracked files no `kama.json` owns. `--each` is right for exactly these — they ARE loose
# single-file programs — and it is where the loose derivation gets swept. xfail fixtures are excluded
# because they are MEANT not to compile; a probe row from one says nothing.
owned() {   # is any kama.json at or above this file's directory?
    d=${1%/*}
    while [ -n "$d" ] && [ "$d" != "." ]; do
        [ -f "$d/kama.json" ] && return 0
        case "$d" in */*) d=${d%/*} ;; *) d="." ;; esac
    done
    [ -f "kama.json" ]
}
: > "$tmp/loose"
for f in $(git ls-files '*.kama' | grep -v '^tests/xfail/'); do
    owned "$f" || echo "$f" >> "$tmp/loose"
done
"$KAMA" check --each $(cat "$tmp/loose") --probe-modules 2>/dev/null | grep '^kama-module' >> "$tmp/all.tsv" || true

[ -s "$tmp/all.tsv" ] || { echo "check-modules: the corpus sweep produced no rows" >&2; exit 1; }

# THE assertion, and it maintains itself rather than resting on a list that rots: for every file that
# declares a `namespace`, if a project owns it then its derived module must BE that declaration. That is
# the fact the whole cutover rests on — the derivation reproduces every declaration before any of them is
# deleted — and it is stated per file, so a new fixture whose layout does not support the namespace it
# claims lands here by construction.
#
# A file no project owns is the 2e population: nothing derives a module for it (it is a loose file in its
# own root), so the derivation has nothing to be wrong about. Counted below, never asserted on.
git grep -l '^namespace ' -- '*.kama' | sort > "$tmp/declaring"
: > "$tmp/wrong"
: > "$tmp/unmeasured"
while read -r f; do
    owned "$f" || continue
    want=$(sed -n 's/^namespace  *\([A-Za-z_][A-Za-z0-9_:]*\) *;.*/\1/p' "$f" | head -1)
    got=$(awk -F'\t' -v want="$ROOT/$f" '$2 == want { print $4; exit }' "$tmp/all.tsv")
    if   [ -z "$got" ];        then echo "  $f (declares \`$want\`, but no probe row reached it)" >> "$tmp/unmeasured"
    elif [ "$got" != "$want" ]; then echo "  $f: declares \`$want\`, derives \`$got\`" >> "$tmp/wrong"
    fi
done < "$tmp/declaring"

decl_total=$(wc -l < "$tmp/declaring" | tr -d ' ')
if [ -s "$tmp/wrong" ]; then
    bad "a file's derived module disagrees with the namespace it declares:"
    cat "$tmp/wrong" >&2
elif [ -s "$tmp/unmeasured" ]; then
    bad "a file in a project declares a namespace no probe row reached — the sweep is not measuring it:"
    cat "$tmp/unmeasured" >&2
else
    ok "every declaring file a project owns derives exactly the namespace it declares ($decl_total declare one in all)"
fi

# A mismatch anywhere else — a file with no declaration to compare, or one in the loose sweep — would be
# the derivation contradicting itself, so it is a failure independent of the per-file walk above.
mm=$(awk -F'\t' '$5=="mismatch" { print "  " $2 " (declared " $3 ", derived " $4 ")" }' "$tmp/all.tsv")
[ -z "$mm" ] && ok "no other declared/derived disagreement anywhere in the sweep" \
             || { bad "a declared/derived disagreement outside the declaring set:"; printf '%s\n' "$mm" >&2; }

# The blind spot, stated out loud. These are not failures and not passes — they are the measure of what
# this probe cannot answer, and they only shrink when the corpus gains manifests (or, for the last
# column, when phase 2e deletes the declarations outright).
#
# ⚠️ And the part a count cannot show: THE EMBEDDED PRELUDE IS NOT IN THIS POPULATION AT ALL. It reaches
# the emitter through setPrelude/addPreludeModule at setup, never through loadProgramUnits, so no probe
# row is ever emitted for it — `<prelude>` and `<prelude>/std/memory/*` are unmeasured here, and their
# identity is the open question §2f.30 flags. `synthetic` below is a TRIPWIRE, not coverage: it should
# read 0 forever, and a non-zero would mean the population moved under this guard.
echo "  blind spot: $(awk -F'\t' '$6=="loose"{n++} END{print n+0}' "$tmp/all.tsv") loose rows, \
$(awk -F'\t' '$5=="declared-only"{n++} END{print n+0}' "$tmp/all.tsv") declaring a namespace nothing derives; \
the embedded prelude is outside this probe entirely (synthetic tripwire: \
$(awk -F'\t' '$5=="synthetic"{n++} END{print n+0}' "$tmp/all.tsv"))"

# A project the sweep could not enter is not a failure — three fixtures declare `dependencies` whose view
# only `kama pkg install` materializes, and installing would write into the worktree (check-clean-tree
# forbids it) — but it MUST be named, or the sweep silently shrinks as the corpus grows.
if [ -s "$tmp/silent" ]; then
    echo "  note: $(wc -l < "$tmp/silent" | tr -d ' ') project(s) emitted no rows, so nothing in them was measured:"
    sed 's/^/        /' "$tmp/silent"
fi

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
echo "check-modules: with no manifest, a module is still a folder"

# §2i: a loose build has no manifest to name anything, so the operand set does it — the deepest common
# ancestor of the operands' directories is the root, and a file's module is its own directory below it.
# NOT ONE FILE HERE DECLARES A NAMESPACE, which is the whole point: this is the shape the corpus takes
# after 2e, and until the loose arm existed a program like it could not be built at all. (Run it against
# a compiler without that arm: `area` emits as `_F<n>__area` while `app.kama`'s import resolves the CALL
# to `geo__area`, so the C compiler is handed a call to a function nobody defined.)
l="$tmp/loose-prog"
mkdir -p "$l/geo/deep" "$l/oddly-named" "$tmp/lc"
cat > "$l/app.kama" <<'KAMA'
import geo::{area};
import geo::deep::{nested};
fn int32 helper() { return 1; }
fn int32 main() { return area() + nested() + helper(); }
KAMA
printf 'export { area };\nfn int32 area() { return 2; }\n'       > "$l/geo/area.kama"
printf 'export { nested };\nfn int32 nested() { return 3; }\n'   > "$l/geo/deep/nested.kama"
printf 'fn int32 unreachable() { return 9; }\n'                  > "$l/oddly-named/x.kama"

if ! "$KAMA" build "$l/app.kama" "$l/geo/area.kama" "$l/geo/deep/nested.kama" "$l/oddly-named/x.kama" \
        -o "$tmp/lc/app" --keep-c > "$tmp/loose.log" 2>&1; then
    bad "a loose program whose modules are folders does not build:"
    head -5 "$tmp/loose.log" >&2
else
    lsyms=$(grep -ohE '\b(geo__[A-Za-z_]+|_F[0-9]+__[A-Za-z_]+)' "$tmp/lc"/*.c "$tmp/lc"/*.h 2>/dev/null | sort -u)
    lemitted() { printf '%s\n' "$lsyms" | grep -Fxq "$1" && ok "$2" || bad "$2 — no \`$1\` in the emitted C"; }
    lemitted geo__area        "a loose file's folder is its module, with no manifest and no declaration"
    lemitted geo__deep__nested "...composing every folder down from the root"
    # §2e.27, and the one rung the loose derivation deliberately does NOT reach: a file in the root
    # itself has no folder below the root to be named by, so it stays file-private. That is what makes it
    # unimportable, which is the rule 2e turns into a diagnostic.
    printf '%s\n' "$lsyms" | grep -qE '^_F[0-9]+__helper' \
        && ok "a file in the loose ROOT has no module, so it keeps the file-private scope (§2e.27)" \
        || bad "a file in the loose root gained a module — the root is not a folder below itself"
    # A folder whose name is not a legal kama identifier is not a module: nothing could write
    # `import oddly-named::{ … }`. It must not become one ANYWAY and put a hyphen in a C symbol.
    grep -qE '[A-Za-z0-9_]-[A-Za-z0-9_]*__' "$tmp/lc"/*.c "$tmp/lc"/*.h 2>/dev/null \
        && bad "a folder that is not a legal identifier reached a C symbol" \
        || ok "...while a folder no \`import\` could spell is not a module at all"
    rc=0; "$tmp/lc/app" || rc=$?
    [ "$rc" = 6 ] && ok "and it runs: 2 + 3 + 1" || bad "the loose program exited $rc, expected 6"
fi

# The invariant that makes the derivation an IDENTITY and not a position (§1b): it reads a SET, so the
# order the operands are written in cannot reach the emitted C. `_F<n>` still numbers by load order —
# that is exactly what phase 4 replaces — so compare only the module-scoped symbols.
mkdir -p "$tmp/lc2"
if "$KAMA" build "$l/oddly-named/x.kama" "$l/geo/deep/nested.kama" "$l/geo/area.kama" "$l/app.kama" \
        -o "$tmp/lc2/app" --keep-c > "$tmp/loose2.log" 2>&1; then
    a=$(grep -ohE '\bgeo__[A-Za-z_]+' "$tmp/lc"/*.c  "$tmp/lc"/*.h  2>/dev/null | sort -u)
    b=$(grep -ohE '\bgeo__[A-Za-z_]+' "$tmp/lc2"/*.c "$tmp/lc2"/*.h 2>/dev/null | sort -u)
    [ -n "$a" ] && [ "$a" = "$b" ] \
        && ok "the operand ORDER does not reach the symbols — the derivation reads a set" \
        || bad "reordering the operands changed the module symbols"
else
    bad "the same program does not build with its operands in another order"
fi

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-modules: PASS" || echo "check-modules: FAIL" >&2
exit "$fail"
