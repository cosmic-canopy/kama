#!/usr/bin/env bash
# Measure execution time (hyperfine), peak RSS (/usr/bin/time -v), artifact size,
# and exit-code checksum for every (track, lang, workload). Writes a TSV consumed
# by report.py. Runs inside the bench image.
set -uo pipefail
cd "$(dirname "$0")/../.."

OUT=bench/build/results.tsv
printf "track\tlang\tworkload\ttime_ms\trss_kb\tsize_bytes\texit\n" > "$OUT"

WORKLOADS="${1:-all}"
[ "$WORKLOADS" = "all" ] && WORKLOADS="fib pi collatz dispatch alloc fnptr"

NATIVE="cstar c cpp rust go csharp lua python"
WASM="cstar-wasm js ts"

# Notes:
# - Run the bench with NOTHING else competing for CPU/IO; parallel/thermal load skews short
#   workloads. (The /work bind mount adds only ~0.3-1.7 ms — negligible; measured in place.)
# - cstar-wasm runs under `node --no-liftoff`: by default V8 compiles short-lived wasm with the
#   BASELINE tier (Liftoff — fast compile, slow code) and may never reach the optimizing tier
#   (TurboFan) before the process exits, especially under load — which made wasm look 4x slower
#   and wildly variable (sigma up to 9.8 ms). `--no-liftoff` forces TurboFan, so we measure
#   OPTIMIZED wasm (stable, ~0.3 ms sigma) — what a real long-running app gets (its hot loops
#   tier up on their own), and a fair compare vs V8's auto-JIT'd JS. The flag is a no-op for JS.
cmd_for() {  # lang workload -> run command (empty if artifact missing)
  local l=$1 w=$2
  case $l in
    cstar)      [ -x bench/build/cstar/$w ]   && echo "bench/build/cstar/$w" ;;
    c)          [ -x bench/build/c/$w ]       && echo "bench/build/c/$w" ;;
    cpp)        [ -x bench/build/cpp/$w ]      && echo "bench/build/cpp/$w" ;;
    rust)       [ -x bench/build/rust/$w ]     && echo "bench/build/rust/$w" ;;
    go)         [ -x bench/build/go/$w ]       && echo "bench/build/go/$w" ;;
    csharp)     [ -f bench/build/csharp/bench.dll ] && echo "dotnet bench/build/csharp/bench.dll $w" ;;
    lua)        echo "lua5.4 bench/src/lua/$w.lua" ;;
    python)     echo "python3 bench/src/python/$w.py" ;;
    cstar-wasm) [ -f bench/build/wasm/$w.js ] && echo "node --no-liftoff bench/build/wasm/$w.js" ;;
    js)         echo "node bench/src/js/$w.js" ;;
    ts)         [ -f bench/build/ts/$w.js ]   && echo "node bench/build/ts/$w.js" ;;
  esac
}

size_for() {  # lang workload -> artifact size (bytes) or 0
  local l=$1 w=$2 f=""
  case $l in
    cstar) f=bench/build/cstar/$w ;;  c) f=bench/build/c/$w ;;  cpp) f=bench/build/cpp/$w ;;
    rust) f=bench/build/rust/$w ;;    go) f=bench/build/go/$w ;;
    csharp) f=bench/build/csharp/bench.dll ;;  cstar-wasm) f=bench/build/wasm/$w.wasm ;;
  esac
  [ -n "$f" ] && [ -f "$f" ] && stat -c %s "$f" 2>/dev/null || echo 0
}

measure_one() {  # track lang workload
  local track=$1 l=$2 w=$3
  local cmd; cmd=$(cmd_for "$l" "$w")
  [ -z "$cmd" ] && { echo "  skip $track/$l/$w (missing)"; return; }

  local ec; eval "$cmd" >/dev/null 2>&1; ec=$?

  local t="NA"
  # --ignore-failure: our checksum IS the exit code (non-zero), not a failure.
  if hyperfine --warmup 2 --runs 8 --shell=none --ignore-failure --export-json bench/build/hf.json "$cmd" >/dev/null 2>&1; then
    t=$(python3 -c "import json;print(round(json.load(open('bench/build/hf.json'))['results'][0]['median']*1000,2))" 2>/dev/null || echo NA)
  fi

  local rss; rss=$(/usr/bin/time -v bash -c "$cmd" 2>&1 >/dev/null | awk '/Maximum resident/{print $6}')
  [ -z "$rss" ] && rss=NA

  local sz; sz=$(size_for "$l" "$w")

  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$track" "$l" "$w" "$t" "$rss" "$sz" "$ec" >> "$OUT"
  echo "  $track/$l/$w  time=${t}ms  rss=${rss}kb  size=${sz}B  exit=$ec"
}

for w in $WORKLOADS; do
  echo "== measuring $w =="
  for l in $NATIVE; do measure_one native "$l" "$w"; done
  for l in $WASM;   do measure_one wasm   "$l" "$w"; done
done
echo "measure done -> $OUT"
