#!/bin/sh
# check-compilefor.sh — conditional-compilation guard for the `@compileFor(FLAG)` decl gate. The whole
# point of the feature is that a gated-out declaration is dropped BY THE KAMA COMPILER — its symbol
# never exists, and NO C `#ifdef` reaches the emitted output (the Kama compiler does the selection). We
# prove that on the transpiled C rather than trusting the exit code alone, by building the SAME fixture
# two ways and asserting the gated body appears in exactly one:
#   1. DEBUG build   — the `@compileFor(DEBUG)` body (a distinctive `= 251;`) IS present.
#   2. RELEASE build — that body is GONE, and the `@compileFor(!DEBUG)` fallback (`= 7;`) is present.
#      (A C `#ifdef` scheme would emit BOTH bodies; seeing exactly one proves Kama-level selection.)
#   3. The RELEASE binary actually returns 7 (end-to-end; the ordinary fixture run covers debug -> 251).
# tests/compilefor_mode.kama also runs as a normal fixture (its .expect asserts the debug exit, 251).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/compilefor_mode.kama"

if [ ! -x "$KAMA" ]; then echo "check-compilefor: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-compilefor: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
dbgc="$tmp/dbg.c"
relc="$tmp/rel.c"
relbin="$tmp/rel"

# 1. DEBUG — the `@compileFor(DEBUG)` body is kept.
"$KAMA" transpile --no-line "$FIXTURE" -o "$dbgc" >/dev/null
if ! grep -q '= 251;' "$dbgc"; then
    echo "check-compilefor: FAIL — DEBUG build dropped the @compileFor(DEBUG) body (expected '= 251;')" >&2
    exit 1
fi

# 2. RELEASE — that body is dropped; the @compileFor(!DEBUG) fallback survives.
"$KAMA" transpile --no-line --release "$FIXTURE" -o "$relc" >/dev/null
if grep -q '= 251;' "$relc"; then
    echo "check-compilefor: FAIL — RELEASE build STILL contains the @compileFor(DEBUG) body ('= 251;'):" >&2
    echo "  a gated-out decl must not reach the emitted C (no #ifdef — Kama does the selection)" >&2
    exit 1
fi
if ! grep -q 'kama_ret_0 = 7;' "$relc"; then
    echo "check-compilefor: FAIL — RELEASE build is missing the @compileFor(!DEBUG) fallback ('= 7;')" >&2
    exit 1
fi

# 3. RELEASE end-to-end — the fallback build returns 7.
"$KAMA" build --release "$FIXTURE" -o "$relbin" >/dev/null
if "$relbin"; then rc=0; else rc=$?; fi
if [ "$rc" != 7 ]; then
    echo "check-compilefor: FAIL — RELEASE binary returned $rc, expected 7" >&2
    exit 1
fi

# 3b. THE EXTERN FORMS — a gated `extern "<h>";` must emit NO `#include`, which is the half of the
#     platform seam that lets two backends each own their own C header. Asserted on the transpiled C
#     rather than the exit code because the failure mode is silent: the program links and runs either
#     way, and the only symptom is a header pulled into a build that cannot have it (an emscripten
#     build reaching for <CoreAudio/CoreAudio.h>).
#
#     ⚠️ This checks `kama transpile`, i.e. the SINGLE-TU CEmitter::emit path, deliberately. That path
#     used to run emitIncludes BEFORE collectProgram — and therefore before pruneInactiveDecls — so the
#     gate was a no-op on it while working correctly on the multi-file path. Reproduced before it was
#     fixed: a DEBUG build emitted BOTH headers. Keep the assertion on this path.
efix="$ROOT/tests/compilefor_extern.kama"
[ -f "$efix" ] || { echo "check-compilefor: missing $efix" >&2; exit 1; }
"$KAMA" transpile --no-line "$efix" -o "$tmp/ext_dbg.c" >/dev/null
"$KAMA" transpile --no-line --release "$efix" -o "$tmp/ext_rel.c" >/dev/null
if ! grep -q 'include "stdlib.h"' "$tmp/ext_dbg.c"; then
    echo "check-compilefor: FAIL — DEBUG build dropped the @compileFor(DEBUG) extern header <stdlib.h>" >&2
    exit 1
fi
if grep -q 'include "stdlib.h"' "$tmp/ext_rel.c"; then
    echo "check-compilefor: FAIL — RELEASE build STILL emits the @compileFor(DEBUG) extern header:" >&2
    echo "  a gated-out \`extern \"<h>\";\` must emit no #include (this is the emitIncludes/prune ORDER bug)" >&2
    exit 1
fi
# ...and the gated `extern fn` with it: RELEASE defines its own `abs`, DEBUG calls libc's bare symbol.
if ! grep -q 'k_Fcompilefor_extern__abs(' "$tmp/ext_rel.c"; then
    echo "check-compilefor: FAIL — RELEASE build is missing the @compileFor(!DEBUG) kama fallback for abs" >&2
    exit 1
fi
if grep -q 'k_Fcompilefor_extern__abs(' "$tmp/ext_dbg.c"; then
    echo "check-compilefor: FAIL — DEBUG build kept the kama fallback; the gated \`extern fn\` did not win" >&2
    exit 1
fi

# 4. STRICT VALIDATION — a `kama.json` manifest DECLARES the valid flag universe, so an undeclared
#    `@compileFor(...)` flag is a hard error (typo protection), not a silent drop. Self-contained in a
#    temp dir so it doesn't leave a manifest next to the shared xfail fixtures.
proj="$tmp/proj"
mkdir -p "$proj/src"
cat > "$proj/kama.json" <<'JSON'
{ "name": "strictdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "flags": { "TELEMETRY": {} }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$proj/src/app.kama" <<'KAMA'
@compileFor(TELMETRY) fn int32 typo() { return 1; }
fn int32 main() { return 0; }
KAMA
if "$KAMA" build "$proj/kama.json" -o "$proj/app" >/dev/null 2>"$tmp/strict.err"; then
    echo "check-compilefor: FAIL — strict manifest accepted an undeclared @compileFor flag (TELMETRY)" >&2
    exit 1
fi
if ! grep -qF "undeclared flag" "$tmp/strict.err"; then
    echo "check-compilefor: FAIL — undeclared-flag rejected, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/strict.err" >&2
    exit 1
fi

# 4b. RESERVED NAMES — the build configuration owns DEBUG/RELEASE/HOSTED, the OS_/ARCH_/ABI_ namespaces
#     and every built-in TARGET name. A `flags` entry may not claim one: before targets were triples,
#     `--define WINDOWS` was the platform convention, and silently allowing it back would give a project
#     two different `WINDOWS`es that mean different things.
res="$tmp/reserved"
mkdir -p "$res/src"
cat > "$res/kama.json" <<'JSON'
{ "name": "reserveddemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "flags": { "WINDOWS": {} }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$res/src/app.kama" <<'KAMA'
fn int32 main() { return 0; }
KAMA
if "$KAMA" build "$res/kama.json" -o "$res/app" >/dev/null 2>"$tmp/reserved.err"; then
    echo "check-compilefor: FAIL — a manifest declared the built-in target name WINDOWS as a user flag" >&2
    exit 1
fi
if ! grep -qF "build-configuration name" "$tmp/reserved.err"; then
    echo "check-compilefor: FAIL — reserved-name rejection used an unexpected diagnostic:" >&2
    sed 's/^/  /' "$tmp/reserved.err" >&2
    exit 1
fi

# 4c. TARGET RESOLUTION — a bare NAME comes from the built-in catalog (case-insensitively), anything with
#     a '-' is an anonymous <arch>-<os>-<abi> triple needing no config at all, and an unknown name is a
#     hard error that lists what IS available.
if ! "$KAMA" transpile --no-line "$ROOT/tests/arith.kama" --target aarch64-linux-gnu -o "$tmp/tri.c" >/dev/null 2>&1; then
    echo "check-compilefor: FAIL — an anonymous target triple was rejected" >&2
    exit 1
fi
if "$KAMA" transpile --no-line "$ROOT/tests/arith.kama" --target NOSUCHTARGET -o "$tmp/no.c" >/dev/null 2>"$tmp/tgt.err"; then
    echo "check-compilefor: FAIL — an unknown target name was accepted" >&2
    exit 1
fi
if ! grep -qF "unknown target" "$tmp/tgt.err"; then
    echo "check-compilefor: FAIL — unknown-target rejection used an unexpected diagnostic:" >&2
    sed 's/^/  /' "$tmp/tgt.err" >&2
    exit 1
fi

# 4d. DERIVED FLAGS — selecting a target inserts one flag per triple component, so a gate can name the OS
#     without the project declaring anything. `wasm32-emscripten-none` must satisfy ARCH_WASM32 and not
#     OS_LINUX; `aarch64-linux-gnu` the reverse. This is what makes `@compileFor(OS_LINUX)` work with no
#     manifest, and it is the mechanism the retired NATIVE/EMBEDDED names used to hardcode.
cat > "$tmp/derived.kama" <<'KAMA'
@compileFor(OS_LINUX)  fn int32 pick() { return 1; }
@compileFor(!OS_LINUX) fn int32 pick() { return 2; }
fn int32 main() { return pick(); }
KAMA
"$KAMA" transpile --no-line "$tmp/derived.kama" --target aarch64-linux-gnu -o "$tmp/derived_linux.c" >/dev/null
if ! grep -q 'kama_ret_0 = 1;' "$tmp/derived_linux.c"; then
    echo "check-compilefor: FAIL — a linux triple did not activate the derived OS_LINUX flag" >&2
    exit 1
fi
"$KAMA" transpile --no-line "$tmp/derived.kama" --target wasm32-emscripten-none -o "$tmp/derived_wasm.c" >/dev/null
if ! grep -q 'kama_ret_0 = 2;' "$tmp/derived_wasm.c"; then
    echo "check-compilefor: FAIL — a non-linux triple still activated the derived OS_LINUX flag" >&2
    exit 1
fi

# 4e. SELECT GROUPS — a project declares its own single-select axes under `select`, and `--select
#     GROUP=VALUE` picks one. The value's name joins the active flag set (so no separate `flags` entry is
#     needed), `inherits` pulls the base in with it, and asking for two values of one group is an error —
#     the ambiguity `--define XBOX --define PS5` used to allow silently.
sel="$tmp/sel"
mkdir -p "$sel/src"
cat > "$sel/kama.json" <<'JSON'
{ "name": "seldemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "select": { "BUILD_TYPE": { "FAST": { "inherits": "RELEASE" } },
              "CONSOLE":    { "XBOX": { "default": true }, "PS5": {} } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$sel/src/app.kama" <<'KAMA'
@compileFor(FAST)     fn int32 a() { return 1; }
@compileFor(!FAST)    fn int32 a() { return 0; }
@compileFor(RELEASE)  fn int32 b() { return 20; }
@compileFor(!RELEASE) fn int32 b() { return 0; }
@compileFor(XBOX)     fn int32 c() { return 100; }
@compileFor(PS5)      fn int32 c() { return 200; }
fn int32 main() { return a() + b() + c(); }
KAMA
# default: BUILD_TYPE=DEBUG (built-in), CONSOLE=XBOX (manifest `default: true`) -> 0 + 0 + 100
"$KAMA" build "$sel/kama.json" -o "$sel/dflt" >/dev/null 2>&1
if "$sel/dflt"; then rc=0; else rc=$?; fi
if [ "$rc" != 100 ]; then
    echo "check-compilefor: FAIL — select defaults gave $rc, expected 100 (DEBUG + the manifest's XBOX)" >&2
    exit 1
fi
# --select overrides the manifest default
"$KAMA" build --select CONSOLE=PS5 "$sel/kama.json" -o "$sel/ps5" >/dev/null 2>&1
if "$sel/ps5"; then rc=0; else rc=$?; fi
if [ "$rc" != 200 ]; then
    echo "check-compilefor: FAIL — --select CONSOLE=PS5 gave $rc, expected 200" >&2
    exit 1
fi
# a user BUILD_TYPE inherits its base: FAST is active AND so is RELEASE (and its -O3/strip driver behavior)
"$KAMA" build --select BUILD_TYPE=FAST "$sel/kama.json" -o "$sel/fast" >/dev/null 2>&1
if "$sel/fast"; then rc=0; else rc=$?; fi
if [ "$rc" != 121 ]; then
    echo "check-compilefor: FAIL — --select BUILD_TYPE=FAST gave $rc, expected 121 (FAST + inherited RELEASE + XBOX)" >&2
    exit 1
fi
# single-select really is single
if "$KAMA" build --select CONSOLE=PS5 --select CONSOLE=XBOX "$sel/kama.json" -o "$sel/dup" >/dev/null 2>"$tmp/dup.err"; then
    echo "check-compilefor: FAIL — a single-select group accepted two values" >&2
    exit 1
fi
if ! grep -qF "single-select" "$tmp/dup.err"; then
    echo "check-compilefor: FAIL — two-values rejection used an unexpected diagnostic:" >&2
    sed 's/^/  /' "$tmp/dup.err" >&2
    exit 1
fi
# an undeclared value / group is rejected, not silently ignored
if "$KAMA" build --select CONSOLE=WII "$sel/kama.json" -o "$sel/bad" >/dev/null 2>"$tmp/badval.err"; then
    echo "check-compilefor: FAIL — --select accepted a value the group does not declare" >&2
    exit 1
fi
if "$KAMA" build --select NOSUCHGROUP=X "$sel/kama.json" -o "$sel/bad2" >/dev/null 2>"$tmp/badgrp.err"; then
    echo "check-compilefor: FAIL — --select accepted an undeclared group" >&2
    exit 1
fi

# 5. PLATFORM SEAM — `@compileFor` on a `type` (not just a fn): a contract + per-target gated impls,
#    exactly one surviving. On the native transpile the NativeClock impl is present and the WasmClock
#    impl is wholly absent (struct + ctor + methods + vtable) — proving whole-type selection. The wasm
#    test leg builds the complement (WasmClock kept, NativeClock dropped) and asserts the same exit.
platc="$tmp/platform.c"
"$KAMA" transpile --no-line "$ROOT/tests/compilefor_platform.kama" -o "$platc" >/dev/null
if ! grep -q 'NativeClock' "$platc"; then
    echo "check-compilefor: FAIL — host build dropped the @compileFor(!ARCH_WASM32) type (NativeClock)" >&2
    exit 1
fi
if grep -q 'WasmClock' "$platc"; then
    echo "check-compilefor: FAIL — host build STILL contains the @compileFor(ARCH_WASM32) type (WasmClock);" >&2
    echo "  a gated-out TYPE (struct + methods + vtable) must not reach the emitted C" >&2
    exit 1
fi

# 6. kama.local.json (M5.2) — a gitignored sibling deep-merges over kama.json: a flag it declares extends the
#    valid universe (no longer "undeclared"), and one it marks `default:true` is active. Reuse the strict proj:
#    its kama.json declares only TELEMETRY, so LOCALFLAG is undeclared until the local manifest adds it.
#
# ⚠️ Its OWN project directory, not the strict one it copies. A manifest operand names every file under
# the project's `source` root, so sharing a directory with the strict fixture would compile that
# fixture's `main` alongside this one's — two entry points in one program.
loc="$tmp/local"
mkdir -p "$loc/src"
cat > "$loc/kama.json" <<'JSON'
{ "name": "localdemo", "version": "0.1.0", "kind": "executable", "entry": "src/local.kama",
  "flags": { "TELEMETRY": {} }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$loc/kama.local.json" <<'JSON'
{ "flags": { "LOCALFLAG": { "default": true } } }
JSON
cat > "$loc/src/local.kama" <<'KAMA'
@compileFor(LOCALFLAG) fn int32 gated() { return 42; }
fn int32 main() { return gated(); }
KAMA
localc="$tmp/local.c"
if ! "$KAMA" transpile --no-line "$loc/kama.json" -o "$localc" >/dev/null 2>"$tmp/local.err"; then
    echo "check-compilefor: FAIL — kama.local.json did not declare LOCALFLAG (build rejected it):" >&2
    sed 's/^/  /' "$tmp/local.err" >&2
    exit 1
fi
if ! grep -q 'gated' "$localc"; then
    echo "check-compilefor: FAIL — local default:true flag did not keep the @compileFor(LOCALFLAG) body" >&2
    exit 1
fi
rm -f "$proj/kama.local.json"

# 6b. THE FILE GATE — `file @compileFor(FLAG);` on a unit's first line.
#
# ⚠️ Asserted on the TRANSPILED C, not on an exit code, and for a sharper reason than §1's. A file gate's
# whole job is that the excluded file is never ADMITTED — not parsed-then-pruned, not compiled-then-
# discarded. tests/file_gate.d/ proves the selection end-to-end on both legs (two files declaring one
# function; admit both and the build fails on a duplicate), but an exit code cannot tell "excluded" from
# "included and happened not to matter". What follows builds ONE project two ways and reads the C.
#
# The other three assertions are the rulings that have no other instrument:
#   - a strict manifest validates a flag named in a FILE gate exactly as it does one on a declaration;
#   - a file the CLI NAMED and whose gate excludes it is a hard ERROR, while the same file COLLECTED by a
#     manifest build is skipped silently — the asymmetry is the ruling, so both halves are checked;
#   - every source gated out reports in kama's own words. That last one is here because it regressed to
#     `clang: error: no input files` in development: a message about the wrong tool, for a build that did
#     exactly what its source told it to.
fg="$tmp/filegate"
mkdir -p "$fg/src"
cat > "$fg/kama.json" <<'JSON'
{ "name": "fgdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "flags": { "NEVER_ON": {} }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$fg/src/app.kama" <<'KAMA'
import { platformValue };
fn int32 main() { return platformValue(); }
KAMA
cat > "$fg/src/impl_host.kama" <<'KAMA'
file @compileFor(!ARCH_WASM32);
export { platformValue };
fn int32 platformValue() { return 17; }
KAMA
cat > "$fg/src/impl_wasm.kama" <<'KAMA'
file @compileFor(ARCH_WASM32);
export { platformValue };
fn int32 platformValue() { return 23; }
KAMA
"$KAMA" transpile --no-line "$fg/kama.json" -o "$tmp/fg_host.c" >/dev/null 2>"$tmp/fg.err" || {
    echo "check-compilefor: FAIL — a host build of the file-gate project did not compile:" >&2
    sed 's/^/  /' "$tmp/fg.err" >&2; exit 1
}
if ! grep -q '= 17;' "$tmp/fg_host.c"; then
    echo "check-compilefor: FAIL — the host build dropped the @compileFor(!ARCH_WASM32) FILE (expected '= 17;')" >&2
    exit 1
fi
if grep -q '= 23;' "$tmp/fg_host.c"; then
    echo "check-compilefor: FAIL — the host build STILL contains the @compileFor(ARCH_WASM32) file's body;" >&2
    echo "  a gated-out FILE must never be admitted to the compilation at all" >&2
    exit 1
fi
"$KAMA" transpile --no-line "$fg/kama.json" --target wasm32-emscripten-none -o "$tmp/fg_wasm.c" >/dev/null 2>&1
if ! grep -q '= 23;' "$tmp/fg_wasm.c" || grep -q '= 17;' "$tmp/fg_wasm.c"; then
    echo "check-compilefor: FAIL — a wasm build did not select the complementary file gate" >&2
    exit 1
fi
# ...the same file NAMED on the command line is an ERROR, not a build that quietly produces nothing.
if "$KAMA" check "$fg/src/impl_wasm.kama" >/dev/null 2>"$tmp/fg_operand.err"; then
    echo "check-compilefor: FAIL — a gated-out file named as an OPERAND was accepted" >&2
    exit 1
fi
if ! grep -qF "nothing to compile" "$tmp/fg_operand.err"; then
    echo "check-compilefor: FAIL — a gated-out operand was rejected, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/fg_operand.err" >&2
    exit 1
fi
# ...while the manifest build above, which COLLECTED that same file, skipped it without a word. Proven by
# the host transpile having succeeded at all — it is the same file, and it holds no error of its own.
#
# A file gate is validated like any other: a strict manifest rejects a flag it does not declare.
cat > "$fg/src/impl_host.kama" <<'KAMA'
file @compileFor(NEVR_ON);
export { platformValue };
fn int32 platformValue() { return 17; }
KAMA
if "$KAMA" check "$fg/kama.json" >/dev/null 2>"$tmp/fg_strict.err"; then
    echo "check-compilefor: FAIL — a strict manifest accepted an undeclared flag in a FILE gate (NEVR_ON)" >&2
    exit 1
fi
if ! grep -qF "undeclared flag" "$tmp/fg_strict.err"; then
    echo "check-compilefor: FAIL — undeclared flag in a file gate rejected, but with an unexpected diagnostic:" >&2
    sed 's/^/  /' "$tmp/fg_strict.err" >&2
    exit 1
fi
# EVERY source gated out — kama's own sentence, not the C compiler's.
allg="$tmp/allgated"
mkdir -p "$allg/src"
cat > "$allg/kama.json" <<'JSON'
{ "name": "allgated2", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "flags": { "NEVER_ON": {} }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$allg/src/app.kama" <<'KAMA'
file @compileFor(NEVER_ON);
fn int32 main() { return 0; }
KAMA
if "$KAMA" build "$allg/kama.json" -o "$allg/app" >/dev/null 2>"$tmp/fg_empty.err"; then
    echo "check-compilefor: FAIL — a project whose every file is gated out produced a build" >&2
    exit 1
fi
if ! grep -qF "every source file is excluded" "$tmp/fg_empty.err"; then
    echo "check-compilefor: FAIL — an all-gated project must say so in kama's own words, not the C" >&2
    echo "  compiler's (this regressed to \`clang: error: no input files\` once). Got:" >&2
    sed 's/^/  /' "$tmp/fg_empty.err" >&2
    exit 1
fi

# 7. THE DOCS DO NOT TEACH A RETIRED FLAG NAME.
#
# ⚠️ This half exists because §4b above — proving the COMPILER rejects a reserved/undeclared flag — is
# exactly what made the docs drifting invisible. `docs/SPEC.md` and `docs/KEYWORDS.md` both taught
# `@compileFor(NATIVE)` / `(WASM)` / `(EMBEDDED)` / `(WINDOWS)`, and none of those is a flag: a built-in
# target NAME deliberately does not become one (see derivedTargetFlags in kama.driver.cpp). A manifest
# build rejects the name, but a LOOSE build reads no manifest and treats an undeclared flag as simply
# inactive — so the documented spelling compiles clean and the declaration is silently GONE. The first
# external project on kama nearly shipped it, off our own examples.
#
# The live platform gates are the derived ones (`ARCH_*`/`OS_*`/`ABI_*`, plus `HOSTED`/`SIMD128`), which
# is what tests/compilefor_platform.kama uses.
# ⚠️ Scanned over FENCED ```kama BLOCKS ONLY, not the whole document — the same split
# check-doc-spelling.sh makes, and for the same reason. Prose that WARNS about a retired name has to be
# able to spell it (SPEC.md and ROADMAP_DETAIL.md both do); what costs a reader a build cycle is a
# copyable example. Scanning everything meant this guard rejected its own fix, which is how you end up
# with a waiver list, and a guard with a waiver list is a guard nobody trusts.
bad=$(for f in $(find "$ROOT/docs" "$ROOT/agents" "$ROOT/seed" -name '*.md' 2>/dev/null | sort); do
        awk -v rel="${f#"$ROOT"/}" '
            /^[[:space:]]*```kama/ { inblk = 1; next }
            /^[[:space:]]*```/     { inblk = 0; next }
            # The boundary class is why this is not a plain substring match: `OS_WINDOWS` and
            # `ARCH_WASM32` are the CORRECT spellings and both contain a retired name.
            inblk && /@compileFor\(([^)]*[(!,[:space:]])?(NATIVE|WASM|EMBEDDED|WINDOWS|MACOS|LINUX)[,)]/ { print rel ":" NR ":" $0 }
        ' "$f"
      done)
if [ -n "$bad" ]; then
    echo "check-compilefor: FAIL — a kama code block teaches a @compileFor flag that does not exist." >&2
    printf '%s\n' "$bad" | sed 's/^/  /' >&2
    echo "  A built-in TARGET name is not a flag; in a loose build the decl is silently DROPPED." >&2
    echo "  Use the derived gates: ARCH_WASM32 / OS_WINDOWS / OS_NONE / HOSTED (tests/compilefor_platform.kama)." >&2
    exit 1
fi

echo "check-compilefor: PASS (@compileFor selects one fn/type in the Kama compiler; no #ifdef in emitted C;
  the FILE gate selects one whole unit per target, is validated under a strict manifest, errors on a
  NAMED operand while a COLLECTED file is skipped, and says so itself when every file is gated out;\n  strict manifest rejects undeclared AND reserved flag names; target names/triples resolve and derive\n  ARCH_/OS_/ABI_ flags; select groups pick one value, inherit, and reject duplicates;\n  kama.local.json extends the flag universe;\n  and no kama block teaches a retired target-name flag)"
