# kama benchmark results

_Generated: 2026-10-11 10:18 · arch: aarch64 (Linux) · in the `kama-bench` container_

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
- `bulk`: checksum = 173 (exit code) — ✓ all match

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
  `map`/`math`: its `tsconfig.json` compiles only fib/pi/collatz/dispatch/alloc/fnptr/bulk.)
- **map** — the hash-map **library-design** row: the same workload, but each language uses its *idiomatic*
  map — kama `Map<int32, int64>`, C++ `unordered_map`, Rust `HashMap`, Go `map`, C# `Dictionary`, Java
  `HashMap` (boxed), Lua table, Python `dict`, JS `Map`. **⚠️ This measures map DESIGN, not codegen** —
  hash strength, growth policy, and layout all differ, so the languages legitimately do not cluster.
  **C is absent by design, not by omission: C has no stdlib hashmap.** Its hand-rolled map is what
  `map_kernel` runs, in every language at once; entering that bespoke, preallocated structure here and
  ranking it against everyone else's general-purpose library maps would read as a codegen win when the
  actual finding is the trivial "a purpose-built preallocated map beats a general-purpose one".
  Every map that can be pre-sized is (see *Methodology — pre-sizing*), so what remains is hash and
  layout: kama 4.68 ms, C++ `unordered_map` 4.7 ms, Rust `HashMap` 7.47 ms.
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

- **bulk** — bulk byte movement, the shape a protocol reader has: 64× (assemble a 4 MiB buffer from chunks of
  a 64 KiB source, 7 B to 64 KiB each, then copy it whole into a fixed buffer and sample it). Each language uses
  its idiomatic bulk append and slice copy — kama `DynamicArray.addAll` + `View.copyFrom`, Rust
  `extend_from_slice` + `copy_from_slice`, C++ `vector::insert` + `std::copy`, Go `append(s, xs...)` + `copy`,
  C `realloc` + `memcpy`, C# `List<byte>.AddRange(span)` + `CopyTo`, Java `System.arraycopy` into a doubled
  `byte[]` (its standard library's only growable byte buffer, `ByteArrayOutputStream`, cannot hand its bytes
  over without a second copy), Python `bytearray +=`, JS/TS `Uint8Array.set` into a doubled buffer, Lua
  `table.move`. So this row measures what each standard library makes of `memcpy`; the AOT cluster should
  converge on the machine's copy bandwidth.

## NATIVE — execution time (median, ms)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 6.01 (1.0×) | 6.05 (1.0×) | 6.11 (1.0×) | 6.02 (1.0×) | 10.1 (1.7×) | 28.93 (4.8×) | 25.67 (4.3×) | 89.37 (14.9×) | 213.27 (35.5×) |
| pi | 11.74 (1.0×) | 11.74 (1.0×) | 12.04 (1.0×) | 11.95 (1.0×) | 14 (1.2×) | 32.24 (2.7×) | 36.89 (3.1×) | 119.91 (10.2×) | 1653.27 (140.8×) |
| collatz | 65.83 (1.0×) | 65.61 (1.0×) | 65.92 (1.0×) | 65.84 (1.0×) | 91.04 (1.4×) | 123.48 (1.9×) | 133.64 (2.0×) | 962.92 (14.6×) | 2960.04 (45.0×) |
| dispatch | 6.26 (1.0×) | 6.18 (1.0×) | 6.43 (1.0×) | 6.37 (1.0×) | 13.51 (2.2×) | 26.93 (4.3×) | 29.94 (4.8×) | 102.93 (16.4×) | 612.84 (97.9×) |
| alloc | 1.19 (1.0×) | 1.16 (1.0×) | 1.6 (1.3×) | 2.29 (1.9×) | 6.61 (5.6×) | 27.66 (23.2×) | 45.55 (38.3×) | 24.24 (20.4×) | 108.29 (91.0×) |
| fnptr | 2.51 (1.0×) | 2.5 (1.0×) | 2.73 (1.1×) | 2.71 (1.1×) | 4.99 (2.0×) | 31.08 (12.4×) | 29.77 (11.9×) | 137.54 (54.8×) | 713.22 (284.2×) |
| map | 4.68 (1.0×) | — | 4.7 (1.0×) | 7.47 (1.6×) | 22.92 (4.9×) | 56.84 (12.1×) | 42.55 (9.1×) | 7.41 (1.6×) | 125.36 (26.8×) |
| map_kernel | 2.68 (1.0×) | 2.62 (1.0×) | 2.86 (1.1×) | 2.68 (1.0×) | 4.89 (1.8×) | 28.64 (10.7×) | 29.05 (10.8×) | 49.78 (18.6×) | 212.99 (79.5×) |
| math | 3.11 (1.0×) | 3.07 (1.0×) | 3.29 (1.1×) | 3.59 (1.2×) | 77.38 (24.9×) | 113.04 (36.3×) | 111.25 (35.8×) | 3816.79 (1227.3×) | 4269.27 (1372.8×) |
| bulk | 10.74 (1.0×) | 10.71 (1.0×) | 73.7 (6.9×) | 10.86 (1.0×) | 303.48 (28.3×) | 122.55 (11.4×) | 76.85 (7.2×) | 3504.15 (326.3×) | 23.47 (2.2×) |

## NATIVE — peak resident memory (MB)

| workload | kama | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 21 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 79 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 21 | 40 | 2 | 8 |
| map | 7 | n/a | 7 | 5 | 4 | 24 | 68 | 4 | 21 |
| map_kernel | 4 | 4 | 5 | 4 | 4 | 24 | 43 | 13 | 18 |
| math | 2 | 2 | 3 | 2 | 2 | 21 | 127 | 2 | 8 |
| bulk | 9 | 8 | 15 | 9 | 34 | 72 | 173 | 194 | 16 |

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
| C# (JIT) | 9.5 KB | 9.5 KB | + .NET runtime |
| Java (JIT) | 5.5 KB | 5.5 KB | + JVM |
| Lua | 0.1 KB | 0.1 KB – 1.9 KB | source (+ Lua) |
| Python | 0.1 KB | 0.1 KB – 1.8 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` — 10 workloads; C skips `map`); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. kama's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| kama | 1713 ms | 10 |
| C | 412 ms | 9 |
| C++ | 897 ms | 10 |
| Rust | 4311 ms | 10 |
| Go | 2029 ms | 10 |
| C# (JIT) | 2917 ms | 1 |
| Java (JIT) | 833 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 19.88 (1.0×) | 27.03 (1.4×) | 27.37 (1.4×) |
| pi | 20.66 (1.0×) | 25.89 (1.3×) | 25.88 (1.3×) |
| collatz | 93 (1.0×) | 418.21 (4.5×) | 408.19 (4.4×) |
| dispatch | 29.29 (1.0×) | 21.84 (0.7×) | 22.01 (0.8×) |
| alloc | 17.09 (1.0×) | 15.87 (0.9×) | 16.03 (0.9×) |
| fnptr | 11.19 (1.0×) | 41.92 (3.7×) | 40.64 (3.6×) |
| map | 19.56 (1.0×) | 46.26 (2.4×) | — |
| map_kernel | 16.79 (1.0×) | 27.03 (1.6×) | — |
| math | 15.91 (1.0×) | 161.77 (10.2×) | — |
| bulk | 27.74 (1.0×) | 48.74 (1.8×) | 47.86 (1.7×) |

## WASM track — peak resident memory (MB)

| workload | kama→wasm | JS | TS |
|---|---|---|---|
| fib | 43 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 45 | 44 | 45 |
| alloc | 45 | 46 | 46 |
| fnptr | 42 | 45 | 45 |
| map | 50 | 54 | n/a |
| map_kernel | 46 | 50 | n/a |
| math | 43 | 49 | n/a |
| bulk | 55 | 82 | 83 |

## WASM track — module size

_The size column is each language's `fib` artifact; the range spans every workload it built._

| lang | package size (`fib`) | range across workloads | kind |
|---|---|---|---|
| kama→wasm | 0.4 KB | 0.2 KB – 12.2 KB | + wasm/JS host |
| JS | 0.1 KB | 0.1 KB – 1.9 KB | source (+ node) |
| TS | 0.1 KB | 0.1 KB – 1.1 KB | source (+ node) |

## WASM track — compile time

_kama→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

| lang | compile time | binaries built |
|---|---|---|
| kama→wasm | 6580 ms | 10 |
| TS | 266 ms | 1 |

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
