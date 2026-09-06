#!/bin/sh
# check-panic-multitu.sh — the hosted panic handler (`setPanicHandler`) must fire even when the panic
# ORIGINATES in a different translation unit than the one that registered it. The fatal paths
# (bounds/panic/assert) are `static inline` and inlined into every TU, and the handler slot is process-global
# by contract — so it has EXTERNAL LINKAGE with a single definition in the entry TU (kama_runtime.h +
# `isEntry` in kama.cemit.cpp). A per-TU `static` slot would leave a panic raised in library code reading its
# own empty copy and silently taking the default abort instead of the user's handler. This reproduces the
# cross-TU case with a two-file (two-TU) build. Native only (a panic aborts; wasm/SAN abort codes differ — the
# same reason trap fixtures are gated), and it can't be a single-file `tests/*.kama` trap fixture (one file is
# one TU, so it can't exhibit the cross-TU bug).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-panic-multitu: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# lib/lib.kama — a separate module (its own TU). `boom` panics from HERE, not from main's TU. It sits in
# its OWN FOLDER because a module is a folder (SPEC.md §Modules) and a loose build names one by
# its directory: two files sharing one directory are both in the loose ROOT, which §2e.27 makes unimportable.
mkdir -p "$tmp/lib"
cat > "$tmp/lib/lib.kama" <<'KAMA'
export { boom };
fn void boom() {
    panic(msg: "from-lib-TU");
}
KAMA

# main.kama — registers the handler in the ENTRY TU, then triggers a panic that runs in lib's TU.
cat > "$tmp/main.kama" <<'KAMA'
import { lib::boom };
extern "<unistd.h>";
extern fn int64 write(int32 fd, UnsafeConstPtr buf, usize n);
unsafe fn void onPanic() {
    string m = "HANDLER-RAN-CROSS-TU\n";
    write(fd: 2, buf: m.cstr(), n: cast<usize>(m.length()));
}
fn int32 main() {
    setPanicHandler(handler: onPanic);
    boom();          // the panic originates in lib's TU — the handler must still fire
    return 0;
}
KAMA

"$KAMA" build "$tmp/main.kama" "$tmp/lib/lib.kama" -o "$tmp/prog" >/dev/null 2>"$tmp/build.err" || {
    echo "check-panic-multitu: FAIL — build failed" >&2; sed 's/^/  /' "$tmp/build.err" >&2; exit 1; }

# The program is EXPECTED to abort (non-zero) — `|| rc=$?` keeps `set -e` from killing the script here.
rc=0; "$tmp/prog" >/dev/null 2>"$tmp/err" || rc=$?
# The runtime always terminates after the handler. "Terminated" is spelled differently per platform:
# POSIX reports a signal death as 128+signo, while on Windows abort() is not a signal at all — the CRT
# exits 127. That is the same divergence run_tests.sh records for the whole tests/trap/ leg, which it
# skips on Windows for exactly this reason. What this guard is actually about is the cross-TU handler
# below, so assert the portable half: the program did not exit 0.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) abort_ok=$([ "$rc" -ne 0 ] && echo 1 || echo 0) ;;
    *)                    abort_ok=$([ "$rc" -ge 128 ] && echo 1 || echo 0) ;;
esac
if [ "$abort_ok" != 1 ]; then
    echo "check-panic-multitu: FAIL — expected a runtime abort, got $rc" >&2
    sed 's/^/  /' "$tmp/err" >&2; exit 1
fi
# The custom handler must have fired despite the panic originating in another TU.
grep -qF "HANDLER-RAN-CROSS-TU" "$tmp/err" || {
    echo "check-panic-multitu: FAIL — the panic handler did NOT run for a panic raised in another TU" >&2
    echo "  (the process-global handler slot is not shared across translation units)" >&2
    sed 's/^/  /' "$tmp/err" >&2; exit 1; }

echo "check-panic-multitu: PASS (setPanicHandler fires for a panic raised in a different TU)"
