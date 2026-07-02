#!/usr/bin/env bash
# Compile every benchmark artifact into bench/build/. Runs inside the bench image.
# Continues past individual failures and reports them (some toolchains may be absent).
set -uo pipefail
cd "$(dirname "$0")/../.."   # repo root (/work)

mkdir -p bench/build/{cstar,c,cpp,rust,go,wasm,csharp,java}
WORKLOADS="fib pi collatz dispatch alloc fnptr"

# Compile-time metric: accumulate per-language wall-clock (ms) + artifact count while building,
# emitted to bench/build/compile.tsv for report.py. Single-build snapshot (not hyperfine-averaged) —
# that's what a "compile time" number is. now_ms uses GNU date (%3N = ms); the bench image is Ubuntu.
now_ms() { date +%s%3N; }
declare -A CT CN
add_ct() { CT[$1]=$(( ${CT[$1]:-0} + $(now_ms) - $2 )); CN[$1]=$(( ${CN[$1]:-0} + 1 )); }

echo "== building cstar compiler (clean, to match this image's toolchain) =="
make clean >/dev/null 2>&1
make cstar >/dev/null 2>&1 && echo "  ok cstar compiler" || echo "  FAIL cstar compiler"
# NB: building the cstar compiler itself is toolchain setup, NOT counted as user compile time.

for w in $WORKLOADS; do
  echo "== $w =="
  t=$(now_ms); ./cstar build bench/src/cstar/$w.cstar -o bench/build/cstar/$w --release >/dev/null 2>&1; rc=$?
  add_ct cstar $t; [ $rc -eq 0 ] && echo "  ok cstar" || echo "  FAIL cstar"
  # cstar→wasm at -O3 (speed) for a fair compute comparison vs JS — note `cstar
  # build --release --target wasm` uses -Oz (size); here we transpile + emcc -O3.
  # Strict IEEE FP (NO -ffast-math): the JS/C/Rust baselines are all strict, so the wasm
  # build must be too for an apples-to-apples compare. The `pi` float loop legitimately
  # trails V8 here — see docs/ROADMAP.md (V8 tiers short-lived wasm via Liftoff, not the
  # optimizing TurboFan a long-running app would get).
  t=$(now_ms)
  ( ./cstar transpile bench/src/cstar/$w.cstar -o bench/build/wasm/$w.c --no-line >/dev/null 2>&1 \
    && emcc -std=c11 -O3 -DNDEBUG -I. bench/build/wasm/$w.c -o bench/build/wasm/$w.js >/dev/null 2>&1 ); rc=$?
  add_ct cstar-wasm $t; [ $rc -eq 0 ] && echo "  ok cstar-wasm" || echo "  FAIL cstar-wasm"
  t=$(now_ms); clang   -O2 -DNDEBUG -s bench/src/c/$w.c   -o bench/build/c/$w     2>/dev/null; rc=$?
  add_ct c $t;   [ $rc -eq 0 ] && echo "  ok c"   || echo "  FAIL c"
  t=$(now_ms); clang++ -O2 -DNDEBUG -s bench/src/cpp/$w.cpp -o bench/build/cpp/$w 2>/dev/null; rc=$?
  add_ct cpp $t; [ $rc -eq 0 ] && echo "  ok cpp" || echo "  FAIL cpp"
  t=$(now_ms); rustc -C opt-level=3 -C strip=symbols bench/src/rust/$w.rs -o bench/build/rust/$w 2>/dev/null; rc=$?
  add_ct rust $t; [ $rc -eq 0 ] && echo "  ok rust" || echo "  FAIL rust"
  t=$(now_ms); ( cd bench/src/go && go build -o /work/bench/build/go/$w $w.go ) 2>/dev/null; rc=$?
  add_ct go $t; [ $rc -eq 0 ] && echo "  ok go" || echo "  FAIL go"
done

echo "== csharp (Release, JIT) =="
t=$(now_ms); dotnet publish bench/src/csharp -c Release -o bench/build/csharp >/dev/null 2>&1; rc=$?
add_ct csharp $t; [ $rc -eq 0 ] && echo "  ok csharp" || echo "  FAIL csharp"

echo "== csharp (Native AOT, best-effort) =="
dotnet publish bench/src/csharp -c Release -p:PublishAot=true -o bench/build/csharp-aot >/dev/null 2>&1 && echo "  ok csharp-aot" || echo "  skip csharp-aot"
# (AOT build time intentionally not tracked — best-effort/optional; the JIT publish is the C# build.)

echo "== java (HotSpot JIT) =="
t=$(now_ms); javac -d bench/build/java bench/src/java/Bench.java 2>/dev/null; rc=$?
add_ct java $t; [ $rc -eq 0 ] && echo "  ok java" || echo "  FAIL java"

echo "== typescript -> js =="
t=$(now_ms); ( cd bench/src/ts && tsc -p tsconfig.json ) 2>/dev/null; rc=$?
add_ct ts $t; [ $rc -eq 0 ] && echo "  ok ts" || echo "  FAIL ts"

# Per-language compile time (interpreted lua/python/js have NO compile step -> omitted). `artifacts`
# is how many binaries that build produced: native compiled langs build one per workload; C#/Java
# build ONE multi-workload binary (dispatch on args[0]) — report.py notes this so the numbers are
# read fairly (total wall-clock to make that language's whole bench runnable).
CT_TSV=bench/build/compile.tsv
printf "lang\tcompile_ms\tartifacts\n" > "$CT_TSV"
for k in cstar c cpp rust go csharp java cstar-wasm ts; do
  printf "%s\t%s\t%s\n" "$k" "${CT[$k]:-NA}" "${CN[$k]:-0}" >> "$CT_TSV"
done

echo "build done"
