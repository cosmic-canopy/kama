# kama benchmark results

_Generated: 2026-08-01 15:38 · arch: aarch64 (Linux) · in the `kama-bench` container_

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
  idiomatic owning collection (kama `List<Owned<Shape>>`, C++ `vector<unique_ptr>`, Rust
  `Vec<Box<dyn>>`, C array of heap `Shape*`, Go `[]interface`, C#/Java `Shape[]`).
- **alloc** — 2000× (build a growable list, append 1..1000, sum, drop) ≈ 2M appends + 2000 lifetimes
  (allocator / GC pressure vs RAII; each language uses its idiomatic growable list — kama `List<int32>`,
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
  (kama/C/C++/Rust) should converge, exactly as it does on the compute kernels. (TS is absent: its
  `tsconfig.json` covers only the six compute workloads.)
- **map** — the hash-map **library-design** row: the same workload, but each language uses its *idiomatic*
  map — kama `Map<int32, int64>`, C++ `unordered_map`, Rust `HashMap`, Go `map`, C# `Dictionary`, Java
  `HashMap` (boxed), Lua table, Python `dict`, JS `Map`. **⚠️ This measures map DESIGN, not codegen** —
  hash strength, growth policy, and layout all differ, so the languages legitimately do not cluster.
  **C is absent by design, not by omission: C has no stdlib hashmap.** Its hand-rolled map is what
  `map_kernel` runs, in every language at once; entering that bespoke, preallocated structure here and
  ranking it against everyone else's general-purpose library maps would read as a codegen win when the
  actual finding is the trivial "a purpose-built preallocated map beats a general-purpose one".
  kama's real peer here is Rust's `HashMap`, which also grows from small.
  For the record, kama's two design choices and what each costs (measured, 100 k×10, `-O3`): a **stronger
  default hash** (splitmix64, two dependent 64-bit multiplies vs one) — give C the same hash and it goes
  2.9 → 5.7 ms; and **no preallocation by default** (grows from 8, rehashing on a bulk insert) — give kama
  a single-multiply hash and it goes 8.7 → 3.3 ms ≈ C. Both knobs ship (`Map.withCapacity` /
  `Map.reserve`, and the pluggable `H: Hasher` slot whose `FastHasher` *is* C's single Fibonacci multiply),
  so a kama program that wants C's tradeoff can write `Map<int32, int64, FastHasher>.withCapacity(...)`.
  They are deliberately NOT used here: tuning one language's row while the others stay idiomatic would
  just tilt the mismatch the other way. That is what `map_kernel` is for.
- **math** — 2×10⁶ iterations of the `std::math` hot ops an engine leans on: `Vec4` add/sub/scale, `dot`,
  `Mat4*Vec4`, `Mat4*Mat4`, and the `Quat` Hamilton product. Every input is a small integer-valued
  float32 so all intermediates are **exactly representable** (`|v| < 2^24`) — the checksum is therefore
  bit-identical across the float32 (kama/C/C++/Rust/Go) and float64 (JS/Lua/Python) backends. This is a
  **codegen/SIMD-vectorization** number: kama's math types carry a SIMD-ready layout, so this row tracks
  whether the field-by-field ops lower to packed SIMD as the backend evolves (ROADMAP §2 SIMD campaign).

## NATIVE — execution time (median, ms)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 5.99 (1.0×) | 5.96 (1.0×) | 6.18 (1.0×) | 6.19 (1.0×) | 10.13 (1.7×) | 32.62 (5.4×) | 26.68 (4.5×) | 91.05 (15.2×) | 215.75 (36.0×) |
| pi | 11.93 (1.0×) | 11.85 (1.0×) | 12.1 (1.0×) | 12.07 (1.0×) | 14.2 (1.2×) | 35.41 (3.0×) | 36.32 (3.0×) | 121.6 (10.2×) | 1647.18 (138.1×) |
| collatz | 66.23 (1.0×) | 65.89 (1.0×) | 66.09 (1.0×) | 66.02 (1.0×) | 91.69 (1.4×) | 125.65 (1.9×) | 135.29 (2.0×) | 973.4 (14.7×) | 2944.1 (44.5×) |
| dispatch | 6.36 (1.0×) | 6.24 (1.0×) | 6.56 (1.0×) | 6.53 (1.0×) | 14.38 (2.3×) | 30.68 (4.8×) | 29.87 (4.7×) | 104.5 (16.4×) | 613.06 (96.4×) |
| alloc | 1.34 (1.0×) | 1.19 (0.9×) | 1.61 (1.2×) | 2.34 (1.7×) | 6.68 (5.0×) | 22.96 (17.1×) | 46.49 (34.7×) | 24.56 (18.3×) | 111.19 (83.0×) |
| fnptr | 2.57 (1.0×) | 2.5 (1.0×) | 2.8 (1.1×) | 2.72 (1.1×) | 5.1 (2.0×) | 32.92 (12.8×) | 30.11 (11.7×) | 139.07 (54.1×) | 705.62 (274.6×) |
| map | 8.2 (1.0×) | — | 4.83 (0.6×) | 8.91 (1.1×) | 23.51 (2.9×) | 63.17 (7.7×) | 46.7 (5.7×) | 7.82 (1.0×) | 139.41 (17.0×) |
| map_kernel | 2.72 (1.0×) | 2.6 (1.0×) | 2.91 (1.1×) | 3.05 (1.1×) | 4.97 (1.8×) | 31.24 (11.5×) | 28.09 (10.3×) | 52.96 (19.5×) | 224.06 (82.4×) |
| math | 3.14 (1.0×) | 3.09 (1.0×) | 3.37 (1.1×) | 3.61 (1.1×) | 78.25 (24.9×) | 120.73 (38.4×) | 111.13 (35.4×) | 3882.03 (1236.3×) | 4319.75 (1375.7×) |

## NATIVE — peak resident memory (MB)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 20 | 40 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 21 | 41 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 41 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 21 | 41 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 7 | 25 | 82 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 20 | 41 | 2 | 8 |
| map | 5 | n/a | 7 | 4 | 4 | 27 | 70 | 4 | 21 |
| map_kernel | 4 | 4 | 5 | 4 | 4 | 24 | 44 | 13 | 18 |
| math | 2 | 2 | 3 | 2 | 2 | 21 | 129 | 2 | 8 |

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
| C# (JIT) | 9.0 KB | + .NET runtime |
| Java (JIT) | 4.9 KB | + JVM |
| Lua | 0.1 KB | source (+ Lua) |
| Python | 0.1 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` = 6); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. kama's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| kama | 1227 ms | 9 |
| C | 323 ms | 8 |
| C++ | 756 ms | 9 |
| Rust | 2077 ms | 9 |
| Go | 1437 ms | 9 |
| C# (JIT) | 1525 ms | 1 |
| Java (JIT) | 295 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 20.85 (1.0×) | 28.74 (1.4×) | 28.61 (1.4×) |
| pi | 21.36 (1.0×) | 27.2 (1.3×) | 26.87 (1.3×) |
| collatz | 94.48 (1.0×) | 418.47 (4.4×) | 417.1 (4.4×) |
| dispatch | 28.23 (1.0×) | 22.84 (0.8×) | 22.49 (0.8×) |
| alloc | 18.21 (1.0×) | 16.54 (0.9×) | 17.14 (0.9×) |
| fnptr | 12.31 (1.0×) | 45.64 (3.7×) | 41.76 (3.4×) |
| map | 23.73 (1.0×) | 46.71 (2.0×) | — |
| map_kernel | 16.9 (1.0×) | 27.04 (1.6×) | — |
| math | 16.96 (1.0×) | 164.16 (9.7×) | — |

## WASM track — peak resident memory (MB)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 43 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 45 | 44 | 45 |
| alloc | 46 | 46 | 46 |
| fnptr | 42 | 45 | 45 |
| map | 48 | 54 | n/a |
| map_kernel | 46 | 50 | n/a |
| math | 42 | 51 | n/a |

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
| kama→wasm | 5718 ms | 9 |
| TS | 249 ms | 1 |

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
