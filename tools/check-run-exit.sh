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
fn uint8 main() {
    InlineArray<int32>#(2) a = [0; 2];
    int32 i = 2;
    return cast<uint8>(a[i]);   // out of bounds: traps in a debug build
}
KAMA
cat > "$tmp/seven.kama" <<'KAMA'
fn uint8 main() { return 7; }
KAMA

"$KAMA" build "$tmp/trap.kama" -o "$tmp/trap" >/dev/null 2>&1
# The runs build in a temp directory of their own, which must be empty afterwards: each `kama run` used to leave an
# object cache there, `.kama-cache/kama-run-<pid>/`, named for a process no later build shares.
mkdir "$tmp/runtmp"
TMPDIR=$(kama_native_path "$tmp/runtmp"); export TMPDIR
set +e
# In a braced group with its stderr closed off: the shell reports a child's signal death ("Abort trap: 6")
# on its own stderr, and that line is the expected outcome, not news.
{ "$tmp/trap" >/dev/null 2>&1; } 2>/dev/null; direct=$?
"$KAMA" run "$tmp/trap.kama" >/dev/null 2>&1; viaRun=$?
"$KAMA" run "$tmp/seven.kama" >/dev/null 2>&1; seven=$?
# Four runs at once share the temp directory, and each must still build: a native run's generated files once went
# straight into it, named for their module (`seven.c`, `std__ptr__ptr.c`), so concurrent runs wrote, compiled and
# removed each other's and failed with "no such file" or a failed link (9 runs of 20, four at a time, 0.9.947).
for i in 1 2 3 4; do ( "$KAMA" run "$tmp/seven.kama" >"$tmp/par$i.log" 2>&1; echo $? >"$tmp/par$i.rc" ) & done
wait
set -e
left=$(ls -A "$tmp/runtmp")
for i in 1 2 3 4; do
    if [ "$(cat "$tmp/par$i.rc")" != 7 ]; then
        echo "check-run-exit: FAIL — one of four \`kama run\`s at once exited $(cat "$tmp/par$i.rc"), not 7:" >&2
        sed 's/^/    /' "$tmp/par$i.log" >&2; exit 1
    fi
done
# The race above is likely, not certain, so the cause is checked directly too: a run writes nothing in the temp
# directory but its own `kama-run-<pid>/`. `--keep-c` keeps that directory, so what a run wrote is still there to see.
if [ -z "$left" ]; then
    mkdir "$tmp/keeptmp"
    TMPDIR=$(kama_native_path "$tmp/keeptmp") "$KAMA" run --keep-c "$tmp/seven.kama" >/dev/null 2>&1 || true
    kept=$(ls -A "$tmp/keeptmp")
    if [ "$(ls -A "$tmp/keeptmp" | wc -l)" -ne 1 ] || [ ! -d "$tmp/keeptmp/$kept" ] \
       || [ "${kept#kama-run-}" = "$kept" ]; then
        echo "check-run-exit: FAIL — \`kama run\` wrote outside a directory of its own in the temp directory:" \
             "$(ls -A "$tmp/keeptmp" | tr '\n' ' ')" >&2; exit 1
    fi
    if [ ! -f "$tmp/keeptmp/$kept/seven.c" ]; then
        echo "check-run-exit: FAIL — \`kama run --keep-c\` kept no seven.c in $kept: $(ls -A "$tmp/keeptmp/$kept" | tr '\n' ' ')" >&2
        exit 1
    fi
fi

if [ "$direct" -eq 0 ]; then
    echo "check-run-exit: FAIL — the trapping probe exited 0 when run directly; the probe no longer traps" >&2; exit 1
fi
if [ "$viaRun" -ne "$direct" ]; then
    echo "check-run-exit: FAIL — \`kama run\` of a trapping program exited $viaRun, the binary itself $direct" >&2; exit 1
fi
if [ "$seven" -ne 7 ]; then
    echo "check-run-exit: FAIL — \`kama run\` of \`return 7\` exited $seven" >&2; exit 1
fi
if [ -n "$left" ]; then
    echo "check-run-exit: FAIL — \`kama run\` left this in its temp directory: $left" >&2; exit 1
fi
echo "check-run-exit: OK (a trap exits $direct both ways; a plain exit code is forwarded; four runs at once all build; a run writes only in a directory of its own, and leaves the temp directory empty)"
