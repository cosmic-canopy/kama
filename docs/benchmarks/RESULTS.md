# cstar benchmark results

_Generated: 2026-07-07 16:59 · arch: aarch64 (Linux) · in the `cstar-bench` container_

Toolchains: clang `Ubuntu clang version 18.1.3 (1ubuntu1)` · rustc 1.79.0 (129f3b996 2024-06-10) · go version go1.22.5 linux/arm64 · dotnet 8.0.422 · openjdk version "21.0.11" 2026-04-21 · node v22.16.0 · Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio · Python 3.12.3
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median); the `(N×)` after each time is relative to cstar for that workload (native → cstar, wasm → cstar→wasm). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

cstar transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads cstar is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. The signals worth trusting here are:
1. cstar (native) vs **managed/interpreted** languages (C#, Java, Go, Lua, Python),
2. **peak RSS**, **compile time**, and **package size** (the low-footprint / self-contained goal),
3. on the WASM track, **cstar→wasm vs hand-written JS/TS** under the same node.

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
(cstar/C/C++/Rust) converges there, while **Go**'s interface dispatch trails ~2×. The
**`alloc`** workload (added once `List<T>` landed in M9) is the one to watch for the no-GC story: it
churns ~2M growable-list appends and 2000 collection lifetimes, so it contrasts cstar's deterministic
**RAII** free against the **garbage collectors** (Go, C#, Java, Lua, Python, JS) and against the RAII
peers (C++ `vector`, Rust `Vec`). Watch its **peak RSS** in particular — GC runtimes keep dead
allocations resident until a collection runs.

## Fairness gate (checksum equality)

Every language must produce the same exit-code checksum as cstar per workload, or the algorithms have
diverged:

- `fib`: checksum = 225 (exit code) — ✓ all match
- `pi`: checksum = 27 (exit code) — ✓ all match
- `collatz`: checksum = 2 (exit code) — ✓ all match
- `dispatch`: checksum = 0 (exit code) — ✓ all match
- `alloc`: checksum = 64 (exit code) — ✓ all match
- `fnptr`: checksum = 0 (exit code) — ✓ all match

## Workloads
- **fib** — naive recursive Fibonacci summed over 0..31 (function-call / stack-frame cost).
- **pi** — Leibniz series, 2×10⁷ terms, float64 (FP throughput; cleanest cross-language compare).
- **collatz** — sum of Collatz stopping times for 1..699 999 (integer ALU + unpredictable branches).
- **dispatch** — 8×10⁶ virtual calls over a **heterogeneous, heap-owned collection** of mixed
  concrete types built at runtime, so the concrete type is *not* knowable at the call site and the
  call **cannot be devirtualized** — a true dynamic-dispatch measurement. Each language uses its
  idiomatic owning collection (cstar `List<Owned<Shape>>`, C++ `vector<unique_ptr>`, Rust
  `Vec<Box<dyn>>`, C array of heap `Shape*`, Go `[]interface`, C#/Java `Shape[]`).
- **alloc** — 2000× (build a growable list, append 1..1000, sum, drop) ≈ 2M appends + 2000 lifetimes
  (allocator / GC pressure vs RAII; each language uses its idiomatic growable list — cstar `List<int32>`,
  C++ `vector`, Rust `Vec`, Go slice, C# `List`, Java `ArrayList`, Lua table, Python/JS array, C manual realloc).
- **fnptr** — 8×10⁶ indirect calls through a function pointer, routed through a function boundary
  (`apply(op, x)`) so the call stays genuinely indirect (the fnptr analog of `dispatch`'s virtual calls).
  Each language uses its idiomatic callable — cstar `fnptr` (a bare C function pointer, zero-cost), C/C++
  function pointers, Rust `fn` pointers, Go func values, **C# `Func<>` delegates**, **Java
  `LongUnaryOperator` method refs**, Lua/Python/JS functions.

## NATIVE — execution time (median, ms)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 7.44 (1.0×) | 7.35 (1.0×) | 7.53 (1.0×) | 6.08 (0.8×) | 10.1 (1.4×) | 28.22 (3.8×) | 25.61 (3.4×) | 89.37 (12.0×) | 214.68 (28.9×) |
| pi | 11.78 (1.0×) | 11.81 (1.0×) | 11.95 (1.0×) | 11.96 (1.0×) | 13.88 (1.2×) | 30.16 (2.6×) | 35.56 (3.0×) | 119.55 (10.1×) | 1612.48 (136.9×) |
| collatz | 65.53 (1.0×) | 65.61 (1.0×) | 66.02 (1.0×) | 65.65 (1.0×) | 90.93 (1.4×) | 123.74 (1.9×) | 133.87 (2.0×) | 964.76 (14.7×) | 2937.23 (44.8×) |
| dispatch | 6.2 (1.0×) | 6.21 (1.0×) | 7.38 (1.2×) | 6.4 (1.0×) | 13.64 (2.2×) | 25.32 (4.1×) | 28.48 (4.6×) | 103.17 (16.6×) | 624.38 (100.7×) |
| alloc | 1.2 (1.0×) | 1.14 (0.9×) | 1.6 (1.3×) | 2.27 (1.9×) | 6.46 (5.4×) | 23.15 (19.3×) | 42.57 (35.5×) | 24.26 (20.2×) | 108.28 (90.2×) |
| fnptr | 2.53 (1.0×) | 2.51 (1.0×) | 2.73 (1.1×) | 2.68 (1.1×) | 5.15 (2.0×) | 31.38 (12.4×) | 28.01 (11.1×) | 139.09 (55.0×) | 711.12 (281.1×) |

## NATIVE — peak resident memory (MB)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 19 | 38 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 7 | 25 | 79 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |

## NATIVE — package size

_What you ship: a **self-contained** binary needs no runtime; managed/interpreted rows are the
assembly/source only and additionally require the noted runtime (.NET / JVM / interpreter)._

| lang | package size | kind |
|---|---|---|
| cstar | 66.0 KB | self-contained |
| C | 66.1 KB | self-contained |
| C++ | 66.1 KB | self-contained |
| Rust | 322.3 KB | self-contained |
| Go | 1604.8 KB | self-contained |
| C# (JIT) | 6.0 KB | + .NET runtime |
| Java (JIT) | 2.7 KB | + JVM |
| Lua | 0.1 KB | source (+ Lua) |
| Python | 0.1 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` = 6); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. cstar's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| cstar | 424 ms | 6 |
| C | 227 ms | 6 |
| C++ | 459 ms | 6 |
| Rust | 1272 ms | 6 |
| Go | 1313 ms | 6 |
| C# (JIT) | 1505 ms | 1 |
| Java (JIT) | 251 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 19.94 (1.0×) | 27.43 (1.4×) | 26.85 (1.3×) |
| pi | 21.49 (1.0×) | 25.91 (1.2×) | 26.59 (1.2×) |
| collatz | 92.62 (1.0×) | 415.34 (4.5×) | 417.63 (4.5×) |
| dispatch | 27.52 (1.0×) | 21.73 (0.8×) | 22.29 (0.8×) |
| alloc | 16.1 (1.0×) | 15.67 (1.0×) | 15.78 (1.0×) |
| fnptr | 11.16 (1.0×) | 42.25 (3.8×) | 41.92 (3.8×) |

## WASM track — peak resident memory (MB)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 42 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 44 | 44 | 44 |
| alloc | 44 | 46 | 46 |
| fnptr | 42 | 45 | 45 |

## WASM track — module size

| lang | package size | kind |
|---|---|---|
| cstar→wasm | 0.4 KB | + wasm/JS host |
| JS | 0.1 KB | source (+ node) |
| TS | 0.1 KB | source (+ node) |

## WASM track — compile time

_cstar→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

| lang | compile time | binaries built |
|---|---|---|
| cstar→wasm | 3135 ms | 6 |
| TS | 233 ms | 1 |

## Caveats
- **`dispatch` measures *true* dynamic dispatch.** An earlier variant called two stack locals of
  known `final` type, which let optimizers (notably rustc) devirtualize the call to plain arithmetic
  — comparing Rust's *devirtualized* code against everyone else's real indirect calls (a ~5×
  artifact). The current variant dispatches over a heap-owned heterogeneous collection the optimizer
  cannot resolve, so every AOT language performs a genuine vtable call: **Rust converges to C** (the
  artifact is gone) and **cstar ties the C/C++/Rust cluster**. Only **Go**'s interface dispatch
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
