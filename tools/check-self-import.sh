#!/bin/sh
# check-self-import.sh — a file that imports a SIBLING from inside its own namespace must be checkable
# and buildable ON ITS OWN, not only through a consumer.
#
# The defect this guards: a CLI input used to register its whole NAMESPACE as already provided, so a
# self-import (`import my::mod::{Bee}` from inside `namespace my::mod`) was skipped and the sibling file
# never loaded. The file then reported "module `my.mod` does not export `Bee`" plus a cascade from every
# type that failed to resolve — while the same module built perfectly through a consumer, because a
# FOREIGN import loads the module whole. So nothing in the fixture suite could see it: every fixture is a
# program, and a program imports its modules from the outside.
#
# What it broke in practice was every command whose input IS a member file — `kama check`, and therefore
# the language server, on 8 of the stdlib's own files and on any multi-file module a user writes. It is
# guarded here rather than as a fixture because the failing operation is "check ONE file of a module",
# which the fixture harness has no shape for.
#
# Three assertions, because the fix has two halves and a way to overshoot:
#   1. A member file with a self-import checks clean STANDALONE (the bug).
#   2. It resolves from the right ROOT — `<root>/my/mod/a.kama` must find `<root>/my/mod/`, not
#      `my/mod/my/mod/`. Getting half 1 right and half 2 wrong turns the silent wrong answer into a hard
#      "cannot resolve module", which is what the first cut of the fix did.
#   3. The consumer path still builds AND RUNS correctly — the half that always worked must stay working.
# Plus: no stdlib file reports the failure, which is the case that was actually reported.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-self-import: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A two-file module, nested so the namespace path is a real one to resolve (`my::mod` -> `my/mod/`).
mkdir -p "$tmp/my/mod"
cat > "$tmp/my/mod/a.kama" <<'EOF'
namespace my::mod;
import my::mod::{Bee};
export { Aye, useBee };
type value Aye { public int32 x; public ctor of(int32 x) { this.x = x; } }
fn int32 useBee() { Bee b = Bee.of(y: 2); return b.y; }
EOF
cat > "$tmp/my/mod/b.kama" <<'EOF'
namespace my::mod;
export { Bee, Cee };
type value Bee { public int32 y; public ctor of(int32 y) { this.y = y; } }
type value Cee { public int32 z; public ctor of(int32 z) { this.z = z; } }
EOF
# The other half, and the one with no import to hang resolution off: SPEC says "files of one directory
# share a namespace, so a sibling is reachable unqualified with no `import` at all". `c.kama` uses `Cee`
# that way. It is the shape lib/std/math/mat.kama has — which reported 202 unknown-type errors when
# checked alone, purely because nothing pinned its siblings.
cat > "$tmp/my/mod/c.kama" <<'EOF'
namespace my::mod;
export { useCee };
fn int32 useCee() { Cee c = Cee.of(z: 5); return c.z; }
EOF
cat > "$tmp/app.kama" <<'EOF'
import my::mod::{Aye, useBee};
fn int32 main() { Aye a = Aye.of(x: 1); return a.x + useBee(); }
EOF

# 1 + 2. The member file, checked on its own. Both halves of the fix land here: a wrong `provided` entry
# gives "does not export", a wrong resolve root gives "cannot resolve module".
if ! "$KAMA" check "$tmp/my/mod/a.kama" > "$tmp/check.out" 2>&1; then
    echo "check-self-import: FAIL — checking a member file with a self-import did not succeed" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi
if grep -q "does not export" "$tmp/check.out"; then
    echo "check-self-import: FAIL — a self-import reported 'does not export'; the sibling file was never loaded" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi
if grep -q "cannot resolve module" "$tmp/check.out"; then
    echo "check-self-import: FAIL — a self-import resolved from the wrong root (searched <dir>/my/mod, not <dir>)" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi

# 2b. The bare-name half: a member file that names a sibling with NO import at all. There is no import
#     for resolution to hang off, so this fails unless the loader pulls the module for a namespaced input.
if ! "$KAMA" check "$tmp/my/mod/c.kama" > "$tmp/checkc.out" 2>&1; then
    echo "check-self-import: FAIL — a member file naming a sibling WITHOUT an import did not check standalone" >&2
    echo "  (SPEC: files of one directory share a namespace, so a sibling is reachable unqualified)" >&2
    cat "$tmp/checkc.out" >&2
    exit 1
fi

# 3. The consumer path — a foreign import of the same module — still builds and runs. `useBee()` returns
# 2 and `a.x` is 1, so the program's exit code is 3; a wrong answer here means the fix changed which
# units get loaded for an ordinary build.
if ! "$KAMA" build "$tmp/app.kama" -o "$tmp/app" > "$tmp/build.out" 2>&1; then
    echo "check-self-import: FAIL — the consumer program stopped building" >&2
    cat "$tmp/build.out" >&2
    exit 1
fi
set +e
"$tmp/app"; rc=$?
set -e
if [ "$rc" -ne 3 ]; then
    echo "check-self-import: FAIL — the consumer program returned $rc, expected 3" >&2
    exit 1
fi

# 4. The reported case: no stdlib file may report it. Eight did (net/net, net/udp, io/streams, and five
# in collections) — every one of them a file that imports a sibling from inside its own namespace.
bad=0
for f in $(find "$ROOT/lib/std" -name '*.kama'); do
    if "$KAMA" check "$f" 2>&1 | grep -q "does not export"; then
        echo "check-self-import: FAIL — ${f#$ROOT/} reports 'does not export' when checked alone" >&2
        bad=1
    fi
done
[ "$bad" -eq 0 ] || exit 1

echo "check-self-import: OK (member file checks standalone; consumer build unchanged; stdlib clean)"
