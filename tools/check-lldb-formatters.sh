#!/bin/sh
# check-lldb-formatters.sh — kama values inspect as kama values in a debugger.
#
# `include/kama_lldb.py` teaches LLDB the mapping back from the C lowering: a `string` is a pointer and
# two sizes, an `Optional` is a tag plus a union, a container is a pointer and a count, and every field
# the user declared carries the `k_` register. This drives a real debugger over a real binary and reads
# what it prints, because there is no cheaper way to know: the file is Python that LLDB loads and calls,
# so nothing about it is checked by compiling anything, and a typo in a provider degrades to "no
# formatter applied" rather than to an error.
#
# It is written against what was MEASURED, not what the SB API was assumed to do. Two findings are baked
# into the expectations here and into the module:
#   * a category defined `--language c` registers, reports enabled, and never applies — the providers
#     only took effect once the category was left unscoped;
#   * a summary REPLACES a value's children unless it is added with `-e`, so `len=2` with no elements
#     under it was strictly worse than the raw struct it replaced.
#
# ⚠️ Skips where there is no debugger. ROADMAP_DETAIL §10 records that the Windows box has no lldb, and
# a guard that cannot run must say so rather than pass silently.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-lldb-formatters: $KAMA not built" >&2; exit 1; fi

if ! command -v lldb >/dev/null 2>&1; then
    echo "check-lldb-formatters: SKIP (no lldb on this host)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "check-lldb-formatters: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }

# The compiler is the one that knows where its own formatters are (exe-relative, beside the runtime
# headers), and asking it here is also what asserts that surface exists and answers.
init=$("$KAMA" demangle --lldb-init 2>"$tmp/e") \
    || fail "\`kama demangle --lldb-init\` failed" "$tmp/e"
case "$init" in
    'command script import "'*'/kama_lldb.py"') ;;
    *) fail "--lldb-init printed something unexpected: $init" ;;
esac
py=$("$KAMA" demangle --lldb-init-path)
[ -f "$py" ] || fail "--lldb-init-path named $py, which does not exist"

cat > "$tmp/probe.kama" <<'KAMA'
import { std::collections::DynamicArray, std::collections::Map, std::memory::Shared, std::memory::Weak };

type value Point { public int32 x; public int32 y;
                   public ctor make(int32 x, int32 y) { this.x = x; this.y = y; return this; } }
type resource Leaf { int32 v; public ctor make(int32 v) { this.v = v; } ~Leaf(){}
                     public fn int32 get() { return this.v; } }

fn int32 probe(int32 near)
{
    string s = "hello";
    Optional<string> some = Optional::Some(value: "inside");
    Optional<string> none = Optional::None;
    Point pt = Point.make(x: 3, y: 4);
    DynamicArray<string> xs = DynamicArray.empty();
    xs.add(item: "ab"); xs.add(item: "cde");
    Map<string, int32> m = Map.empty();
    m.put(key: "kk", value: 5);
    Shared<Leaf> sh = new Leaf.make(v: 9);
    Shared<Leaf> sh2 = sh;
    Weak<Leaf> w = sh.downgrade();
    return near + pt.x + cast<int32>(xs.length()) + cast<int32>(m.length()) + sh2.get();
}
fn int32 main() { return probe(near: 1); }
KAMA
BPLINE=22   # the `return` — every local above it is live by then

"$KAMA" build "$tmp/probe.kama" -o "$tmp/probe" > "$tmp/b" 2>&1 || fail "the probe did not build" "$tmp/b"

lldb -b -o "$init" \
     -o "br set -f probe.kama -l $BPLINE" \
     -o run -o 'frame variable' -o 'bt' "$tmp/probe" > "$tmp/out" 2>&1 || true

grep -q 'kama: value formatters loaded' "$tmp/out" \
    || fail "the formatter module did not load" "$tmp/out"
# Without this the run stopped somewhere else (or not at all) and every assertion below would be
# vacuous — a `frame variable` at the wrong stop prints uninitialized locals that match nothing.
grep -q "probe.kama:$BPLINE" "$tmp/out" \
    || fail "never stopped at probe.kama:$BPLINE, so the rest proves nothing" "$tmp/out"

# `want <description> <fixed string>` — the rendering, as a reader sees it.
want() {
    grep -qF -- "$2" "$tmp/out" || fail "$1 — expected to find: $2" "$tmp/out"
    echo "  ok: $1"
}

want 'a string reads as its text'            'k_s = "hello"'
want 'a present Optional reads as Some(...)' 'k_some = Some("inside")'
want 'an absent Optional reads as None'      'k_none = None'
want "a user type's fields lose the k_"      'k_pt = (x = 3, y = 4)'
want 'a container reads as its length'       'k_xs = len=2'
want '...and expands to its elements'        '[0] = "ab"'
want '...in order'                           '[1] = "cde"'
want 'a Map is keyed by its key, not a slot' '["kk"] = 5'
want 'a Shared reports its refcounts'        'k_sh = strong=2 weak=1'
want 'a Shared expands to its pointee'       '* = (v = 9)'

# The NEGATIVE half, and the part most likely to rot: a provider that silently stops applying leaves the
# raw C behind, and every `want` above would still pass if the formatter merely ADDED to it. These name
# the lowering that must no longer be visible.
for raw in 'kama_data = ' 'kama_len = ' 'kama_tag = ' 'k_x = 3' 'k_data = 0x'; do
    ! grep -qF -- "$raw" "$tmp/out" \
        || fail "the C lowering is still showing: \`$raw\` — a provider stopped applying" "$tmp/out"
done
echo "  ok: none of the C lowering is visible"

# --- FRAME NAMES, which are LLDB's half of the name problem ---------------------------------------
# `frame-format` takes a `${script.frame:…}` hook, so the CALL STACK is fixable without an editor: a
# plain `lldb`, a terminal `bt` and every editor that is not VS Code get readable frames from this. It
# is lexical — it cannot render `Pair<int32>` — and an editor running the name layer upgrades it; this
# is the floor, not the ceiling.
want 'a frame reads as the kama function'   'probe(near=1)'
want '...and the entry point is `main`'     '`main()'
# ⚠️ The hook replaces ${function.name-with-args} for EVERY frame in the process, not just kama's, so
# a frame belonging to anything else must come through untouched — including its `+ offset`.
grep -qE 'dyld`start \+ [0-9]+' "$tmp/out" \
    || fail "a non-kama frame lost its default rendering — the frame-format hook is not passing foreign frames through" "$tmp/out"
echo "  ok: a non-kama frame is untouched"

echo "check-lldb-formatters: OK (a real lldb over a real binary renders kama values AND frames as kama)"
