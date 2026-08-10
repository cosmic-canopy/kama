# Closure pruning — a directory import should not compile the directory

**Status: NEXT. Not started.** Design of record. Delete this file when it ships, moving the record to
`docs/SPEC.md` (module resolution is language surface) and the residual to `docs/ROADMAP.md` §9.

> **Read this first.** Every brief in this repo has been wrong somewhere load-bearing, and this one has
> already been wrong once: it originally asserted that intra-directory imports would drag most of a
> directory in transitively, so resolution-level pruning could not work. That was an assumption, not a
> measurement, and **it is false** (see *The import graph is sparse*). Re-run everything below.

## Why

A **directory module resolves to every `.kama` in the directory**. The `{…}` symbol list controls what
is *visible*, not what is *compiled*. `resolveModuleFiles` ([kama.driver.cpp:410](../kama.driver.cpp))
looks for `<root>/<a>/<b>.kama` and, failing that, takes a flat `listKamaFiles` of `<root>/<a>/<b>/`
([:204](../kama.driver.cpp)).

So `examples/httpd` names four imports and compiles **32 translation units**. Every one is parsed,
analyzed, emitted as C, and handed to a C compiler — in every build, and **analyzed again on every LSP
keystroke**.

### The worked example that makes it concrete

`lib/std/fmt/parse.kama:2` is `import std::num::{int64Min};` — **one constant**. That pulls in all five
files of `lib/std/num/` (`endian`, `fixed`, `limits`, `ops`, `wrapping`). Only `limits.kama` defines
`int64Min`. **All five are dead in httpd**, and the five import nothing at all, so there is no
transitive dependency to blame. 5 files for 1 constant.

## The measurements (2026-08-10, 10-core M-series, `examples/httpd`)

**20 of the 32 units contribute no live symbol to the binary.** Two populations wanting different
attention:

- **13 near-empty generic TUs** — `deque`, `dynamic_array`, `fixed_array`, `map`, `priority_queue`,
  `set`, `slot_map`, `sort`, `sorted_map`, `sorted_set`, `view`, `ptr`, `stream` — **1 definition
  each**, because a generic materializes only where it is instantiated.
- **7 units of real, entirely dead code** — `fixed` (35 defs), `poll` (20), `ops` (18), `limits` (17),
  `endian` (14), `hasher` (12), `wrapping` (10).

**The dead units cost MORE wall-clock than the live ones**, which is the counterintuitive number this
design turns on:

| httpd, `clang -c` at `-P 10` | wall |
|---|---|
| the **20 dead** TUs | **0.15 s** |
| the **12 live** TUs | 0.12 s |

A near-empty TU is not cheap: it still parses ~1237 lines of `kama_runtime.h` + ~951 of `kama_os.h`
plus the **whole-program** `<stem>.gen.h`. Per-TU cost is dominated by headers, not by the unit.

Against a 0.67 s `-j 10` build of httpd: ~0.15 s recoverable from the C compile, plus a proportional
share of the ~0.34 s front end (all 32 units parsed + analyzed). **Estimate ~0.67 s → ~0.37 s, i.e.
another ~1.8×** on top of what `-j` already gave. ⚠️ The front-end half of that is an *estimate*, not a
measurement — it is the first thing to check (see *Take this measurement first*).

### Reproduce

```sh
d=$(mktemp -d)
./kama build examples/httpd/httpd.kama -o "$d/httpd" --keep-c    # -j leaves the .o next to the .c
clang "$d"/*.o -o "$d/stripped" -Wl,-dead_strip
nm -jU "$d/stripped" | sort -u > "$d/final"
for o in "$d"/*.o; do
  nm -jU "$o" | sort -u > "$d/t"
  [ "$(comm -12 "$d/t" "$d/final" | wc -l)" = 0 ] && echo "DEAD $(basename "$o")"
done
```

Do **not** try to recover kama's flags with `--cc echo` + `sed` for that link — linking prebuilt objects
needs none of them, and the sed mangles `-fsanitize=…` into a bogus filename.

⚠️ **`./kama` may be a Linux binary.** `bench/run` does `make clean && make kama` in the container and
the root symlink follows whichever platform built last; the failure looks like `permission denied`, and
if stderr is redirected it looks like a real empty result. `./dev build` restores it.

## The import graph is sparse — this is what makes the cheap fix viable

The original assumption was that collections files import each other enough that resolving only the
named symbols would still drag the directory in. **Measured, and false:**

| unit | imports |
|---|---|
| `allocator`, `bit_set`, `hasher`, `priority_queue` | *nothing* |
| `view`, `deque`, `map`, `slot_map` | `std::ptr::{relocate}` |
| `dynamic_array` | `std::ptr::{relocate}`, `std::collections::{View}` |
| `fixed_array` | `std::collections::{View}` |
| `set` | `std::collections::{Map, MapKeyIter}` |
| `sort` | `std::collections::{View, DynamicArray}` |
| `sorted_map` | `std::memory::{Owned}` |
| `sorted_set` | `std::collections::{SortedMap, DynamicArray}` |

httpd names `{DynamicArray, FixedArray}`, whose intra-directory closure is `dynamic_array`,
`fixed_array`, `view` — **3 of 14**. And all five `std::num` files import nothing, so that directory
prunes 5 → 1 with no transitive drag whatever.

Reproduce: `for f in lib/std/collections/*.kama; do echo "$f: $(grep -h '^import' "$f")"; done`

## The fork

**A — resolution-level pruning (recommended).** Resolve a directory-module import to the files that
*define the named symbols*, plus their transitive intra-package closure. Needs a symbol→file index for
the directory, which means **parsing** every file in it — but parsing is the cheap part; the win is
that only the needed units are **analyzed, emitted and compiled**. No whole-program reachability, no
change to what a *reachable* unit emits.

**B — post-analysis reachability pruning.** Drop units contributing no reachable symbol after analysis.
Strictly more powerful (it also catches a unit that is legitimately imported but unused), and strictly
more dangerous — it is whole-program, and it interacts with everything in *Traps* below.

**Recommendation: A, and measure before considering B.** A reaches httpd's `std::num` 5→1 and
collections 14→3 on its own. Only take B if A's measured result falls well short of the 20.

## Take this measurement FIRST

The C-compile half is measured (0.15 s of 0.27 s). **The front-end half is not.** Before building
anything, get the front end's share as a function of unit count:

```sh
KAMA_TIMING=1 ./kama check examples/httpd/httpd.kama     # 32 units: closure-parse / prelude-parse / analyze
KAMA_TIMING=1 ./kama check tests/arith.kama              # 1 unit: the fixed floor
```

Then hand-build a variant of httpd that imports only what it uses, and compare. If the front end does
not shrink roughly in proportion to the unit count, the ~1.8× estimate is wrong and this is a
C-compile-only lever worth about half as much. **This estimate has not been checked once; the campaign
it comes from got two estimates wrong by ~4×.**

## Traps — each of these has drawn blood in this repo before

1. **The multi-TU `static` hazard class.** The logging campaign (`adfffce`) fixed process-global
   `static`s — the panic hook, the log sink, argv slots, and `std::process`'s pid park — by defining
   each **once, in the entry TU**. Dropping units changes which TUs exist. Prove with a fixture that a
   pruned build still has exactly one of each, and that `tools/check-panic-multitu.sh` and
   `check-log.sh` still pass. This is the single most likely way to break something silently.
2. **`pruneInactiveDecls` / `CompilationUnit::prunedNames`.** `@compileFor` already removes decls in
   place, and rung 2 of the build-perf campaign found a real defect exactly here (a second emitter over
   an already-pruned unit reported a phantom `export list names 'sort' but there is no such top-level
   declaration`, because `lib/std/collections/sort.kama` both exports and `@compileFor(!NOHEAP)`-gates
   `sort`). Two prunes now interact. `tools/check-batch.sh` is where a new assertion belongs.
3. **Export manifests.** A module's `export` list is checked against its top-level decls. Pruning a
   file changes what a directory module is understood to export.
4. **Observable semantics change: a compile error in an unused sibling stops failing the build.** Today
   a broken `lib/std/collections/deque.kama` fails every program that imports anything from
   `collections`. After pruning it would not. That is arguably an improvement, but it **is** a
   behavioral change, which is why this is a **pre-1.0** item — 1.0 is the API-stability point.
5. **`--release` folds to one unity TU** ([kama.driver.cpp:5909](../kama.driver.cpp)), so pruning
   changes *what is folded in*, not the TU count. Check both build modes.
6. **Packages.** `packageSourceFiles` ([:325](../kama.driver.cpp)) returns a manifest's declared
   `sources` instead of a flat listing. Pruning must compose with that, not bypass it.
7. **Cycles.** `set` → `Map`, `sort` → `View, DynamicArray`. Nothing observed is cyclic, but the
   resolver must not loop if a cycle appears.

## Where the code is

Re-grep before trusting any of these — line refs in this repo's prose drift, and these are from
2026-08-10:

- `listKamaFiles` [:204](../kama.driver.cpp) — the flat directory listing that is the whole problem.
- `packageSourceFiles` [:325](../kama.driver.cpp) — the manifest-declared alternative.
- `resolveModuleFiles` [:410](../kama.driver.cpp) — file-module, else package sources, else flat listing.
- `loadProgramUnits` [:561](../kama.driver.cpp) — the BFS over the import graph; where a symbol index
  and a needed-set would live.

## Staging

1. **Measure the front-end share** (above). Decide whether this is a ~1.8× lever or a ~1.4× one.
2. **Symbol→file index per directory module**, built by parsing the directory. No behavior change yet —
   assert the index finds every symbol every current import names, across the whole corpus.
3. **Prune at resolution**, behind an env escape hatch (`KAMA_NO_PRUNE=1`) so the whole suite can be
   A/B'd on ONE binary — the pattern `KAMA_NO_BATCH` and `KAMA_BUILD_JOBS` both proved.
4. **Guard** (`tools/check-closure-pruning.sh`, glob-enrolled): httpd resolves to N units not 32; the
   process-global singletons still exist exactly once; a program's behavior is identical pruned vs not,
   across the fixture corpus; `--release` unaffected in output.
5. **On by default**, `./dev matrix` green, record the measured delta in ROADMAP §9 and the resolution
   rule in SPEC.

## What this does NOT fix

The LSP's per-keystroke floor is ~85 ms, **86 % of it `CEmitter::analyze`** over the closure plus
prelude. Pruning shrinks the closure, so it should cut that substantially — but the **prelude** share
is a fixed floor pruning cannot touch, and analysis is still re-run from scratch every keystroke. If
the floor is still uncomfortable after this, the remaining fix is incremental/cached *analysis*
(ROADMAP §10), which is a different and much larger project.
