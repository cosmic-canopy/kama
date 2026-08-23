#!/bin/sh
# check-self-import.sh — a file that imports a SIBLING from inside its own module must be analyzable
# without a consumer: through its project, and through a loose build that names every source.
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
#   1. A member file's self-import resolves, and its siblings come with it.
#   2. A member file that names a sibling with NO import at all resolves too — there is no import edge
#      for resolution to hang off, so only "load the module" can answer it.
#   3. The consumer path still builds AND RUNS correctly — the half that always worked must stay working.
# Plus: no stdlib file reports the failure, which is the case that was actually reported.
#
# ⚠️ HOW THESE ARE SPELLED CHANGED WITH §2i, and the change is the point rather than an accommodation.
# `kama check <one member file>` is a LOOSE build — the operand's basename is the mode — and a loose build
# has no manifest and does no searching, so one file of a multi-file module is not a program. The two
# supported ways to analyze a member are the project (`kama check kama.json`) and `kama lsp`, which walks
# to the project because an editor is handed a buffer and never a command line (§2g.35). Both still rest
# on the loader pulling a module's other files in, which is what this guard exists for; what is gone is
# the spelling that asked the filesystem to guess.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-self-import: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A project with one two-file module, nested so the module name is a real one to compose (`my::mod`).
mkdir -p "$tmp/proj/src/mod"
cat > "$tmp/proj/kama.json" <<'JSON'
{
  "name": "my", "version": "0.1.0", "kind": "library",
  "modules": { ".": { "visibility": "internal" }, "mod": { "visibility": "public" } }
}
JSON
cat > "$tmp/proj/src/mod/a.kama" <<'EOF'
namespace my::mod;
import my::mod::{Bee};
export { Aye, useBee };
type value Aye { public int32 x; public ctor of(int32 x) { this.x = x; } }
fn int32 useBee() { Bee b = Bee.of(y: 2); return b.y; }
EOF
cat > "$tmp/proj/src/mod/b.kama" <<'EOF'
namespace my::mod;
export { Bee, Cee };
type value Bee { public int32 y; public ctor of(int32 y) { this.y = y; } }
type value Cee { public int32 z; public ctor of(int32 z) { this.z = z; } }
EOF
# The other half, and the one with no import to hang resolution off: SPEC says the files of one module
# reach each other unqualified, with no `import` at all. `c.kama` uses `Cee` that way. It is the shape
# lib/std/math/mat.kama has — which reported 202 unknown-type errors when checked alone, purely because
# nothing pinned its siblings.
cat > "$tmp/proj/src/mod/c.kama" <<'EOF'
namespace my::mod;
export { useCee };
fn int32 useCee() { Cee c = Cee.of(z: 5); return c.z; }
EOF

# 1 + 2. The module, checked through its project. A wrong `provided` entry gives "does not export"; a
# module whose files are not pulled together gives "unknown type `Cee`".
if ! "$KAMA" check "$tmp/proj/kama.json" > "$tmp/check.out" 2>&1; then
    echo "check-self-import: FAIL — checking the project did not succeed" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi
if grep -q "does not export" "$tmp/check.out"; then
    echo "check-self-import: FAIL — a self-import reported 'does not export'; the sibling file was never loaded" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi
if grep -q "unknown type" "$tmp/check.out"; then
    echo "check-self-import: FAIL — a member naming a sibling WITHOUT an import lost it" >&2
    echo "  (SPEC: the files of one module reach each other unqualified)" >&2
    cat "$tmp/check.out" >&2
    exit 1
fi

# 2b. The same module reached the way an EDITOR reaches it: one file, and the project found by walking.
# `kama query <manifest> <file>` is the CLI spelling of the scope the language server computes for itself,
# so this is the standalone-member case as it survives §2i.
if ! "$KAMA" query "$tmp/proj/kama.json" "$tmp/proj/src/mod/c.kama" --symbols > "$tmp/q.out" 2>&1 \
   || ! grep -q "useCee" "$tmp/q.out"; then
    echo "check-self-import: FAIL — a member file has no symbols when reached through its project" >&2
    cat "$tmp/q.out" >&2
    exit 1
fi

# 2c. ...and the spelling §2i makes INVALID says so rather than half-working. `consumer.kama` declares no
# namespace — the shape every file has after 2e — so handed over ALONE it is a loose one-file program that
# does not carry `my::mod`, and nothing may go looking for it.
#
# ⚠️ Precisely what was MEASURED, because the neighbouring claim is not true yet: `a.kama` handed over
# alone still checks clean today, through ctxOf's temporary declaration rung (`namespace my::mod;` names
# it, and the same-directory sibling scan then pulls b and c). That spelling stops working in 2e with the
# other 90 declarations, and asserting it here now would be asserting a prediction.
printf 'import my::mod::{Aye};\nfn int32 use() { return Aye.of(x: 1).x; }\n' > "$tmp/proj/src/consumer.kama"
rc=0
"$KAMA" check "$tmp/proj/src/consumer.kama" > "$tmp/alone.out" 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "check-self-import: FAIL — one file of a project was accepted as a whole program" >&2
    exit 1
fi
grep -q "cannot resolve module 'my::mod'" "$tmp/alone.out" || {
    echo "check-self-import: FAIL — naming one member file failed for the wrong reason:" >&2
    head -3 "$tmp/alone.out" >&2
    exit 1
}

# 3. The consumer path — a foreign import of the same module — still builds and runs. Loose, with EVERY
# source named, which is what §2i asks of a build with no manifest: `useBee()` returns 2 and `a.x` is 1,
# so the program's exit code is 3. A wrong answer here means the fix changed which units get loaded.
mkdir -p "$tmp/app/my/mod"
cp "$tmp/proj/src/mod/"*.kama "$tmp/app/my/mod/"
cat > "$tmp/app/app.kama" <<'EOF'
import my::mod::{Aye, useBee};
fn int32 main() { Aye a = Aye.of(x: 1); return a.x + useBee(); }
EOF
if ! "$KAMA" build "$tmp/app/app.kama" "$tmp/app/my/mod/a.kama" "$tmp/app/my/mod/b.kama" \
        "$tmp/app/my/mod/c.kama" -o "$tmp/app/app" > "$tmp/build.out" 2>&1; then
    echo "check-self-import: FAIL — the consumer program stopped building" >&2
    cat "$tmp/build.out" >&2
    exit 1
fi
set +e
"$tmp/app/app"; rc=$?
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

echo "check-self-import: OK (a module's files load together through its project and through a loose build that names them all; one file alone is refused by name; stdlib clean)"
