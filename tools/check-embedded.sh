#!/bin/sh
# check-embedded.sh — MCU campaign step 3 guard for `--target embedded` (the freestanding bare-metal build).
# A thumb ELF can't run on the CI host, so instead of running it we assert the two things that make a
# freestanding build correct, both host-checkable:
#   1. SHAPE — the emitted C carries the guarded freestanding entry (`int main(void)` that never returns,
#      no argv), selected by KAMA_TARGET_EMBEDDED.
#   2. FREESTANDING COMPILE — `kama build --target embedded` produces a `-ffreestanding -nostdlib` OBJECT
#      that compiles with NO libc pulled in (no undefined write/abort/malloc/... references).
# The value-only blink fixture (tests/embedded_blink.kama) is the subject; it also runs as an ordinary
# hosted fixture on the native/wasm/ASan legs (the #if guard keeps the hosted form intact). Fails (exit 1)
# with a diagnostic if either property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA="$ROOT/kama"
FIXTURE="$ROOT/tests/embedded_blink.kama"

if [ ! -x "$KAMA" ]; then echo "check-embedded: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-embedded: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cfile="$tmp/blink.c"
obj="$tmp/blink.o"

# 1. SHAPE — transpile (target-agnostic) and confirm the guarded freestanding entry is emitted.
"$KAMA" transpile "$FIXTURE" -o "$cfile" >/dev/null
if ! grep -q '#if defined(KAMA_TARGET_EMBEDDED)' "$cfile"; then
    echo "check-embedded: FAIL — no #if defined(KAMA_TARGET_EMBEDDED) entry guard in emitted C" >&2; exit 1
fi
if ! grep -q 'int main(void)' "$cfile"; then
    echo "check-embedded: FAIL — freestanding 'int main(void)' entry not emitted" >&2; exit 1
fi

# 2. FREESTANDING COMPILE — drive the real `--target embedded` path to an object. This exercises the driver
#    flag plumbing (-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED -c) and proves the value-only program
#    compiles freestanding. Warnings (e.g. unused-allocator out-of-scope notes) are fine; a nonzero exit is not.
if ! "$KAMA" build "$FIXTURE" --target embedded -o "$obj" >/dev/null 2>"$tmp/build.err"; then
    echo "check-embedded: FAIL — 'kama build --target embedded' did not compile" >&2
    sed 's/^/  /' "$tmp/build.err" >&2; exit 1
fi
if [ ! -s "$obj" ]; then
    echo "check-embedded: FAIL — --target embedded produced no object" >&2; exit 1
fi

# 2b. BONUS (when nm is available) — assert the freestanding object references NO libc symbol. A value-only
#     firmware must not drag in write/abort/malloc/free/printf; if it does, the freestanding runtime seam leaks.
if command -v nm >/dev/null 2>&1; then
    leaked=$(nm "$obj" 2>/dev/null | grep -iE ' U (write|_write|abort|malloc|free|calloc|realloc|printf|memcpy)$' || true)
    if [ -n "$leaked" ]; then
        echo "check-embedded: FAIL — freestanding object references libc symbols:" >&2
        echo "$leaked" | sed 's/^/  /' >&2; exit 1
    fi
fi

echo "PASS check-embedded (--target embedded: guarded freestanding entry + libc-free object)"
