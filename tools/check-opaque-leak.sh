#!/bin/sh
# check-opaque-leak.sh — a synthetic OPAQUE TYPE PARAMETER must never outlive the walk that minted it.
#
# What this is about. `checkUninstantiatedTemplates` checks a generic nobody instantiates by binding each
# type parameter to a synthetic type — `__opq_<template>_<param>` — whose methods are exactly what that
# parameter's bounds promise. That makes `T` a real type, so `View<T>` is an ordinary instance and the
# whole rule set reaches inside the body. The cost is that the walk REGISTERS real things: classes,
# generic-type instances, contract instances, collections.
#
# None of it describes code anyone asked to compile, and two consumers run AFTER the walk:
#   * `analyze()` builds the query index (`buildDefSites`/`buildPositions`) once the walk returns, so a
#     leaked `View___opq_f_T` becomes a type the LSP will offer in completion and go-to-definition.
#   * a diagnostic renders through `demangleForDisplay`, and a leaked mangle reads as
#     `DynamicArray<__opq::F4::neverCalled3_T>` — a compiler-internal name in a message about the user's
#     own generic, where the type they wrote is `T`.
#
# `probeSandboxBegin`/`probeSandboxEnd` snapshot the key sets and erase what appeared, and the display map
# turns an opaque back into the parameter's source name. Both are invisible when they work, which is
# exactly the kind of thing that rots — hence a guard rather than a comment.
#
# Three assertions, one per escape route. ⚠️ THEY ARE NOT EQUALLY PROVEN, and saying so is the point of
# this paragraph — a guard that cannot fail reads as coverage while providing none. Each was checked by
# BREAKING the mechanism it guards and confirming what happened:
#
#   1. emitted C never contains the mangle (`--keep-c`, so the real generated source is read)
#        — CANARY. With `probeSandboxEnd` disabled this still passes: the walk runs last, after every
#          byte of C is written, so there is nothing left to contaminate.
#   2. `kama query` never reports one as a symbol
#        — CANARY. Also still passes with the sandbox disabled: `buildDefSites` indexes AST declaration
#          nodes, not `_classes`, so a leaked entry is not reachable from any query mode today. The
#          sandbox is therefore DEFENSIVE — it keeps the tables honest for whatever reads them next —
#          and these two assertions exist to notice when that changes, not to prove it has not.
#   3. a DIAGNOSTIC about a generic renders the parameter as written, never as the mangle
#        — REAL. Disabling the `_opaqueDisplay` lookup in `demangleForDisplay` makes this fail with
#          `DynamicArray<__opq::F4::neverCalled_T>`, which is what a user would have read.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-opaque-leak: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A program with BOTH shapes the walk probes: a generic function and a generic type, neither instantiated,
# each reaching through its parameter so the opaque is actually exercised rather than merely minted.
cat > "$tmp/app.kama" <<'EOF'
import { std::collections::DynamicArray, std::collections::View };

fn isize neverCalledFn<T>(ref DynamicArray<T> d) {
    isize n = d.length();
    borrow d.view() as v { n = n + v.length(); }
    return n;
}

type value NeverUsed<T: Comparable<T>> {
    T item;
    public ctor make(T x) { this.item = give x; }
    public const fn Ordering vs(const ref T other) { return this.item.compareTo(other: other); }
}

fn int32 main() { return 0; }
EOF

# ---- 1. the emitted C ---------------------------------------------------------------------------------
if ! "$KAMA" build "$tmp/app.kama" -o "$tmp/app" --keep-c > "$tmp/build.log" 2>&1; then
    echo "check-opaque-leak: FAIL — the fixture does not build:" >&2
    cat "$tmp/build.log" >&2
    exit 1
fi

if grep -rl '__opq' "$tmp" --include='*.c' --include='*.h' > "$tmp/hits" 2>/dev/null && [ -s "$tmp/hits" ]; then
    echo "check-opaque-leak: FAIL — an opaque type parameter reached the emitted C:" >&2
    while read -r f; do grep -n '__opq' "$f" | head -3 | sed "s|^|  $f:|" >&2; done < "$tmp/hits"
    echo "  A probe registers real instances; probeSandboxEnd is what erases them." >&2
    exit 1
fi

# ---- 2. the query index ------------------------------------------------------------------------------
# `analyze()` builds it after the walk returns, so anything left in `_classes` is visible here.
qout=$("$KAMA" query "$tmp/app.kama" --symbols --search opq 2>&1 || true)
if printf '%s' "$qout" | grep -q '__opq'; then
    echo "check-opaque-leak: FAIL — an opaque type parameter is in the query index (the LSP offers it):" >&2
    printf '%s' "$qout" | grep '__opq' | head -3 | sed 's|^|  |' >&2
    exit 1
fi

# ---- 3. the diagnostic -------------------------------------------------------------------------------
# The message must name the parameter the user wrote. `DynamicArray` has no `get`, so this rejects — and
# what it says about the receiver's type is the whole point.
cat > "$tmp/bad.kama" <<'EOF'
import { std::collections::DynamicArray };
fn void neverCalled<T>(ref DynamicArray<T> src) {
    T item = src.get(index: 0);
}
fn int32 main() { return 0; }
EOF

dout=$("$KAMA" check "$tmp/bad.kama" 2>&1 || true)
if printf '%s' "$dout" | grep -q '__opq'; then
    echo "check-opaque-leak: FAIL — a diagnostic renders the opaque mangle instead of the parameter:" >&2
    printf '%s' "$dout" | grep '__opq' | head -2 | sed 's|^|  |' >&2
    echo "  demangleForDisplay reads _opaqueDisplay for exactly this." >&2
    exit 1
fi
if ! printf '%s' "$dout" | grep -q 'DynamicArray<T>'; then
    echo "check-opaque-leak: FAIL — the diagnostic does not name the receiver as \`DynamicArray<T>\`:" >&2
    printf '%s' "$dout" | grep 'has no method' | head -2 | sed 's|^|  |' >&2
    exit 1
fi

echo "check-opaque-leak: PASS (diagnostics name the parameter; C + query index clean — see the header on which of these can fail)"
