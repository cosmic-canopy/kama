# cstar benchmarks

Reproducible, containerized cross-language benchmarks. Compares cstar's execution speed and memory
footprint against other languages, and cstar→wasm against hand-written JS/TS. Used as an ongoing guide
for cstar optimization. Latest results: [`../docs/benchmarks/RESULTS.md`](../docs/benchmarks/RESULTS.md).

## Run

```sh
bench/run build-image   # one-time: build the cstar-bench toolchain image (heavy, ~3-4 GB)
bench/run all           # build every language, measure, regenerate RESULTS.md
# or step by step:
bench/run build         # compile cstar + all languages into bench/build/
bench/run run [workload]# measure time/RSS/size (default: all)
bench/run report        # render docs/benchmarks/RESULTS.md
```

Engine auto-detected (podman preferred); override with `CSTAR_ENGINE=docker`.

## What's compared

- **Native:** cstar vs C, C++, Rust, Go, C# (JIT), Lua, Python.
- **WASM (under node):** cstar→wasm (`-O3`) vs JavaScript, TypeScript.
- **Workloads:** `fib` (recursion), `pi` (float64), `collatz` (integer), `dispatch` (virtual calls) —
  compute-bound, since cstar has no arrays/heap yet. Each emits a checksum as its exit code; the runner
  asserts every language produces the **same** checksum (fairness gate).
- **Metrics:** time (`hyperfine`), peak RSS (`/usr/bin/time -v`), artifact size.

## Layout

```
bench/
  Dockerfile          multi-language toolchain image (emsdk + dotnet/rust/go/lua/ts/hyperfine)
  run                 podman/docker wrapper
  scripts/            build.sh, measure.sh, report.py
  src/<lang>/         identical workloads per language
  build/              compiled artifacts (gitignored)
```

Honesty note: cstar→C is built by the same clang as the C baseline, so native compute matches C/C++ by
design. The meaningful signals are cstar vs managed/interpreted languages, footprint/size, and wasm vs
JS/TS. See the report's "How to read this" section.
