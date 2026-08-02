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
"$KAMA" transpile tests/ecs_pattern.kama -o "$tmp/ecs.c" >/dev/null

fail=0
# 1. the contract-bounded generic monomorphized, and calls Timer__tick DIRECTLY
if grep -q '_F4__tickAll___F4__Timer' "$tmp/ecs.c"; then
    body=$(sed -n '/^static int32_t _F4__tickAll___F4__Timer/,/^}/p' "$tmp/ecs.c")
    if echo "$body" | grep -q '_F4__Timer__tick('; then
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
body=$(sed -n '/^void _F4__integrate(std/,/^}/p' "$tmp/ecs.c")
if echo "$body" | grep -q '_F4__Transform\* t'; then
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
