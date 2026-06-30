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
NATIVE = ["cstar", "c", "cpp", "rust", "go", "csharp", "lua", "python"]
WASM = ["cstar-wasm", "js", "ts"]
LABEL = {"cstar": "cstar", "c": "C", "cpp": "C++", "rust": "Rust", "go": "Go",
         "csharp": "C# (JIT)", "lua": "Lua", "python": "Python",
         "cstar-wasm": "cstar→wasm", "js": "JS", "ts": "TS"}

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
    out = ["| workload | " + " | ".join(LABEL[l] for l in langs) + " |",
           "|" + "---|" * (len(langs) + 1)]
    for w in WORKLOADS:
        out.append("| " + w + " | " + " | ".join(cell(get(track, l, w, "time_ms")) for l in langs) + " |")
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
    out = ["| lang | artifact size |", "|---|---|"]
    seen = {}
    for l in langs:
        for w in WORKLOADS:
            v = get(track, l, w, "size_bytes")
            if v and v not in ("0", "NA", None):
                seen[l] = int(v); break
    for l in langs:
        if l in seen:
            kb = seen[l] / 1024.0
            out.append(f"| {LABEL[l]} | {kb:.1f} KB |")
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
    "node": sh("node", "--version"),
    "lua": sh("bash", "-c", "lua5.4 -v 2>&1 | head -1"),
    "python": sh("python3", "--version"),
}

md = f"""# cstar benchmark results

_Generated: {env['generated']} · arch: {env['arch']} ({env['kernel']}) · in the `cstar-bench` container_

Toolchains: clang `{env['clang']}` · {env['rustc']} · {env['go']} · dotnet {env['dotnet']} · node {env['node']} · {env['lua']} · {env['python']}
Timing: `hyperfine --warmup 2 --runs 8 --shell=none` (median). Peak RSS: `/usr/bin/time -v`.

## How to read this (please read before drawing conclusions)

cstar transpiles to C and is compiled by the **same clang** as the C baseline, so on native compute
workloads cstar is expected to be **within measurement noise of C/C++** — that is the design, not a
finding. The signals worth trusting here are:
1. cstar (native) vs **managed/interpreted** languages (C#, Go, Lua, Python),
2. **peak RSS** and **artifact size** (the low-footprint goal),
3. on the WASM track, **cstar→wasm vs hand-written JS/TS** under the same node.

**Methodology — run isolated:** these are short workloads, so **parallel load badly skews them** — run the
bench with nothing else competing for CPU/IO. (We verified the repo bind mount, `/work` via virtiofs/9p on
macOS/Windows, adds only ~0.3–1.7 ms for native *and* wasm under a controlled idle measurement — negligible,
within run-to-run noise — so artifacts are measured in place.) The wasm track is strict IEEE (no
`-ffast-math`), matching the C/Rust/JS baselines.

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

{chr(10).join(gate_lines)}

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

{time_table("native", NATIVE)}

## NATIVE — peak resident memory (MB)

{rss_table("native", NATIVE)}

## NATIVE — artifact size

{size_table("native", NATIVE)}

## WASM track — execution time under node (median, ms)

{time_table("wasm", WASM)}

## WASM track — peak resident memory (MB)

{rss_table("wasm", WASM)}

## WASM track — module size

{size_table("wasm", WASM)}

## Caveats
- **arm64 results** — not comparable to x86 runs (arch recorded above).
- **JIT warmup** (C#, node): mitigated by hyperfine warmups; tiny workloads still partly reflect startup.
- **Container overhead** applies equally to all languages, so relative numbers are fair; absolute numbers
  carry slight overhead.
- **C# AOT** is built best-effort (`bench/build/csharp-aot`); the table shows the JIT runtime.
- Numbers are a snapshot to guide optimization, regenerated by `bench/run all`; do not hand-edit.
"""

os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
with open(OUT_MD, "w") as f:
    f.write(md)
with open(OUT_JSON, "w") as f:
    json.dump({"env": env, "rows": rows}, f, indent=2)

print(f"wrote {OUT_MD}")
print("fairness gate:", "PASS" if ok else "FAIL (checksum mismatch — see RESULTS.md)")
sys.exit(0 if ok else 1)
