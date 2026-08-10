# Closure pruning — a directory import should not compile the directory

**Status: NEXT. Step 1 (measure) DONE; steps 2-5 not started.** Design of record. Delete this file when
it ships, moving the record to `docs/SPEC.md` (module resolution is language surface) and the residual
to `docs/ROADMAP.md` §9.

**The campaign is GO: measured at 40.5 % off an `examples/httpd` build**, against a decision rule of
≥ 25 % fixed before the number was taken. Start at *Staging* step 2.

> **Read this first.** Every brief in this repo has been wrong somewhere load-bearing, and this one has
> now been wrong **four times** — three of them corrected on 2026-08-10 by actually running the Step-1
> measurement it demanded. Every claim below has been re-taken against a compiler built at `-O2`
> (ROADMAP §9 lever 7); anything you find quoted from before that date was measured against an
> unoptimized compiler and is wrong by roughly 8× on the front end. Re-run everything anyway.

## Why

A **directory module resolves to every `.kama` in the directory**. The `{…}` symbol list controls what
is *visible*, not what is *compiled*. `resolveModuleFiles` ([kama.driver.cpp:410](../../kama.driver.cpp))
looks for `<root>/<a>/<b>.kama` and, failing that, takes a flat `listKamaFiles` of `<root>/<a>/<b>/`
([:204](../../kama.driver.cpp)).

So `examples/httpd` names four imports and compiles **32 translation units**. Every one is parsed,
analyzed, emitted as C, and handed to a C compiler — in every build, and **analyzed again on every LSP
keystroke**.

### The worked example that makes it concrete

`lib/std/fmt/parse.kama:2` is `import std::num::{int64Min};` — **one constant**. That pulls in all five
files of `lib/std/num/` (`endian`, `fixed`, `limits`, `ops`, `wrapping`). Only `limits.kama` defines
`int64Min`. **All five are dead in httpd**, and the five import nothing at all, so there is no
transitive dependency to blame. 5 files for 1 constant.

## The measurements (2026-08-10, 10-core M-series, `examples/httpd`, compiler at `-O2`)

**The number that decides the campaign.** Hand-prune a copy of `lib/std` inside a fake install tree
(`<d>/bin/kama` + `<d>/lib/kama/std`, which is what `resolveStdlibDir` looks for) and build httpd
against it. No compiler changes needed:

| httpd | full (32 TU) | pruned (17 TU) | saving |
|---|---|---|---|
| `kama build -j 10` | 0.37 s | **0.22 s** | **40.5 %** |
| `kama build -j 1` | 0.98 s | 0.55 s | 44 % |
| `kama check` front end | 41.1 ms | 27.1 ms | 34 % |

Clears the ≥ 25 % bar this campaign was gated on. The 15 units dropped are `collections/{deque, map,
priority_queue, set, slot_map, sort, sorted_map, sorted_set, hasher, bit_set}`, `num/{endian, fixed,
ops, wrapping}`, `net/poll`.

**20 of the 32 units contribute no live symbol to the binary**, but only 15 are reachable by
resolution-level pruning — see *What resolution-level pruning cannot reach* below. Two populations
wanting different attention:

- **13 near-empty generic TUs** — `deque`, `dynamic_array`, `fixed_array`, `map`, `priority_queue`,
  `set`, `slot_map`, `sort`, `sorted_map`, `sorted_set`, `view`, `ptr`, `stream` — **1 definition
  each**, because a generic materializes only where it is instantiated.
- **7 units of real, entirely dead code** — `fixed` (35 defs), `poll` (20), `ops` (18), `limits` (17),
  `endian` (14), `hasher` (12), `wrapping` (10).

**The dead units cost MORE wall-clock than the live ones**, which is the counterintuitive number this
design turns on:

| httpd, `clang -c` at `-j 10` | wall |
|---|---|
| the **20 dead** TUs | **0.15 s** |
| the **12 live** TUs | 0.12 s |

A near-empty TU is not cheap: it still parses ~1237 lines of `kama_runtime.h` + ~951 of `kama_os.h`
plus the **whole-program** `<stem>.gen.h`. Per-TU cost is dominated by headers, not by the unit. This
is also why the C compile is now the dominant term in a build: `-O2` on the compiler cut the front end
8× and left these untouched.

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

## What resolution-level pruning cannot reach

**5 of the 20 dead units are dead but not droppable.** `dynamic_array`, `fixed_array`, `view`, `ptr`,
`stream` emit no live symbol, yet httpd *genuinely imports* their symbols — they are near-empty
precisely because a generic materializes at its instantiation site, in the importing TU. No
resolution-level scheme can drop a file whose symbol was named. Reaching those 5 needs the
whole-program reachability of option B, and they are the cheap population anyway (1 definition each).
Budget for **32 → 17**, not 32 → 12.

## The import graph is sparse — but the import graph is NOT the closure

⚠️ **This section was the brief's third fatal error and is the one most likely to bite an
implementation.** The table below is real, and it is *not sufficient*:
`lib/std/collections/priority_queue.kama` appears here as importing *nothing*, yet its very first field
is `DynamicArray<T, A> data;`. Unqualified names resolve against the file's **own namespace**,
program-wide, with no `import` required
([kama.cemit.cpp `resolveFuncImpl`](../../kama.cemit.cpp)) — so directory siblings, which share one
namespace, reference each other implicitly. `map.kama`'s `H: Hasher = DefaultHasher` is the same thing.

**A closure computed from `import` edges alone under-computes and will emit calls to undefined
functions.** The needed-set must also be seeded from *top-level names a kept file textually
references*, iterated to a fixpoint. Over-pulling there is harmless (less pruning); under-pulling is a
broken build.

Second half of the same defect: `provided` in `loadProgramUnits` is keyed by **namespace**, not symbol,
so a self-import (`lib/std/net/udp.kama:5` does `import std::net::{SocketAddr, RecvFrom}` from *inside*
`std::net`) is short-circuited today only because the whole directory loads at once. Under pruning that
skip silently means "do not load the file defining `SocketAddr`". Make it per-symbol.

With that caveat stated, the import edges themselves:

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

httpd names `{DynamicArray, FixedArray}`. By *import edges alone* that closes over `dynamic_array`,
`fixed_array`, `view` — 3 of 14. The measured prune above kept **4** (`allocator` as well), and all five
`std::num` files import nothing, so that directory goes 5 → 1 with no transitive drag whatever.

⚠️ The gap between "3 by import edges" and "4 as measured" is the reference-closure point above, in
miniature — do not treat the 3 as the answer. Whether `allocator` is *strictly* required was not
isolated (the measurement deleted a fixed 15-file set and confirmed the result compiles and runs); a
correct implementation derives it rather than copying this list.

Reproduce: `for f in lib/std/collections/*.kama; do echo "$f: $(grep -h '^import' "$f")"; done`

## The fork

**A — resolution-level pruning (recommended, and now measured).** Resolve a directory-module import to
the files that *define the named symbols*, plus their transitive intra-directory closure — following
both `import` edges **and** textual references to sibling top-level names (see the warning above).
Needs a symbol→file index for the directory, which means **parsing** every file in it. No whole-program
reachability, no change to what a *reachable* unit emits.

**B — post-analysis reachability pruning.** Drop units contributing no reachable symbol after analysis.
Strictly more powerful — it is the only thing that reaches the 5 named-but-dead generic TUs — and
strictly more dangerous: whole-program, and it interacts with everything in *Traps* below.

**Recommendation: A. B is not worth it.** A is measured at 40.5 % off an httpd build. The 5 units B
would add beyond A are the *cheapest* population (1 definition each), so B buys little for a large
increase in risk.

**On "parsing every file in the directory is wasteful" — it was a real objection and it is now dead.**
At `-O0`, `closure-parse` was 54 % of the front end and indexing-by-parse would have thrown the larger
half of the win away. After ROADMAP §9 lever 7 the *entire* front end is 41 ms of a 370 ms build and
the parse share is 13.7 ms, so parsing the whole directory and pruning before analyze/emit/compile
costs ~8 ms and keeps essentially all of the 40.5 %. **Index by full parse.** Do not build a separate
lightweight declaration scanner to avoid those 8 ms — a second name-extraction path that can drift from
the real parser is exactly the kind of complexity `docs/GOALS.md` says to refuse.

## The Step-1 measurement — TAKEN, 2026-08-10

This section used to say "take this measurement FIRST" and warn that the front-end half of the estimate
had never been checked. It has now been taken, and it found something bigger than this campaign:
**the compiler was built with no `-O` flag at all.** See ROADMAP §9 lever 7. The consequences for this
brief are recorded above — the headline is now a *measured* 40.5 %, not an estimated ~1.8×.

The commands, kept because they are still how you re-take it:

```sh
KAMA_TIMING=1 ./kama check examples/httpd/httpd.kama     # 32 units: closure-parse / prelude-parse / analyze
KAMA_TIMING=1 ./kama check tests/arith.kama              # 1 unit: the fixed floor
```

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
5. **`--release` folds to one unity TU** ([kama.driver.cpp:5909](../../kama.driver.cpp)), so pruning
   changes *what is folded in*, not the TU count. Check both build modes.
6. **Packages.** `packageSourceFiles` ([:325](../../kama.driver.cpp)) returns a manifest's declared
   `sources` instead of a flat listing. Pruning must compose with that, not bypass it.
7. **Cycles.** `set` → `Map`, `sort` → `View, DynamicArray`. Nothing observed is cyclic, but the
   resolver must not loop if a cycle appears — and the reference-driven closure above makes a cycle far
   more likely than the import graph alone suggests. Fixpoint with a visited set, not recursion.
8. **`emitIncludes` and module emission must see the SAME unit list.** `emitProgram` calls
   `emitIncludes(units)` before iterating the same vector for `emitModuleContent`
   ([kama.cemit.cpp](../../kama.cemit.cpp)), and `externsHeader(...)` — which gates whether
   `kama_log_slot` and the `std::process` pid park get *defined* in the entry TU — reads what
   `emitIncludes` populated. Prune *between* those two points and you emit a definition with no
   `#include`, or a reference with no definition. Prune before `emitProgram`, never inside it.
9. **The index must be built from the RAW parse, before `pruneInactiveDecls`.** Resolution happens at
   parse time; `@compileFor` pruning happens later, in `collectProgram`. If the index filters on
   `@compileFor`, then `--no-heap` + `import std::collections::{sort}` resolves to *no file* and the
   user gets `cannot resolve module 'std::collections'` instead of the tailored "not available in this
   build configuration" diagnostic.

## Where the code is

Re-grep before trusting any of these — line refs in this repo's prose drift, and these are from
2026-08-10:

- `listKamaFiles` [:204](../../kama.driver.cpp) — the flat directory listing that is the whole problem.
- `packageSourceFiles` [:325](../../kama.driver.cpp) — the manifest-declared alternative.
- `resolveModuleFiles` [:410](../../kama.driver.cpp) — file-module, else package sources, else flat listing.
- `loadProgramUnits` [:561](../../kama.driver.cpp) — the BFS over the import graph; where a symbol index
  and a needed-set would live.

## Staging

1. ~~Measure the front-end share.~~ **DONE** — 40.5 % off an httpd build, see above. Start at step 2.
2. **Symbol→file index per directory module**, built by parsing the directory (index the raw parse —
   trap 9). It must map **every top-level declared name**, not just the `export` list, because siblings
   reach each other's unexported names through the shared namespace. No behavior change yet — assert
   the index finds every symbol every current import names, across the whole corpus.
2½. **The reference-driven closure.** Fixpoint: seed with the files defining the named symbols; repeat
   pulling in any sibling defining a top-level name a kept file references, plus `import` edges, until
   stable. Validate against the corpus BEFORE wiring it to resolution — for every current program, the
   computed needed-set must be a superset of what is actually required. `examples/httpd` is the
   reference case: it must land on **17** units.
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
