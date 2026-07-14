# kama benchmark results

_Generated: 2026-07-13 20:02 · arch: aarch64 (Linux) · in the `kama-bench` container_

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
(kama/C/C++/Rust) converges there, while **Go**'s interface dispatch trails ~2×. The
**`alloc`** workload (added once `DynamicArray<T>` landed in M9) is the one to watch for the no-GC story: it
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
  C++ `vector`, Rust `Vec`, Go slice, C# `DynamicArray`, Java `ArrayList`, Lua table, Python/JS array, C manual realloc).
- **fnptr** — 8×10⁶ indirect calls through a function pointer, routed through a function boundary
  (`apply(op, x)`) so the call stays genuinely indirect (the fnptr analog of `dispatch`'s virtual calls).
  Each language uses its idiomatic callable — kama `fnptr` (a bare C function pointer, zero-cost), C/C++
  function pointers, Rust `fn` pointers, Go func values, **C# `Func<>` delegates**, **Java
  `LongUnaryOperator` method refs**, Lua/Python/JS functions.
- **map** — a hash-map throughput test: insert 100 000 int keys, then look every key up 10× in a
  scrambled (bijective LCG) order (hash + probe cost). Each language uses its idiomatic map — kama
  `Map<int32, int64>`, C **hand-rolled open-addressing** (no stdlib hashmap), C++ `unordered_map`, Rust
  `HashMap`, Go `map`, C# `Dictionary`, Java `HashMap` (boxed), Lua table, Python `dict`, JS `Map`.
  **⚠️ Unlike the compute kernels (identical algorithms), `map` compares each language's _idiomatic map
  design_ — hash strength, structure, and preallocation all differ — so it is NOT a pure codegen number
  and the languages do not cluster.** kama's `Map` is at codegen parity with C for equal work: the headline
  gap is that kama's stdlib chooses a **stronger default hash** (splitmix64 — two dependent 64-bit multiplies
  vs C's single multiply) and does **not yet preallocate** (grows from 8, rehashing on a bulk insert, while
  the C map is sized up front). Measured (100 k×10, `-O3`): give C the same splitmix64 hash and it goes
  2.9 → 5.7 ms; give kama a single-multiply hash and it goes 8.7 → 3.3 ms ≈ C. With an equal hash **and**
  equal preallocation, kama ≈ C (the Map machinery — probe, `Optional`, value copy — is already at parity).
  Both levers are stdlib design choices tracked in ROADMAP §5 (Map `reserve` + a pluggable hasher).

## NATIVE — execution time (median, ms)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 6.05 (1.0×) | 6.38 (1.1×) | 6.31 (1.0×) | 8.09 (1.3×) | 10.36 (1.7×) | 32.28 (5.3×) | 26.78 (4.4×) | 96.87 (16.0×) | 214.24 (35.4×) |
| pi | 11.93 (1.0×) | 11.77 (1.0×) | 12.13 (1.0×) | 12.02 (1.0×) | 14.02 (1.2×) | 32.44 (2.7×) | 36.45 (3.1×) | 122.92 (10.3×) | 1643.04 (137.7×) |
| collatz | 66.3 (1.0×) | 66.19 (1.0×) | 66.23 (1.0×) | 66.3 (1.0×) | 91.05 (1.4×) | 122.79 (1.9×) | 136.75 (2.1×) | 977.63 (14.7×) | 2947.83 (44.5×) |
| dispatch | 6.3 (1.0×) | 6.24 (1.0×) | 6.49 (1.0×) | 6.46 (1.0×) | 13.93 (2.2×) | 25.14 (4.0×) | 29.5 (4.7×) | 103.42 (16.4×) | 642.01 (101.9×) |
| alloc | 1.21 (1.0×) | 1.16 (1.0×) | 1.58 (1.3×) | 2.3 (1.9×) | 6.53 (5.4×) | 24.03 (19.9×) | 44.68 (36.9×) | 24.42 (20.2×) | 105.58 (87.3×) |
| fnptr | 2.5 (1.0×) | 2.51 (1.0×) | 2.72 (1.1×) | 2.69 (1.1×) | 5.06 (2.0×) | 32.04 (12.8×) | 28.8 (11.5×) | 139.06 (55.6×) | 707.93 (283.2×) |
| map | 8.95 (1.0×) | 2.76 (0.3×) | 4.63 (0.5×) | 8.58 (1.0×) | 23.06 (2.6×) | 58.59 (6.5×) | 42.39 (4.7×) | 7.4 (0.8×) | 127.96 (14.3×) |

## NATIVE — peak resident memory (MB)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 19 | 38 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 79 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| map | 5 | 4 | 7 | 4 | 5 | 27 | 68 | 3 | 21 |

## NATIVE — package size

_What you ship: a **self-contained** binary needs no runtime; managed/interpreted rows are the
assembly/source only and additionally require the noted runtime (.NET / JVM / interpreter)._

| lang | package size | kind |
|---|---|---|
| kama | 66.0 KB | self-contained |
| C | 66.1 KB | self-contained |
| C++ | 66.1 KB | self-contained |
| Rust | 322.3 KB | self-contained |
| Go | 1604.8 KB | self-contained |
| C# (JIT) | 6.5 KB | + .NET runtime |
| Java (JIT) | 3.2 KB | + JVM |
| Lua | 0.1 KB | source (+ Lua) |
| Python | 0.1 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` = 6); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. kama's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| kama | 738 ms | 7 |
| C | 298 ms | 7 |
| C++ | 626 ms | 7 |
| Rust | 1575 ms | 7 |
| Go | 1350 ms | 7 |
| C# (JIT) | 1415 ms | 1 |
| Java (JIT) | 232 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 19.42 (1.0×) | 30.45 (1.6×) | 28.63 (1.5×) |
| pi | 20.94 (1.0×) | 26.12 (1.2×) | 27.04 (1.3×) |
| collatz | 92.75 (1.0×) | 419.05 (4.5×) | 423.02 (4.6×) |
| dispatch | 27.73 (1.0×) | 22.07 (0.8×) | 22.86 (0.8×) |
| alloc | 16.22 (1.0×) | 16.65 (1.0×) | 15.96 (1.0×) |
| fnptr | 11.1 (1.0×) | 42.94 (3.9×) | 42.13 (3.8×) |
| map | 22.95 (1.0×) | 47.13 (2.1×) | — |

## WASM track — peak resident memory (MB)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 42 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 44 | 44 | 44 |
| alloc | 44 | 46 | 46 |
| fnptr | 42 | 45 | 45 |
| map | 48 | 54 | n/a |

## WASM track — module size

| lang | package size | kind |
|---|---|---|
| kama→wasm | 0.4 KB | + wasm/JS host |
| JS | 0.1 KB | source (+ node) |
| TS | 0.1 KB | source (+ node) |

## WASM track — compile time

_kama→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

| lang | compile time | binaries built |
|---|---|---|
| kama→wasm | 3864 ms | 7 |
| TS | 229 ms | 1 |

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
