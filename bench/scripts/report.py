#!/usr/bin/env python3
"""Render bench/build/results.tsv -> docs/benchmarks/RESULTS.md (+ results.json).

Builds time / peak-RSS / artifact-size tables for the native and wasm tracks, and
runs the fairness gate: every language must produce the same exit-code checksum as
kama for a given workload (a mismatch means the algorithms diverged).

`report.py --from-json` re-renders RESULTS.md from the committed results.json (its rows,
compile times and toolchain record) without measuring anything — for a prose-only change
to this file. It runs on the host; the toolchain line still describes the container run.
"""
import csv, json, os, subprocess, sys, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TSV = os.path.join(ROOT, "bench/build/results.tsv")
OUT_MD = os.path.join(ROOT, "docs/benchmarks/RESULTS.md")
OUT_JSON = os.path.join(ROOT, "docs/benchmarks/results.json")

WORKLOADS = ["fib", "pi", "collatz", "dispatch", "alloc", "fnptr", "map", "map_kernel", "math"]
NATIVE = ["kama", "c", "cpp", "rust", "go", "csharp", "java", "lua", "python"]
WASM = ["kama-wasm", "js", "ts"]
LABEL = {"kama": "kama", "c": "C", "cpp": "C++", "rust": "Rust", "go": "Go",
         "csharp": "C# (JIT)", "java": "Java (JIT)", "lua": "Lua", "python": "Python",
         "kama-wasm": "kama→wasm", "js": "JS", "ts": "TS"}
# What "package size" means per language: a self-contained native binary vs code that needs an
# external runtime (managed assembly / interpreted source). Keeps the size table honest.
RUNTIME = {"kama": "self-contained", "c": "self-contained", "cpp": "self-contained",
           "rust": "self-contained", "go": "self-contained",
           "csharp": "+ .NET runtime", "java": "+ JVM",
           "lua": "source (+ Lua)", "python": "source (+ Python)",
           "js": "source (+ node)", "ts": "source (+ node)", "kama-wasm": "+ wasm/JS host"}

FROM_JSON = "--from-json" in sys.argv[1:]

def sh(*a):
    try: return subprocess.check_output(a, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception: return "?"

# Compile time (bench/build/compile.tsv): lang -> (compile_ms, artifacts). Interpreted langs absent.
COMPILE = {}
rows = []
if FROM_JSON:
    with open(OUT_JSON) as f:
        _saved = json.load(f)
    rows = _saved["rows"]
    COMPILE = {l: (c["compile_ms"], c["artifacts"]) for l, c in _saved["compile"].items()}
else:
    _ctp = os.path.join(ROOT, "bench/build/compile.tsv")
    if os.path.exists(_ctp):
        with open(_ctp) as f:
            for r in csv.DictReader(f, delimiter="\t"):
                COMPILE[r["lang"]] = (r["compile_ms"], r["artifacts"])
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
    base = langs[0]   # kama / kama-wasm — the per-workload baseline for the xN multiplier
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

def sizes(track, l):
    """workload -> artifact bytes, for every workload this language built."""
    out = {}
    for w in WORKLOADS:
        v = get(track, l, w, "size_bytes")
        if v and v not in ("0", "NA"):
            out[w] = int(v)
    return out

def kb(n): return f"{n / 1024.0:.1f} KB"

def size_table(track, langs):
    # One column per question: the size of the smallest program (fib — every language has it), and
    # the spread across all workloads, since a program that pulls in the stdlib collections is larger.
    out = [f"| lang | package size (`{WORKLOADS[0]}`) | range across workloads | kind |", "|---|---|---|---|"]
    for l in langs:
        s = sizes(track, l)
        if WORKLOADS[0] not in s:
            continue
        lo, hi = min(s.values()), max(s.values())
        rng = kb(lo) if kb(lo) == kb(hi) else f"{kb(lo)} – {kb(hi)}"
        out.append(f"| {LABEL[l]} | {kb(s[WORKLOADS[0]])} | {rng} | {RUNTIME.get(l, '')} |")
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
    ref = get("native", "kama", w, "exit")
    if ref is None: continue
    mism = []
    for track, langs in (("native", NATIVE), ("wasm", WASM)):
        for l in langs:
            e = get(track, l, w, "exit")
            if e is not None and e != ref:
                mism.append(f"{LABEL[l]}={e}"); ok = False
    status = "✓ all match" if not mism else "✗ MISMATCH: " + ", ".join(mism)
    gate_lines.append(f"- `{w}`: checksum = {ref} (exit code) — {status}")

def ms(lang, w, track="native"):
    return get(track, lang, w, "time_ms") or "?"

# Which workloads each compiled language sits out (so the `binaries built` column explains itself).
def skipped(track, lang):
    return [w for w in WORKLOADS if get(track, lang, w, "time_ms") is None]
_skips = [f"{LABEL[l]} skips " + ", ".join(f"`{w}`" for w in skipped("native", l))
          for l in ("kama", "c", "cpp", "rust", "go") if skipped("native", l)]
SKIP_NOTE = ("; " + "; ".join(_skips)) if _skips else ""

env = _saved["env"] if FROM_JSON else {
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

md = f"""# kama benchmark results

_Generated: {env['generated']} · arch: {env['arch']} ({env['kernel']}) · in the `kama-bench` container_

Toolchains: clang `{env['clang']}` · {env['rustc']} · {env['go']} · dotnet {env['dotnet']} · {env['java']} · node {env['node']} · {env['lua']} · {env['python']}
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

{chr(10).join(gate_lines)}

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
  `map`/`math`: its `tsconfig.json` compiles only fib/pi/collatz/dispatch/alloc/fnptr.)
- **map** — the hash-map **library-design** row: the same workload, but each language uses its *idiomatic*
  map — kama `Map<int32, int64>`, C++ `unordered_map`, Rust `HashMap`, Go `map`, C# `Dictionary`, Java
  `HashMap` (boxed), Lua table, Python `dict`, JS `Map`. **⚠️ This measures map DESIGN, not codegen** —
  hash strength, growth policy, and layout all differ, so the languages legitimately do not cluster.
  **C is absent by design, not by omission: C has no stdlib hashmap.** Its hand-rolled map is what
  `map_kernel` runs, in every language at once; entering that bespoke, preallocated structure here and
  ranking it against everyone else's general-purpose library maps would read as a codegen win when the
  actual finding is the trivial "a purpose-built preallocated map beats a general-purpose one".
  Every map that can be pre-sized is (see *Methodology — pre-sizing*), so what remains is hash and
  layout: kama {ms("kama", "map")} ms, C++ `unordered_map` {ms("cpp", "map")} ms, Rust `HashMap` {ms("rust", "map")} ms.
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

## NATIVE — execution time (median, ms)

{time_table("native", NATIVE)}

## NATIVE — peak resident memory (MB)

{rss_table("native", NATIVE)}

## NATIVE — package size

_What you ship: a **self-contained** binary needs no runtime; managed/interpreted rows are the
assembly/source only and additionally require the noted runtime (.NET / JVM / interpreter). The size
column is each language's `{WORKLOADS[0]}` artifact; the range spans every workload it built. C# and
Java ship one multi-workload assembly, so their size is the same everywhere._

{size_table("native", NATIVE)}

## NATIVE — compile time

_Wall-clock to compile that language's bench artifacts (single build, not averaged). The compiled
languages build **one binary per workload** (`binaries built` — {len(WORKLOADS)} workloads{SKIP_NOTE}); C# and Java build **one**
multi-workload binary that dispatches on `args[0]`. kama's figure is transpile-to-C **plus** clang.
Interpreted languages (Lua, Python, JS) have no compile step and are omitted._

{compile_table(["kama", "c", "cpp", "rust", "go", "csharp", "java"])}

## WASM track — execution time under node (median, ms)

{time_table("wasm", WASM)}

## WASM track — peak resident memory (MB)

{rss_table("wasm", WASM)}

## WASM track — module size

_The size column is each language's `{WORKLOADS[0]}` artifact; the range spans every workload it built._

{size_table("wasm", WASM)}

## WASM track — compile time

_kama→wasm is transpile-to-C **plus** `emcc -O3`; TS is `tsc`. Hand-written JS has no compile step._

{compile_table(["kama-wasm", "ts"])}

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
"""

os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
with open(OUT_MD, "w") as f:
    f.write(md)
if not FROM_JSON:
    with open(OUT_JSON, "w") as f:
        json.dump({"env": env, "rows": rows,
                   "compile": {l: {"compile_ms": c, "artifacts": n} for l, (c, n) in COMPILE.items()}},
                  f, indent=2)

print(f"wrote {OUT_MD}")
print("fairness gate:", "PASS" if ok else "FAIL (checksum mismatch — see RESULTS.md)")
sys.exit(0 if ok else 1)
