#!/usr/bin/env python3
"""Render bench/build/results.tsv -> docs/benchmarks/RESULTS.md (+ results.json).

Builds time / peak-RSS / artifact-size tables for the native and wasm tracks, and
runs the fairness gate: every language must produce the same exit-code checksum as
cstar for a given workload (a mismatch means the algorithms diverged).
"""
import csv, json, os, subprocess, sys, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TSV = os.path.join(ROOT, "bench/build/results.tsv")
OUT_MD = os.path.join(ROOT, "docs/benchmarks/RESULTS.md")
OUT_JSON = os.path.join(ROOT, "docs/benchmarks/results.json")

WORKLOADS = ["fib", "pi", "collatz", "dispatch", "alloc", "fnptr"]
NATIVE = ["cstar", "c", "cpp", "rust", "go", "csharp", "java", "lua", "python"]
WASM = ["cstar-wasm", "js", "ts"]
LABEL = {"cstar": "cstar", "c": "C", "cpp": "C++", "rust": "Rust", "go": "Go",
         "csharp": "C# (JIT)", "java": "Java (JIT)", "lua": "Lua", "python": "Python",
         "cstar-wasm": "cstar→wasm", "js": "JS", "ts": "TS"}
# What "package size" means per language: a self-contained native binary vs code that needs an
# external runtime (managed assembly / interpreted source). Keeps the size table honest.
RUNTIME = {"cstar": "self-contained", "c": "self-contained", "cpp": "self-contained",
           "rust": "self-contained", "go": "self-contained",
           "csharp": "+ .NET runtime", "java": "+ JVM",
           "lua": "source (+ Lua)", "python": "source (+ Python)",
           "js": "source (+ node)", "ts": "source (+ node)", "cstar-wasm": "+ wasm/JS host"}

# Compile time (bench/build/compile.tsv): lang -> (compile_ms, artifacts). Interpreted langs absent.
COMPILE = {}
_ctp = os.path.join(ROOT, "bench/build/compile.tsv")
if os.path.exists(_ctp):
    with open(_ctp) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            COMPILE[r["lang"]] = (r["compile_ms"], r["artifacts"])

def sh(*a):
    try: return subprocess.check_output(a, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception: return "?"

rows = []
with open(TSV) as f:
    for r in csv.DictReader(f, delimiter="\t"):
        rows.append(r)

def get(track, lang, w, field):
    for r in rows:
        if r["track"] == track and r["lang"] == lang and r["workload"] == w:
            return r[field]
    return None

def cell(v, unit=""):
    if v is None: return "—"
    if v in ("NA", "?"): return "n/a"
    return f"{v}{unit}"

def time_table(track, langs):
    base = langs[0]   # cstar / cstar-wasm — the per-workload baseline for the xN multiplier
    out = ["| workload | " + " | ".join(LABEL[l] for l in langs) + " |",
           "|" + "---|" * (len(langs) + 1)]
    for w in WORKLOADS:
        b = get(track, base, w, "time_ms")
        cells = []
        for l in langs:
            v = get(track, l, w, "time_ms")
            try:    cells.append(f"{v} ({float(v)/float(b):.1f}×)")
            except (TypeError, ValueError, ZeroDivisionError): cells.append(cell(v))
        out.append("| " + w + " | " + " | ".join(cells) + " |")
    return "\n".join(out)

def rss_table(track, langs):
    out = ["| workload | " + " | ".join(LABEL[l] for l in langs) + " |",
           "|" + "---|" * (len(langs) + 1)]
    for w in WORKLOADS:
        cells = []
        for l in langs:
            v = get(track, l, w, "rss_kb")
            cells.append("n/a" if v in (None, "NA") else f"{int(v)//1024}")
        out.append("| " + w + " | " + " | ".join(cells) + " |")
    return "\n".join(out)

def size_table(track, langs):
    out = ["| lang | package size | kind |", "|---|---|---|"]
    for l in langs:
        sz = None
        for w in WORKLOADS:
            v = get(track, l, w, "size_bytes")
            if v and v not in ("0", "NA", None):
                sz = int(v); break
        if sz is None:
            continue
        out.append(f"| {LABEL[l]} | {sz / 1024.0:.1f} KB | {RUNTIME.get(l, '')} |")
    return "\n".join(out)

def compile_table(langs):
    out = ["| lang | compile time | binaries built |", "|---|---|---|"]
    for l in langs:
        if l in COMPILE:
            ms, n = COMPILE[l]
            out.append(f"| {LABEL[l]} | {cell(ms, ' ms')} | {n} |")
    return "\n".join(out)

# fairness gate
gate_lines = []
ok = True
for w in WORKLOADS:
    ref = get("native", "cstar", w, "exit")
    if ref is None: continue
    mism = []
    for track, langs in (("native", NATIVE), ("wasm", WASM)):
        for l in langs:
            e = get(track, l, w, "exit")
            if e is not None and e != ref:
                mism.append(f"{LABEL[l]}={e}"); ok = False
    status = "✓ all match" if not mism else "✗ MISMATCH: " + ", ".join(mism)
    gate_lines.append(f"- `{w}`: checksum = {ref} (exit code) — {status}")

env = {
    "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
    "arch": sh("uname", "-m"),
    "kernel": sh("uname", "-s"),
    "clang": sh("bash", "-c", "clang --version | head -1"),
    "rustc": sh("rustc", "--version"),
    "go": sh("bash", "-c", "go version"),
    "dotnet": sh("dotnet", "--version"),
    "java": sh("bash", "-c", "java -version 2>&1 | head -1"),
    "node": sh("node", "--version"),
    "lua": sh("bash", "-c", "lua5.4 -v 2>&1 | head -1"),
    "python": sh("python3", "--version"),
}

md = f"""# cstar benchmark results

_Generated: {env['generated']} · arch: {env['arch']} ({env['kernel']}) · in the `cstar-bench` container_

Toolchains: clang `{env['clang']}` · {env['rustc']} · {env['go']} · dotnet {env['dotnet']} · {env['java']} · node {env['node']} · {env['lua']} · {env['python']}
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

{chr(10).join(gate_lines)}

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

{time_table("native", NATIVE)}

## NATIVE — peak resident memory (MB)

{rss_table("native", NATIVE)}

## NATIVE — package size

_What you ship: a **self-contained** binary needs no runtime; managed/interpreted rows are the
assembly/source only and additionally require the noted runtime (.NET / JVM / interpreter)._

{size_table("native", NATIVE)}

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` = 6); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. cstar's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

{compile_table(["cstar", "c", "cpp", "rust", "go", "csharp", "java"])}

## WASM track — execution time under node (median, ms)

{time_table("wasm", WASM)}

## WASM track — peak resident memory (MB)

{rss_table("wasm", WASM)}

## WASM track — module size

{size_table("wasm", WASM)}

## WASM track — compile time

_cstar→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

{compile_table(["cstar-wasm", "ts"])}

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
"""

os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
with open(OUT_MD, "w") as f:
    f.write(md)
with open(OUT_JSON, "w") as f:
    json.dump({"env": env, "rows": rows,
               "compile": {l: {"compile_ms": ms, "artifacts": n} for l, (ms, n) in COMPILE.items()}},
              f, indent=2)

print(f"wrote {OUT_MD}")
print("fairness gate:", "PASS" if ok else "FAIL (checksum mismatch — see RESULTS.md)")
sys.exit(0 if ok else 1)
