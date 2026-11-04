#!/bin/sh
# run.sh — build pipe.kama and its tokio and Go twins, and run the arms INTERLEAVED, one line per run into <out.tsv> (KR-264).
#
# Usage: bench/pipe/run.sh <out.tsv> [rounds]        then: python3 bench/pipe/summarize.py <out.tsv>
#        CONFIGS="pingpong:1000000:64:4096 …" narrows a run: workload:count:bytes:capacity.
#        SLOTS="kama tokio" narrows the arms. CANDIDATES="<name>=<compiler> …" adds an arm `kama-<name>` per compiler: the
#        same program built by a worktree holding one candidate change, measured in the same rounds as the tree's own.
#
# Run it natively on macOS for kama and tokio, and in the bench image for all three (`bench/run sh`), since Go is not
# installed on the Mac. Each round runs every arm on every config as a fresh process, in an order rotated by the round,
# so drift lands on every arm alike. `kama` runs twice, the second time as `kama#2`: its gap to itself is the noise floor
# a difference has to clear. An arm whose toolchain is missing is left out with a line saying so. tokio runs in the three
# shapes rust/src/main.rs names, since on a runtime with an I/O driver they differ by a factor of tens; `tokio` alone is the
# shape a server's tasks have, and the one to compare with. `kama-root` makes one side of a ping-pong the root.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-15}
ROOT=$(CDPATH= cd -- "$here/../.." && pwd); . "$ROOT/tools/kama-bin.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

"$KAMA" build "$here/pipe.kama" -o "$tmp/kama" --release >/dev/null
avail="kama kama-root"
for c in ${CANDIDATES:-}; do
    "${c#*=}" build "$here/pipe.kama" -o "$tmp/kama-${c%%=*}" --release >/dev/null
    avail="$avail kama-${c%%=*}"
done
if command -v cargo >/dev/null; then
    (cd "$here/rust" && CARGO_TARGET_DIR="$tmp/cargo" cargo build --release --offline -q 2>/dev/null \
        || CARGO_TARGET_DIR="$tmp/cargo" cargo build --release -q)
    cp "$tmp/cargo/release/pipe" "$tmp/tokio"
    avail="$avail tokio tokio-nodriver tokio-test"
else
    echo "run.sh: no tokio arm (no cargo here)" >&2
fi
if command -v go >/dev/null; then
    (cd "$here/go" && go build -o "$tmp/go" .)
    avail="$avail go"
else
    echo "run.sh: no Go arm (no go here)" >&2
fi

default_slots="kama kama#2 $(for a in $avail; do [ "$a" = kama ] || printf '%s ' "$a"; done)"
slots=""
for s in ${SLOTS:-$default_slots}; do
    case " $avail " in
        *" ${s%#*} "*) slots="$slots $s" ;;
        *) echo "run.sh: no $s here" >&2 ;;
    esac
done
configs=${CONFIGS:-"ops:2000000:64:8192 pingpong:1000000:64:4096 pingpong:1000000:4096:8192 bulk:200000:8192:8192 bulk:30000:65536:8192"}

printf 'round\tslot\tworkload\tcount\tbytes\tcapacity\tns\n' > "$out"
n=$(echo $slots | wc -w | tr -d ' ')
r=1
while [ "$r" -le "$rounds" ]; do
    for cfg in $configs; do
        workload=${cfg%%:*}; rest=${cfg#*:}
        count=${rest%%:*}; rest=${rest#*:}
        bytes=${rest%%:*}; capacity=${rest#*:}
        set -- $slots
        k=$(( (r - 1) % n )); while [ "$k" -gt 0 ]; do x=$1; shift; set -- "$@" "$x"; k=$((k - 1)); done
        for slot in "$@"; do
            arm=${slot%#*}; root=0; shape=server; bin=$tmp/$arm
            case $arm in
                kama-root) root=1; bin=$tmp/kama ;;
                tokio-*) shape=${arm#tokio-}; bin=$tmp/tokio ;;
            esac
            if [ "$arm" = go ] && [ "$workload" = ops ]; then continue; fi    # `net.Pipe` has no buffer: main.go says
            if [ "$arm" = kama-root ] && [ "$workload" != pingpong ]; then continue; fi
            row=$(BENCH_WORKLOAD=$workload BENCH_COUNT=$count BENCH_BYTES=$bytes BENCH_CAPACITY=$capacity BENCH_ROOT=$root BENCH_TOKIO=$shape "$bin")
            printf '%s\t%s\t%s\n' "$r" "$slot" "$row" >> "$out"
        done
    done
    echo "round $r/$rounds done" >&2
    r=$((r + 1))
done
