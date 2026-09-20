#!/bin/sh
# check-clean-tree.sh — a build writes ONLY inside its output directory.
#
# Why this guard exists. For years the compiler dropped generated C next to whatever source it was
# handed, and the repo coped by GROWING ITS .gitignore: a blanket `*.c` / `*.wasm` / `*.html` / `*.js`,
# a `tests/*` allowlist inversion (a compiled fixture is a stem with no extension, and no glob matches
# one), and six scattered `!` rescues to un-ignore the real sources those blankets swallowed. The
# rescues were not complete — `examples/httpd/public/index.html` is hand-written, is linked from that
# example's README, and was silently untracked the whole time because `*.html` hid it.
#
# An ignore rule does not fix pollution, it hides it. This guard is the replacement: it asserts the
# emitter's actual invariant, so the ignore file never has to grow again. If it fails, find what wrote
# the file — do NOT add a pattern to .gitignore.
#
# What it holds down:
#   1. A manifest-less `kama build x.kama -o DIR/x` writes the binary and NOTHING else.
#   2. A manifest-less build with no `-o` writes beside the source (documented behavior — see
#      docs/targets.md) but still leaves no generated .c behind.
#   3. A FAILING build cleans up too. This is the one that actually regressed: the cleanup used to sit
#      on the success path, and the .c was registered for removal only AFTER the call that wrote it, so
#      every failed compile leaked. 271 stale .c files (~72 KB each) had accumulated in tests/xfail/,
#      which is the corpus where every fixture is *meant* to fail.
#   4. `--keep-c` still keeps the C. The fix must not turn into "always delete".
#   5. A multi-unit build confines its per-module .c + .gen.h to the output directory.
#
# Hermetic on purpose: everything happens in a private mktemp dir, so this says nothing about the
# developer's own untracked scratch files and cannot fail because of them. It reaches the compiler
# through $KAMA (run-checks.sh exports an absolute one) rather than the ./kama symlink, which
# check-no-inheritance.sh repoints while it builds.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu
exec </dev/null

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-clean-tree: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

bad() { echo "check-clean-tree: FAIL — $1" >&2; fail=1; }

# `ls -A` of a directory, sorted — used where the expected content is exact (a SOURCE directory).
listing() { (cd "$1" && ls -A | LC_ALL=C sort | tr '\n' ' '); }

expect_listing() {
    _dir=$1; _want=$2; _what=$3
    _got=$(listing "$_dir")
    [ "$_got" = "$_want" ] || bad "$_what
    expected: $_want
    actual:   $_got"
}

# The output directory is checked for the ABSENCE of intermediates rather than by exact listing.
# What legitimately lands there includes an `app.dSYM` bundle: on macOS a debug build's DWARF lives in
# a bundle beside the binary, not in the executable. It used to appear only at -j1, where the driver
# hands clang one combined compile+link command and clang runs dsymutil itself; a parallel build
# compiled to .o and linked those, and got none — which meant its debug map pointed at objects the
# build had deleted and the binary could not be debugged at all. The driver now consolidates on that
# path too, so the bundle appears at EVERY -j. Debug symbols in the output directory are correct; a
# leftover .c is not.
# ⚠️ `.kama-cache/` is the ONE thing a build leaves behind on purpose (KR-2): the per-TU object cache, which
# is what makes a rebuild skip the compiles whose input did not change. It is scoped to the output binary and
# lives inside the output directory, so the rule this guard exists for is unchanged — a build still writes
# nothing outside `dirname(-o)`, and nothing at all into the SOURCE tree. `--no-cache` restores the old
# behaviour exactly, and case 6 below holds that down: with it, not even the cache directory appears.
expect_no_intermediates() {
    _dir=$1; _what=$2
    _got=$(find "$_dir" -name '.kama-cache' -prune -o \( -name '*.c' -o -name '*.gen.h' -o -name '*.o' -o -name '*.c.tmp' \) -print | tr '\n' ' ')
    [ -z "$_got" ] || bad "$_what
    leftover intermediates: $_got"
}

# A fixture that compiles, and one that is REJECTED by the front end (tests/xfail is the corpus of
# things that must not compile). Copied out of the tree so nothing here writes into the worktree.
GOOD="$ROOT/tests/array_sum.kama"
BAD="$ROOT/tests/xfail/use_after_move.kama"
[ -f "$GOOD" ] || { echo "check-clean-tree: missing fixture $GOOD" >&2; exit 1; }
[ -f "$BAD"  ] || { echo "check-clean-tree: missing fixture $BAD"  >&2; exit 1; }

# 1. -o into a subdirectory: the binary lands there and nothing else does. The SOURCE directory is
#    untouched — the property that lets tests/ hold 1400 fixtures with no ignore rules.
mkdir -p "$tmp/c1/src" "$tmp/c1/dest"
cp "$GOOD" "$tmp/c1/src/app.kama"
( cd "$tmp/c1" && "$KAMA" build src/app.kama -o dest/app >/dev/null 2>&1 ) \
    || bad "case 1: a good build failed"
expect_listing "$tmp/c1/src"  "app.kama " "case 1: the source directory gained a file"
expect_no_intermediates "$tmp/c1/dest" "case 1: the output directory kept a generated intermediate"

# 2. No -o at all. docs/targets.md is explicit that a loose file with no manifest builds beside its
#    source, so the binary IS expected here — but the generated .c is not.
mkdir -p "$tmp/c2"
cp "$GOOD" "$tmp/c2/app.kama"
( cd "$tmp/c2" && "$KAMA" build app.kama >/dev/null 2>&1 ) || bad "case 2: a good build failed"
expect_no_intermediates "$tmp/c2" "case 2: a no--o build left an intermediate beside the source"
[ -x "$tmp/c2/app" ] || bad "case 2: a no--o build did not produce the binary beside its source"

# 3. THE REGRESSION. A build that fails must clean up exactly like one that succeeds.
mkdir -p "$tmp/c3"
cp "$BAD" "$tmp/c3/bad.kama"
if ( cd "$tmp/c3" && "$KAMA" build bad.kama -o bad >/dev/null 2>&1 ); then
    bad "case 3: the xfail fixture COMPILED — pick another, this case proves nothing now"
fi
expect_listing "$tmp/c3" "bad.kama " "case 3: a FAILED build leaked its generated C (the 271-file bug)"

# 3b. Same, with no -o: the shape a user hits when they run `kama build app.kama` and it does not
#     compile. This is the one that littered somebody's working directory.
mkdir -p "$tmp/c3b"
cp "$BAD" "$tmp/c3b/bad.kama"
( cd "$tmp/c3b" && "$KAMA" build bad.kama >/dev/null 2>&1 ) || true
expect_listing "$tmp/c3b" "bad.kama " "case 3b: a FAILED no--o build leaked its generated C"

# 4. --keep-c is a promise: the C stays. A cleanup fix that ignores the flag would break the one
#    workflow the README advertises (drop the emitted C into an existing C codebase).
mkdir -p "$tmp/c4"
cp "$GOOD" "$tmp/c4/app.kama"
( cd "$tmp/c4" && "$KAMA" build app.kama -o app --keep-c >/dev/null 2>&1 ) \
    || bad "case 4: --keep-c build failed"
if [ -z "$(find "$tmp/c4" -name '*.c' -print -quit)" ]; then
    bad "case 4: --keep-c kept no .c at all"
fi

# 5. Multi-unit: per-module .c and the shared .gen.h are named off the OUTPUT, and must land in the
#    output directory rather than beside whichever module they came from.
#
#    The sources are written HERE rather than copied from a fixture. This used to `cp
#    tests/mod_basic.d/*.kama` and broke the moment that fixture grew a src/ tree — which says the
#    dependency was wrong, not the migration: what this case is about is where intermediates land, and
#    that needs three units and nothing else. Borrowing a fixture's layout coupled it to a shape it
#    never cared about.
mkdir -p "$tmp/c5/src" "$tmp/c5/dest"
#    The three units are INDEPENDENT — no cross-file calls — which is deliberate twice over: a file with
#    no `namespace` is file-private today, so a call across them would not resolve at all; and this case
#    wants three translation units, not a dependency graph.
printf 'fn int32 twice(int32 v) { return v * 2; }\n'  > "$tmp/c5/src/math.kama"
printf 'fn int32 area(int32 w) { return w * w; }\n'   > "$tmp/c5/src/shapes.kama"
printf 'fn int32 main() { return 0; }\n'              > "$tmp/c5/src/main.kama"
if ( cd "$tmp/c5" && "$KAMA" build src/*.kama -o dest/app >/dev/null 2>&1 ); then
    expect_listing "$tmp/c5/src" "main.kama math.kama shapes.kama " \
        "case 5: a multi-unit build wrote intermediates next to the modules"
    expect_no_intermediates "$tmp/c5/dest" \
        "case 5: the output directory kept per-module .c / .gen.h"
else
    bad "case 5: the multi-unit build failed"
fi

# 6. `--no-cache`: the object cache is the one thing a build leaves behind (KR-2), and this is the way out.
#    With it the output directory holds the binary and NOTHING else — not the intermediates, and not the
#    cache directory either. A default that cannot be turned off is a default nobody can debug around.
#    ⚠️ Asserted as the ABSENCE of the cache and of intermediates, NOT as an exact listing. An exact
#    listing also forbids `app.dSYM`, which is legitimate output — macOS keeps a debug build's DWARF in
#    a bundle beside the binary, inside `dirname(-o)` where this guard's whole rule already allows it.
#    It was an exact listing when it was written, and that only ever passed when the guard was run BY
#    HAND: `tools/run-checks.sh` exports KAMA_BUILD_JOBS=1, which hands clang one combined compile+link
#    command, and clang runs dsymutil itself. Measured at -j1 against a driver that emits no dSYM of its
#    own: the bundle is there. It is now there at every -j, since the driver consolidates the debug map
#    on the per-TU path too (a Mach-O debug map pointing at deleted objects is not debuggable).
mkdir -p "$tmp/c6"
printf 'fn int32 main() { return 0; }\n' > "$tmp/c6/app.kama"
if ( cd "$tmp/c6" && "$KAMA" build app.kama --no-cache -o app >/dev/null 2>&1 ); then
    [ -e "$tmp/c6/.kama-cache" ] && bad "case 6: --no-cache still wrote the object cache"
    expect_no_intermediates "$tmp/c6" "case 6: the --no-cache build left intermediates behind"
else
    bad "case 6: the --no-cache build failed"
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "check-clean-tree: PASS (a build writes only its output: -o, no--o, FAILED, --keep-c, multi-unit, --no-cache)"
