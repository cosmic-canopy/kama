#!/bin/sh
# check-demangle.sh — `kama demangle` renders EMITTED C names back to the kama that produced them.
#
# Every name kama owns reaches C in a prefixed register (SPEC § *C names*): `k_` for the user's, `kama_`
# for the compiler's. A debugger reads those out of the debug info, so `kama demangle` is what turns them
# back (KR-32). It is a SUBCOMMAND and not a map written beside the build, because a map is a record that
# can diverge from the binary being debugged and would misname a frame silently.
#
# What this guard is really holding down is the ONE case the two spellings collide on. A declaration's
# leaf and a local are both spelled `k_<something>`:
#
#     type value k_Box { public int32 k_x; }   ->  k_Fdm__k_Box  with a member  k_k_x
#     int32 k_near = …                         ->  k_k_near
#     int32 near   = …                         ->  k_near
#
# so `k_Fdm__k_Box` must come back `k_Box` (the author's own type name, NOT `Box`), while `k_k_near`
# must come back `k_near` and `k_near` must come back `near`. The rule that separates them is not
# lexical — it is "strip the register only off a token no table claimed" — and nothing else in the suite
# exercises it, because a diagnostic never carries a `k_` name at all (the analysis maps are keyed by the
# kama spelling; the prefix is the C write only).
#
# The generic rows matter for the same reason from the other direction: a debugger's locals and frames are
# full of generic instances, and rendering those needs the front end's tables, not a lexical rule.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-demangle: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The file STEM is what mints the file-private scope (`dm.kama` -> `k_Fdm`), so the name here is load
# bearing and the expectations below spell it.
cat > "$tmp/dm.kama" <<'KAMA'
import { std::collections::DynamicArray };

// A user type whose own name is already in the user register, so the emitted name doubles the prefix.
type value k_Box {
    public int32 k_x;
    public ctor make(int32 k_x) { this.k_x = k_x; return this; }
}

type value Pair<T> {
    public T a;
    public T b;
    public ctor make(T a, T b) { this.a = a; this.b = b; return this; }
    public fn T sum() { return this.a + this.b; }
}

fn int32 main() {
    int32 near = 1;
    int32 k_near = 2;
    k_Box b = k_Box.make(k_x: near);
    Pair<int32> p = Pair.make(a: k_near, b: 3);
    DynamicArray<string> names = DynamicArray.empty();
    names.add(item: "ab");
    Optional<string> first = Optional::Some(value: "cd");
    int32 n = match (first) { case Some(value: s): cast<int32>(s.length()); case None: 0; };
    return b.k_x + p.sum() + cast<int32>(names.length()) + n;
}
KAMA

# The fixture must ANALYZE CLEANLY, or the tables the answers come off are whatever error recovery
# happened to leave behind — and a later change to recovery would move these answers for a reason that
# has nothing to do with demangling.
"$KAMA" check "$tmp/dm.kama" > "$tmp/chk" 2>&1 \
    || { echo "check-demangle: FAIL — the probe no longer analyzes cleanly:" >&2; sed 's/^/  /' "$tmp/chk" >&2; exit 1; }

fail() { echo "check-demangle: FAIL — $1" >&2; exit 1; }

# `want <mangled> <expected>` — one name through the `--` form.
want() {
    got=$("$KAMA" demangle "$tmp/dm.kama" -- "$1" 2>"$tmp/err") || {
        echo "check-demangle: FAIL — \`kama demangle -- $1\` exited nonzero:" >&2
        sed 's/^/  /' "$tmp/err" >&2; exit 1
    }
    [ "$got" = "$2" ] || fail "\`$1\` demangled to \`$got\`, expected \`$2\`"
}

# --- the register, and the collision it has to survive -------------------------------------------
want k_near               near        # a local the author called `near`
want k_k_near             k_near      # ...and one the author really did call `k_near`
want k_Fdm__k_Box         k_Box       # a DECLARATION's leaf is not in the register: do not strip it
want k_k_x                k_x         # ...but its member is
want k_Fdm__k_Box__make   k_Box::make

# --- generic instances: the half no lexical rule can reach ---------------------------------------
want k_Fdm__Pair_int32          'Pair<int32>'
want k_Fdm__Pair_int32__sum     'Pair<int32>::sum'
# A stdlib instance, with the allocator left at its default — which must NOT be rendered, or every
# container in the debugger reads `DynamicArray<string, GlobalAllocator>`.
want std__collections__DynamicArray_string_kama__GlobalAllocator        'std::collections::DynamicArray<string>'
want std__collections__DynamicArray_string_kama__GlobalAllocator__dtor  'std::collections::DynamicArray<string>::dtor'
# A PRELUDE instance. The prelude's scope is implicit in source, and stripping it before the instance
# lookup used to destroy the key this needs — `Optional_string` instead of `Optional<string>`.
want kama__Optional_string  'Optional<string>'
want kama_main              main

# --- a name is rewritten IN PLACE, so a whole line of C survives its surroundings -----------------
line=$(printf 'int32_t k_Fdm__Pair_int32__sum(k_Fdm__Pair_int32* self)' | "$KAMA" demangle "$tmp/dm.kama")
[ "$line" = 'int32_t Pair<int32>::sum(Pair<int32>* self)' ] \
    || fail "a C declaration line demangled to \`$line\`"

# --- the BATCH, which is the whole reason this is a subcommand and not a flag ---------------------
# One analysis is ~140ms and one answer off the finished tables is a lookup, so a debugger that repaints
# locals at every stop needs many answers from ONE process. If stdout is left block-buffered the caller
# waits forever for a line sitting in this process's buffer, so this also proves the flush.
printf 'k_near\nk_k_near\nk_Fdm__Pair_int32__sum\n\nkama__Optional_string\n' > "$tmp/in.txt"
"$KAMA" demangle "$tmp/dm.kama" < "$tmp/in.txt" > "$tmp/out.txt" 2>"$tmp/err" \
    || { echo "check-demangle: FAIL — the stdin form exited nonzero:" >&2; sed 's/^/  /' "$tmp/err" >&2; exit 1; }
cat > "$tmp/want.txt" <<'EOF'
near
k_near
Pair<int32>::sum

Optional<string>
EOF
if ! diff -u "$tmp/want.txt" "$tmp/out.txt" > "$tmp/diff.txt"; then
    echo "check-demangle: FAIL — the batch answered differently (want vs got):" >&2
    sed 's/^/  /' "$tmp/diff.txt" >&2
    exit 1
fi

echo "check-demangle: OK (the register strips exactly once and never off a declaration's leaf;"
echo "                    generic instances render through the front end; a batch answers line for line)"
