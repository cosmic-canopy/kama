#!/bin/sh
# The `--no-heap` verdict DERIVES what kama's own runtime C does with the heap, by reading the shipped headers
# (KR-74). This guard proves that reading actually happens, and that it decides the verdict.
#
# Why it needs to exist. Every other no-heap fixture would still pass if the header scan quietly stopped
# running: they reach allocations the EMITTER writes, which were judged long before this row. The scan's own
# contribution — "this runtime extern allocates, and into which heap" — used to be 55 hand-placed `@heap`
# marks, and deleting them moved the whole question behind a code path nothing observed directly. A scan that
# silently found nothing would fail OPEN: programs accepted, `--no-heap` quietly meaning less than it says.
#
# It is also the tripwire for KR-68. That row proposes moving the OS seam behind plain prototypes with its
# bodies in one TU. If that lands as written, the bodies this scan reads are gone from the headers and every
# `std::fs`/`std::process`/`std::net` allocation fact disappears — silently, and in the accepting direction.
# This guard fails loudly instead, and whoever takes KR-68 gets told that the scan must then read that TU too.
#
# The method is mutation: stage a private copy of the runtime tree, plant an allocation in a header that
# NOTHING declares to kama, and require the verdict to change. The planted call is `malloc(` inside a header
# body; the kama program calls only the wrapper, so `malloc` never appears in the emitted C. A refusal is
# therefore possible only by reading the header — which is precisely the claim.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"        # $KAMA, absolute, for the platform that built last
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-header-scan: FAIL — $*" >&2; exit 1; }

# A FLAT staged tree: `resolveRuntimeDir` tries `<exeDir>/kama_runtime.h` before the dev-tree branch, so a
# binary sitting beside a set of headers resolves to THOSE. That is what makes a private, mutable copy of the
# runtime possible without touching the worktree (tools/check-clean-tree.sh would catch that, rightly).
mkdir -p "$tmp/stage"
cp "$KAMA" "$tmp/stage/kama"
cp "$ROOT"/include/*.h "$tmp/stage/"
STAGED="$tmp/stage/kama"

cat > "$tmp/prog.kama" <<'EOF'
extern "<kama_runtime.h>";
extern fn UnsafePtr probe_planted_alloc(usize n);
unsafe fn int32 grab() { UnsafePtr p = probe_planted_alloc(n: 8); return 0; }
@noheap fn int32 tick() { return grab(); }
fn int32 main() { return tick(); }
EOF

# 1. BASELINE — the same symbol, with a body in the same header that allocates NOTHING. The program builds.
#    Both stages differ in exactly one thing, the header's text, which is what makes stage 2 attributable.
cp "$tmp/stage/kama_runtime.h" "$tmp/kama_runtime.h.orig"
{ cat "$tmp/kama_runtime.h.orig"
  printf '#ifndef PROBE_PLANTED\n#define PROBE_PLANTED\nstatic inline void* probe_planted_alloc(size_t n) { (void)n; return (void*)0; }\n#endif\n'
} > "$tmp/stage/kama_runtime.h"
if ! "$STAGED" build "$tmp/prog.kama" -o "$tmp/base.out" >/dev/null 2>"$tmp/base.err"; then
    echo "  $(cat "$tmp/base.err")" >&2
    fail "the staged tree cannot build a program whose header body allocates nothing — the staging is wrong, not the scan"
fi

# 2. PLANT — the SAME symbol, now reaching the C allocator. Nothing in kama declares it `@heap`; nothing in the
#    emitted C spells `malloc`. Only the header says so, so only reading the header can refuse it.
{ cat "$tmp/kama_runtime.h.orig"
  printf '#ifndef PROBE_PLANTED\n#define PROBE_PLANTED\nstatic inline void* probe_planted_alloc(size_t n) { extern void* malloc(size_t); return malloc(n); }\n#endif\n'
} > "$tmp/stage/kama_runtime.h"
if "$STAGED" build "$tmp/prog.kama" -o "$tmp/planted.out" >/dev/null 2>"$tmp/planted.err"; then
    fail "a \`@noheap\` body reached a header function that calls malloc, and it was ACCEPTED — the runtime header scan is not running, so every runtime allocation fact is silently missing"
fi
if ! grep -qF 'probe_planted_alloc' "$tmp/planted.err"; then
    echo "  $(cat "$tmp/planted.err")" >&2
    fail "the planted allocation was refused, but the diagnostic does not name \`probe_planted_alloc\` — the chain must reach the header body that allocates, or the verdict cannot be checked by the person who gets it"
fi
if ! grep -qF 'malloc' "$tmp/planted.err"; then
    echo "  $(cat "$tmp/planted.err")" >&2
    fail "the planted allocation was refused without naming \`malloc\` — the message must name the CAUSE, which is the half of KR-74 that makes a verdict auditable"
fi

# 3. THE FUNNEL IS NOT A FOREIGN HEAP — the distinction the whole row turns on, checked on the staged tree so
#    it cannot pass by accident of the worktree. A pool-backed program that formats a number draws from the
#    pool and must BUILD; the same program reaching a foreign allocator must not. (tests/noheap_pool_fmt.d and
#    tests/xfail/noheap_pool_resolve.d pin both in the fixture corpus; this re-checks the derived half here,
#    because it is the half that depends on the header text being read.)
sed -n '/^@globalAllocator/,/^}$/p' "$ROOT/tests/global_allocator_pool.kama" > "$tmp/pool.inc"
{ echo 'import { std::concurrent::Atomic };'; cat "$tmp/pool.inc"
  echo 'fn int32 main() { int32 n = 7; string s = "${n}"; return cast<int32>(s.length()); }'
} > "$tmp/pool.kama"
if ! "$STAGED" build --no-heap "$tmp/pool.kama" -o "$tmp/pool.out" >/dev/null 2>"$tmp/pool.err"; then
    echo "  $(cat "$tmp/pool.err")" >&2
    fail "a declared \`@globalAllocator\` program was refused for FORMATTING a number — the funnel is being read as a foreign heap, which is the verdict KR-74 exists to correct"
fi

echo "check-header-scan: PASS (the shipped headers decide the runtime's allocation facts: a planted header allocation is refused and named, an undeclared one builds, and a declared pool still serves a formatted string)"
