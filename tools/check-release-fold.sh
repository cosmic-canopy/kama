#!/bin/sh
# check-release-fold.sh — a native `--release` build folds every module into ONE C translation unit, and a
# file-scope helper that two modules both need is then defined once.
#
# A debug build compiles one C file per module, so the emitter gives each module its own `static` copy of the
# helpers its bodies use: an isolate trampoline, an intrinsic target's contract vtbl and its thunks, a bindable's
# release thunk. They are named after what they serve, not after the module, because they are identical. The
# release build concatenates the modules' C into one file, and until `0.9.543` each copy came along: two files that
# passed an `int32` to a contract parameter through a `type adapter`, or that each spawned the same worker, built in
# debug and stopped every release build at "redefinition of '…'". The fixture suite builds debug only, so nothing
# saw it.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-release-fold: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# One program, three helper kinds, each needed by both `a.kama` and `b.kama`.
p="$tmp/fold"
mkdir -p "$p/src"
cat > "$p/kama.json" <<'EOF'
{ "name": "fold", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } } }
EOF
cat > "$p/src/shared.kama" <<'EOF'
import { std::memory::Owned };
export { Shown, show, Counter, worker, Compare, Scaler, sub };
// An intrinsic target bound to a contract: a vtbl + thunk per using module.
type contract Shown for value, resource, enum, intrinsic { const fn int64 twice(); }
type adapter <int32> implements Shown { public const fn int64 twice() { return cast<int64>(this) * 2i64; } }
fn int64 show(Shown v) { return v.twice(); }
// A worker spawned with a borrow: an isolate trampoline per spawning module.
type resource Counter implements Sendable {
    int64 n = 0;
    public ctor make() { }
    public fn void bump() { this.n = this.n + 1; }
    public const fn int64 value() { return this.n; }
}
fn void worker(ref Counter c) { c.bump(); }
// A bindable over an `Owned` box: a release thunk per binding module.
fnptr int32 Compare(int32 a, int32 b);
type value Scaler {
    int32 k;
    public ctor make(int32 k) { this.k = k; }
    fn int32 apply(int32 a, int32 b) { return (a - b) * this.k; }
}
fn int32 sub(int32 a, int32 b) { return a - b; }
EOF
for m in a b; do
cat > "$p/src/$m.kama" <<EOF
import { show, Counter, worker, Compare, Scaler, std::concurrent::Isolate, std::memory::Owned };
export { run_$m };
fn int64 run_$m() {
    Counter c = Counter.make();
    scope { spawn worker(c: ref c); }
    Owned<Scaler> s = new Scaler.make(k: 2);
    BindableFunctionPtr<Compare> f = BindableFunctionPtr.bind(obj: s, method: Scaler::apply);
    return show(v: 3) + c.value() + cast<int64>(f(a: 5, b: 4));   // 6 + 1 + 2
}
EOF
done
cat > "$p/src/main.kama" <<'EOF'
import { run_a, run_b };
fn int32 main() { return cast<int32>(run_a() + run_b()); }   // 18
EOF

for tier in --debug --release; do
    if ! "$KAMA" build "$p/kama.json" $tier -o "$tmp/fold$tier" >"$tmp/build.log" 2>&1; then
        echo "check-release-fold: FAIL — the $tier build of two modules sharing helpers did not build:" >&2
        sed 's/^/  /' "$tmp/build.log" | head -8 >&2
        fail=1; continue
    fi
    rc=0; "$tmp/fold$tier" || rc=$?
    if [ "$rc" != 18 ]; then
        echo "check-release-fold: FAIL — the $tier build returned $rc, expected 18" >&2
        fail=1
    fi
done

[ "$fail" = 0 ] || exit 1
echo "check-release-fold: PASS (an adapter vtbl, an isolate trampoline and a bindable release thunk, each needed by two modules: debug and the one-unit release build)"
