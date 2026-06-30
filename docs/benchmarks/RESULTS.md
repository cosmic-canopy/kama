# cstar benchmark results

_Generated: 2026-06-30 17:31 · arch: aarch64 (Linux) · in the `cstar-bench` container_

Toolchains: clang `Ubuntu clang version 18.1.3 (1ubuntu1)` · rustc 1.79.0 (129f3b996 2024-06-10) · go version go1.22.5 linux/arm64 · dotnet 8.0.422 · node v22.16.0 · Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio · Python 3.12.3
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

cstar transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads cstar is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. The signals worth trusting here are:
1. cstar (native) vs **managed/interpreted** languages (C#, Go, Lua, Python),
2. **peak RSS** and **artifact size** (the low-footprint goal),
3. on the WASM track, **cstar→wasm vs hand-written JS/TS** under the same node.

The compute workloads (fib/pi/collatz/dispatch) are tuned so the slow interpreters finish quickly; the
fast compiled languages run in a few ms, so small absolute differences between them are noise. The
**`alloc`** workload (added once `List<T>` landed in M9) is the one to watch for the no-GC story: it
churns ~2M growable-list appends and 2000 collection lifetimes, so it contrasts cstar's deterministic
**RAII** free against the **garbage collectors** (Go, C#, Lua, Python, JS) and against the RAII peers
(C++ `vector`, Rust `Vec`). Watch its **peak RSS** in particular — GC runtimes keep dead allocations
resident until a collection runs.

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
  C++ `vector`, Rust `Vec`, Go slice, C# `List`, Lua table, Python/JS array, C manual realloc).
- **fnptr** — 8×10⁶ indirect calls through a function pointer, routed through a function boundary
  (`apply(op, x)`) so the call stays genuinely indirect (the fnptr analog of `dispatch`'s virtual calls).
  Each language uses its idiomatic callable — cstar `fnptr` (a bare C function pointer, zero-cost), C/C++
  function pointers, Rust `fn` pointers, Go func values, **C# `Func<>` delegates**, Lua/Python/JS functions.

## NATIVE — execution time (median, ms)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|
| fib | 7.41 | 7.34 | 7.63 | 6.06 | 10.38 | 31.08 | 88.0 | 210.87 |
| pi | 11.77 | 11.79 | 11.95 | 11.91 | 13.95 | 33.51 | 118.82 | 1633.05 |
| collatz | 66.11 | 65.75 | 65.88 | 65.57 | 91.08 | 125.29 | 959.84 | 2943.07 |
| dispatch | 6.17 | 6.98 | 6.41 | 1.23 | 5.1 | 24.64 | 141.64 | 702.5 |
| alloc | 1.18 | 1.17 | 1.59 | 2.24 | 6.69 | 26.42 | 24.26 | 106.74 |
| fnptr | 2.49 | 2.52 | 2.74 | 2.63 | 5.04 | 33.31 | 138.87 | 737.06 |

## NATIVE — peak resident memory (MB)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 19 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 6 | 25 | 2 | 8 |
| fnptr | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |

## NATIVE — artifact size

| lang | artifact size |
|---|---|
| cstar | 66.0 KB |
| C | 66.1 KB |
| C++ | 66.1 KB |
| Rust | 322.3 KB |
| Go | 1604.8 KB |
| C# (JIT) | 6.0 KB |

## WASM track — execution time under node (median, ms)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 19.41 | 28.51 | 27.03 |
| pi | 22.89 | 25.72 | 26.1 |
| collatz | 188.05 | 410.61 | 420.74 |
| dispatch | 28.35 | 21.25 | 21.17 |
| alloc | 14.65 | 15.83 | 15.96 |
| fnptr | 13.28 | 42.69 | 40.71 |

## WASM track — peak resident memory (MB)

| workload | cstar→wasm | JS | TS |
|---|---|---|---|
| fib | 42 | 44 | 44 |
| pi | 43 | 45 | 45 |
| collatz | 43 | 45 | 45 |
| dispatch | 43 | 45 | 45 |
| alloc | 45 | 46 | 46 |
| fnptr | 43 | 45 | 45 |

## WASM track — module size

| lang | artifact size |
|---|---|
| cstar→wasm | 0.4 KB |

## Caveats
- **arm64 results** — not comparable to x86 runs (arch recorded above).
- **JIT warmup** (C#, node): mitigated by hyperfine warmups; tiny workloads still partly reflect startup.
- **Container overhead** applies equally to all languages, so relative numbers are fair; absolute numbers
  carry slight overhead.
- **C# AOT** is built best-effort (`bench/build/csharp-aot`); the table shows the JIT runtime.
- Numbers are a snapshot to guide optimization, regenerated by `bench/run all`; do not hand-edit.
