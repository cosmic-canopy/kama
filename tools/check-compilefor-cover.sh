#!/bin/sh
# check-compilefor-cover.sh — THE COVER (KR-54). `kama check` owes every `@compileFor` gate in a project's
# own files one analysis in which that gate is ACTIVE. Before this, a declaration or file the gate dropped
# was pruned before collection and therefore never looked at: `tests/file_gate.d` with a broken wasm-only
# unit said "OK (2 units analyzed)" of three files, on the box where nobody builds wasm.
#
# What is asserted here, each one a ruling this guard is the record of:
#   1. a broken gated-out FILE in a project fails the check, tagged with the flags that reproduce it;
#   2. the pristine project still passes, and says how many configurations it took;
#   3. a broken gated-out DECLARATION fails on every axis, not just TARGET — BUILD_TYPE, the `flags` bag,
#      a user single-select group, OUTPUT and `--no-heap` each hide the other half of a gate written on it;
#   4. an explicit configuration flag means EXACTLY that configuration (the fast path, and the way the tag
#      is reproduced), so it must NOT report what another configuration would say;
#   5. a legitimate one-target declaration — an extern that exists only there — still passes. This is the
#      case that makes "just resolve names before pruning" wrong, so it is the one that must not regress;
#   6. a gate no known target can activate is an ERROR saying how to lift it (KR-69, which retired the
#      stub-code allowance), declaring the target lifts it AND covers the code, and a gate that can
#      NEVER be active is an error naming why;
#   6b. the cover does not follow the HOST — both arches kama supports are covered on every machine;
#   7. `--each` and `--json` carry the cover: the same verdict, and the configuration in the envelope;
#   8. the corpus sweep — every gated fixture in tests/ passes its own cover.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-compilefor-cover: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-compilefor-cover: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }

# ---- 1 + 2: a gated-out FILE in a project --------------------------------------------------------
proj="$tmp/fg"
mkdir -p "$proj/src"
cat > "$proj/kama.json" <<'JSON'
{ "name": "coverdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$proj/src/app.kama" <<'KAMA'
import { platformValue };
fn int32 main() { return platformValue(); }
KAMA
cat > "$proj/src/impl_host.kama" <<'KAMA'
file @compileFor(!ARCH_WASM32);
export { platformValue };
fn int32 platformValue() { return 17; }
KAMA
cat > "$proj/src/impl_wasm.kama" <<'KAMA'
file @compileFor(ARCH_WASM32);
export { platformValue };
fn int32 platformValue() { return 23; }
KAMA
"$KAMA" check "$proj/kama.json" >/dev/null 2>"$tmp/pristine.err" \
    || fail "the pristine file-gate project did not pass its cover:" "$tmp/pristine.err"
grep -q "2 configurations" "$tmp/pristine.err" \
    || fail "the pristine project passed, but did not report the configurations it checked:" "$tmp/pristine.err"

# The rot: the wasm-only file names a type that does not exist. A host build is unaffected and must stay so.
printf '\nfn int32 rotted(Zork z) { return 0; }\n' >> "$proj/src/impl_wasm.kama"
"$KAMA" build "$proj/kama.json" -o "$tmp/fg.bin" >/dev/null 2>&1 \
    || fail "a host BUILD of the project stopped compiling — a build judges its own configuration only"
if "$KAMA" check "$proj/kama.json" >/dev/null 2>"$tmp/rot.err"; then
    fail "a broken gated-out FILE passed the check — the rot this guard exists for" "$tmp/rot.err"
fi
grep -qF "[--target WASM]" "$tmp/rot.err" \
    || fail "the gated-out file's error was reported, but not tagged with the configuration:" "$tmp/rot.err"
grep -qF "Zork" "$tmp/rot.err" || fail "the tagged diagnostic is not the rot itself:" "$tmp/rot.err"

# ---- 3: every axis, not just TARGET ---------------------------------------------------------------
# One project, one broken declaration per axis, each gated so the default configuration cannot see it.
ax="$tmp/axes"
mkdir -p "$ax/src"
cat > "$ax/kama.json" <<'JSON'
{ "name": "axesdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "flags": { "TELEMETRY": {} },
  "select": { "CONSOLE": { "XBOX": { "default": true }, "PS5": {} } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
for axis in 'ARCH_WASM32:--target WASM' '!DEBUG:--release' 'TELEMETRY:--define TELEMETRY' \
            'PS5:--select CONSOLE=PS5' 'NOHEAP:--no-heap' 'SHARED:--select OUTPUT=SHARED'; do
    gate=${axis%%:*}; want=${axis#*:}
    cat > "$ax/src/app.kama" <<KAMA
@compileFor($gate)
fn int32 rotted(Zork z) { return 0; }
fn int32 main() { return 0; }
KAMA
    if "$KAMA" check "$ax/kama.json" >/dev/null 2>"$tmp/axis.err"; then
        fail "a broken declaration behind @compileFor($gate) passed the cover" "$tmp/axis.err"
    fi
    grep -qF "[$want]" "$tmp/axis.err" \
        || fail "@compileFor($gate) was covered, but not by [$want]:" "$tmp/axis.err"
done

# ---- 4: an explicit configuration flag means exactly that configuration ---------------------------
cat > "$ax/src/app.kama" <<'KAMA'
@compileFor(ARCH_WASM32)
fn int32 rotted(Zork z) { return 0; }
fn int32 main() { return 0; }
KAMA
"$KAMA" check "$ax/kama.json" --release >/dev/null 2>"$tmp/one.err" \
    || fail "an explicit configuration reported a gate belonging to another one:" "$tmp/one.err"
"$KAMA" check "$ax/kama.json" --target WASM >/dev/null 2>"$tmp/wasm.err" \
    && fail "--target WASM did not reproduce what the cover's tag said it would"
grep -qF "Zork" "$tmp/wasm.err" || fail "the tag's own command reported something else:" "$tmp/wasm.err"
grep -qF "[--target" "$tmp/wasm.err" \
    && fail "a single explicit configuration tagged its diagnostics — there is nothing to distinguish"

# ---- 5: the legitimate one-target declaration still passes ----------------------------------------
# A gated declaration may NAME things that exist only on its own target — an extern over a platform
# header is the whole reason `@compileFor` gates the bodyless forms. This is what a pre-prune name
# resolution would have broken, so the cover must analyze each gate under a target that HAS those names.
"$KAMA" check "$ROOT/tests/compilefor_extern.kama" >/dev/null 2>"$tmp/extern.err" \
    || fail "a legitimate target-only extern was refused by the cover:" "$tmp/extern.err"
"$KAMA" check "$ROOT/tests/compilefor_platform.kama" >/dev/null 2>"$tmp/plat.err" \
    || fail "the platform contract seam (one impl per target) was refused by the cover:" "$tmp/plat.err"

# ---- 6: unknown, unreachable, impossible -----------------------------------------------------------
# No target kama knows has this arch, so nothing ever compiles the declaration and nothing ever analyses
# it. SPEC allowed exactly that until 2026-09-18 as "stub code for a platform you do not support yet";
# the allowance was LIFTED (KR-69), because a rule that cannot tell it from a typo buys silence in both
# cases. The error says the one thing that lifts it.
cat > "$tmp/unreachable.kama" <<'KAMA'
@compileFor(ARCH_RISCV64)
fn int32 future(Zork z) { return 0; }
fn int32 main() { return 0; }
KAMA
if "$KAMA" check "$tmp/unreachable.kama" >/dev/null 2>"$tmp/unreach.err"; then
    fail "a gate no known target can activate was accepted — it is checked by nobody, so it is an error"
fi
grep -qF 'select.TARGET' "$tmp/unreach.err" \
    || fail "the unknown-platform gate was refused without saying how to lift it:" "$tmp/unreach.err"

# ...and DECLARING the target lifts it and brings the code into the cover, which is the exchange the
# ruling made: the gate becomes legal AND the type error behind it is found, tagged with the target.
decl="$tmp/declared"
mkdir -p "$decl/src"
cat > "$decl/kama.json" <<'JSON'
{ "name": "riscvdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "select": { "TARGET": { "RISCV": { "triple": "riscv64-linux-gnu" } } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$decl/src/app.kama" <<'KAMA'
@compileFor(ARCH_RISCV64)
fn int32 future(Zork z) { return 0; }
fn int32 main() { return 0; }
KAMA
if "$KAMA" check "$decl/kama.json" >/dev/null 2>"$tmp/decl.err"; then
    fail "declaring the target made the gate legal but did not bring it into the cover:" "$tmp/decl.err"
fi
grep -qF "[--target RISCV]" "$tmp/decl.err" \
    || fail "the declared target's gate was covered, but not tagged with it:" "$tmp/decl.err"
grep -qF "Zork" "$tmp/decl.err" || fail "the tagged diagnostic is not the rot itself:" "$tmp/decl.err"
grep -qF "no target kama knows" "$tmp/decl.err" \
    && fail "the gate was still refused as unknown after its target was declared:" "$tmp/decl.err"

# ...while a gate that can never be active under ANY configuration is a typo, and says which two names.
for pair in 'DEBUG, RELEASE:BUILD_TYPE' 'OS_LINUX, OS_WINDOWS:triple component' 'X, !X:!X'; do
    gate=${pair%%:*}; want=${pair#*:}
    cat > "$tmp/never.kama" <<KAMA
@compileFor($gate)
fn int32 never() { return 0; }
fn int32 main() { return 0; }
KAMA
    if "$KAMA" check "$tmp/never.kama" >/dev/null 2>"$tmp/never.err"; then
        fail "@compileFor($gate) can never be active, and the check accepted it"
    fi
    grep -qF "can never be active" "$tmp/never.err" \
        || fail "@compileFor($gate) failed, but not as an impossible gate:" "$tmp/never.err"
    grep -qF "$want" "$tmp/never.err" \
        || fail "@compileFor($gate) was refused without naming why ($want):" "$tmp/never.err"
done

# ---- 6b: the cover does not follow the HOST --------------------------------------------------------
# A built-in target NAME resolves its arch to the host's, so before KR-69 the cover reached only ONE of
# the two arches kama supports — whichever machine happened to ask. A type error behind `ARCH_AARCH64`
# was found on an arm Mac and reported OK here; `ARCH_X86_64` the other way round. Half of the commonest
# arch pair in systems code went unchecked on every machine, and a different half per machine. Both must
# be found, on every host — which is what makes "this code is checked somewhere" a property of the
# program rather than of the box that ran the check.
cat > "$tmp/arches.kama" <<'KAMA'
@compileFor(ARCH_AARCH64) fn int32 armRot(Zork z) { return 0; }
@compileFor(ARCH_X86_64)  fn int32 x86Rot(Nope n) { return 0; }
fn int32 main() { return 0; }
KAMA
if "$KAMA" check "$tmp/arches.kama" >/dev/null 2>"$tmp/arch.err"; then
    fail "neither arch's rot was found — the cover reaches no arch at all" "$tmp/arch.err"
fi
grep -qF "Zork" "$tmp/arch.err" \
    || fail "the aarch64 gate was not covered on this host (the cover still follows it):" "$tmp/arch.err"
grep -qF "Nope" "$tmp/arch.err" \
    || fail "the x86_64 gate was not covered on this host (the cover still follows it):" "$tmp/arch.err"

# ...and a gate on THIS build's own triple is checked too, whatever the catalog holds: `--target` is one
# of the three ways a component becomes known (a catalog target, a declared one, this build's own).
cat > "$tmp/board.kama" <<'KAMA'
@compileFor(OS_ESP32)
fn int32 boardOnly() { return 1; }
fn int32 main() { return 0; }
KAMA
"$KAMA" check --target xtensa-esp32-elf "$tmp/board.kama" >/dev/null 2>"$tmp/board.err" \
    || fail "a gate naming this build's OWN triple component was refused:" "$tmp/board.err"

# ---- 7: --each and --json carry the cover ----------------------------------------------------------
cat > "$tmp/rot1.kama" <<'KAMA'
@compileFor(ARCH_WASM32)
fn int32 rotted(Zork z) { return 0; }
fn int32 main() { return 0; }
KAMA
cp "$tmp/rot1.kama" "$tmp/rot2.kama"
"$KAMA" check --each "$tmp/rot1.kama" "$tmp/rot2.kama" >"$tmp/each.out" 2>/dev/null || true
[ "$(grep -c '^1 ' "$tmp/each.out")" = "2" ] \
    || fail "--each did not cover both programs (the parse cache must be keyed by configuration):" "$tmp/each.out"
"$KAMA" check --json "$tmp/rot1.kama" >"$tmp/j.out" 2>/dev/null || true
grep -qF '"configuration":"--target WASM"' "$tmp/j.out" \
    || fail "--json did not carry the configuration a diagnostic came from:" "$tmp/j.out"
grep -qF '"configurations":2' "$tmp/j.out" \
    || fail "--json did not report how many configurations were checked:" "$tmp/j.out"

# ---- 8: the corpus sweep ----------------------------------------------------------------------------
# Every gated fixture must pass its OWN cover, which is what keeps this feature honest against the corpus
# rather than only against the projects invented above.
for f in "$ROOT"/tests/compilefor_*.kama "$ROOT"/tests/simd128_flag.kama; do
    [ -f "$f" ] || continue
    "$KAMA" check "$f" >/dev/null 2>"$tmp/sweep.err" \
        || fail "$(basename "$f") does not pass its own cover:" "$tmp/sweep.err"
done
for d in "$ROOT"/tests/file_gate.d "$ROOT"/tests/compilefor_manifest.d "$ROOT"/tests/query/cfg; do
    [ -f "$d/kama.json" ] || continue
    "$KAMA" check "$d/kama.json" >/dev/null 2>"$tmp/sweepd.err" \
        || fail "$(basename "$d") does not pass its own cover:" "$tmp/sweepd.err"
done

echo "check-compilefor-cover: PASS (every gate checked: file + decl, six axes, tags reproduce, "\
"impossible and unknown-platform refused, declaring a target covers it, both arches on every host, "\
"--each/--json, corpus swept)"
