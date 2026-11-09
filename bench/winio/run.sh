#!/bin/sh
# run.sh — build bench/winio's probes and run each, interleaved by round, into <out.tsv>: the measurements behind KR-3
# step 8's brief (ROADMAP_DETAIL s6-windows-io). Windows only, from msys2 UCRT64 (cc, ws2_32, mswsock); today.kama needs
# the kama compiler, $KAMA or ./kama.
#
# Usage: bench/winio/run.sh <out.tsv> [rounds]
#
# Every row starts with the probe's name and the round (0 for the one-shot checks), then that probe's own columns, which
# each file's header names. The output goes where the caller says, never into the worktree.
set -eu
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) ;; *) echo "run.sh: Windows only" >&2; exit 2 ;; esac
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-3}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
for p in timer facts complete pipes transmit; do
    cc -O2 -std=c11 -Wall -Wextra -Werror -o "$tmp/$p.exe" "$here/$p.c" -lws2_32 -lmswsock
done
"${KAMA:-./kama}" build "$here/today.kama" -o "$tmp/today.exe" --release > /dev/null
tag() { while IFS= read -r line; do printf '%s\t%s\t%s\n' "$1" "$2" "$line"; done; }
{
    r=1
    while [ "$r" -le "$rounds" ]; do
        "$tmp/today.exe" | tag today "$r"
        for us in 100 1000 5000; do
            for arm in tick packet percall reuse; do "$tmp/timer.exe" "$arm" "$us" 200 | tag timer "$r"; done
        done
        for arm in assoc assoc-skip event router blocking thread; do "$tmp/complete.exe" "$arm" 50000 | tag complete "$r"; done
        for c in 1 2 4 16; do
            for cfg in "65536 400" "1048576 60" "67108864 2"; do
                set -- $cfg
                for m in copy tf; do "$tmp/transmit.exe" "$m" "$c" "$1" "$2" | tag transmit "$r"; done
            done
        done
        "$tmp/pipes.exe" rate | tag pipes "$r"
        r=$((r + 1))
    done
    "$tmp/timer.exe" rearm 0 100000 | tag timer 0
    "$tmp/facts.exe" | tag facts 0
    for m in pos again cancel "cap 4" "cap 8"; do "$tmp/transmit.exe" $m | tag transmit 0; done
    "$tmp/pipes.exe" reopen | tag pipes 0
    for how in overlapped reopened anon; do "$tmp/pipes.exe" child "$how" 10 | tag pipes 0; done
} > "$out"
