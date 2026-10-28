#!/bin/sh
# run.sh — build uring.c and run every arm INTERLEAVED, one line per run into <out.tsv> (KR-253 decision 8).
#
# Usage: bench/run uring <out.tsv> [rounds]            then: python3 bench/uring/summarize.py <out.tsv>
#        SLOTS="et ring-op et#2" CONFIGS="10000:400000:64" narrow a run to some arms and loads (conns:requests:reply).
#
# Linux only, and it needs io_uring: `bench/run uring` starts the container with seccomp unconfined and its SELinux label
# disabled, since Podman's seccomp answers ENOSYS and Fedora's container type EACCES. This script refuses to measure
# where the ring cannot be set up, rather than measure nothing. Each round runs every arm on every load as a fresh
# process, in an order rotated by the round, so drift lands on every arm alike. The reference arm, `et` (kama's epoll
# protocol today), runs twice, the second time as `et#2`: its gap to itself is the noise floor a real difference has to
# clear. The output goes where the caller says, never into the worktree.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-15}
[ "$(uname)" = Linux ] || { echo "run.sh: io_uring is Linux only" >&2; exit 2; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -o "$tmp/uring" "$here/uring.c"
"$tmp/uring" ring-op 3 3 64 >/dev/null || { echo "run.sh: io_uring is unavailable here (see the error above)" >&2; exit 2; }
slots=${SLOTS:-"et ring-poll ring-op ring-op-pf ring-send ring-send-pf ring-multishot et#2"}
configs=${CONFIGS:-"100:200000:64 1000:300000:64 10000:400000:64 1000:300000:4096 10000:400000:4096"}
printf 'round\tslot\tkernel\t%s\n' "$("$tmp/uring" -h)" > "$out"
kernel=$(uname -r)
n=$(echo $slots | wc -w | tr -d ' ')
r=1
while [ "$r" -le "$rounds" ]; do
    for cfg in $configs; do
        c=${cfg%%:*}; rest=${cfg#*:}; q=${rest%%:*}; reply=${rest#*:}
        set -- $slots
        k=$(( (r - 1) % n )); while [ "$k" -gt 0 ]; do x=$1; shift; set -- "$@" "$x"; k=$((k - 1)); done
        for slot in "$@"; do
            printf '%s\t%s\t%s\t%s\n' "$r" "$slot" "$kernel" "$("$tmp/uring" "${slot%#*}" "$c" "$q" "$reply")" >> "$out"
        done
    done
    echo "round $r/$rounds done" >&2
    r=$((r + 1))
done
