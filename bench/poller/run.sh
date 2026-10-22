#!/bin/sh
# run.sh — build poller.c and run every arm of this host's backend INTERLEAVED, one line per run into <out.tsv>.
#
# Usage: bench/poller/run.sh <out.tsv> [rounds]        then: python3 bench/poller/summarize.py <out.tsv>
#        SLOTS="lt clear-read clear-read#2" CONFIGS="pingpong:10000:400000" narrow a run to some arms and loads.
#
# Each round runs every arm on every workload as a fresh process, in an order rotated by the round, so drift lands on
# every arm alike (this box drifts by ±13%; one arm after another measures the drift). The reference arm runs twice,
# the second time as `<arm>#2`: its gap to itself is the noise floor a real difference has to clear. The output goes
# where the caller says, never into the worktree.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-15}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -o "$tmp/poller" "$here/poller.c"
case $(uname) in
    Linux)  slots=${SLOTS:-"lt et oneshot et#2"} ;;
    Darwin) slots=${SLOTS:-"lt clear dispatch dispatch-now clear#2"} ;;
    *)      echo "run.sh: no backend arms for $(uname)" >&2; exit 2 ;;
esac
configs=${CONFIGS:-"pingpong:100:200000 pingpong:10000:400000 pipeline:100:320000 pipeline:10000:640000"}
printf 'round\tslot\t%s\n' "$("$tmp/poller" -h)" > "$out"
n=$(echo $slots | wc -w | tr -d ' ')
r=1
while [ "$r" -le "$rounds" ]; do
    for cfg in $configs; do
        w=${cfg%%:*}; rest=${cfg#*:}; c=${rest%%:*}; q=${rest#*:}
        set -- $slots
        k=$(( (r - 1) % n )); while [ "$k" -gt 0 ]; do x=$1; shift; set -- "$@" "$x"; k=$((k - 1)); done
        for slot in "$@"; do
            printf '%s\t%s\t%s\n' "$r" "$slot" "$("$tmp/poller" "${slot%#*}" "$w" "$c" "$q")" >> "$out"
        done
    done
    echo "round $r/$rounds done" >&2
    r=$((r + 1))
done
