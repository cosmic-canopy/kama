#!/bin/sh
# check-construct-frame.sh — a named constructor builds its value where it will live, and costs no copy of it.
#
# A ctor was a value-returning factory: it built the object in its own `kama_self` and returned it, and each site
# copied the result into place. A 16 KB `type value` therefore cost its size two or three times per frame: a local
# `T x = T.make(…)` held the temporary and `x` (32,848 bytes at -O0), `new T.make(…)` held a 16 KB temporary for
# an object that lives on the heap, and a ctor building a field from another ctor held both (32,864 under GCC).
# Since 0.9.559 such a ctor is `T__make__into(T* self, …)` and every site that has storage hands it over (KR-120);
# since 0.9.560 a fallible one builds in the `Ok` payload of the `Result` it is handed (32,880 bytes at -O0 before).
# Read back from the C compiler itself (`-fstack-usage`), at -O0 and -O2, as check-fill-frame.sh does.
#
# And a `match` reads a plain value payload in place (KR-131): `case Ok(value: f): f.opaque()` over a local or a
# temporary held `Fal` twice — the subject and the copy in `f` — 32,912 bytes at -O0 and 32,832 at -O2 on 0.9.654,
# because the arm hands `f`'s address to C the compiler cannot see (`cfOpaque`), so not even -O2 could drop the copy.
#
# And two shapes outside the ctor work until KR-197: a large value RETURNED into a local, `Big b = newBig(…)`, lowered
# to `Big b;` and then `b = newBig(…);`, so the call returned into a temporary that was copied into `b` (32,824 bytes
# at -O0); and a `[v; N]` field default, a by-value fill into a temporary as big as the array, in the frame of every
# ctor (35,400 bytes for a 35 KB pair). The local is now initialized by the call, `Big b = newBig(…);`, which lets the
# C compiler hand the call `b` as its return slot, and the field is filled in place.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-construct-frame: $KAMA not built" >&2; exit 1; fi
CC_=${CC:-cc}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-construct-frame: FAIL — $1" >&2; exit 1; }

printf '#include <stdint.h>\nint64_t cfOpaque(const int64_t* p);\n' > "$tmp/cf_opaque.h"
cat > "$tmp/cf.kama" <<'KAMA'
import { std::memory::Owned };
extern "cf_opaque.h";
extern fn int64 cfOpaque(UnsafeConstPtr<int64> p);   // defined nowhere the C compiler can see
type value Big {
    InlineArray<int64>#(2048) a;
    int64 n;
    public ctor make(int64 v) { this.a = [v; (2048)]; this.n = v; }
    public const fn int64 last() { return this.a[2047] + this.n; }
}
type enum Why implements Error { Negative; public const fn string message() { return "negative"; } }
type value Fal {
    InlineArray<int64>#(2048) a;
    public ctor Result<Fal, Why> make(int64 v) {
        if (v < 0i64) { return Result::Err(error: Why::Negative); }
        this.a = [v; (2048)];
    }
    public const fn int64 last() { return this.a[2047]; }
    public unsafe const fn int64 opaque() { return cfOpaque(p: addr(of: this.a[0])); }
}
fn int64 matchedLocal(int64 v) { Result<Fal, Why> r = Fal.make(v: v); return match (r) { case Ok(value: f): f.opaque(); case Err(error: e): 0i64; }; }
fn int64 matchedTemp(int64 v) { return match (Fal.make(v: v)) { case Ok(value: f): f.opaque(); case Err(error: e): 0i64; }; }
fn int64 lastOf(const ref Result<Fal, Why> r) { return match (r) { case Ok(value: f): f.last(); case Err(error: e): 0i64; }; }
fn int64 fallible(int64 v) { Result<Fal, Why> r = Fal.make(v: v); return lastOf(r: r); }
type value Outer {
    Big b;
    int64 k;
    public ctor make(int64 v) { this.b = Big.make(v: v); this.k = v; }
    public const fn int64 last() { return this.b.last() + this.k; }
}
fn int64 local(int64 v) { Big b = Big.make(v: v); return b.last(); }
fn int64 heap(int64 v) { Owned<Big> p = new Big.make(v: v); return p.last(); }
fn int64 outer(int64 v) { Outer o = Outer.make(v: v); return o.last(); }
fn Big newBig(int64 v) { return Big.make(v: v); }
fn int64 returned(int64 v) { Big b = newBig(v: v); return b.last(); }
type value Det {
    InlineArray<int64>#(2048) fft = [0i64; 2048];
    int64 k = 0;
    public ctor tuned(int64 k) { this.k = k; }
    public const fn int64 last() { return this.fft[2047] + this.k; }
}
fn int64 det(int64 v) { Owned<Det> d = new Det.tuned(k: v); return d.last(); }
fn uint8 main() { return cast<uint8>(local(v: 1i64) + heap(v: 2i64) + outer(v: 3i64) + fallible(v: 4i64)
                                     + matchedLocal(v: 5i64) + matchedTemp(v: 6i64) + returned(v: 7i64) + det(v: 8i64)); }
KAMA
"$KAMA" transpile "$tmp/cf.kama" -o "$tmp/cf.c" --no-line >/dev/null 2>"$tmp/t.err" || { cat "$tmp/t.err" >&2; fail "transpile failed"; }

for opt in -O0 -O2; do
    rm -f "$tmp"/*.su
    if ! (cd "$tmp" && "$CC_" -std=c11 $opt -fwrapv -I"$ROOT/include" -fstack-usage -c cf.c -o cf.o) >/dev/null 2>"$tmp/cc.err"; then
        echo "check-construct-frame: SKIP — $CC_ does not take -fstack-usage"; exit 0
    fi
    su=$(ls "$tmp"/*.su 2>/dev/null | head -1)
    [ -n "$su" ] || { echo "check-construct-frame: SKIP — $CC_ wrote no .su file"; exit 0; }
    frame() { awk -F'\t' -v f="$1" '$1 ~ (":" f "$") {print $2}' "$su"; }
    loc=$(frame k_Fcf__local); hp=$(frame k_Fcf__heap); ot=$(frame k_Fcf__outer); oi=$(frame k_Fcf__Outer__make__into)
    [ -n "$oi" ] || oi=$(frame k_Fcf__Outer__make)   # a compiler with no into form: its ctor IS the value form
    fl=$(frame k_Fcf__fallible); fi=$(frame k_Fcf__Fal__make__into)
    ml=$(frame k_Fcf__matchedLocal); mt=$(frame k_Fcf__matchedTemp)
    [ -n "$fi" ] || fi=$(frame k_Fcf__Fal__make)   # a compiler with no into form
    rt=$(frame k_Fcf__returned); dt=$(frame k_Fcf__Det__tuned__into)
    [ -n "$dt" ] || dt=$(frame k_Fcf__Det__tuned)
    [ -n "$loc" ] && [ -n "$hp" ] && [ -n "$ot" ] && [ -n "$oi" ] && [ -n "$fl" ] && [ -n "$fi" ] \
        && [ -n "$ml" ] && [ -n "$mt" ] && [ -n "$rt" ] && [ -n "$dt" ] || fail "$opt: did not find every function in $su"
    # `Big` is 16,392 bytes. One copy plus spill room passes; a second (≥ 32,784) does not.
    [ "$loc" -lt 24576 ] || fail "$opt: \`Big b = Big.make(…)\` has a $loc-byte frame — Big is 16,392, so it holds a temporary too"
    [ "$ot" -lt 24576 ]  || fail "$opt: \`Outer o = Outer.make(…)\` has a $ot-byte frame — Outer is 16,400, so it holds a temporary too"
    [ "$hp" -lt 1024 ]   || fail "$opt: \`new Big.make(…)\` has a $hp-byte frame — the object lives on the heap, so the frame holds a temporary"
    [ "$oi" -lt 1024 ]   || fail "$opt: \`this.b = Big.make(…)\` in a ctor costs a $oi-byte frame — the field was built in a temporary"
    [ "$fi" -lt 1024 ]   || fail "$opt: a fallible ctor costs a $fi-byte frame — it built its object in a local, not in its \`Ok\` payload"
    [ "$fl" -lt 24576 ]  || fail "$opt: \`Result<Fal, Why> r = Fal.make(…)\` has a $fl-byte frame — Fal is 16,384, so it holds a temporary too"
    [ "$ml" -lt 24576 ]  || fail "$opt: \`match (r) { case Ok(value: f): … }\` over a local has a $ml-byte frame — it copied the payload into \`f\`"
    [ "$mt" -lt 24576 ]  || fail "$opt: \`match (Fal.make(…)) { case Ok(value: f): … }\` has a $mt-byte frame — it copied the payload into \`f\`"
    [ "$rt" -lt 24576 ]  || fail "$opt: \`Big b = newBig(…)\` has a $rt-byte frame — the call returned into a temporary copied into \`b\`"
    [ "$dt" -lt 1024 ]   || fail "$opt: a \`[v; N]\` field default costs a $dt-byte ctor frame — it was filled in a temporary"
done
echo "check-construct-frame: PASS (a 16 KB value built by its ctor, fallible or not, or returned by a function, costs one copy in a local's frame, and none for a \`new\` block, a field under construction, a \`[v; N]\` field default or the ctor's own frame, and a \`match\` reads its payload in place, at -O0 and -O2)"
