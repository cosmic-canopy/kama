#!/bin/sh
# check-wait-weak-link.sh — `Atomic.wait` uses no private symbol, and below macOS 14.4 it still links and runs.
#
# Decision 14 of KR-3's pools brief, refined by step 2's W3: a thread asleep in `Atomic.wait` sleeps on the public
# `os_sync_wait_on_address`, which exists from macOS 14.4 and iOS 17.4, and below that on its own condvar. The
# private `__ulock_wait` that every other runtime on the platform reaches for would stand in the way of App Review
# (KR-281), so no binary kama writes may name it. And kama's macOS floor is 14.0, so the public call must be a WEAK
# reference behind `__builtin_available`: a strong one makes dyld refuse to launch the program on 14.0–14.3 at all,
# before a line of it runs, and no fixture on this box (macOS 26) would notice.
#
# ⚠️ THE GUARD PROVES ITS OWN INSTRUMENT. The same program built at the SDK's own deployment target, above 14.4, must
# reference the symbol STRONGLY: a pattern that could never tell the two apart fails there instead of passing.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
case "$(uname -s)" in Darwin) ;; *) echo "check-wait-weak-link: skipped (macOS only)"; exit 0;; esac
fail() { echo "check-wait-weak-link: FAIL — $*" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/w.kama" <<'EOF'
import { std::concurrent::Atomic };
fn uint8 main() {
    Atomic<uint32> c = Atomic.make(value: 7);
    c.notifyAll();
    return cast<uint8>(c.wait(whileEquals: 0));
}
EOF
mkdir "$tmp/old" "$tmp/new"
# --no-cache: the deployment target reaches clang through the environment, which the object cache does not key on.
MACOSX_DEPLOYMENT_TARGET=14.0 "$KAMA" build "$tmp/w.kama" --no-cache -o "$tmp/old/w" >/dev/null 2>"$tmp/err" \
    || { sed 's/^/  /' "$tmp/err" >&2; fail "the program did not build at deployment target 14.0"; }
"$KAMA" build "$tmp/w.kama" --no-cache -o "$tmp/new/w" >/dev/null 2>"$tmp/err" \
    || { sed 's/^/  /' "$tmp/err" >&2; fail "the program did not build at the SDK's deployment target"; }

nm -m "$tmp/old/w" > "$tmp/old.nm"
nm -m "$tmp/new/w" > "$tmp/new.nm"
grep -q '(undefined) weak external _os_sync_wait_on_address' "$tmp/old.nm" \
    || fail "at deployment target 14.0, os_sync_wait_on_address is not a weak reference: the program cannot start on 14.0-14.3"
grep '_os_sync_wait_on_address' "$tmp/new.nm" | grep -qv 'weak' \
    || fail "the instrument cannot tell: above 14.4 the reference should be strong, and nm -m shows no strong one"
if grep -q 'ulock' "$tmp/old.nm" "$tmp/new.nm"; then
    fail "a binary names a private __ulock symbol: $(grep -h ulock "$tmp/old.nm" "$tmp/new.nm" | head -1)"
fi

st=0; "$tmp/old/w" || st=$?
[ "$st" = 7 ] || fail "the 14.0 build answered $st, not the 7 the cell holds"

echo "PASS check-wait-weak-link (os_sync_wait_on_address weak at 14.0 and strong above it; no __ulock)"
