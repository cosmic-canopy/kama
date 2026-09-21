# kama benchmarks

Reproducible, containerized cross-language benchmarks. Compares kama's execution speed and memory
footprint against other languages, and kama→wasm against hand-written JS/TS. Used as an ongoing guide
for kama optimization. Latest results: [`../docs/benchmarks/RESULTS.md`](../docs/benchmarks/RESULTS.md).

## Run

```sh
bench/run build-image   # one-time: build the kama-bench toolchain image (heavy, ~3-4 GB)
bench/run all           # build + run + report in one container (does NOT build the image)
# or step by step:
bench/run build         # compile kama + all languages into bench/build/
bench/run run [workload]# measure time/RSS/size (default: all nine workloads)
bench/run report        # render docs/benchmarks/RESULTS.md + results.json
bench/run sh            # interactive shell in the bench image
```

For a timing you intend to quote, `build` first and then `run` separately, taking the min of several:
a measurement taken straight after `all` rebuilt everything reads high (see the header of
`src/kama/map.kama`).

To re-render RESULTS.md after editing only its prose in `scripts/report.py`, on the host and without
measuring anything: `python3 bench/scripts/report.py --from-json` (reads the committed
`docs/benchmarks/results.json`).

Engine auto-detected (podman preferred); override with `KAMA_ENGINE=docker`.

## What's compared

- **Native:** kama vs C, C++, Rust, Go, C# (JIT), Java (JIT), Lua, Python. C# Native AOT is also
  built, best-effort, into `bench/build/csharp-aot`, but not measured — the tables show the JIT build.
- **WASM (under node):** kama→wasm (`-O3`) vs JavaScript, TypeScript.
- **Workloads** (nine; one source per language under `src/<lang>/`):
  - `fib` — naive recursive Fibonacci summed over 0..31 (call / stack-frame cost).
  - `pi` — Leibniz series, 2×10⁷ float64 terms.
  - `collatz` — Collatz stopping times summed over 1..699 999 (integer ALU + branches).
  - `dispatch` — 8×10⁶ virtual calls over a heap-owned, heterogeneous `DynamicArray<Owned<Shape>>`
    (cannot be devirtualized).
  - `alloc` — 2000× build/fill/sum/drop a `DynamicArray<int32>` of 1000 items (RAII vs GC churn).
  - `fnptr` — 8×10⁶ indirect calls through an `fnptr`, routed through a function boundary.
  - `map` — 100 000 inserts then 10 scrambled lookup passes on each language's *idiomatic* map, pre-sized
    where the language has a capacity API (a library-design number; C has no stdlib map and sits out).
  - `map_kernel` — the same workload on one hand-rolled open-addressing map, identical in every language
    (a codegen number).
  - `math` — 2×10⁶ iterations of `std::math` `Vec4`/`Mat4`/`Quat` ops (tracks SIMD lowering).

  Each emits a checksum as its exit code; the report asserts every language produces the **same**
  checksum (fairness gate). TS compiles only the first six (see `src/ts/tsconfig.json`).
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
