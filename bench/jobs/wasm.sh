#!/bin/bash
# wasm.sh — `parallel_for` and pool.c under node, with `main` on a pthread as a browser build runs it (KR-3's pool brief). Run it where
# emcc and node are: `tools/cdev exec bash bench/jobs/wasm.sh <out.tsv> [rounds]`. kama's wasm does not read node's environment, so its
# settings are baked in per build; pool.c gets them through wasm-env.js. Both builds start their page with 8 Workers ready
# (PTHREAD_POOL_SIZE, KAMA_PTHREAD_POOL for kama), so neither creates one inside a timed call. Prints nothing; one line per
# run into <out.tsv>.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$here/../.." && pwd); . "$ROOT/tools/kama-bin.sh"
out=${1:?usage: wasm.sh <out.tsv> [rounds]}
rounds=${2:-5}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
emcc -O3 -DNDEBUG -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=512MB -sSTACK_SIZE=256KB -sDEFAULT_PTHREAD_STACK_SIZE=256KB \
     -sENVIRONMENT=node,worker -sPTHREAD_POOL_SIZE=8 --pre-js "$here/wasm-env.js" -sEXPORTED_RUNTIME_METHODS=ENV "$here/pool.c" -o "$W/pool.js" 2>/dev/null
ncpu=$(nproc)   # kama's pool is `cpuCount()` threads, the caller one of them; pool.c's is told the same
configs="tight:1 tight:2 tight:4 frame:1 frame:2 frame:4"
for cfg in $configs; do
    shape=${cfg%:*}; k=${cfg#*:}; r=70; c=300; [ "$shape" = tight ] && r=0 && c=1000
    sed -e "s/\"BENCH_SHAPE\", dflt: \"tight\"/\"BENCH_SHAPE\", dflt: \"$shape\"/" -e "s/\"BENCH_WORKERS\", dflt: \"4\"/\"BENCH_WORKERS\", dflt: \"$k\"/" \
        -e "s/\"BENCH_CALLS\", dflt: \"2000\"/\"BENCH_CALLS\", dflt: \"$c\"/" -e "s/\"BENCH_ROUNDS\", dflt: \"0\"/\"BENCH_ROUNDS\", dflt: \"$r\"/" "$here/jobs.kama" > "$W/j_${shape}_$k.kama"
    KAMA_PTHREAD_POOL=8 "$KAMA" build "$W/j_${shape}_$k.kama" -o "$W/j_${shape}_$k.js" --target wasm --cc emcc --release >/dev/null 2>&1
done
: > "$out"
n=1
while [ "$n" -le "$rounds" ]; do
    for cfg in $configs; do
        shape=${cfg%:*}; k=${cfg#*:}; r=70; c=300; [ "$shape" = tight ] && r=0 && c=1000
        a=$(node "$W/j_${shape}_$k.js" 2>/dev/null | tail -1)
        b=$(BENCH_SHAPE=$shape BENCH_WORKERS=$k BENCH_POOL=$ncpu BENCH_CALLS=$c BENCH_ROUNDS=$r node "$W/pool.js" 2>/dev/null | tail -1)
        printf 'kama\t%s\t%s\npool\t%s\t%s\n' "$n" "$a" "$n" "$b" >> "$out"
    done
    n=$((n + 1))
done
