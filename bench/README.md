# kama benchmarks

Reproducible, containerized cross-language benchmarks. Compares kama's execution speed and memory
footprint against other languages, and kama→wasm against hand-written JS/TS. Used as an ongoing guide
for kama optimization. Latest results: [`../docs/benchmarks/RESULTS.md`](../docs/benchmarks/RESULTS.md).

## Run

```sh
bench/run build-image   # one-time: build the kama-bench toolchain image (heavy, ~3-4 GB)
bench/run all           # build every language, measure, regenerate RESULTS.md
# or step by step:
bench/run build         # compile kama + all languages into bench/build/
bench/run run [workload]# measure time/RSS/size (default: all)
bench/run report        # render docs/benchmarks/RESULTS.md
```

Engine auto-detected (podman preferred); override with `KAMA_ENGINE=docker`.

## What's compared

- **Native:** kama vs C, C++, Rust, Go, C# (JIT), Java (JIT), Lua, Python.
- **WASM (under node):** kama→wasm (`-O3`) vs JavaScript, TypeScript.
- **Workloads:** `fib` (recursion), `pi` (float64), `collatz` (integer), `dispatch` (virtual calls) —
  compute-bound, since kama has no arrays/heap yet. Each emits a checksum as its exit code; the runner
  asserts every language produces the **same** checksum (fairness gate).
- **Metrics:** time (`hyperfine`), peak RSS (`/usr/bin/time -v`), compile time (build wall-clock),
  package size (self-contained binary, or code + external runtime).

## Layout

```
bench/
  Dockerfile          multi-language toolchain image (emsdk + dotnet/rust/go/jdk/lua/ts/hyperfine)
  run                 podman/docker wrapper
  scripts/            build.sh, measure.sh, report.py
  src/<lang>/         identical workloads per language
  build/              compiled artifacts (gitignored)
```

Honesty note: kama→C is built by the same clang as the C baseline, so native compute matches C/C++ by
design. The meaningful signals are kama vs managed/interpreted languages, footprint/size, and wasm vs
JS/TS. See the report's "How to read this" section.
