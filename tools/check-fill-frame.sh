#!/bin/sh
# check-fill-frame.sh — `[v; N]` costs the array's own storage on the stack, and nothing more.
#
# A fill literal was the VALUE `T__fill(v)`: built in a temporary, then copied into its destination. So the
# local `InlineArray<int64>#(1024) a = [s; (1024)];` held two copies of an 8 KB array in its frame — 16,464 bytes
# at -O0 and 16,432 at -O2 — and a constructor's `this.slots = [blank; (1024)];` an 8 KB temporary for storage
# that already lived in the object. Since 0.9.534 a fill whose destination is a place (a local being declared, a
# field or element being assigned) is written there through `T__fillInto`. This reads the frames back from the C
# compiler itself (`-fstack-usage`), at -O0 and -O2, because a fixture's `-Wframe-larger-than` would be judged on
# every leg, and ASan's redzones and wasm's shadow stack price a frame differently.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-fill-frame: $KAMA not built" >&2; exit 1; fi
CC_=${CC:-cc}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-fill-frame: FAIL — $1" >&2; exit 1; }

cat > "$tmp/fill.kama" <<'KAMA'
fn int64 local(int64 s, isize i) {
    InlineArray<int64>#(1024) a = [s; (1024)];
    a[i] = 0;
    return a[1023];
}
type resource Holder {
    InlineArray<int64>#(1024) slots;
    public ctor make(int64 blank) { this.slots = [blank; (1024)]; }
    public const fn int64 last() { return this.slots[1023]; }
}
fn int32 main() { Holder h = Holder.make(blank: 2i64); return cast<int32>(local(s: 7i64, i: 3) + h.last()); }
KAMA
"$KAMA" transpile "$tmp/fill.kama" -o "$tmp/fill.c" --no-line >/dev/null 2>"$tmp/t.err" || { cat "$tmp/t.err" >&2; fail "transpile failed"; }

for opt in -O0 -O2; do
    rm -f "$tmp"/*.su
    if ! (cd "$tmp" && "$CC_" -std=c11 $opt -fwrapv -I"$ROOT/include" -fstack-usage -c fill.c -o fill.o) >/dev/null 2>"$tmp/cc.err"; then
        echo "check-fill-frame: SKIP — $CC_ does not take -fstack-usage"; exit 0
    fi
    su=$(ls "$tmp"/*.su 2>/dev/null | head -1)
    [ -n "$su" ] || { echo "check-fill-frame: SKIP — $CC_ wrote no .su file"; exit 0; }
    # A frame line is `file:line:col:NAME<TAB>BYTES<TAB>KIND`; the names are kama's mangled ones.
    loc=$(awk -F'\t' '$1 ~ /:k_Ffill__local$/ {print $2}' "$su")
    mk=$(awk -F'\t' '$1 ~ /:k_Ffill__Holder__make$/ {print $2}' "$su")
    [ -n "$loc" ] && [ -n "$mk" ] || fail "$opt: did not find both functions in $su"
    # The array is 8,192 bytes. One copy plus spill room passes; two copies (≥ 16,384) does not.
    [ "$loc" -lt 12288 ] || fail "$opt: a local filled by \`[s; (1024)]\` has a $loc-byte frame — the array is 8,192, so it holds a second copy"
    [ "$mk" -lt 1024 ] || fail "$opt: a field filled by \`this.slots = [blank; (1024)]\` costs a $mk-byte frame — the fill went through a temporary"
done
echo "check-fill-frame: PASS (an 8 KB fill costs one copy in a local's frame and none for a field, at -O0 and -O2)"
