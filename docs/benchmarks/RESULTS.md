# cstar benchmark results

_Generated: 2026-07-02 20:10 · arch: aarch64 (Linux) · in the `cstar-bench` container_

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

The compute workloads (fib/pi/collatz/dispatch) are tuned so the slow interpreters finish quickly; the
fast compiled languages run in a few ms, so small absolute differences between them are noise. The
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
- **dispatch** — 8×10⁶ virtual-method calls through a base reference (dynamic-dispatch cost).
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
| fib | 7.41 (1.0×) | 7.37 (1.0×) | 7.55 (1.0×) | 6.39 (0.9×) | 10.28 (1.4×) | 31.42 (4.2×) | 26.66 (3.6×) | 88.98 (12.0×) | 213.75 (28.8×) |
| pi | 11.82 (1.0×) | 11.81 (1.0×) | 12.06 (1.0×) | 11.9 (1.0×) | 13.9 (1.2×) | 32.3 (2.7×) | 36.33 (3.1×) | 117.99 (10.0×) | 1598.52 (135.2×) |
| collatz | 65.65 (1.0×) | 65.56 (1.0×) | 66.16 (1.0×) | 66.09 (1.0×) | 91.09 (1.4×) | 123.91 (1.9×) | 133.1 (2.0×) | 962.49 (14.7×) | 2940.77 (44.8×) |
| dispatch | 6.19 (1.0×) | 6.16 (1.0×) | 6.42 (1.0×) | 1.22 (0.2×) | 5.05 (0.8×) | 24.77 (4.0×) | 26.22 (4.2×) | 141.8 (22.9×) | 701.46 (113.3×) |
| alloc | 1.17 (1.0×) | 1.18 (1.0×) | 1.64 (1.4×) | 2.29 (2.0×) | 6.64 (5.7×) | 24.24 (20.7×) | 45.58 (39.0×) | 24.27 (20.7×) | 108.7 (92.9×) |
| fnptr | 2.49 (1.0×) | 2.47 (1.0×) | 2.71 (1.1×) | 2.65 (1.1×) | 5.02 (2.0×) | 32.01 (12.9×) | 28.75 (11.5×) | 138.95 (55.8×) | 705.67 (283.4×) |

## NATIVE — peak resident memory (MB)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 2 | 2 | 2 | 19 | 39 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 79 | 2 | 8 |
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
| Java (JIT) | 2.6 KB | + JVM |
| Lua | 0.1 KB | source (+ Lua) |
| Python | 0.1 KB | source (+ Python) |

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` = 6); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. cstar's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

| lang | compile time | binaries built |
|---|---|---|
| cstar | 299 ms | 6 |
| C | 239 ms | 6 |
| C++ | 377 ms | 6 |
| Rust | 1273 ms | 6 |
| Go | 1316 ms | 6 |
| C# (JIT) | 1535 ms | 1 |
| Java (JIT) | 264 ms | 1 |

## WASM track — execution time under node (median, ms)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 19.74 (1.0×) | 27.16 (1.4×) | 27.18 (1.4×) |
| pi | 21.14 (1.0×) | 26.08 (1.2×) | 25.73 (1.2×) |
| collatz | 92.35 (1.0×) | 412.57 (4.5×) | 419.1 (4.5×) |
| dispatch | 23.08 (1.0×) | 20.67 (0.9×) | 20.74 (0.9×) |
| alloc | 16.29 (1.0×) | 16.26 (1.0×) | 15.93 (1.0×) |
| fnptr | 11.18 (1.0×) | 41.72 (3.7×) | 41.25 (3.7×) |

## WASM track — peak resident memory (MB)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 42 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 44 |
| dispatch | 42 | 45 | 45 |
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
| TS | 224 ms | 1 |

## Caveats
- **arm64 results** — not comparable to x86 runs (arch recorded above).
- **JIT warmup** (C#, Java, node): mitigated by hyperfine warmups; tiny workloads still partly reflect
  startup. Java (HotSpot) has the heaviest fixed startup here, so startup-dominated workloads (e.g. `fib`)
  understate the warmed-up steady-state a **long-running app like Minecraft** gets from HotSpot's C2 tier;
  the longer compute loops (pi/collatz/dispatch/fnptr) still tier up within a run.
- **Container overhead** applies equally to all languages, so relative numbers are fair; absolute numbers
  carry slight overhead.
- **C# AOT** is built best-effort (`bench/build/csharp-aot`); the table shows the JIT runtime.
- Numbers are a snapshot to guide optimization, regenerated by `bench/run all`; do not hand-edit.
