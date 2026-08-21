#!/bin/sh
# check-seed.sh — the `kama seed` guard.
#
# A template is the one kind of code that rots invisibly: nothing else in the tree compiles it, and the
# person who would notice is a brand-new user in their first minute with the language. So this guard
# seeds each project kind into a temp dir and BUILDS AND RUNS the result. That is the only check that
# keeps a template honest — asserting the bytes came out is asserting nothing about whether they work.
#
# What it holds down, beyond "the files appear":
#   1. A NON-TERMINAL never prompts. seed is the driver's only interactive path.
#   2. A seed lands entirely or not at all — no half-written project, ever.
#   3. A seeded library is IMPORTABLE, proven by a second project importing it for an exit code.
#   4. Every rejected name/version/kind writes nothing at all.
#
# ⚠️ `exec </dev/null` below is LOAD-BEARING. `kama seed` prompts when stdin is a terminal, and
# run-checks.sh's run_one redirects a guard's stdout and stderr but NOT its stdin. The parallel pool is
# accidentally safe — POSIX gives an asynchronous list /dev/null for stdin when job control is off, and
# guards launch with `&` — but the `# check-heavy:` path runs synchronously, and running THIS FILE by
# hand from a terminal would otherwise hang on a prompt whose output is inside a log nobody is watching.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu
exec </dev/null

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-seed: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# ---------------------------------------------------------------------------------------------------
echo "check-seed: a non-terminal takes the defaults instead of prompting"

# No --yes, and stdin is not a terminal. This must not prompt and must not hang.
a="$tmp/a"
"$KAMA" seed "$a" >"$tmp/a.out" 2>&1 || bad "a non-interactive seed failed"
[ -f "$a/kama.json" ] && ok "a non-interactive seed writes the project" \
                      || bad "a non-interactive seed wrote no kama.json"
grep -q 'package name' "$tmp/a.out" && bad "seed prompted with stdin not a terminal" \
                                    || ok "no prompt text on a non-terminal"

# A pipe is not an answer stream: piped junk must be ignored, not consumed as answers.
b="$tmp/b"
printf 'zzz\nzzz\nzzz\nzzz\n' | "$KAMA" seed "$b" >/dev/null 2>&1 || bad "a piped seed failed"
grep -q '"name": "b"' "$b/kama.json" && ok "piped stdin is not read as answers" \
                                     || bad "piped stdin leaked into the manifest: $(cat "$b/kama.json")"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: usage"

"$KAMA" seed --nosuchflag >"$tmp/usage.out" 2>&1 && bad "an unknown flag was accepted" \
                                                 || ok "an unknown flag is rejected"
grep -q 'kama seed \[<dir>\]' "$tmp/usage.out" && ok "the rejection prints usage" \
                                               || bad "no usage on a bad flag"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: executable"

e="$tmp/exe"
"$KAMA" seed "$e" --yes --kind executable --name demoapp >/dev/null 2>&1 || bad "seeding an executable failed"
for f in kama.json src/app.kama .gitignore README.md; do
    [ -f "$e/$f" ] || bad "executable seed did not write $f"
done
ok "executable seed writes the four files"
grep -q '"kind": "executable"'  "$e/kama.json" && ok "manifest declares its kind" || bad "no kind in the manifest"
grep -q '"entry": "src/app.kama"' "$e/kama.json" && ok "manifest declares entry" || bad "no entry in the manifest"
grep -q '"sources": \["src"\]'    "$e/kama.json" && ok "manifest declares sources" || bad "no sources in the manifest"

# The claim the whole template rests on: it runs.
if ( cd "$e" && "$KAMA" run ) >"$tmp/run.out" 2>&1; then
    grep -q 'hello from demoapp' "$tmp/run.out" && ok "the seeded executable runs and greets by name" \
                                                || bad "ran, but printed: $(head -1 "$tmp/run.out")"
else
    bad "the seeded executable did not run:"; sed 's/^/    /' "$tmp/run.out" >&2
fi

# No placeholder may survive substitution anywhere in the tree.
if grep -rq 'KAMA_SEED_' "$e" 2>/dev/null; then
    bad "an unsubstituted placeholder survived: $(grep -rlo 'KAMA_SEED_[A-Z_]*' "$e" | head -1)"
else ok "no placeholder token survives"; fi

# Artifacts collect under out/, not on top of the sources (see check-target.sh for the full rule). Note
# this needs an explicit `build`: `kama run` compiles into $TMPDIR and deliberately leaves nothing behind.
( cd "$e" && "$KAMA" build src/app.kama ) >/dev/null 2>&1 || bad "the seeded project did not build"
[ -d "$e/out" ] && ok "build output went to out/" || bad "no out/ after a build"
[ -z "$(find "$e/src" -type f ! -name '*.kama')" ] && ok "src/ holds only sources" \
                                                   || bad "the build left artifacts in src/"
# ...and the .gitignore the seed wrote actually covers it.
grep -q '^out/$' "$e/.gitignore" && ok "the seeded .gitignore covers out/" \
                                 || bad "the seeded .gitignore does not ignore out/"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: library"

l="$tmp/lib"
"$KAMA" seed "$l" --yes --kind library --name demolib >/dev/null 2>&1 || bad "seeding a library failed"
grep -q '"kind": "library"' "$l/kama.json" && ok "library declares its kind" || bad "library has no kind"
grep -q '"sources"' "$l/kama.json" && ok "library declares sources" || bad "library has no sources"
grep -q '"entry"'   "$l/kama.json" && bad "a library should have no entry" || ok "library declares no entry"
[ -f "$l/src/demolib.kama" ] && ok "the library source is named for the package" \
                             || bad "expected src/demolib.kama; got: $(ls "$l/src")"
"$KAMA" check "$l/src/demolib.kama" >/dev/null 2>&1 && ok "the seeded library analyzes clean" \
                                                    || bad "the seeded library does not analyze"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: monorepo, and that a seeded library is actually importable"

w="$tmp/ws"
"$KAMA" seed "$w" --yes --kind monorepo --name acme --members engine,server >/dev/null 2>&1 \
    || bad "seeding a monorepo failed"
grep -q '"projects": \["engine", "server"\]' "$w/kama.json" \
    && ok "the root lists its members explicitly" || bad "root manifest: $(cat "$w/kama.json")"
grep -q '"entry"\|"sources"\|"kind"' "$w/kama.json" && bad "a monorepo root should be a pure aggregator" \
                                                   || ok "the root is a pure aggregator, with no kind of its own"
[ -f "$w/engine/src/engine.kama" ] && [ -f "$w/server/src/server.kama" ] \
    && ok "each member is seeded as a library" || bad "a member is missing its source"

# THE assertion. server imports engine and returns its answer, so the exit code proves the whole chain:
# the member manifests are right, `sources` makes engine importable, and the root composes them.
# Deleting `"sources"` from engine/kama.json makes this fail with `cannot resolve module 'engine'` —
# which is exactly why seed emits that key for every kind that has files.
( cd "$w/server" && "$KAMA" pkg add engine --path ../engine ) >/dev/null 2>&1 \
    || bad "pkg add of a sibling failed"
printf 'import engine::{ answer };\nfn int32 main() { return answer(); }\n' > "$w/server/src/server.kama"
# ⚠️ Built from INSIDE the member, not by path from here. projectManifestDir looks only in the input
# file's own directory and then the CWD — it does not walk up the way owningPackageDir does — so
# `kama build ws/server/src/server.kama` from out here cannot find ws/server/.kama/deps and fails with
# `cannot resolve module 'engine'`. Pre-existing, and unrelated to seeding; noted in ROADMAP_DETAIL §10.
( cd "$w/server" && "$KAMA" build src/server.kama -o "$tmp/srv" ) >"$tmp/ws.out" 2>&1 || {
    bad "the composed monorepo did not build:"; sed 's/^/    /' "$tmp/ws.out" >&2; }
if [ -x "$tmp/srv" ]; then
    if "$tmp/srv"; then rc=0; else rc=$?; fi
    [ "$rc" = 42 ] && ok "a member imports its sibling (exit 42)" \
                   || bad "the composed program returned $rc, expected 42"
fi

# ---------------------------------------------------------------------------------------------------
echo "check-seed: seeding never half-writes"

# An existing manifest stops it dead, and --force must NOT be able to authorize that: a manifest carries
# dependencies, a toolchain pin and a flag universe nothing here could reconstruct.
"$KAMA" seed "$e" --yes >"$tmp/re.out" 2>&1 && bad "re-seeding an existing project succeeded" \
                                            || ok "an existing kama.json stops the seed"
grep -q 'already a kama project' "$tmp/re.out" && ok "and says why" || bad "unclear re-seed error"

# Remove ONLY the manifest, so the refusal has to come from the pre-flight rather than that check. A
# naive per-file writer rewrites kama.json and then bails on src/app.kama — this catches exactly that.
rm "$e/kama.json"
before=$(shasum "$e/README.md" | cut -d' ' -f1)
"$KAMA" seed "$e" --yes >/dev/null 2>&1 && bad "a colliding seed succeeded" || ok "a colliding seed is refused"
[ -f "$e/kama.json" ] && bad "the refused seed still rewrote kama.json (no pre-flight)" \
                      || ok "the refused seed wrote nothing at all"
[ "$(shasum "$e/README.md" | cut -d' ' -f1)" = "$before" ] && ok "existing files are untouched" \
                                                           || bad "a refused seed modified README.md"
"$KAMA" seed "$e" --yes --force >/dev/null 2>&1 && ok "--force overwrites" || bad "--force failed"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: a rejected argument writes nothing at all"

# Each of these must exit 2 AND leave no directory behind. The monorepo case is the sharp one: its third
# member is bad, so two members would already have been written by anything that validates as it goes.
n=0
reject() {   # reject <label> <args...>
    n=$((n + 1))
    d="$tmp/rej$n"
    shift_label=$1; shift
    "$KAMA" seed "$d" --yes "$@" >"$tmp/rej$n.out" 2>&1 && {
        bad "$shift_label was accepted"; return; }
    [ -e "$d" ] && bad "$shift_label left $d behind" || ok "$shift_label is rejected, writing nothing"
}
reject "a hyphenated library name"   --kind library --name my-lib
reject "an uppercase name"           --name BadName
reject "a name with a space"         --name 'Bad Name'
reject "a keyword as a library name" --kind library --name return
reject "a library named std"         --kind library --name std
reject "a two-part version"          --version 1.2
reject "an unknown kind"             --kind nonsense
reject "a monorepo with no members"  --kind monorepo
reject "--members on a library"      --kind library --members a,b
reject "a bad THIRD member"          --kind monorepo --members a,BAD-2,c
reject "a duplicate member"          --kind monorepo --members a,b,a

# A hyphen is refused for an EXECUTABLE too. It used to be allowed — "nothing imports it, so the
# identifier rule does not apply" — but a project's name is its ROOT NAMESPACE for both kinds, and an
# executable's own symbols are qualified by it, so `my-app` is no more spellable there than in an import.
reject "a hyphenated executable name" --kind executable --name my-app

# A monorepo ROOT is the one name still exempt: it aggregates members and has no namespace of its own.
"$KAMA" seed "$tmp/hyph" --yes --kind monorepo --name my-repo --members a,b >/dev/null 2>&1 \
    && ok "a hyphenated MONOREPO root name is allowed" || bad "a hyphenated monorepo name was refused"

# The rejection has to teach, not just refuse.
grep -q 'root namespace' "$tmp/rej1.out" && ok "the name error explains why (root namespace)" \
                                         || bad "the name error does not mention the root namespace"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: agent guidance is opt-in"

g="$tmp/ag"
"$KAMA" seed "$g" --yes --claude >/dev/null 2>&1 || bad "seed --claude failed"
[ -f "$g/AGENTS.md" ] && [ -f "$g/CLAUDE.md" ] && ok "--claude writes AGENTS.md + CLAUDE.md" \
                                               || bad "--claude did not write the agent files"
h="$tmp/noag"
"$KAMA" seed "$h" --yes >/dev/null 2>&1
[ -f "$h/AGENTS.md" ] && bad "a plain seed wrote AGENTS.md; it is opt-in" \
                      || ok "a plain seed writes no AGENTS.md"

# ---------------------------------------------------------------------------------------------------
echo "check-seed: template hygiene"

# Only the two documented placeholders may appear, or a template silently ships a literal token.
strays=$(grep -rho 'KAMA_SEED_[A-Z_]*' "$ROOT/seed" | sort -u | grep -v '^KAMA_SEED_NAME$' | grep -v '^KAMA_SEED_IDENT$' || true)
[ -z "$strays" ] && ok "seed/ uses only KAMA_SEED_NAME and KAMA_SEED_IDENT" \
                 || bad "unknown placeholder(s) in seed/: $strays"

# DOTLESS on purpose: a literal seed/.gitignore would be a real gitignore governing seed/.
[ -f "$ROOT/seed/gitignore" ] && ok "seed/gitignore exists" || bad "seed/gitignore is missing"
[ -e "$ROOT/seed/.gitignore" ] && bad "seed/.gitignore exists — it would govern seed/ for real" \
                               || ok "seed/.gitignore does not exist"

# The .kama templates must be valid kama on their own. check-treesitter's corpus covers parsing; this
# covers the compiler agreeing, which is the claim that lets them be seeded unmodified.
for t in "$ROOT/seed/app.kama" "$ROOT/seed/lib.kama"; do
    "$KAMA" check "$t" >/dev/null 2>&1 && ok "$(basename "$t") analyzes clean as it sits" \
                                       || bad "$(basename "$t") does not analyze"
done

if [ "$fail" != 0 ]; then echo "check-seed: FAILED" >&2; exit 1; fi
echo "check-seed: OK"
