# cstar benchmark results

_Generated: 2026-06-30 18:39 · arch: aarch64 (Linux) · in the `cstar-bench` container_

Toolchains: clang `Ubuntu clang version 18.1.3 (1ubuntu1)` · rustc 1.79.0 (129f3b996 2024-06-10) · go version go1.22.5 linux/arm64 · dotnet 8.0.422 · node v22.16.0 · Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio · Python 3.12.3
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

cstar transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads cstar is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. The signals worth trusting here are:
1. cstar (native) vs **managed/interpreted** languages (C#, Go, Lua, Python),
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
| fib | 7.41 | 7.33 | 7.54 | 6.37 | 10.26 | 28.48 | 89.23 | 210.11 |
| pi | 11.79 | 11.74 | 12.04 | 11.97 | 13.97 | 32.34 | 117.58 | 1634.97 |
| collatz | 65.64 | 65.54 | 66.1 | 65.74 | 91.9 | 123.73 | 961.1 | 2941.75 |
| dispatch | 6.18 | 6.13 | 6.42 | 1.19 | 5.04 | 21.68 | 141.38 | 708.79 |
| alloc | 1.18 | 1.21 | 1.53 | 2.26 | 6.45 | 24.65 | 24.01 | 109.23 |
| fnptr | 2.49 | 2.52 | 2.73 | 2.66 | 5.04 | 33.08 | 138.81 | 717.81 |

## NATIVE — peak resident memory (MB)

| workload | cstar | C | C++ | Rust | Go | C# (JIT) | Lua | Python |
|---|---|---|---|---|---|---|---|---|
| fib | 2 | 2 | 3 | 2 | 2 | 19 | 2 | 8 |
| pi | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| collatz | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| dispatch | 2 | 2 | 3 | 2 | 2 | 20 | 2 | 8 |
| alloc | 2 | 2 | 3 | 2 | 7 | 25 | 2 | 8 |
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
| fib | 19.46 | 26.64 | 27.1 |
| pi | 20.79 | 26.17 | 26.16 |
| collatz | 92.42 | 417.5 | 414.1 |
| dispatch | 21.92 | 20.59 | 20.57 |
| alloc | 16.41 | 16.15 | 15.64 |
| fnptr | 11.34 | 42.09 | 40.47 |

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
- **JIT warmup** (C#, node): mitigated by hyperfine warmups; tiny workloads still partly reflect startup.
- **Container overhead** applies equally to all languages, so relative numbers are fair; absolute numbers
  carry slight overhead.
- **C# AOT** is built best-effort (`bench/build/csharp-aot`); the table shows the JIT runtime.
- Numbers are a snapshot to guide optimization, regenerated by `bench/run all`; do not hand-edit.
