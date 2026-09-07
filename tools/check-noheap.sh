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
#   5. TRANSITIVE — the flag and the attribute must agree about a program whose allocation is one call
#      away. They reach that agreement by DIFFERENT routes, which is the point of asserting it here: the
#      attribute propagates along the call graph, while the flag gates every body and so rejects the
#      helper at its own declaration, needing no propagation at all. Until this row, the two legs never
#      met — the xfail corpus drove only the attribute and this guard only the flag — and the fact that
#      `@noheap` stopped at the first callee was invisible to the whole suite.
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
import { std::memory::Owned };
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
{ "name": "nh", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "no-heap": true,
  "modules": { ".": { "visibility": "internal" } } }
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
  "select": { "TARGET": { "HOST": { "no-heap": false } } }, "modules": { ".": { "visibility": "internal" } } }
JSON
if ! "$KAMA" build "$proj/kama.json" -o "$tmp/m2.out" >/dev/null 2>"$tmp/m2.err"; then
    echo "check-noheap: FAIL — a target's \`no-heap\`: false did not override the project's:" >&2
    sed 's/^/  /' "$tmp/m2.err" >&2; exit 1
fi

# 4c. The value set is CLOSED, checked in the reader — a typo must not read as some truthiness nobody
#     wrote down. This is the same rule `kind` gets, and for the same reason.
cat > "$proj/kama.json" <<'JSON'
{ "name": "nh", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "no-heap": "yes",
  "modules": { ".": { "visibility": "internal" } } }
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
#    NB the container is `InlineArray<T>#(N)`, not `FixedArray<T>`, and that is not cosmetic: a
#    `FixedArray<T, A: Allocator = GlobalAllocator>` heap-allocates its buffer, so once the flag applied
#    the allocator leaf program-wide this program became — correctly — a no-heap violation, and asserting
#    it BUILDS would have asserted the bug. `InlineArray` is the stack-allocated container SPEC says a
#    no-heap region is obliged to use, which is what makes it the honest subject for "sorting a fixed
#    buffer with no heap is the MCU/audio case".
sortsrc="$tmp/nhsort.kama"
cat > "$sortsrc" <<'EOF'
import { std::collections::View, std::collections::sortUnstable };
fn int32 main() {
    InlineArray<int32>#(3) a = [3, 1, 2];
    borrow a.viewMut() as v {
        sortUnstable(items: v);
        return v[0] * 100 + v[1] * 10 + v[2];
    }
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

# 5. TRANSITIVE — an allocation one call away from `main`, and the control proving the flag is the cause.
transrc="$tmp/trans.kama"
cat > "$transrc" <<'EOF'
import { std::memory::Owned };
type resource Box { int32 v; public ctor make(int32 v) { this.v = v; } public fn int32 get() { return this.v; } }
fn int32 helper(int32 x) { Owned<Box> b = new Box.make(v: x); return b.get(); }
fn int32 main() { return helper(x: 3); }
EOF
if "$KAMA" build --no-heap "$transrc" -o "$tmp/g.out" >/dev/null 2>"$tmp/tr.err"; then
    echo "check-noheap: FAIL — '--no-heap' accepted a program whose allocation is one call away" >&2; exit 1
fi
if ! grep -qF "heap allocation (new) is forbidden" "$tmp/tr.err"; then
    echo "check-noheap: FAIL — the transitive program was rejected, but not by the no-heap gate:" >&2
    sed 's/^/  /' "$tmp/tr.err" >&2; exit 1
fi
if ! "$KAMA" build "$transrc" -o "$tmp/h.out" >/dev/null 2>"$tmp/trc.err"; then
    echo "check-noheap: FAIL — the transitive program does not build even WITHOUT '--no-heap':" >&2
    sed 's/^/  /' "$tmp/trc.err" >&2; exit 1
fi
# ...and the SAME source, gated by the ATTRIBUTE instead of the flag, must be rejected by the propagation
# rather than at the helper. Asserted on the message, because "it failed" is what a guard that has stopped
# testing anything also reports.
attrsrc="$tmp/attr.kama"
sed 's/^fn int32 main/@noheap fn int32 main/' "$transrc" > "$attrsrc"
if "$KAMA" build "$attrsrc" -o "$tmp/i.out" >/dev/null 2>"$tmp/at.err"; then
    echo "check-noheap: FAIL — '@noheap' accepted a program whose allocation is one call away" >&2; exit 1
fi
if ! grep -qF "is \`@noheap\`, but this call reaches heap allocation (new)" "$tmp/at.err"; then
    echo "check-noheap: FAIL — '@noheap' rejected the transitive program, but not by the propagation:" >&2
    sed 's/^/  /' "$tmp/at.err" >&2; exit 1
fi

# 6. THE ALLOCATOR LEAF, PROGRAM-WIDE. The row this guard's item 5 could not reach: `--no-heap` gates a
#    DIRECT allocation in every body, but a container does not allocate with `new` — it goes through its
#    `A: Allocator` — and that leaf fact is recorded and never rejected (rejecting it where it is emitted
#    would fail every no-heap build at a line in the PRELUDE the author never wrote). So the leaf reached
#    the walk only from an annotated root, and a `--no-heap` build walked to `malloc` through a container
#    unchallenged. Measured before it was fixed: this exact program built clean.
leafsrc="$tmp/nhleaf.kama"
cat > "$leafsrc" <<'EOF'
import { std::collections::DynamicArray };
fn int32 grow(ref DynamicArray<int32> l, int32 n) { l.add(item: n); return 1; }
fn int32 mid(ref DynamicArray<int32> l, int32 n) { return grow(l: ref l, n: n); }
fn int32 tick(ref DynamicArray<int32> l, int32 n) { return mid(l: ref l, n: n); }
fn int32 main() { DynamicArray<int32> a = DynamicArray.empty(); return tick(l: ref a, n: 7); }
EOF
if "$KAMA" build --no-heap "$leafsrc" -o "$tmp/l.out" >/dev/null 2>"$tmp/leaf.err"; then
    echo "check-noheap: FAIL — '--no-heap' reached malloc through a container" >&2; exit 1
fi
if ! grep -qF 'drawing from `GlobalAllocator`' "$tmp/leaf.err"; then
    echo "check-noheap: FAIL — the container program was rejected, but not by the allocator leaf:" >&2
    sed 's/^/  /' "$tmp/leaf.err" >&2; exit 1
fi
# ...and it must say `--no-heap`, not `@noheap`. The two roots of the same walk make two different claims,
# and naming an attribute the author did not write sends them looking for one.
if ! grep -qF 'this build is `--no-heap`, but `grow`' "$tmp/leaf.err"; then
    echo "check-noheap: FAIL — the flag's diagnostic does not name the flag and the boundary function:" >&2
    sed 's/^/  /' "$tmp/leaf.err" >&2; exit 1
fi

# 6b. ANCHORED AT THE BOUNDARY. Every function on the chain reaches the leaf, so a naive rule reports the
#     same `l.add(x)` once per stack frame — `main`, `mid`, `grow`. Only the innermost user body is
#     actionable (it is the frame holding the call), and only stdlib names would appear beyond it. Asserted
#     as a COUNT, because "it was rejected" is what an over-reporting build also does.
if grep -qF 'but `mid`' "$tmp/leaf.err" || grep -qF 'but `tick`' "$tmp/leaf.err"; then
    echo "check-noheap: FAIL — the flag reported an outer frame as well as the boundary function:" >&2
    sed 's/^/  /' "$tmp/leaf.err" >&2; exit 1
fi
# ...and it must never blame the stdlib or the prelude, which the author cannot change. `DynamicArray.add`
# is ON the chain (it should be — that is the witness) but must never be the SUBJECT of a message.
if grep -qE 'but `(std::|GlobalAllocator|Template)' "$tmp/leaf.err"; then
    echo "check-noheap: FAIL — the flag blamed a library body the author did not write:" >&2
    sed 's/^/  /' "$tmp/leaf.err" >&2; exit 1
fi

# 6c. THE FLAG STAYS USABLE — the same container over an ARENA still builds and runs. `A` is a type
#     parameter, so `DynamicArray<T, BumpAllocator>` is a different monomorph reaching a different
#     `allocate`; a rule that rejected this one too would have banned the idiom real-time code uses.
arenasrc="$tmp/nharena.kama"
cat > "$arenasrc" <<'EOF'
import { std::collections::DynamicArray, std::collections::Arena, std::collections::BumpAllocator };
fn int32 grow(DynamicArray<int32, BumpAllocator> l, int32 n) { l.add(item: n); return cast<int32>(l.length()) + n; }
fn int32 main() {
    Arena arena = Arena.make(capacity: 65536);
    DynamicArray<int32, BumpAllocator> a = DynamicArray.withAllocator(allocator: arena.handle());
    return grow(l: a, n: 41);   // 1 + 41
}
EOF
if ! "$KAMA" build --no-heap "$arenasrc" -o "$tmp/ar.out" >/dev/null 2>"$tmp/ar.err"; then
    echo "check-noheap: FAIL — an ARENA-backed container must still build under '--no-heap':" >&2
    sed 's/^/  /' "$tmp/ar.err" >&2; exit 1
fi
rc=0
"$tmp/ar.out" >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "check-noheap: FAIL — the arena-backed container under '--no-heap' did not run (expected 42, got $rc)" >&2; exit 1
fi

echo "PASS no-heap (--no-heap rejects heap allocation, composes with --target embedded, drops the allocating sort; the kama.json 'no-heap' key does the same, is per-target overridable, and refuses a non-boolean; flag and attribute agree on a transitive allocation; the flag applies the GlobalAllocator leaf program-wide, anchored at the innermost user body and never blaming the stdlib, while an arena-backed container still builds and runs)"
