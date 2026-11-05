#!/bin/sh
# run.sh — what a thread started before `main` costs a process (KR-3's pool brief, decision 4), and what `cpuCount()` answers under a limit.
# Run on the host, from the repo root: sh bench/jobs/probes/before_main/run.sh. Linux runs in the bench image, wasm in emsdk's.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT; cp "$here"/* "$tmp"
ENGINE=${ENGINE:-podman}; IMG=localhost/kama-bench:latest
$ENGINE run --rm -v "$tmp":/w -w /w $IMG bash -c 'go build -o thr thr.go; gcc -O1 -pthread uns.c -o uns; gcc -O1 -pthread forked.c -o forked; gcc -O1 cpus.c -o cpus; rustc -O avail.rs -o avail
    ./thr; ./uns; ./uns x; ./forked; ./cpus; ./avail; echo "-- taskset -c 0,1"; taskset -c 0,1 ./cpus; taskset -c 0,1 ./avail; taskset -c 0,1 ./thr'
echo "-- --cpus=1"; $ENGINE run --rm --cpus=1 -v "$tmp":/w -w /w $IMG bash -c './cpus; ./avail; ./thr'
echo "-- wasm under node"; $ENGINE run --rm -v "$tmp":/w -w /w docker.io/emscripten/emsdk:latest bash -c 'emcc -O2 -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=256MB wasmfirst.c -o wf.js && node wf.js'
