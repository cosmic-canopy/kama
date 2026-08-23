#!/bin/sh
# check-noheap.sh — MCU campaign step 5 guard for the `--no-heap` build flag (the whole-program no-heap
# subset). The `tests/xfail/*` loop builds every fixture with NO extra flags, so a flag-driven rejection
# can't be tested there — this dedicated guard drives the real `--no-heap` path and asserts:
#   1. REJECT   — a program that heap-allocates (`new`) FAILS to build under `--no-heap`, with the
#                 "heap allocation ... is forbidden" diagnostic (the right error, not any failure).
#   2. CONTROL  — the SAME program builds fine WITHOUT `--no-heap` (so the flag is what rejects it).
#   3. COMPOSES — `--no-heap` composes with `--target embedded` (independent axes) and still rejects.
#   4. MANIFEST  — a project says it once as a `no-heap` key instead of remembering the flag every time,
#      and a target may say "not this one". The key is what makes the rule reliable: the FLAG fails
#      silently when forgotten — the build simply succeeds with allocation allowed.
# `@noheap` (the per-region attribute) is exercised by tests/xfail/noheap_* instead. Fails (exit 1) with a
# diagnostic if any property breaks. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-noheap: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src="$tmp/has_new.kama"
cat > "$src" <<'EOF'
import std::memory::{Owned};
type resource Box { int32 v; public ctor make(int32 v) { this.v = v; } public fn int32 get() { return this.v; } }
fn int32 main() { Owned<Box> b = new Box.make(v: 3); return b.get(); }
EOF

# 1. REJECT — `--no-heap` must reject the heap allocation with the specific diagnostic.
if "$KAMA" build --no-heap "$src" -o "$tmp/a.out" >/dev/null 2>"$tmp/nh.err"; then
    echo "check-noheap: FAIL — '--no-heap' accepted a program that calls 'new'" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/nh.err"; then
    echo "check-noheap: FAIL — '--no-heap' rejected, but not with the no-heap diagnostic:" >&2
    sed 's/^/  /' "$tmp/nh.err" >&2; exit 1
fi

# 2. CONTROL — the identical program must build WITHOUT the flag (proves the flag is the cause).
if ! "$KAMA" build "$src" -o "$tmp/b.out" >/dev/null 2>"$tmp/ctrl.err"; then
    echo "check-noheap: FAIL — the program does not build even WITHOUT '--no-heap':" >&2
    sed 's/^/  /' "$tmp/ctrl.err" >&2; exit 1
fi

# 3. COMPOSES — `--no-heap` is target-independent; it must still reject under `--target embedded`.
if "$KAMA" build --no-heap --target embedded "$src" -o "$tmp/c.o" >/dev/null 2>"$tmp/emb.err"; then
    echo "check-noheap: FAIL — '--no-heap --target embedded' accepted a 'new'" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/emb.err"; then
    echo "check-noheap: FAIL — '--no-heap --target embedded' rejected without the no-heap diagnostic:" >&2
    sed 's/^/  /' "$tmp/emb.err" >&2; exit 1
fi

# 4. THE MANIFEST KEY — the same rejection with no flag on the command line at all.
proj="$tmp/proj"; mkdir -p "$proj/src"
cp "$src" "$proj/src/app.kama"
cat > "$proj/kama.json" <<'JSON'
{ "name": "nh", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "no-heap": true }
JSON
if "$KAMA" build "$proj/kama.json" -o "$tmp/m.out" >/dev/null 2>"$tmp/m.err"; then
    echo "check-noheap: FAIL — the manifest's \`no-heap\` did not reject a 'new'" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/m.err"; then
    echo "check-noheap: FAIL — the manifest key rejected, but not with the no-heap diagnostic:" >&2
    sed 's/^/  /' "$tmp/m.err" >&2; exit 1
fi

# 4b. ...and a TARGET overrides it wholesale, which is the per-target exception the key exists to allow:
#     no heap on the board, a heap on the host that builds the tooling.
cat > "$proj/kama.json" <<'JSON'
{ "name": "nh", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "no-heap": true,
  "select": { "TARGET": { "HOST": { "no-heap": false } } } }
JSON
if ! "$KAMA" build "$proj/kama.json" -o "$tmp/m2.out" >/dev/null 2>"$tmp/m2.err"; then
    echo "check-noheap: FAIL — a target's \`no-heap\`: false did not override the project's:" >&2
    sed 's/^/  /' "$tmp/m2.err" >&2; exit 1
fi

# 4c. The value set is CLOSED, checked in the reader — a typo must not read as some truthiness nobody
#     wrote down. This is the same rule `kind` gets, and for the same reason.
cat > "$proj/kama.json" <<'JSON'
{ "name": "nh", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "no-heap": "yes" }
JSON
if "$KAMA" build "$proj/kama.json" -o "$tmp/m3.out" >/dev/null 2>"$tmp/m3.err"; then
    echo "check-noheap: FAIL — a non-boolean \`no-heap\` was accepted" >&2; exit 1
fi
if ! grep -qF 'expected `true` or `false`' "$tmp/m3.err"; then
    echo "check-noheap: FAIL — a non-boolean \`no-heap\` failed without naming the value problem:" >&2
    sed 's/^/  /' "$tmp/m3.err" >&2; exit 1
fi

# 4. STDLIB OPT-OUT — `--no-heap` contributes a `NOHEAP` flag, which the stdlib uses to DROP the
#    declarations that need an allocator. The stable `sort` builds index buffers, so it must vanish in a
#    no-heap build (with a diagnostic that says why, not "unknown function"), while the in-place
#    `sortUnstable` must still be there: sorting a fixed buffer with no heap is the MCU/audio case.
sortsrc="$tmp/nhsort.kama"
cat > "$sortsrc" <<'EOF'
import std::collections::{FixedArray, View, sortUnstable};
fn int32 main() {
    FixedArray<int32> fa = FixedArray::<int32>.make(size: 3);
    fa[0] = 3; fa[1] = 1; fa[2] = 2;
    int32 r = 0;
    borrow fa.view() as v {
        sortUnstable(items: v);
        r = v[0] * 100 + v[1] * 10 + v[2];
    }
    return r;
}
EOF
if ! "$KAMA" build --no-heap "$sortsrc" -o "$tmp/d.out" >/dev/null 2>"$tmp/su.err"; then
    echo "check-noheap: FAIL — 'sortUnstable' must build under '--no-heap' (it permutes in place):" >&2
    sed 's/^/  /' "$tmp/su.err" >&2; exit 1
fi
rc=0
"$tmp/d.out" >/dev/null 2>&1 || rc=$?   # `|| ` so the intentional non-zero exit survives `set -e`
if [ "$rc" != "123" ]; then
    echo "check-noheap: FAIL — 'sortUnstable' under '--no-heap' did not sort (expected exit 123, got $rc)" >&2; exit 1
fi
badsrc="$tmp/nhsortbad.kama"
sed 's/sortUnstable/sort/g' "$sortsrc" > "$badsrc"
if "$KAMA" build --no-heap "$badsrc" -o "$tmp/e.out" >/dev/null 2>"$tmp/st.err"; then
    echo "check-noheap: FAIL — '--no-heap' accepted the allocating 'sort'" >&2; exit 1
fi
if ! grep -qF "is not available in this build configuration" "$tmp/st.err"; then
    echo "check-noheap: FAIL — 'sort' was rejected under '--no-heap', but not with the build-config diagnostic:" >&2
    sed 's/^/  /' "$tmp/st.err" >&2; exit 1
fi
if ! "$KAMA" build "$badsrc" -o "$tmp/f.out" >/dev/null 2>"$tmp/stc.err"; then
    echo "check-noheap: FAIL — 'sort' does not build even WITHOUT '--no-heap':" >&2
    sed 's/^/  /' "$tmp/stc.err" >&2; exit 1
fi

echo "PASS no-heap (--no-heap rejects heap allocation, composes with --target embedded, drops the allocating sort; the kama.json 'no-heap' key does the same, is per-target overridable, and refuses a non-boolean)"
