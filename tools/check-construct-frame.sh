#!/bin/sh
# check-construct-frame.sh — a named constructor builds its value where it will live, and costs no copy of it.
#
# A ctor was a value-returning factory: it built the object in its own `kama_self` and returned it, and each site
# copied the result into place. A 16 KB `type value` therefore cost its size two or three times per frame: a local
# `T x = T.make(…)` held the temporary and `x` (32,848 bytes at -O0), `new T.make(…)` held a 16 KB temporary for
# an object that lives on the heap, and a ctor building a field from another ctor held both (32,864 under GCC).
# Since 0.9.559 such a ctor is `T__make__into(T* self, …)` and every site that has storage hands it over (KR-120).
# Read back from the C compiler itself (`-fstack-usage`), at -O0 and -O2, as check-fill-frame.sh does.
#
# Not asserted: a FUNCTION returning a large value (`fn Big f() { return Big.make(…); }`). Its result is built in
# place in the return temporary, but whether that temporary is the caller's return slot is the C compiler's call:
# clang makes it so at -O0, GCC only at -O2.
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

cat > "$tmp/cf.kama" <<'KAMA'
import { std::memory::Owned };
type value Big {
    InlineArray<int64>#(2048) a;
    int64 n;
    public ctor make(int64 v) { this.a = [v; (2048)]; this.n = v; }
    public const fn int64 last() { return this.a[2047] + this.n; }
}
type value Outer {
    Big b;
    int64 k;
    public ctor make(int64 v) { this.b = Big.make(v: v); this.k = v; }
    public const fn int64 last() { return this.b.last() + this.k; }
}
fn int64 local(int64 v) { Big b = Big.make(v: v); return b.last(); }
fn int64 heap(int64 v) { Owned<Big> p = new Big.make(v: v); return p.last(); }
fn int64 outer(int64 v) { Outer o = Outer.make(v: v); return o.last(); }
fn int32 main() { return cast<int32>(local(v: 1i64) + heap(v: 2i64) + outer(v: 3i64)); }
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
    [ -n "$loc" ] && [ -n "$hp" ] && [ -n "$ot" ] && [ -n "$oi" ] || fail "$opt: did not find every function in $su"
    # `Big` is 16,392 bytes. One copy plus spill room passes; a second (≥ 32,784) does not.
    [ "$loc" -lt 24576 ] || fail "$opt: \`Big b = Big.make(…)\` has a $loc-byte frame — Big is 16,392, so it holds a temporary too"
    [ "$ot" -lt 24576 ]  || fail "$opt: \`Outer o = Outer.make(…)\` has a $ot-byte frame — Outer is 16,400, so it holds a temporary too"
    [ "$hp" -lt 1024 ]   || fail "$opt: \`new Big.make(…)\` has a $hp-byte frame — the object lives on the heap, so the frame holds a temporary"
    [ "$oi" -lt 1024 ]   || fail "$opt: \`this.b = Big.make(…)\` in a ctor costs a $oi-byte frame — the field was built in a temporary"
done
echo "check-construct-frame: PASS (a 16 KB value built by its ctor costs one copy in a local's frame, and none for a \`new\` block or a field under construction, at -O0 and -O2)"
