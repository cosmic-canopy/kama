#!/bin/sh
# check-binding-widen.sh — a WIDENING conversion out of a binding emits a BARE C cast, not a runtime check.
#
# The cast trap emits a narrowing check for `cast<T>(x)` whenever it cannot prove the conversion safe, and
# "cannot prove" includes "cannot type the source". An unanswered source falls back to `KAMA_NARROW`, a
# `_Generic` that asks C the question the classifier could not — correct, but paid per evaluation. A
# `foreach` element and a `match`-arm payload were both unanswered sources, so a widening out of either
# carried a check that could never fire: measured at ~19% of a 2M-iteration loop in
# bench/src/kama/alloc.kama before `narrowCheck` could see a loop binding.
#
# This is an EMITTED-C check because it has to be. `tests/binding_widen.kama` returns 22 either way — a
# bare cast and a checked one compute the same answer, so the exit code cannot tell them apart and the
# corpus would go on passing while every widening quietly paid for a check. That is the same shape of
# hole the repo's house rule is about: the fixture proves the arithmetic, this proves the claim.
#
# Native leg, no compiler build of its own (transpile only), private mktemp -d.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/binding_widen.kama"

if [ ! -x "$KAMA" ];  then echo "check-binding-widen: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-binding-widen: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/bw.c"

"$KAMA" transpile --no-line "$FIXTURE" -o "$out" >/dev/null

# The fixture's own function, not the whole unit: the prelude legitimately contains narrowing checks
# (`string.length()` into a `usize`), so grepping the file would always match.
body=$(awk '/^int32_t kama_main\(/,/^}/' "$out")

if [ -z "$body" ]; then
    echo "check-binding-widen: FAIL — could not find kama_main in the emitted C" >&2
    exit 1
fi

# Both bindings must have widened through a plain `(int64_t)` cast.
n_bare=$(printf '%s\n' "$body" | grep -c '((int64_t)(' || true)
if [ "$n_bare" -lt 2 ]; then
    echo "check-binding-widen: FAIL — expected 2 bare (int64_t) widenings (a match-arm binding and a" >&2
    echo "  foreach binding), found $n_bare. The classifier stopped answering for a binding." >&2
    printf '%s\n' "$body" >&2
    exit 1
fi

# …and neither may have emitted a runtime narrowing check. `KAMA_NARROW` is the `_Generic` fallback for an
# unknown source; `kama_narrow_chk_` is the typed one. A widening needs neither.
if printf '%s\n' "$body" | grep -q 'KAMA_NARROW'; then
    echo "check-binding-widen: FAIL — a widening out of a binding emitted the KAMA_NARROW _Generic" >&2
    echo "  fallback, so the classifier could not type the binding. See ROADMAP row 1's record in SPEC.md." >&2
    printf '%s\n' "$body" | grep -n 'KAMA_NARROW' >&2
    exit 1
fi

# The `cast<int32>(s)` on the return line is a genuine NARROWING and must keep its check — otherwise this
# guard would also pass against a compiler that stopped checking casts altogether.
if ! printf '%s\n' "$body" | grep -q 'kama_narrow_chk_'; then
    echo "check-binding-widen: FAIL — the fixture's narrowing `cast<int32>(s)` lost its runtime check;" >&2
    echo "  the cast trap itself has regressed, not just the classifier." >&2
    exit 1
fi

echo "check-binding-widen: PASS (2 bare widenings out of bindings, no KAMA_NARROW, narrowing still checked)"
