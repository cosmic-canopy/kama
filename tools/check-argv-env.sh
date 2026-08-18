#!/bin/sh
# check-argv-env.sh — Task #2 guard: hosted command-line argv + environment access in the prelude floor.
# The standard harness (run_tests.sh) runs fixtures with NO args and doesn't set env, so it can only reach
# the empty paths (covered by tests/args_env_empty.kama). This dedicated runner drives the WITH-args /
# SET-env paths that only a controlled invocation can:
#   1. SHAPE — the synthesized hosted entry now stashes argv via kama_args_init (regression guard for the
#      kama.cemit edit; the whole feature is dead if this call is dropped).
#   2. DIRECT BINARY — build the probe and run it with real argv + a real env var; the fixture self-checks
#      and returns 0 on success (a distinct nonzero code otherwise — see tests/support/argv_env_probe.kama).
#   3. `kama run -- <args>` — proves the driver's `--` passthrough (kama.driver.cpp) reaches the child argv.
# Fails (exit 1) with a diagnostic if any property breaks. Run standalone or from run_tests.sh. Native only
# (runs a native binary + needs a controllable process environment).
#
# check-legs: native san
#
# The marker above is read by tools/run-checks.sh. This is the ONE guard that runs on more than the native
# leg, and it is not redundant there: under KAMA_SAN the script builds the probe WITH sanitizers, so the
# owned-string copies made from real argv/environ are covered by ASan — which the argument-free fixtures
# (tests/args_env_empty.kama) can never reach. Skipped on wasm: it runs a native binary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/support/argv_env_probe.kama"

if [ ! -x "$KAMA" ]; then echo "check-argv-env: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-argv-env: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cfile="$tmp/probe.c"
bin="$tmp/probe"

# 1. SHAPE — transpile and confirm the hosted entry stashes argv. Guards the cemit edit from regressing.
"$KAMA" transpile "$FIXTURE" -o "$cfile" >/dev/null
if ! grep -q 'kama_args_init(argc, argv)' "$cfile"; then
    echo "check-argv-env: FAIL — synthesized hosted main does not call kama_args_init(argc, argv)" >&2; exit 1
fi

# 2. DIRECT BINARY — build, then run with real argv + a real env var. The probe returns 0 iff every
#    argv/env check passes; a nonzero code identifies the first failure. Under KAMA_SAN, build with
#    ASan+UBSan so the owned-string copies in kama_args_at/kama_env_lookup are checked WITH real args
#    (run_tests.sh already exports ASAN_OPTIONS/UBSAN_OPTIONS; -fno-sanitize=function per its rationale).
if [ "${KAMA_SAN:-0}" != "0" ]; then
    build_ok=1
    "$KAMA" build "$FIXTURE" -o "$bin" \
        --cc "clang -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer -g" \
        >/dev/null 2>"$tmp/build.err" || build_ok=0
else
    build_ok=1
    "$KAMA" build "$FIXTURE" -o "$bin" >/dev/null 2>"$tmp/build.err" || build_ok=0
fi
if [ "$build_ok" -ne 1 ]; then
    echo "check-argv-env: FAIL — build failed" >&2; sed 's/^/  /' "$tmp/build.err" >&2; exit 1
fi
set +e
KAMA_PROBE_VAR=probe-value "$bin" alpha beta gamma
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    echo "check-argv-env: FAIL — direct-binary argv/env probe returned $rc (see check number in argv_env_probe.kama)" >&2
    exit 1
fi

# 3. `kama run -- <args>` — the driver builds a temp binary and forwards `-- <args>` to its argv. Same probe,
#    same env; proves the passthrough (inert before this task) now delivers args end-to-end.
set +e
KAMA_PROBE_VAR=probe-value "$KAMA" run "$FIXTURE" -- alpha beta gamma >/dev/null 2>"$tmp/run.err"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    echo "check-argv-env: FAIL — 'kama run -- alpha beta gamma' probe returned $rc" >&2
    sed 's/^/  /' "$tmp/run.err" >&2; exit 1
fi

# 4. CROSS-TU — argv is process-global (external linkage), so a prelude floor `args()` call from a NON-entry
#    TU (a library module) must see the same vector as `main`. A per-TU `static` argv would read empty in the
#    library TU. Two-file build; the program returns 0 iff the library-side count matches main's (== 3 here).
libdir="$tmp/xtu"
mkdir -p "$libdir"
cat > "$libdir/lib.kama" <<'KAMA'
namespace Lib;
export { libArgCount };
fn int32 libArgCount() { return args().count(); }
KAMA
cat > "$libdir/main.kama" <<'KAMA'
import Lib::{libArgCount};
fn int32 main() {
    int32 mc = args().count();     // main's TU
    int32 lc = libArgCount();      // Lib's TU — must see the SAME argv
    if (mc != lc) { return 3; }    // cross-TU mismatch (the bug)
    if (mc != 3)  { return 4; }    // wrong count entirely
    return 0;
}
KAMA
"$KAMA" build "$libdir/main.kama" "$libdir/lib.kama" -o "$libdir/prog" >/dev/null 2>"$tmp/xtu.err" || {
    echo "check-argv-env: FAIL — cross-TU build failed" >&2; sed 's/^/  /' "$tmp/xtu.err" >&2; exit 1; }
set +e
"$libdir/prog" alpha beta gamma
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    echo "check-argv-env: FAIL — a library-TU args() saw a different argv than main (cross-TU, code $rc)" >&2; exit 1
fi

echo "PASS check-argv-env (hosted argv + env: kama_args_init shape + direct-binary run + 'kama run --' forwarding + cross-TU args())"
