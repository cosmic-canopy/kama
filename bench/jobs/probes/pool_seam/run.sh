#!/bin/sh
# run.sh — the pool seam's probe (KR-3 step 2, before commit 3), every leg this machine can reach. From the repo root:
#   sh bench/jobs/probes/pool_seam/run.sh host        the host's own build (macOS or Linux)
#   tools/cdev exec sh bench/jobs/probes/pool_seam/run.sh linux    native, ASan with LeakSanitizer, and TSan
#   tools/cdev exec sh bench/jobs/probes/pool_seam/run.sh wasm     emscripten under node
# Everything is built under a mktemp -d. Results recorded in docs/ROADMAP_DETAIL.md, `s6-pools`.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$here/../../../.." && pwd)
. "$ROOT/tools/kama-bin.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
build() {   # <out> [kama build flags]
    out=$1; shift
    "$KAMA" build "$here/kama.json" "$@" -o "$tmp/$out" >"$tmp/$out.log" 2>&1 || { cat "$tmp/$out.log"; exit 1; }
}
ms() { t0=$(date +%s%N); "$@" >"$tmp/r" 2>&1; rc=$?; t1=$(date +%s%N); echo "rc=$rc ms=$(( (t1 - t0) / 1000000 ))"; }
ends() {    # the two exits, with every worker asleep
    for mode in 0 1; do echo "$1 mode=$mode workers=8: $(PROBE=$mode WORKERS=8 ms "$tmp/$1") $(head -1 "$tmp/r")"; done
}
case "${1:-host}" in
  host|linux)
    build native
    ends native
    for k in 0 1; do PROBE=2 MASKED=$k "$tmp/native"; done
    for k in 1 2; do PROBE=3 MASKED=$k "$tmp/native" >"$tmp/r" 2>&1; echo "overflow masked=$k rc=$?: $(grep -m1 -E 'overflow|still' "$tmp/r")"; done
    PROBE=4 "$tmp/native"
    for k in 0 1; do PROBE=6 MASKED=$k "$tmp/native"; done
    if [ "${1:-host}" = linux ]; then
        build san --cc "clang -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer -g -DKAMA_ALLOC_CHECK"
        build tsan --cc "clang -fsanitize=thread -fno-omit-frame-pointer -g"
        ends san; ends tsan
        echo "san, a lost block with the workers asleep: $(PROBE=5 ms "$tmp/san") $(grep -m1 -o 'LeakSanitizer: detected memory leaks' "$tmp/r")"
    fi ;;
  wasm)
    # `-pthread` reaches a csources file since 0.9.888; before, this build failed to link.
    for mode in 0 1; do for w in 1 8; do
        build "p$mode$w.js" --target wasm --cc "${EMCC:-emcc} -DFORCE_MODE=$mode -DFORCE_N=$w"
        echo "wasm mode=$mode workers=$w: $(ms node --no-concurrent-recompilation "$tmp/p$mode$w.js") $(head -1 "$tmp/r")"
    done; done ;;
  *) echo "usage: run.sh host|linux|wasm" >&2; exit 2 ;;
esac
