#!/bin/sh
# check-extern-rung.sh — the FFI seam is inside the file rung, and one C symbol has one signature.
#
# `extern` used to be the single hole in an otherwise total rule. Every other name a file uses is governed
# by `export` and `import`; an extern was governed by nothing, because it keeps its LITERAL C spelling and
# is never scope-prefixed, so all 39 files declaring `malloc` collapse onto ONE emitter table entry. Two
# defects lived in that collapse, and both were reproduced — built, linked and RUN — before either rule was
# written:
#
#   1. THE RUNG LEAK. `declFileOf` reports "" for an extern on purpose (one entry, many declaring files),
#      so `checkReach` walked straight out and a file could call `malloc` having neither declared nor
#      imported it. It inherited the declaration from whatever OTHER file — including the prelude, which
#      declares `malloc`/`free` for `GlobalAllocator`, so every program got libc's allocator unwritten.
#
#   2. SILENT LAST-WINS. The duplicate-function rule is exempt for extern (re-declaring is the idiom SPEC
#      prescribes), which meant the last declaration collected silently REPLACED every earlier one and all
#      call sites in the program lowered through the winner. kama emits no prototype for an extern, so C
#      cannot catch it either. The measured consequence, and assertion 4 below:
#
#          a.kama  extern fn UnsafePtr memcpy(UnsafePtr dst, UnsafePtr src, usize n);   // true C order
#          b.kama  extern fn UnsafePtr memcpy(UnsafePtr src, UnsafePtr dst, usize n);   // names swapped
#
#      Both files write `memcpy(dst: d, src: s, n: 4)`. Named arguments REORDER off the winning FuncSig, so
#      a.kama's correct call emitted `memcpy(s, d, 4)` — source and destination reversed. It built and ran.
#
# THE RULE IS DECLARE, NOT IMPORT, and assertion 6 is what pins that distinction. An extern is a reference
# to a symbol someone else defines; there is no module surface for an `export` to put it on. A file that
# would rather not repeat the declaration wraps the extern in an ordinary `fn` and exports THAT (assertion
# 7) — which costs nothing, because `--release` folds every unit into one translation unit and a
# pass-through wrapper compiles to assembly byte-identical to the direct call. Measured at 0.9.85.
#
# ⚠️ VERIFIED BY DISABLING THE MECHANISM, not by reading it — and doing that CORRECTED this header, which
# first claimed assertion 6 for the rung. Measured, with each half gated off in turn:
#
#   FFI arm out of `checkReach`      -> 1 and 1b fail.        2, 3, 4, 5, 6, 7 pass.
#   agreement out of the collectors  -> 3, 4 and 5 fail.      1, 1b, 2, 6, 7 pass.
#
# So **assertion 6 is a canary, not an assertion of either rule** — it is refused by the export-list
# validation that has always been there, because an extern registers in `_funcs` under its LITERAL name
# while `export { malloc }` is validated against `qualify("malloc")`. That is worth knowing rather than
# hiding: "an extern is not exportable" needed no code, it falls out of the literal spelling, and this
# assertion is a REGRESSION guard on that property. Assertions 2 and 7 are the CONTROLS and must pass in
# every configuration — a guard whose every line fires when the rule is off is testing the compiler's
# ability to reject something, not the rule.
#
# Guards run in parallel: private mktemp -d, no writes to the worktree, no `cd` outside a subshell, and the
# compiler reached through $KAMA rather than the ./kama symlink.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-extern-rung: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
bad() { echo "  FAIL: $*" >&2; fails=$((fails+1)); }
ok()  { echo "  ok: $*"; }

# Build a two-file project. $1 = dir name, $2 = a.kama body, $3 = main.kama body.
mkproj() {
    mkdir -p "$tmp/$1/src"
    cat > "$tmp/$1/kama.json" <<JSON
{ "name": "$1", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "modules": { ".": { "visibility": "public" } } }
JSON
    printf '%s\n' "$2" > "$tmp/$1/src/a.kama"
    printf '%s\n' "$3" > "$tmp/$1/src/main.kama"
}

# --- 1. A file that names an extern it does not declare is REFUSED -----------------------------------
mkproj leak \
'export { helper };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);
extern fn void free(UnsafePtr p);
unsafe fn int32 helper() { return 1; }' \
'import { helper };
unsafe fn int32 work() {
    UnsafePtr p = malloc(n: cast<usize>(64));
    free(p: p);
    return helper();
}
fn int32 main() { return work(); }'
rc=0; "$KAMA" check "$tmp/leak/kama.json" > "$tmp/leak.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "a file calling an extern declared only in a SIBLING was accepted (the rung leak)"
elif ! grep -q "does not declare it" "$tmp/leak.out"; then
    bad "rejected, but not by the FFI rung — the reason must say the file does not declare it"
    sed -n 1,3p "$tmp/leak.out" >&2
else
    ok "an extern named but not declared in this file is refused"
fi

# The same leak through the PRELUDE, which is the reach that mattered: `malloc`/`free` are declared there
# for `GlobalAllocator`, so without this arm every program gets libc's allocator without writing a line.
# No `<`-prefixed exemption, deliberately — a compiler-owned declaration must not satisfy a user file.
mkdir -p "$tmp/prel"
cat > "$tmp/prel/x.kama" <<'EOF'
unsafe fn int32 work() { UnsafePtr p = malloc(n: cast<usize>(8)); free(p: p); return 0; }
fn int32 main() { return work(); }
EOF
rc=0; "$KAMA" check "$tmp/prel/x.kama" > "$tmp/prel.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "a program inherited the PRELUDE's \`malloc\` declaration — the leak, at its widest reach"
else
    ok "the prelude's own extern declarations do not satisfy a user file"
fi

# --- 2. CONTROL: re-declaring the same extern in N files is the idiom, and stays silent ---------------
mkproj repeat \
'export { helper };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);
extern fn void free(UnsafePtr p);
unsafe fn int32 helper() { UnsafePtr p = malloc(n: cast<usize>(8)); free(p: p); return 1; }' \
'import { helper };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);
extern fn void free(UnsafePtr p);
unsafe fn int32 work() { UnsafePtr p = malloc(n: cast<usize>(64)); free(p: p); return helper(); }
fn int32 main() { return work() - 1; }'
if ! "$KAMA" build "$tmp/repeat/kama.json" -o "$tmp/repeat.bin" > "$tmp/repeat.out" 2>&1; then
    bad "re-declaring an extern in each file that calls it was refused — that is the SPEC idiom"
    sed -n 1,3p "$tmp/repeat.out" >&2
elif ! "$tmp/repeat.bin"; then
    bad "the repeated-declaration program built but did not run correctly"
else
    ok "re-declaring an extern in every file that names it builds and runs (the control)"
fi

# --- 3. Disagreeing RETURN type is refused, naming both files -----------------------------------------
mkproj ret \
'export { fromA };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);
unsafe fn UnsafePtr fromA() { return malloc(n: cast<usize>(64)); }' \
'import { fromA };
extern "<stdlib.h>";
extern fn int32 malloc(int32 n);
unsafe fn int32 work() { UnsafePtr q = fromA(); return malloc(n: 8); }
fn int32 main() { return work(); }'
rc=0; "$KAMA" check "$tmp/ret/kama.json" > "$tmp/ret.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "two declarations of \`malloc\` with different return types were accepted"
elif ! grep -q "disagrees with the one in" "$tmp/ret.out"; then
    bad "rejected, but not as a disagreement — the reason must name the other declaration"
    sed -n 1,3p "$tmp/ret.out" >&2
elif ! grep -q "a.kama" "$tmp/ret.out"; then
    bad "the disagreement did not NAME the other declaring file, which is the whole diagnostic"
else
    ok "disagreeing extern return types are refused, naming the other file"
fi

# --- 4. THE SILENT MISCOMPILE: parameter names swapped ------------------------------------------------
# Nothing about this reaches C — both spellings emit `memcpy(...)` — so this rule is the only thing between
# a swapped `dst`/`src` and a program that runs.
mkproj swap \
'export { copyInA };
extern "<string.h>";
extern fn UnsafePtr memcpy(UnsafePtr dst, UnsafePtr src, usize n);
unsafe fn void copyInA(UnsafePtr d, UnsafePtr s) { memcpy(dst: d, src: s, n: cast<usize>(4)); }' \
'import { copyInA };
extern "<string.h>";
extern fn UnsafePtr memcpy(UnsafePtr src, UnsafePtr dst, usize n);
unsafe fn void copyInMain(UnsafePtr d, UnsafePtr s) { memcpy(dst: d, src: s, n: cast<usize>(4)); }
fn int32 main() { return 0; }'
rc=0; "$KAMA" check "$tmp/swap/kama.json" > "$tmp/swap.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "two \`memcpy\` declarations with SWAPPED parameter names were accepted — named args reorder off"
    bad "  the winning signature, so one file's correct call emits \`memcpy(src, dst, n)\`. Silent."
elif ! grep -q "is named" "$tmp/swap.out"; then
    bad "rejected, but the reason did not name the parameter that disagrees"
    sed -n 1,3p "$tmp/swap.out" >&2
else
    ok "a parameter-name swap between two declarations of one C symbol is refused"
fi

# --- 5. An extern STRUCT whose fields disagree --------------------------------------------------------
# Field ACCESS emits the literal C name, so a reordering is harmless; a differing field TYPE is not, and it
# used to be caught only downstream, blaming the innocent file.
mkproj estruct \
'export { quotInA };
extern "<stdlib.h>";
type extern value div_t { int32 quot; int32 rem; }
extern fn div_t div(int32 numer, int32 denom);
unsafe fn int32 quotInA() { div_t r = div(numer: 17, denom: 5); return r.quot; }' \
'import { quotInA };
extern "<stdlib.h>";
type extern value div_t { int64 quot; int64 rem; }
extern fn div_t div(int32 numer, int32 denom);
fn int32 main() { return quotInA(); }'
rc=0; "$KAMA" check "$tmp/estruct/kama.json" > "$tmp/estruct.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "two \`type extern value div_t\` declarations with different field types were accepted"
elif ! grep -q "disagrees with the one in" "$tmp/estruct.out"; then
    bad "the extern struct was rejected, but not as a disagreement naming the other declaration"
    sed -n 1,3p "$tmp/estruct.out" >&2
else
    ok "disagreeing extern struct fields are refused, naming the other file"
fi

# --- 6. CANARY: an extern is DECLARED, never exported or imported -------------------------------------
# The distinction the design rests on. An extern has no module surface: `export { malloc }` must not be the
# way to share it, or the rule becomes "declare OR import" — two ways to do one thing, and every one of the
# 84 project-less files that declares an extern needs somewhere to import it FROM.
# ⚠️ Neither rule enforces this; the export-list validation does, because an extern's table key is its
# LITERAL name and `export` is validated against the qualified one. Kept as a regression guard on a
# property that falls out of the spelling — see the header's fail-check table.
mkproj expo \
'export { malloc };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);' \
'import { expo::malloc };
unsafe fn int32 work() { UnsafePtr p = malloc(n: cast<usize>(8)); return 0; }
fn int32 main() { return work(); }'
rc=0; "$KAMA" check "$tmp/expo/kama.json" > "$tmp/expo.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    bad "\`export { malloc }\` + \`import\` was accepted — an extern is not a module symbol, and if this"
    bad "  path works the rule silently became 'declare OR import' with two ways to do one thing"
else
    ok "an extern cannot be exported and imported — the rule is DECLARE"
fi

# --- 7. CONTROL: the WRAPPER is the sharing mechanism, and it works today ------------------------------
mkproj wrap \
'export { alloc, release };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);
extern fn void free(UnsafePtr p);
unsafe fn UnsafePtr alloc(usize n) { return malloc(n: n); }
unsafe fn void release(UnsafePtr p) { free(p: p); }' \
'import { wrap::alloc, wrap::release };
unsafe fn int32 work() { UnsafePtr p = alloc(n: cast<usize>(64)); release(p: p); return 0; }
fn int32 main() { return work(); }'
if ! "$KAMA" build "$tmp/wrap/kama.json" -o "$tmp/wrap.bin" > "$tmp/wrap.out" 2>&1; then
    bad "wrapping an extern in an ordinary \`fn\` and exporting THAT was refused — that is the sharing path"
    sed -n 1,3p "$tmp/wrap.out" >&2
elif ! "$tmp/wrap.bin"; then
    bad "the wrapper program built but did not run correctly"
else
    ok "an extern is shared by wrapping it in an ordinary fn (the control, and the documented way)"
fi

if [ "$fails" -gt 0 ]; then echo "check-extern-rung: FAIL ($fails)" >&2; exit 1; fi
echo "check-extern-rung: PASS"
