# Build & suite performance — campaign brief

**Status: not started.** Design of record for the compile-time work. Delete this file when the last
milestone ships, once `docs/ROADMAP.md` §9 carries the residual.

> **Read this first.** Every brief in this repo has been wrong somewhere load-bearing, and this one was
> written after being wrong three times in one session. **Re-run the measurement before building on it.**
> Every number below has a reproduction command; if one disagrees with what you observe, trust yourself
> and fix the doc. The specific traps that produced wrong numbers here are in *Measuring, correctly*.

## Why

`./dev test` is **187 s** on a 10-core M-series host for 970 tests. Nothing about that is one problem;
it is four, in different places, and the two cheapest are not compiler work at all.

## The measured breakdown

Native leg, 187 s total (`./dev test`; phase lines are printed by the harness since `69c1123`):

| phase | wall | what dominates it |
|---|---|---|
| `tools/check-*.sh` guards, **serial**, before the fan-out | **~73 s** | `check-query.sh` 36 s, `check-packages.sh` 11 s, ~20 others ≈ 15 s |
| single-file fixtures (597, parallel at NCPU) | 82 s | kama front end 36 % / clang 64 % |
| analysis agreement (915 `kama check`) | 20 s | pure front end |
| multi-file + xfail | 12 s | |

Reproduce: `./dev test 2>&1 | grep '^phase:'`, and per-guard with
`for g in tools/check-*.sh; do /usr/bin/time -p sh $g; done`.

## The four levers, cheapest first

### 1. The guards run twice — `./dev matrix` (harness only)

`run_tests.sh` runs **23** guards inline; `./dev check` then runs **all 24** again via its glob. `./dev
matrix` is `test` + `check`, so that whole ~73 s block is paid twice.

Careful: the inline copies exist so a bare `./run_tests.sh` (CI, container) still covers them. Don't
just delete them — gate them, so `./dev test` can skip what `./dev check` is about to do, while a bare
run stays complete.

    grep -oE "tools/check-[a-z-]+\.sh" run_tests.sh | sort -u | wc -l   # 23
    ls tools/check-*.sh | wc -l                                          # 24

### 2. The guards run serially, on one core (harness only)

They are independent scripts; nine cores idle for 73 s. Parallelizing bounds the block at its slowest
member rather than their sum — ~73 s → ~36 s, limited by `check-query.sh` until lever 3 lands.

Watch for: some guards build the compiler or write to shared temp paths. Check for collisions before
fanning out.

### 3. Cache the front end — the broadest lever

Every invocation re-parses and re-analyzes the prelude **and every imported `std::` tree**, used or not.

    KAMA_TIMING=1 ./kama check tests/parse_radix.kama
    # closure-parse=137ms prelude-parse=28ms analyze=81ms total=246ms

So **parse ≈ 2/3, analyze ≈ 1/3** for a std-heavy fixture. This lever reaches further than any other:

- ~**100 %** of the 20 s agreement phase (915 × `kama check`)
- ~**36 %** of the 82 s fixture phase
- nearly all of `check-query.sh`'s 36 s — **233 assertions, each a separate full analysis of the same
  one file.** The single most cacheable workload in the tree.
- the **LSP's** fixed per-keystroke floor (ROADMAP §10) — nothing else touches this
- **user projects**, which pin a toolchain whose `lib/` never changes during development

**Shape: a serialized symbol-table snapshot, NOT a prebuilt object.** kama is whole-program
monomorphizing and `lib/` is generic templates plus `static inline`, so there is nothing to compile
ahead of time — `Map<string,int32>` does not exist until a program instantiates it. Cache the
post-parse, post-collect declaration state. Prior art: Clang PCH, Rust `rmeta`, Swift `.swiftmodule`.

Two things make it real work, both already known:

- **`pruneInactiveDecls` rewrites units in place** per flag configuration. The cached state must be the
  pre-prune, pre-instantiation tables, and the key must carry the **flag universe** — not just the
  toolchain version and a content hash.
- **A stale hit must be impossible, not unlikely.** Content-hash the inputs; never trust an mtime.

**The cheaper rung, worth pricing first** because it needs the same "resettable emitter" work and
nothing else: a **batch mode** — N independent programs in one process, reusing the analyzed front end.
Useless to user projects, but it would collapse the suite's ~1,900 processes per leg, and it is the
natural fix for `check-query.sh` specifically.

### 4. Object caching — the clang 64 %

`kama build` emits **one `.c` per unit** and passes them all to **one** clang invocation with **no
`-c`**, so no object files ever exist and nothing can be cached or shared.

    ./kama build tests/parse_radix.kama -o /tmp/x --cc echo   # 24 .c files, one command, no -c

Yet the duplication across the suite is enormous — 60 random fixtures:

    455 emitted TUs, 49 distinct  →  89 % byte-identical duplicates

(each `std::collections` module emitted *identically* 23 times). Compiling only the distinct ones:
**16.2 s → 3.3 s (4.8×)**.

Unlocking it needs **compile-to-object-then-link** — the machinery already exists in the `outStatic`
path (`clang -c` per TU, then `ar`) — plus a content-addressed `.o` cache keyed on content + flags.
The same change gives **user projects incremental rebuilds**, which are impossible today.

⚠️ **Two traps, both verified:**

- Under `-g`, identical `.c` at *different paths* produce **different** `.o` — debug info embeds the
  path. `-fdebug-prefix-map` makes them byte-identical again (tested: `-g` → differ; no `-g` → identical;
  `-g -fdebug-prefix-map` → identical). Bonus: that also makes builds reproducible, which ROADMAP §10's
  C-naming item separately wants.
- Per-TU compilation is **slower cold** — 455 separate `clang -c` calls cost 16.2 s against ~9.4 s for
  today's one-invocation-per-fixture. The cache is *part of* this change, not a follow-on.

## Deliberately not taken

**Folding a build to a single TU.** The biggest raw number — 24 TUs 0.52 s vs 1 folded TU 0.08 s
(6.7×), producing an identical binary — and `kama transpile` already does exactly this. Rejected
because it changes what kama **emits**, not just how it is compiled, and multi-TU emission is
load-bearing: the logging campaign fixed a whole multi-TU `static` hazard class (`adfffce` — the
process-global panic hook, log sink and argv slots), which a single-TU suite would stop exercising.
Decided with the user, 2026-08-09.

**A precompiled `kama_runtime.h`.** Superseded by lever 4, which eliminates ~89 % of TU compiles
outright rather than making each one cheaper. Keep it in mind for the ~11 % that still compile: it took
24-TU parsing from 0.33 s → 0.10 s in isolation. Note the two PCH measurements *disagreed* (parse-only
3.4× vs full-compile 10 %) and the discrepancy was never resolved — most likely clang silently ignored a
flag-mismatched PCH in the slower run. Re-measure before relying on it.

## Measuring, correctly

Three traps produced wrong numbers in the session that wrote this brief. All three make things look
**faster** than they are:

- **zsh does not word-split unquoted `$FLAGS`.** `clang $F *.c` passes the whole flag string as one
  argument, clang errors on `-std=...`, and the run "takes 0.09 s". Run timing scripts under
  `sh -c`/`bash -c`, or use arrays.
- **`kama build` deletes its generated `.c` unless `--keep-c`.** Replaying a captured clang command
  afterwards compiles nothing and looks 10× faster. Always check the exit code *and* that an output file
  appeared.
- **The Bash tool resets cwd between calls.** A `cd` in one call does not persist; a later `./out.bin`
  runs somewhere else. Keep a measurement in one invocation.

Also: **`$EPOCHREALTIME` is bash 5**, and macOS ships bash 3.2, so per-fixture slowest-N is still
container-only. Phase timing uses `date +%s` and works everywhere.

## A negative result worth not repeating

The concurrency gate genuinely never throttled on macOS (`wait -n` is bash 4.3+; `|| break` let every
job through — 8 jobs against a cap of 2 left 8 running). Fixed in `69c1123`. **It made no difference to
wall-clock: 184 s → 185 s.** It is a correctness fix — an unthrottled suite is a memory-pressure and
timing-sensitivity hazard — not a performance one. Do not re-derive the hypothesis expecting a win.

## Suggested order

1. Lever 1 (stop double-running the guards) — biggest win per line changed, gate only.
2. Lever 2 (parallelize the guards).
3. Lever 3 (front-end cache), starting with the batch-mode rung and `check-query.sh` as its first
   customer.
4. Lever 4 (compile-to-object + `.o` cache).

Re-run `./dev test 2>&1 | grep '^phase:'` after each and record the delta here.
