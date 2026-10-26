#!/usr/bin/env bash
# run.sh — KR-243's server benchmark: build every arm, then run each arm on each config as fresh processes, rotated
# per round, one line per run into <out.tsv>.            then: python3 bench/server/summarize.py <out.tsv>
#
# Usage: bench/server/run.sh <out.tsv> [rounds]
#   SLOTS    the arms, in order; the reference runs twice, the second as `<arm>#2`, its gap to itself the noise floor
#   CONFIGS  `mode:conns` pairs (echo, http, idle)
#   CORES    cores the server gets (default: half of what the machine has); the client gets the rest
#   WARMUP, SECONDS_  the client's warm-up and measured window per run
#
# Run it in the bench image (`bench/run server`) for the Linux numbers; on a macOS host, an arm whose toolchain is
# missing is left out and said so. The output goes where the caller says, never into the worktree.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=${1:?usage: run.sh <out.tsv> [rounds]}
rounds=${2:-15}
mkdir -p "$(dirname "$out")"
tmp=$(mktemp -d); trap 'kill_server; rm -rf "$tmp"' EXIT

ncpu=$( (getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu) 2>/dev/null )
cores=${CORES:-$(( ncpu > 1 ? ncpu / 2 : 1 ))}
warm=${WARMUP:-1}
secs=${SECONDS_:-5}
configs=${CONFIGS:-"echo:100 echo:1000 echo:10000 http:100 http:1000 http:10000 idle:10000"}
linux=false; [ "$(uname)" = Linux ] && linux=true
ulimit -n "$(ulimit -Hn)" 2>/dev/null || ulimit -n 1048576 2>/dev/null || true

# ---- build ---------------------------------------------------------------------------------------------------------
cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -o "$tmp/client" "$here/client.c"
cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -o "$tmp/c-server" "$here/c/server.c"
avail="c"
ROOT=$root; . "$root/tools/kama-bin.sh"   # the compiler built for THIS platform (bench/run builds it in the image first)
if [ -x "$KAMA" ]; then
    "$KAMA" build "$here/kama/server.kama" -o "$tmp/kama-server" --release >/dev/null && avail="$avail kama-isolates"
    "$KAMA" build "$here/kama/fibers.kama" -o "$tmp/kama-fibers" --release >/dev/null && avail="$avail kama-fibers"
fi
if command -v node >/dev/null; then avail="$avail node node-cluster"; fi
if command -v go >/dev/null; then (cd "$here/go" && go build -o "$tmp/go-server" .) && avail="$avail go"; fi
if command -v cargo >/dev/null; then
    (cd "$here/rust" && CARGO_TARGET_DIR="$tmp/cargo" cargo build --release --offline -q 2>/dev/null \
        || CARGO_TARGET_DIR="$tmp/cargo" cargo build --release -q) && cp "$tmp/cargo/release/server" "$tmp/tokio-server" \
        && avail="$avail tokio tokio-percore"
fi
slots=${SLOTS:-"c node node-cluster go tokio tokio-percore kama-isolates kama-fibers c#2"}
run_slots=""
for s in $slots; do case " $avail " in *" ${s%#*} "*) run_slots="$run_slots $s" ;; *) echo "run.sh: no $s here (its toolchain is missing)" >&2 ;; esac; done

# ---- one run -------------------------------------------------------------------------------------------------------
spid=""
kill_server() { [ -n "$spid" ] || return 0; pkill -TERM -P "$spid" 2>/dev/null || true; kill -TERM "$spid" 2>/dev/null || true;
                wait "$spid" 2>/dev/null || true; spid=""; }
start_server() {   # <arm> <mode> <port>
    local arm=$1 mode=$2 port=$3 pin=""
    $linux && pin="taskset -c 0-$((cores - 1))"
    case $arm in
        c)             $pin "$tmp/c-server" "$mode" "$port" "$cores" & ;;
        node)          $pin node "$here/node/server.js" "$mode" "$port" 1 & ;;
        node-cluster)  $pin node "$here/node/server.js" "$mode" "$port" "$cores" & ;;
        go)            GOMAXPROCS=$cores $pin "$tmp/go-server" "$mode" "$port" & ;;
        tokio)         $pin "$tmp/tokio-server" "$mode" "$port" multi "$cores" & ;;
        tokio-percore) $pin "$tmp/tokio-server" "$mode" "$port" per-core "$cores" & ;;
        kama-isolates) BENCH_MODE=$mode BENCH_PORT=$port $pin "$tmp/kama-server" & ;;
        kama-fibers)   BENCH_MODE=$mode BENCH_PORT=$port BENCH_CORES=$cores $pin "$tmp/kama-fibers" & ;;
    esac
    spid=$!
}
tree_pids() { local p=$1; echo "$p"; for c in $(pgrep -P "$p" 2>/dev/null); do tree_pids "$c"; done; }
rss_kb() {   # the server's process tree: peak resident set on Linux, current on macOS
    local sum=0 k
    for p in $(tree_pids "$spid"); do
        if $linux; then k=$(awk '/VmHWM/{print $2}' "/proc/$p/status" 2>/dev/null || echo 0)
        else k=$(ps -o rss= -p "$p" 2>/dev/null | tr -d ' ' || echo 0); fi
        sum=$((sum + ${k:-0}))
    done
    echo "$sum"
}
cpu_s() {    # the server's process tree: user + system seconds so far
    local sum=0 t
    for p in $(tree_pids "$spid"); do
        if $linux; then t=$(awk '{print $14 + $15}' "/proc/$p/stat" 2>/dev/null || echo 0)
        else t=$(ps -o time= -p "$p" 2>/dev/null | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*100+$3; else print 0 }'); fi
        sum=$((sum + ${t:-0}))
    done
    awk -v t="$sum" 'BEGIN { printf "%.2f", t / 100 }'   # Linux USER_HZ and the macOS centiseconds are both 1/100 s
}

port=47200
cthreads=$(( ncpu - cores > 0 ? ncpu - cores : 1 ))
cpin=""; $linux && [ "$ncpu" -gt "$cores" ] && cpin="taskset -c $cores-$((ncpu - 1))"
printf 'round\tslot\t%s\trss_kb\tcpu_s\n' "$("$tmp/client" -h)" > "$out"
n=$(echo $run_slots | wc -w | tr -d ' ')
r=1
while [ "$r" -le "$rounds" ]; do
    for cfg in $configs; do
        mode=${cfg%%:*}; conns=${cfg#*:}
        smode=$mode; [ "$mode" = idle ] && smode=echo
        set -- $run_slots
        k=$(( (r - 1) % n )); while [ "$k" -gt 0 ]; do x=$1; shift; set -- "$@" "$x"; k=$((k - 1)); done
        for slot in "$@"; do
            port=$((port + 1)); [ "$port" -gt 48000 ] && port=47201
            start_server "${slot%#*}" "$smode" "$port"
            sleep 0.3
            if [ "$mode" = idle ]; then
                $cpin "$tmp/client" idle "$port" "$conns" "$cthreads" 0 3 > "$tmp/line" 2> "$tmp/err" &
                cl=$!
                for _ in $(seq 1 200); do grep -q READY "$tmp/err" 2>/dev/null && break; sleep 0.1; done
                rss=$(rss_kb); cpu=$(cpu_s)
                wait "$cl" || true
            else
                CLIENT_WHY=1 $cpin "$tmp/client" "$mode" "$port" "$conns" "$cthreads" "$warm" "$secs" > "$tmp/line" 2> "$tmp/err" || true
                rss=$(rss_kb); cpu=$(cpu_s)
            fi
            line=$(cat "$tmp/line"); [ -n "$line" ] || line="$mode	$conns	$cthreads	0	0	0	0	0	0	0	failed"
            printf '%s\t%s\t%s\t%s\t%s\n' "$r" "$slot" "$line" "$rss" "$cpu" >> "$out"
            [ -s "$tmp/err" ] && sed "s/^/  [$slot $cfg] /" "$tmp/err" | grep -v READY | head -5 >&2 || true
            kill_server
        done
    done
    echo "round $r/$rounds done" >&2
    r=$((r + 1))
done
