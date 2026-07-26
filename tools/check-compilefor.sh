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
KAMA="$ROOT/kama"
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
if ! grep -q '__ret_0 = 7;' "$relc"; then
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

# 4. STRICT VALIDATION — a `kama.json` manifest DECLARES the valid flag universe, so an undeclared
#    `@compileFor(...)` flag is a hard error (typo protection), not a silent drop. Self-contained in a
#    temp dir so it doesn't leave a manifest next to the shared xfail fixtures.
proj="$tmp/proj"
mkdir -p "$proj"
cat > "$proj/kama.json" <<'JSON'
{ "name": "strict-demo", "version": "0.1.0", "flags": { "WINDOWS": {} } }
JSON
cat > "$proj/app.kama" <<'KAMA'
@compileFor(WINODWS) fn int32 typo() { return 1; }
fn int32 main() { return 0; }
KAMA
if "$KAMA" build "$proj/app.kama" -o "$proj/app" >/dev/null 2>"$tmp/strict.err"; then
    echo "check-compilefor: FAIL — strict manifest accepted an undeclared @compileFor flag (WINODWS)" >&2
    exit 1
fi
if ! grep -qF "undeclared flag" "$tmp/strict.err"; then
    echo "check-compilefor: FAIL — undeclared-flag rejected, but not with the expected diagnostic:" >&2
    sed 's/^/  /' "$tmp/strict.err" >&2
    exit 1
fi

# 5. PLATFORM SEAM — `@compileFor` on a `type` (not just a fn): a contract + per-target gated impls,
#    exactly one surviving. On the native transpile the NativeClock impl is present and the WasmClock
#    impl is wholly absent (struct + ctor + methods + vtable) — proving whole-type selection. The wasm
#    test leg builds the complement (WasmClock kept, NativeClock dropped) and asserts the same exit.
platc="$tmp/platform.c"
"$KAMA" transpile --no-line "$ROOT/tests/compilefor_platform.kama" -o "$platc" >/dev/null
if ! grep -q 'NativeClock' "$platc"; then
    echo "check-compilefor: FAIL — native build dropped the @compileFor(NATIVE) type (NativeClock)" >&2
    exit 1
fi
if grep -q 'WasmClock' "$platc"; then
    echo "check-compilefor: FAIL — native build STILL contains the @compileFor(WASM) type (WasmClock);" >&2
    echo "  a gated-out TYPE (struct + methods + vtable) must not reach the emitted C" >&2
    exit 1
fi

# 6. kama.local.json (M5.2) — a gitignored sibling deep-merges over kama.json: a flag it declares extends the
#    valid universe (no longer "undeclared"), and one it marks `default:true` is active. Reuse the strict proj:
#    its kama.json declares only WINDOWS, so LOCALFLAG is undeclared until the local manifest adds it.
cat > "$proj/kama.local.json" <<'JSON'
{ "flags": { "LOCALFLAG": { "default": true } } }
JSON
cat > "$proj/local.kama" <<'KAMA'
@compileFor(LOCALFLAG) fn int32 gated() { return 42; }
fn int32 main() { return gated(); }
KAMA
localc="$tmp/local.c"
if ! "$KAMA" transpile --no-line "$proj/local.kama" -o "$localc" >/dev/null 2>"$tmp/local.err"; then
    echo "check-compilefor: FAIL — kama.local.json did not declare LOCALFLAG (build rejected it):" >&2
    sed 's/^/  /' "$tmp/local.err" >&2
    exit 1
fi
if ! grep -q 'gated' "$localc"; then
    echo "check-compilefor: FAIL — local default:true flag did not keep the @compileFor(LOCALFLAG) body" >&2
    exit 1
fi
rm -f "$proj/kama.local.json"

echo "check-compilefor: PASS (@compileFor selects one fn/type in the Kama compiler; no #ifdef in emitted C; strict manifest rejects undeclared flags; kama.local.json extends the flag universe)"
