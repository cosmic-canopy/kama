# cstar benchmark results

_Generated: 2026-07-02 15:19 · arch: aarch64 (Linux) · in the `cstar-bench` container_

Toolchains: clang `Ubuntu clang version 18.1.3 (1ubuntu1)` · rustc 1.79.0 (129f3b996 2024-06-10) · go version go1.22.5 linux/arm64 · dotnet 8.0.422 · openjdk version "21.0.11" 2026-04-21 · node v22.16.0 · Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio · Python 3.12.3
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median); the `(N×)` after each time is relative to cstar for that workload (native → cstar, wasm → cstar→wasm). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

cstar transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads cstar is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. The signals worth trusting here are:
1. cstar (native) vs **managed/interpreted** languages (C#, Java, Go, Lua, Python),
2. **peak RSS** and **artifact size** (the low-footprint goal),
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
| fib | 7.47 (1.0×) | 7.37 (1.0×) | 7.89 (1.1×) | 6.08 (0.8×) | 10.05 (1.3×) | 33.7 (4.5×) | 26.01 (3.5×) | 91.13 (12.2×) | 214.52 (28.7×) |
| pi | 11.76 (1.0×) | 11.81 (1.0×) | 13.04 (1.1×) | 13.66 (1.2×) | 14.09 (1.2×) | 36.98 (3.1×) | 36.97 (3.1×) | 121.07 (10.3×) | 1652.3 (140.5×) |
| collatz | 65.59 (1.0×) | 65.99 (1.0×) | 65.93 (1.0×) | 65.75 (1.0×) | 91.22 (1.4×) | 122.84 (1.9×) | 136.11 (2.1×) | 972.95 (14.8×) | 2942.07 (44.9×) |
| dispatch | 6.21 (1.0×) | 6.16 (1.0×) | 6.42 (1.0×) | 1.22 (0.2×) | 4.94 (0.8×) | 23.81 (3.8×) | 26.59 (4.3×) | 142.32 (22.9×) | 714.23 (115.0×) |
| alloc | 1.2 (1.0×) | 1.33 (1.1×) | 1.77 (1.5×) | 2.27 (1.9×) | 6.73 (5.6×) | 22.24 (18.5×) | 41.65 (34.7×) | 25.45 (21.2×) | 108.27 (90.2×) |
| fnptr | 2.49 (1.0×) | 2.5 (1.0×) | 2.72 (1.1×) | 2.7 (1.1×) | 5.1 (2.0×) | 31.93 (12.8×) | 28.92 (11.6×) | 138.64 (55.7×) | 703.78 (282.6×) |

## NATIVE — peak resident memory (MB)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Java (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 19 | 39 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 78 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 20 | 39 | 2 | 8 |

## NATIVE — artifact size

| lang | artifact size |
|---|---|
| cstar | 66.0 KB |
| C | 66.1 KB |
| C++ | 66.1 KB |
| Rust | 322.3 KB |
| Go | 1604.8 KB |
| C# (JIT) | 6.0 KB |
| Java (JIT) | 2.6 KB |

## WASM track — execution time under node (median, ms)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 19.39 (1.0×) | 27.69 (1.4×) | 27.49 (1.4×) |
| pi | 21.26 (1.0×) | 26.15 (1.2×) | 26.33 (1.2×) |
| collatz | 94.54 (1.0×) | 420.56 (4.4×) | 426.39 (4.5×) |
| dispatch | 22.28 (1.0×) | 20.95 (0.9×) | 21.02 (0.9×) |
| alloc | 16.25 (1.0×) | 15.78 (1.0×) | 15.96 (1.0×) |
| fnptr | 11.22 (1.0×) | 41.83 (3.7×) | 41.63 (3.7×) |

## WASM track — peak resident memory (MB)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 42 | 44 | 44 |
| pi | 42 | 45 | 45 |
| collatz | 42 | 45 | 45 |
| dispatch | 42 | 45 | 45 |
| alloc | 44 | 46 | 46 |
| fnptr | 42 | 45 | 45 |

## WASM track — module size

| lang | artifact size |
|---|---|
| cstar→wasm | 0.4 KB |

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
