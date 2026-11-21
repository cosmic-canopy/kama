#!/bin/sh
# check-obj-cache.sh — the per-TU object cache reuses what it may and NOTHING else (KR-2).
#
# What it guards. A kama build re-emits every TU's C and compiles all of it, so an edit to one file paid for
# the whole program: measured on a 61-TU project, `kama check` is 0.09 s and the build 0.35 s — the C
# toolchain is ~74 % of it. A `zig cc` install already had a content-addressed cache of its own; a slim
# install (system clang or gcc) had none, and that is what `.kama-cache/` beside the output closes.
#
# ⚠️ The dangerous direction is REUSE, not recompilation: a cache that keeps an object it should have thrown
# away is a silent miscompile, and it looks exactly like a working build. So the assertions below are mostly
# about what must MISS. The one that matters most is the FFI header (case 4): the emitted .c does not change
# when a user's `extern "…"` header does, so a cache keyed on the .c alone would happily link yesterday's
# object. kama compiles with `-MMD` and re-hashes every file the compiler names, which is why that case
# passes — it is the whole reason for the dependency list.
#
# Mtimes, not a compiler shim, decide "was this recompiled": the real toolchain has to run for `-MMD` to
# write a dependency list at all, and a shim that cannot produce one would test a different program.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-obj-cache: $KAMA not built" >&2; exit 1; fi

# ⚠️ The cache lives on the PER-TU compile path, and `-j 1` is not it: there the driver hands the compiler
# every source in ONE invocation that also links, so there are no per-TU objects to keep or to reuse. That is
# the right shape for a unity build and the reason zig clamps to it (zig brings its own cache). The guard pool
# exports KAMA_BUILD_JOBS=1 for its own reasons, so this guard sets its own width — without it every
# assertion below fails for a reason that is not about the cache.
KAMA_BUILD_JOBS=2
export KAMA_BUILD_JOBS

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
bad() { echo "check-obj-cache: FAIL — $1" >&2; fail=1; }
ok()  { echo "  ok: $1"; }

# A project with three units: `main` calls into `lib`, and `spare` is untouched by every edit below.
mkdir -p "$TMP/p/src"
cat > "$TMP/p/kama.json" <<'JSON'
{ "name": "oc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "modules": { ".": { "visibility": "internal" } } }
JSON
printf 'export { bump };\n\nfn int32 bump(int32 x) { return x + 1; }\n'        > "$TMP/p/src/lib.kama"
printf 'export { spare };\n\nfn int32 spare(int32 x) { return x * 2; }\n'      > "$TMP/p/src/spare.kama"
printf 'import { oc::bump };\n\nfn uint8 main() { return cast<uint8>(bump(x: 6)); }\n'      > "$TMP/p/src/main.kama"

build() { ( cd "$TMP/p" && "$KAMA" build kama.json -o out "$@" ) >"$TMP/build.log" 2>&1 \
          || { sed 's/^/    /' "$TMP/build.log" >&2; bad "the build failed ($*)"; return 1; }; }

# Which objects a build REWROTE: a marker file touched just before it, then POSIX `find -newer`. Not
# `ls --time-style`, which is GNU-only — this repo is built on macOS too, where that flag does not exist.
# The sleeps straddle the marker because a file system's timestamp granularity can be a whole second.
mark()      { sleep 1; : > "$TMP/mark"; sleep 1; }
rewritten() { find "${1:-$TMP/p/.kama-cache}" -name '*.o' -newer "$TMP/mark" -print 2>/dev/null \
              | sed 's|.*/||' | sort | tr '\n' ' '; }

build || exit 1
[ -d "$TMP/p/.kama-cache" ] || bad "no cache directory after a build"
[ -n "$(find "$TMP/p/.kama-cache" -name '*.o' -print)" ] || bad "the cache holds no objects after a build"

# 1. Nothing changed: every object is reused.
mark
build || exit 1
got=$(rewritten)
if [ -z "$got" ]; then ok "an unchanged rebuild recompiles nothing"
else bad "an unchanged rebuild recompiled: $got"; fi

# 2. A BODY edit in one unit recompiles that unit — and leaves the untouched one alone. A body edit does not
#    change the shared header, which is what makes this the common dev-loop case.
mark
sed 's/return x + 1;/return x + 4;/' "$TMP/p/src/lib.kama" > "$TMP/lib.new" && mv "$TMP/lib.new" "$TMP/p/src/lib.kama"
build || exit 1
got=$(rewritten)
case "$got" in
    *lib.o*) case "$got" in *spare.o*) bad "a body edit recompiled an untouched unit: $got" ;;
                            *) ok "a body edit recompiles its own unit and no other ($got)" ;; esac ;;
    *) bad "a body edit did not recompile the unit it changed (rewrote: ${got:-nothing})" ;;
esac
rc=0; ( cd "$TMP/p" && ./out ) || rc=$?
[ "$rc" = 10 ] || bad "the rebuilt binary exited $rc, expected 10 — the edit did not reach the program"
[ "$rc" = 10 ] && ok "the rebuilt binary carries the edit (exit 10)"

# 3. A compile FLAG change misses: the exact command is part of the stamp, so a project that adds a `-D`
#    cannot keep objects compiled without it. (Not `--release` — that folds the program into one unity TU,
#    so there are no per-TU objects to reuse or to keep, and it would assert nothing about the cache.)
mark
cat > "$TMP/p/kama.json" <<'JSON'
{ "name": "oc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "cflags": ["-DOC_CACHE_PROBE=1"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
build || exit 1
got=$(rewritten)
[ -n "$got" ] || bad "a build with an added -D reused objects compiled without it"
[ -n "$got" ] && ok "a compile-flag change recompiles"

# 4. ⚠️ THE ONE THAT MATTERS: an edited FFI header the emitted C never mentions differently. A cache keyed on
#    the .c alone reuses a stale object here, and the program silently keeps the old answer.
mkdir -p "$TMP/h"
cat > "$TMP/h/vals.h" <<'H'
#ifndef VALS_H
#define VALS_H
#include <stdint.h>
static inline int32_t vals_answer(void) { return 11; }
#endif
H
cat > "$TMP/h/main.kama" <<'KAMA'
extern "vals.h";
extern fn int32 vals_answer();
unsafe fn int32 go() { return vals_answer(); }
fn uint8 main() { return cast<uint8>(go()); }
KAMA
( cd "$TMP/h" && "$KAMA" build main.kama -o app ) >/dev/null 2>&1 || bad "the FFI build failed"
rc=0; ( cd "$TMP/h" && ./app ) || rc=$?
[ "$rc" = 11 ] || bad "the FFI program exited $rc, expected 11"
sleep 1
sed 's/return 11;/return 22;/' "$TMP/h/vals.h" > "$TMP/vals.new" && mv "$TMP/vals.new" "$TMP/h/vals.h"
( cd "$TMP/h" && "$KAMA" build main.kama -o app ) >/dev/null 2>&1 || bad "the FFI rebuild failed"
rc=0; ( cd "$TMP/h" && ./app ) || rc=$?
if [ "$rc" = 22 ]; then ok "an edited C header recompiles what includes it (-MMD dependency list)"
else bad "an edited C header did NOT invalidate the cached object — the program still answers $rc, not 22"; fi

# 5. `--no-cache` compiles without consulting or writing the cache.
mark
build --no-cache || exit 1
got=$(rewritten)
[ -z "$got" ] || bad "--no-cache wrote to the cache: $got"
[ -z "$got" ] && ok "--no-cache leaves the cache untouched"

# 6. A project's `kama run` builds where `kama build` builds (KR-433), so the two share one cache: a second run
#    recompiles nothing, and a build after a run reuses the run's objects. Only what is launched is the run's own, in
#    a directory of its own, so no executable a run starts is ever in `out/`. Through 0.9.1017 a run built everything
#    in that private directory with the cache off, and compiled every unit on every run.
cp -R "$TMP/p" "$TMP/r" && rm -rf "$TMP/r/out" "$TMP/r/.kama-cache"
run() { rc=0; ( cd "$TMP/$1" && shift && "$KAMA" run kama.json "$@" ) >"$TMP/run.log" 2>&1 || rc=$?
        [ "$rc" = 10 ] || { sed 's/^/    /' "$TMP/run.log" >&2; bad "a project's run exited $rc, expected 10"; }; }
exes() { find "$1" -type f \( -name oc -o -name oc.exe \) -print 2>/dev/null; }
run r
cached=$(find "$TMP/r/out" -name '*.o' -print 2>/dev/null)
[ -n "$cached" ] || bad "a project's run left no objects in its out/ cache"
[ -z "$(exes "$TMP/r/out")" ] || bad "a project's run left its executable in out/: $(exes "$TMP/r/out")"
mark
run r
got=$(rewritten "$TMP/r/out")
if [ -n "$cached" ] && [ -z "$got" ]; then ok "a project's second run recompiles nothing"
elif [ -n "$got" ]; then bad "a project's second run recompiled: $got"; fi
mark
( cd "$TMP/r" && "$KAMA" build kama.json ) >"$TMP/build.log" 2>&1 || { sed 's/^/    /' "$TMP/build.log" >&2; bad "the build after a run failed"; }
got=$(rewritten "$TMP/r/out")
if [ -z "$got" ]; then ok "a build after a run reuses the run's objects"
else bad "a build after a run recompiled: $got"; fi

# 6b. Runs of one project at once take turns (KR-433). They share `out/`, where every generated file and cached object
#     has a fixed name, so without the build directory's lock they write, compile and remove each other's files. Four
#     at once from a cold cache, then a fifth that must find every object made.
cp -R "$TMP/p" "$TMP/c" && rm -rf "$TMP/c/out" "$TMP/c/.kama-cache"
for k in 1 2 3 4; do ( rc=0; ( cd "$TMP/c" && "$KAMA" run kama.json ) >"$TMP/c$k.log" 2>&1 || rc=$?; echo "$rc" > "$TMP/c$k.rc" ) & done
wait
for k in 1 2 3 4; do
    [ "$(cat "$TMP/c$k.rc")" = 10 ] || { sed 's/^/    /' "$TMP/c$k.log" >&2; bad "run $k of four at once exited $(cat "$TMP/c$k.rc"), expected 10"; }
done
waited=$(cat "$TMP"/c[1-4].log | grep -c 'waiting for another kama' || true)
mark
run c
got=$(rewritten "$TMP/c/out")
if [ -z "$got" ]; then ok "four runs of one project at once all pass and leave a whole cache ($waited waited)"
else bad "after four runs at once, the next one recompiled: $got"; fi

# 7. `--no-cache` keeps a run wholly private (KR-433): nothing is written into the project.
cp -R "$TMP/p" "$TMP/n" && rm -rf "$TMP/n/out" "$TMP/n/.kama-cache"
run n --no-cache
[ ! -e "$TMP/n/out" ] && ok "a --no-cache run writes nothing into the project"
[ -e "$TMP/n/out" ] && bad "a --no-cache run wrote into the project: $(find "$TMP/n/out" -print | head -5 | tr '\n' ' ')"

# 8. A loose file is not a project and has no out/ (KR-433): its run writes nothing beside it.
mkdir -p "$TMP/l" && printf 'fn uint8 main() { return 10; }\n' > "$TMP/l/ten.kama"
rc=0; ( cd "$TMP/l" && "$KAMA" run ten.kama ) >"$TMP/run.log" 2>&1 || rc=$?
[ "$rc" = 10 ] || { sed 's/^/    /' "$TMP/run.log" >&2; bad "a loose file's run exited $rc, expected 10"; }
left=$(ls -A "$TMP/l" | grep -v '^ten\.kama$' || true)
[ -z "$left" ] && ok "a loose file's run writes nothing beside it"
[ -z "$left" ] || bad "a loose file's run left: $left"

[ "$fail" -eq 0 ] || exit 1
echo "check-obj-cache: PASS (reuses an unchanged TU, recompiles an edited one, and misses on flags and C headers)"
