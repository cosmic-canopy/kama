#!/bin/sh
# check-run-exit.sh — `kama run` forwards the program's exit status, INCLUDING a death by signal.
#
# A trap aborts the process (SIGABRT), which leaves no exit code, and the driver read the wait status with
# WEXITSTATUS alone — 0 for a signal. So `kama run` of a program that trapped printed the trap and reported
# SUCCESS, while the same binary run directly exited 134. Found by a mutation check on tests/net_addr_v6
# (0.9.371). The fixture harness runs built binaries, so no fixture could see it.
#
# The claim checked is "same status as the binary run directly", not a number: 134 on POSIX, and whatever
# abort() exits with on Windows, where system() already returned the code.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-run-exit: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/trap.kama" <<'KAMA'
fn int32 main() {
    InlineArray<int32>#(2) a = [0; 2];
    int32 i = 2;
    return a[i];   // out of bounds: traps in a debug build
}
KAMA
cat > "$tmp/seven.kama" <<'KAMA'
fn int32 main() { return 7; }
KAMA

"$KAMA" build "$tmp/trap.kama" -o "$tmp/trap" >/dev/null 2>&1
set +e
# In a braced group with its stderr closed off: the shell reports a child's signal death ("Abort trap: 6")
# on its own stderr, and that line is the expected outcome, not news.
{ "$tmp/trap" >/dev/null 2>&1; } 2>/dev/null; direct=$?
"$KAMA" run "$tmp/trap.kama" >/dev/null 2>&1; viaRun=$?
"$KAMA" run "$tmp/seven.kama" >/dev/null 2>&1; seven=$?
set -e

if [ "$direct" -eq 0 ]; then
    echo "check-run-exit: FAIL — the trapping probe exited 0 when run directly; the probe no longer traps" >&2; exit 1
fi
if [ "$viaRun" -ne "$direct" ]; then
    echo "check-run-exit: FAIL — \`kama run\` of a trapping program exited $viaRun, the binary itself $direct" >&2; exit 1
fi
if [ "$seven" -ne 7 ]; then
    echo "check-run-exit: FAIL — \`kama run\` of \`return 7\` exited $seven" >&2; exit 1
fi
echo "check-run-exit: OK (a trap exits $direct both ways; a plain exit code is forwarded)"
