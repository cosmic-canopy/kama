#!/bin/sh
# run.sh — build coldswitch.kama and run its arms INTERLEAVED, one line per run into <out.tsv> (KR-254).
#
# Usage: bench/fiber/run.sh <out.tsv> [rounds]        then: python3 bench/fiber/summarize.py <out.tsv>
#        SLOTS="ring ring#2" CONFIGS="10000:1 10000:64" HOPS=2000000 narrow a run.
#        CANDIDATES="<name>=<compiler> …" adds an arm `ring-<name>` per compiler: the same program built by a worktree
#        holding one candidate change, measured in the same rounds as the tree's own.
#
# Run it natively on macOS, and on Linux in the dev container (`tools/cdev exec sh bench/fiber/run.sh <out>`), where
# the M4's server benchmark runs. Each round runs every arm on every config as a fresh process, in an order rotated by
# the round, so drift lands on every arm alike. `ring` runs twice, the second time as `ring#2`: its gap to itself is the
# noise floor a candidate has to clear. The output goes where the caller says, never into the worktree.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-15}
ROOT=$(CDPATH= cd -- "$here/../.." && pwd); . "$ROOT/tools/kama-bin.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
"$KAMA" build "$here/coldswitch.kama" -o "$tmp/coldswitch" --release >/dev/null
slots=${SLOTS:-"ring touch ring#2"}
for c in ${CANDIDATES:-}; do
    "${c#*=}" build "$here/coldswitch.kama" -o "$tmp/coldswitch-${c%%=*}" --release >/dev/null
    [ -n "${SLOTS:-}" ] || slots="$slots ring-${c%%=*}"
done
configs=${CONFIGS:-"100:1 1000:1 10000:1 100000:1 100:64 1000:64 10000:64 100000:64"}
hops=${HOPS:-2000000}
printf 'round\tslot\tfibers\ttokens\thops\tns\n' > "$out"
n=$(echo $slots | wc -w | tr -d ' ')
r=1
while [ "$r" -le "$rounds" ]; do
    for cfg in $configs; do
        set -- $slots
        k=$(( (r - 1) % n )); while [ "$k" -gt 0 ]; do x=$1; shift; set -- "$@" "$x"; k=$((k - 1)); done
        for slot in "$@"; do
            arm=${slot%#*}; bin=$tmp/coldswitch
            case $arm in ring-*) bin=$tmp/coldswitch-${arm#ring-}; arm=ring ;; esac
            row=$(BENCH_ARM=$arm BENCH_FIBERS=${cfg%%:*} BENCH_TOKENS=${cfg#*:} BENCH_HOPS=$hops "$bin")
            printf '%s\t%s\t%s\n' "$r" "$slot" "$row" >> "$out"
        done
    done
    echo "round $r/$rounds done" >&2
    r=$((r + 1))
done
