# kama benchmark results

_Generated: 2026-09-21 13:07 · arch: aarch64 (Linux) · in the `kama-bench` container_

Toolchains: clang `Ubuntu clang version 18.1.3 (1ubuntu1)` · rustc 1.79.0 (129f3b996 2024-06-10) · go version go1.22.5 linux/arm64 · dotnet 8.0.422 · openjdk version "21.0.11" 2026-04-21 · node v22.16.0 · Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio · Python 3.12.3
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median); the `(N×)` after each time is relative to kama for that workload (native → kama, wasm → kama→wasm). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

kama transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads kama is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. All four LLVM-AOT languages (C, C++, Rust, kama) are compiled at **`-O3`** for an apples-to-apples
comparison — otherwise the optimization level, not the language, dominates a tiny kernel (e.g. `fib` at
C-`-O2` vs Rust-`-O3` differs ~20%, but at equal `-O3` C and Rust are identical). The signals worth trusting here are:
1. kama (native) vs **managed/interpreted** languages (C#, Java, Go, Lua, Python),
2. **peak RSS**, **compile time**, and **package size** (the low-footprint / self-contained goal),
3. on the WASM track, **kama→wasm vs hand-written JS/TS** under the same node.

**Methodology — pre-sizing:** in `map`, every language whose standard map has a capacity API pre-sizes
it for the keys it will insert (kama `Map.withCapacity`, C++ `reserve`, Rust `with_capacity`, Go
`make(m, n)`, C# and Java constructor capacity). Lua, Python and JS have no such API for their built-in
table/dict/`Map`, so they grow from small — the one asymmetry left in that row.

**Methodology — run isolated:** these are short workloads, so **parallel load badly skews them** — run the
bench with nothing else competing for CPU/IO. (The `/work` bind mount, virtiofs/9p on macOS/Windows, adds
only ~0.3–1.7 ms for native *and* wasm under a controlled idle measurement — negligible — so artifacts are
measured in place.)

**WASM is run under `node --no-liftoff`.** V8 compiles wasm in two tiers — **Liftoff** (baseline: fast to
compile, slow to run) then **TurboFan** (optimizing). For these tiny single-shot processes V8 often never
tiers up before exit (worse under load), so default `node` measured *Liftoff* wasm — ~4× slower and wildly
variable (σ up to 9.8 ms), while JS always got its optimizing JIT. `--no-liftoff` forces TurboFan, giving
**optimized, stable** wasm (σ ~0.3 ms) — what a real long-running app gets (its hot loops tier up on their
own) and a fair compare vs V8's auto-JIT'd JS. Strict IEEE throughout (no `-ffast-math`).

The compute workloads (fib/pi/collatz) are tuned so the slow interpreters finish quickly; the fast
compiled languages run in a few ms, so small absolute differences between them are noise — **except
`dispatch`**, which measures *true* dynamic dispatch (see Workloads): the AOT cluster
(kama/C/C++/Rust) converges there, while **Go**'s interface dispatch trails ~2×.

**Two of these rows measure different things and are named accordingly.** `map_kernel` pins the
algorithm, hash, and capacity across every language — a pure **codegen** number, where the AOT cluster
should converge. `map` lets each language use its **idiomatic** map — a **library-design** number, where
they legitimately should not. Read the first for "is kama as fast as C?", the second for "how good is the
shipped container?". C appears only in `map_kernel`: it has no stdlib hashmap to enter in `map`.

The **`alloc`** workload is the one to watch for the no-GC story: it
churns ~2M growable-list appends and 2000 collection lifetimes, so it contrasts kama's deterministic
**RAII** free against the **garbage collectors** (Go, C#, Java, Lua, Python, JS) and against the RAII
peers (C++ `vector`, Rust `Vec`). Watch its **peak RSS** in particular — GC runtimes keep dead
allocations resident until a collection runs.

## Fairness gate (checksum equality)

Every language must produce the same exit-code checksum as kama per workload, or the algorithms have
diverged:

- `fib`: checksum = 225 (exit code) — ✓ all match
- `pi`: checksum = 27 (exit code) — ✓ all match
- `collatz`: checksum = 2 (exit code) — ✓ all match
- `dispatch`: checksum = 0 (exit code) — ✓ all match
- `alloc`: checksum = 64 (exit code) — ✓ all match
- `fnptr`: checksum = 0 (exit code) — ✓ all match
- `map`: checksum = 192 (exit code) — ✓ all match
- `map_kernel`: checksum = 192 (exit code) — ✓ all match
- `math`: checksum = 0 (exit code) — ✓ all match

## Workloads
- **fib** — naive recursive Fibonacci summed over 0..31 (function-call / stack-frame cost).
- **pi** — Leibniz series, 2×10⁷ terms, float64 (FP throughput; cleanest cross-language compare).
- **collatz** — sum of Collatz stopping times for 1..699 999 (integer ALU + unpredictable branches).
- **dispatch** — 8×10⁶ virtual calls over a **heterogeneous, heap-owned collection** of mixed
  concrete types built at runtime, so the concrete type is *not* knowable at the call site and the
  call **cannot be devirtualized** — a true dynamic-dispatch measurement. Each language uses its
  idiomatic owning collection (kama `DynamicArray<Owned<Shape>>`, C++ `vector<unique_ptr>`, Rust
  `Vec<Box<dyn>>`, C array of heap `Shape*`, Go `[]interface`, C#/Java `Shape[]`).
- **alloc** — 2000× (build a growable list, append 1..1000, sum, drop) ≈ 2M appends + 2000 lifetimes
  (allocator / GC pressure vs RAII; each language uses its idiomatic growable list — kama `DynamicArray<int32>`,
  C++ `vector`, Rust `Vec`, Go slice, C# `List`, Java `ArrayList`, Lua table, Python/JS array, C manual realloc).
- **fnptr** — 8×10⁶ indirect calls through a function pointer, routed through a function boundary
  (`apply(op, x)`) so the call stays genuinely indirect (the fnptr analog of `dispatch`'s virtual calls).
  Each language uses its idiomatic callable — kama `fnptr` (a bare C function pointer, zero-cost), C/C++
  function pointers, Rust `fn` pointers, Go func values, **C# `Func<>` delegates**, **Java
  `LongUnaryOperator` method refs**, Lua/Python/JS functions.
- **map_kernel** — the hash-map **codegen** row: every language runs the *same* hand-rolled
  open-addressing (linear-probe) `int32 -> int64` map — one shared hash (a single 32-bit Fibonacci
  multiply), the same fixed 262 144-slot preallocation, no stdlib map anywhere. Insert 100 000 keys, then
  look every key up 10× in a scrambled (bijective LCG) order. Because the algorithm, hash, and capacity
  are pinned, **this is the row that answers "is kama on par with C?"** — the AOT cluster
  (kama/C/C++/Rust) should converge, exactly as it does on the compute kernels. (TS is absent here and in
  `map`/`math`: its `tsconfig.json` compiles only fib/pi/collatz/dispatch/alloc/fnptr.)
- **map** — the hash-map **library-design** row: the same workload, but each language uses its *idiomatic*
  map — kama `Map<int32, int64>`, C++ `unordered_map`, Rust `HashMap`, Go `map`, C# `Dictionary`, Java
  `HashMap` (boxed), Lua table, Python `dict`, JS `Map`. **⚠️ This measures map DESIGN, not codegen** —
  hash strength, growth policy, and layout all differ, so the languages legitimately do not cluster.
  **C is absent by design, not by omission: C has no stdlib hashmap.** Its hand-rolled map is what
  `map_kernel` runs, in every language at once; entering that bespoke, preallocated structure here and
  ranking it against everyone else's general-purpose library maps would read as a codegen win when the
  actual finding is the trivial "a purpose-built preallocated map beats a general-purpose one".
  Every map that can be pre-sized is (see *Methodology — pre-sizing*), so what remains is hash and
  layout: kama 5.24 ms, C++ `unordered_map` 4.9 ms, Rust `HashMap` 8.27 ms.
  kama keeps its **stronger default hash** (splitmix64, two dependent 64-bit multiplies) here; the
  pluggable `H: Hasher` slot's `FastHasher` is `map_kernel`'s single Fibonacci multiply, so a program
  that wants that tradeoff writes `Map<int32, int64, FastHasher>`. It is deliberately NOT used here:
  tuning one language's hash while the others stay idiomatic would tilt the row. That is what
  `map_kernel` is for.
- **math** — 2×10⁶ iterations of the `std::math` hot ops an engine leans on: `Vec4` add/sub/scale, `dot`,
  `Mat4*Vec4`, `Mat4*Mat4`, and the `Quat` Hamilton product. Every input is a small integer-valued
  float32 so all intermediates are **exactly representable** (`|v| < 2^24`) — the checksum is therefore
  bit-identical across the float32 (kama/C/C++/Rust/Go) and float64 (JS/Lua/Python) backends. This is a
  **codegen/SIMD-vectorization** number: kama's math types carry a SIMD-ready layout, so this row tracks
  whether the field-by-field ops lower to packed SIMD as the backend evolves. (There is no SIMD *campaign*
  and no explicit vector surface — vectorization is the C backend's, earned by the layout plus release
  inlining; see [SPEC.md](../SPEC.md) *Math*.)

## NATIVE — execution time (median, ms)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 6.05 (1.0×) | 5.95 (1.0×) | 6.19 (1.0×) | 6.09 (1.0×) | 10.09 (1.7×) | 33.97 (5.6×) | 27.29 (4.5×) | 90.06 (14.9×) | 213.85 (35.3×) |
| pi | 11.71 (1.0×) | 11.84 (1.0×) | 11.96 (1.0×) | 11.89 (1.0×) | 13.92 (1.2×) | 32.28 (2.8×) | 37.91 (3.2×) | 123.02 (10.5×) | 1670.39 (142.6×) |
| collatz | 67.07 (1.0×) | 67.63 (1.0×) | 67.51 (1.0×) | 66.86 (1.0×) | 91.64 (1.4×) | 124.0 (1.8×) | 140.03 (2.1×) | 981.12 (14.6×) | 2957.59 (44.1×) |
| dispatch | 6.48 (1.0×) | 6.25 (1.0×) | 6.62 (1.0×) | 6.52 (1.0×) | 13.87 (2.1×) | 27.83 (4.3×) | 30.5 (4.7×) | 103.71 (16.0×) | 643.08 (99.2×) |
| alloc | 1.34 (1.0×) | 1.14 (0.9×) | 1.62 (1.2×) | 2.29 (1.7×) | 6.66 (5.0×) | 27.0 (20.1×) | 53.46 (39.9×) | 24.96 (18.6×) | 109.61 (81.8×) |
| fnptr | 2.48 (1.0×) | 2.5 (1.0×) | 2.7 (1.1×) | 2.67 (1.1×) | 5.03 (2.0×) | 33.62 (13.6×) | 31.67 (12.8×) | 138.88 (56.0×) | 715.52 (288.5×) |
| map | 5.24 (1.0×) | — | 4.9 (0.9×) | 8.27 (1.6×) | 23.61 (4.5×) | 59.22 (11.3×) | 43.09 (8.2×) | 7.55 (1.4×) | 128.16 (24.5×) |
| map_kernel | 2.73 (1.0×) | 2.7 (1.0×) | 3.02 (1.1×) | 3.03 (1.1×) | 4.84 (1.8×) | 31.89 (11.7×) | 29.09 (10.7×) | 52.26 (19.1×) | 217.64 (79.7×) |
| math | 3.15 (1.0×) | 3.09 (1.0×) | 3.28 (1.0×) | 3.55 (1.1×) | 79.02 (25.1×) | 116.97 (37.1×) | 113.49 (36.0×) | 3916.21 (1243.2×) | 4311.81 (1368.8×) |

## NATIVE — peak resident memory (MB)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 40 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 40 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 82 | 2 | 8 |
| fnptr | 2 | 2 | 2 | 2 | 2 | 21 | 41 | 2 | 8 |
| map | 7 | n/a | 7 | 5 | 5 | 23 | 69 | 3 | 21 |
| map_kernel | 4 | 4 | 5 | 4 | 4 | 24 | 44 | 13 | 18 |
| math | 2 | 2 | 3 | 2 | 2 | 20 | 129 | 2 | 8 |

## NATIVE — package size

_What you ship: a **self-contained** binary needs no runtime; managed/interpreted rows are the
assembly/source only and additionally require the noted runtime (.NET / JVM / interpreter). The size
column is each language's `fib` artifact; the range spans every workload it built. C# and
Java ship one multi-workload assembly, so their size is the same everywhere._

| lang | package size (`fib`) | range across workloads | kind |
|---|---|---|---|
| kama | 66.0 KB | 66.0 KB – 66.2 KB | self-contained |
| C | 66.1 KB | 66.1 KB | self-contained |
| C++ | 66.1 KB | 66.1 KB – 66.3 KB | self-contained |
| Rust | 322.3 KB | 322.3 KB | self-contained |
| Go | 1604.8 KB | 1603.7 KB – 1605.2 KB | self-contained |
| C# (JIT) | 9.0 KB | 9.0 KB | + .NET runtime |
| Java (JIT) | 4.9 KB | 4.9 KB | + JVM |
| Lua | 0.1 KB | 0.1 KB – 1.9 KB | source (+ Lua) |
| Python | 0.1 KB | 0.1 KB – 1.8 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` — 9 workloads; C skips `map`); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. kama's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| kama | 1345 ms | 9 |
| C | 322 ms | 8 |
| C++ | 758 ms | 9 |
| Rust | 2058 ms | 9 |
| Go | 1403 ms | 9 |
| C# (JIT) | 1481 ms | 1 |
| Java (JIT) | 301 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 20.3 (1.0×) | 27.57 (1.4×) | 27.91 (1.4×) |
| pi | 21.06 (1.0×) | 26.47 (1.3×) | 26.45 (1.3×) |
| collatz | 95.66 (1.0×) | 429.51 (4.5×) | 427.28 (4.5×) |
| dispatch | 29.12 (1.0×) | 22.99 (0.8×) | 22.59 (0.8×) |
| alloc | 18.08 (1.0×) | 16.27 (0.9×) | 16.58 (0.9×) |
| fnptr | 11.68 (1.0×) | 42.73 (3.7×) | 41.76 (3.6×) |
| map | 20.15 (1.0×) | 48.44 (2.4×) | — |
| map_kernel | 17.26 (1.0×) | 28.76 (1.7×) | — |
| math | 17.07 (1.0×) | 170.15 (10.0×) | — |

## WASM track — peak resident memory (MB)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 43 | 44 | 44 |
| pi | 43 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 45 | 45 | 45 |
| alloc | 46 | 46 | 46 |
| fnptr | 42 | 45 | 45 |
| map | 50 | 54 | n/a |
| map_kernel | 46 | 50 | n/a |
| math | 43 | 49 | n/a |

## WASM track — module size

_The size column is each language's `fib` artifact; the range spans every workload it built._

| lang | package size (`fib`) | range across workloads | kind |
|---|---|---|---|
| kama→wasm | 0.4 KB | 0.2 KB – 9.5 KB | + wasm/JS host |
| JS | 0.1 KB | 0.1 KB – 1.9 KB | source (+ node) |
| TS | 0.1 KB | 0.1 KB – 0.6 KB | source (+ node) |

## WASM track — compile time

_kama→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

| lang | compile time | binaries built |
|---|---|---|
| kama→wasm | 5757 ms | 9 |
| TS | 244 ms | 1 |

## Caveats
- **`dispatch` measures *true* dynamic dispatch.** An earlier variant called two stack locals of
  known `final` type, which let optimizers (notably rustc) devirtualize the call to plain arithmetic
  — comparing Rust's *devirtualized* code against everyone else's real indirect calls (a ~5×
  artifact). The current variant dispatches over a heap-owned heterogeneous collection the optimizer
  cannot resolve, so every AOT language performs a genuine vtable call: **Rust converges to C** (the
  artifact is gone) and **kama ties the C/C++/Rust cluster**. Only **Go**'s interface dispatch
  trails (~2×).
- **arm64 results** — not comparable to x86 runs (arch recorded above).
- **JIT warmup** (C#, Java, node): mitigated by hyperfine warmups; tiny workloads still partly reflect
  startup. Java (HotSpot) has the heaviest fixed startup here, so startup-dominated workloads (e.g. `fib`)
  understate the warmed-up steady-state a **long-running app like Minecraft** gets from HotSpot's C2 tier;
  the longer compute loops (pi/collatz/dispatch/fnptr) still tier up within a run.
- **Container overhead** applies equally to all languages, so relative numbers are fair; absolute numbers
  carry slight overhead.
- **C# AOT** is built best-effort (`bench/build/csharp-aot`); the table shows the JIT runtime.
- Numbers are a snapshot to guide optimization, regenerated by `bench/run all`; do not hand-edit.
