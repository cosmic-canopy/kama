#!/usr/bin/env bash
# Compile every benchmark artifact into bench/build/. Runs inside the bench image.
# Continues past individual failures and reports them (some toolchains may be absent).
set -uo pipefail
cd "$(dirname "$0")/../.."   # repo root (/work)

mkdir -p bench/build/{cstar,c,cpp,rust,go,wasm,csharp}
WORKLOADS="fib pi collatz dispatch alloc"

echo "== building cstar compiler (clean, to match this image's toolchain) =="
make clean >/dev/null 2>&1
make cstar >/dev/null 2>&1 && echo "  ok cstar compiler" || echo "  FAIL cstar compiler"

for w in $WORKLOADS; do
  echo "== $w =="
  ./cstar build bench/src/cstar/$w.cstar -o bench/build/cstar/$w --release          >/dev/null 2>&1 && echo "  ok cstar"       || echo "  FAIL cstar"
  # cstar→wasm at -O3 (speed) for a fair compute comparison vs JS — note `cstar
  # build --release --target wasm` uses -Oz (size); here we transpile + emcc -O3.
  ( ./cstar transpile bench/src/cstar/$w.cstar -o bench/build/wasm/$w.c --no-line >/dev/null 2>&1 \
    && emcc -std=c11 -O3 -DNDEBUG -I. bench/build/wasm/$w.c -o bench/build/wasm/$w.js >/dev/null 2>&1 ) \
    && echo "  ok cstar-wasm" || echo "  FAIL cstar-wasm"
  clang   -O2 -DNDEBUG -s bench/src/c/$w.c   -o bench/build/c/$w     2>/dev/null && echo "  ok c"   || echo "  FAIL c"
  clang++ -O2 -DNDEBUG -s bench/src/cpp/$w.cpp -o bench/build/cpp/$w 2>/dev/null && echo "  ok cpp" || echo "  FAIL cpp"
  rustc -C opt-level=3 -C strip=symbols bench/src/rust/$w.rs -o bench/build/rust/$w 2>/dev/null && echo "  ok rust" || echo "  FAIL rust"
  ( cd bench/src/go && go build -o /work/bench/build/go/$w $w.go ) 2>/dev/null && echo "  ok go" || echo "  FAIL go"
done

echo "== csharp (Release, JIT) =="
dotnet publish bench/src/csharp -c Release -o bench/build/csharp >/dev/null 2>&1 && echo "  ok csharp" || echo "  FAIL csharp"

echo "== csharp (Native AOT, best-effort) =="
dotnet publish bench/src/csharp -c Release -p:PublishAot=true -o bench/build/csharp-aot >/dev/null 2>&1 && echo "  ok csharp-aot" || echo "  skip csharp-aot"

echo "== typescript -> js =="
( cd bench/src/ts && tsc -p tsconfig.json ) 2>/dev/null && echo "  ok ts" || echo "  FAIL ts"

echo "build done"
