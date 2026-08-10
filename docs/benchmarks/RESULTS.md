# kama benchmark results

_Generated: 2026-08-10 17:25 · arch: aarch64 (Linux) · in the `kama-bench` container_

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
| fib | 6.31 (1.0×) | 6.63 (1.1×) | 6.48 (1.0×) | 6.54 (1.0×) | 10.41 (1.6×) | 37.22 (5.9×) | 27.72 (4.4×) | 92.64 (14.7×) | 216.85 (34.4×) |
| pi | 12.51 (1.0×) | 12.18 (1.0×) | 12.2 (1.0×) | 12.13 (1.0×) | 14.09 (1.1×) | 38.14 (3.0×) | 39.11 (3.1×) | 123.84 (9.9×) | 1615.9 (129.2×) |
| collatz | 68.46 (1.0×) | 67.96 (1.0×) | 68.53 (1.0×) | 67.47 (1.0×) | 91.89 (1.3×) | 126.4 (1.8×) | 140.51 (2.1×) | 980.45 (14.3×) | 2953.37 (43.1×) |
| dispatch | 6.43 (1.0×) | 6.41 (1.0×) | 6.56 (1.0×) | 6.68 (1.0×) | 14.52 (2.3×) | 32.76 (5.1×) | 31.63 (4.9×) | 104.47 (16.2×) | 634.92 (98.7×) |
| alloc | 1.38 (1.0×) | 1.2 (0.9×) | 1.6 (1.2×) | 2.4 (1.7×) | 6.73 (4.9×) | 27.39 (19.8×) | 48.05 (34.8×) | 24.71 (17.9×) | 109.63 (79.4×) |
| fnptr | 2.66 (1.0×) | 2.52 (0.9×) | 2.77 (1.0×) | 2.82 (1.1×) | 5.19 (2.0×) | 32.38 (12.2×) | 31.07 (11.7×) | 139.31 (52.4×) | 700.53 (263.4×) |
| map | 8.04 (1.0×) | — | 4.86 (0.6×) | 8.97 (1.1×) | 23.92 (3.0×) | 65.63 (8.2×) | 45.5 (5.7×) | 7.69 (1.0×) | 136.71 (17.0×) |
| map_kernel | 3.05 (1.0×) | 2.8 (0.9×) | 3.18 (1.0×) | 2.94 (1.0×) | 5.09 (1.7×) | 32.34 (10.6×) | 30.23 (9.9×) | 56.54 (18.5×) | 252.1 (82.7×) |
| math | 3.55 (1.0×) | 3.17 (0.9×) | 3.47 (1.0×) | 3.69 (1.0×) | 81.81 (23.0×) | 118.23 (33.3×) | 118.56 (33.4×) | 3923.27 (1105.1×) | 4310.51 (1214.2×) |

## NATIVE — peak resident memory (MB)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 20 | 38 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 21 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 79 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| map | 5 | n/a | 7 | 4 | 5 | 27 | 69 | 3 | 21 |
| map_kernel | 4 | 4 | 5 | 4 | 4 | 24 | 43 | 13 | 18 |
| math | 2 | 2 | 3 | 2 | 2 | 21 | 128 | 2 | 8 |

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
| kama | 908 ms | 9 |
| C | 346 ms | 8 |
| C++ | 780 ms | 9 |
| Rust | 2172 ms | 9 |
| Go | 1711 ms | 9 |
| C# (JIT) | 1660 ms | 1 |
| Java (JIT) | 329 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 21.6 (1.0×) | 28.74 (1.3×) | 29.01 (1.3×) |
| pi | 22.05 (1.0×) | 28.31 (1.3×) | 28.27 (1.3×) |
| collatz | 96.81 (1.0×) | 426.53 (4.4×) | 431.69 (4.5×) |
| dispatch | 29.04 (1.0×) | 23.29 (0.8×) | 23.07 (0.8×) |
| alloc | 18.92 (1.0×) | 17.3 (0.9×) | 18.18 (1.0×) |
| fnptr | 12.14 (1.0×) | 44.09 (3.6×) | 42.93 (3.5×) |
| map | 23.73 (1.0×) | 48.14 (2.0×) | — |
| map_kernel | 17.97 (1.0×) | 28.75 (1.6×) | — |
| math | 17.9 (1.0×) | 165.16 (9.2×) | — |

## WASM track — peak resident memory (MB)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 43 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 45 | 45 | 45 |
| alloc | 46 | 46 | 46 |
| fnptr | 42 | 45 | 45 |
| map | 48 | 54 | n/a |
| map_kernel | 47 | 50 | n/a |
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
| kama→wasm | 5620 ms | 9 |
| TS | 287 ms | 1 |

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
