#!/bin/sh
# The ECS pattern must stay ZERO-DISPATCH. tests/ecs_pattern.kama documents the architecture; this guard
# proves the claim against the EMITTED C rather than trusting the comment:
#
#   1. a system over View<T> writes through a concrete `T*` — no vtable in the loop
#   2. a contract used as a generic BOUND lowers to a DIRECT call, not an indirect one
#
# If either regresses, the engine story in docs/ENGINE_READINESS.md is no longer true.
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
[ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] || { echo "SKIP check-ecs (native only)"; exit 0; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
SRC=tests/ecs_pattern.kama
"$KAMA" transpile "$SRC" -o "$tmp/ecs.c" >/dev/null

# The fixture is a loose file in no module, so its symbols carry the file-private scope — which since
# §2e.26 is named after the FILE (`kF_ecs_pattern`) rather than numbered by load position (`_F4`). Derived
# from the source name here rather than written out, because the old spelling WAS written out: a prefix
# that stops matching makes this guard report "tickAll was not monomorphized", which reads as a codegen
# regression when it is really a stale guard. Deriving it means a renamed fixture cannot cause that.
# NB the `\n` in the preserved set: `tr -c` complements it, and basename's trailing newline is part of
# tr's input, so leaving it out appends a `_` and the prefix silently matches nothing.
P="kF_$(basename "$SRC" .kama | tr -c 'A-Za-z0-9_\n' '_')"

fail=0
# 1. the contract-bounded generic monomorphized, and calls Timer__tick DIRECTLY
# The instance is found by what `kama demangle` says it is, `tickAll<Timer>`, never by its C name spelled
# here: the instance's spelling changed too (`…__tickAll__kF_ecs_pattern__Timer` until 0.9.850), and a name
# written into this guard went stale the same way the prefix did. The range starts at the definition (its
# line ends `)`), not the prototype (`);`), which would run on into whatever function follows it.
inst=""
for n in $(grep -oE "^static int32_t ${P}__tickAll[A-Za-z0-9_]*\(" "$tmp/ecs.c" | sed 's/^static int32_t //; s/($//' | sort -u); do
    [ "$("$KAMA" demangle "$SRC" -- "$n")" = "tickAll<Timer>" ] && inst=$n
done
if [ -n "$inst" ]; then
    body=$(sed -n "/^static int32_t ${inst}(.*)\$/,/^}/p" "$tmp/ecs.c")
    if echo "$body" | grep -q "${P}__Timer__tick("; then
        echo "  ok: contract bound monomorphized to a DIRECT call (Timer__tick)"
    else
        echo "  FAIL: tickAll<T: Tickable> no longer calls Timer__tick directly"; fail=1
    fi
    if echo "$body" | grep -qE 'vtbl|->vtable'; then
        echo "  FAIL: a vtable reached the contract-bounded loop"; fail=1
    else
        echo "  ok: no vtable in the contract-bounded loop"
    fi
else
    echo "  FAIL: tickAll was not monomorphized for Timer"; fail=1
fi

# 2. the plain system writes through a concrete Transform* with no dispatch
body=$(sed -n "/^void ${P}__integrate(std/,/^}/p" "$tmp/ecs.c")
if echo "$body" | grep -q "${P}__Transform\* k_t"; then
    echo "  ok: the system loop walks a concrete Transform*"
else
    echo "  FAIL: the system loop no longer binds a concrete Transform*"; fail=1
fi
if echo "$body" | grep -qE 'vtbl|->vtable'; then
    echo "  FAIL: a vtable reached the system loop"; fail=1
else
    echo "  ok: no vtable in the system loop"
fi

[ "$fail" = 0 ] && echo "PASS check-ecs-zero-dispatch" || { echo "FAIL check-ecs-zero-dispatch"; exit 1; }
