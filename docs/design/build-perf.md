# Build & suite performance — campaign brief

**Status: levers 1 and 2 shipped; lever 3's first rung shipped. Lever 3's remaining rungs and lever 4
remain — and lever 4's premise was wrong, see below.** Design of record for the compile-time work.
Delete this file when the last milestone ships, once `docs/ROADMAP.md` §9 carries the residual.

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
| `tools/check-*.sh` guards, **serial**, before the fan-out | **~61 s** | `check-query.sh` 35 s, `check-packages.sh` 11 s, ~21 others ≈ 15 s |
| single-file fixtures (597, parallel at NCPU) | 82 s | kama front end 36 % / clang 64 % |
| analysis agreement (915 `kama check`) | 20 s | pure front end |
| multi-file + xfail | 12 s | |

Reproduce: `./dev test 2>&1 | grep '^phase:'`, and per-guard from the runner's own slowest-8 summary
(`sh tools/run-checks.sh --leg native --all`).

(The guard block was first measured at ~73 s; a careful re-run put it at **61 s**, the difference being
whether `check-no-inheritance.sh`'s build directory was warm. Cold it is a full second compiler build.)

## The four levers, cheapest first

### 1. The guards run twice — `./dev matrix` ✅ SHIPPED

`run_tests.sh` ran **22** guards inline; `./dev check` then ran **all 24** again via its glob, so
`./dev matrix` paid the whole block twice. `./dev matrix` now sets `KAMA_SKIP_CHECKS=1` around its
`test all`, and `run_tests.sh` honors it. A bare `./run_tests.sh` (CI, container) never sets it and
stays complete.

The variable reaches the native leg and *not* the container legs, because `tools/cdev` does not forward
host env — which is what we want: `check-argv-env` under `KAMA_SAN` builds the probe *with* sanitizers,
a different assertion from the one `./dev check` makes on the host. That is commented at the site, so it
does not get "tidied up".

**Measured: −61 s from `./dev matrix`.**

### 2. The guards run serially, on one core ✅ SHIPPED

They are independent scripts, and nine cores idled for 61 s — in a block that runs *before* the fixture
fan-out, so nothing else was using them either. There is now one runner, `tools/run-checks.sh`, driving
the guards for both `./dev check` and `run_tests.sh`.

**Measured: 61 s → 38 s** (79 s summed across guards / 38 s wall = 2.1× — individual guards get slower
under contention). The floor is `check-query.sh` at 36.8 s, i.e. *the block is now that one guard*. Only
lever 3 moves it further.

Two properties worth keeping:

- **The glob is the list.** The guard set used to exist twice — a glob in `./dev`, 22 hand-written
  `if` blocks in `run_tests.sh` — and had already drifted: `check-agents.sh` was in one and not the
  other, with no comment saying why. Per-guard policy now lives in the guard's own header
  (`# check-legs: native san`, default `native`; `# check-heavy: yes`), so enrollment stays automatic.
  `check-agents.sh` joined the suite as a result; the native headline is +1.
- **Output is buffered and printed in glob order**, so a parallel run is byte-diffable against a serial
  one. It was, exactly, on the first run.

Collisions found and neutralized before fanning out: `check-no-inheritance.sh` runs `make` from `$ROOT`
and repoints `./kama` for the duration (→ `# check-heavy: yes`, runs alone after the pool drains);
`check-mcu.sh` / `check-softfloat.sh` resolved `$ROOT/kama` directly and would have silently tested the
wrong binary (→ `tools/kama-bin.sh`, and `mcu/build.sh` now honors an exported `$KAMA`);
`check-syntax.sh` wrote `tests/syntax/.snapout` into the worktree (→ private `mktemp -d`). Every guard
also gets a private `KAMA_STORE`.

### 2½. `make` was serial ✅ SHIPPED (not in the original brief)

`build_host() { make; }` on a 10-core box. A from-scratch compiler build is **8 s serial → 3 s at
`-j10`**. Verified safe by building both ways and comparing objects: every `.o` is byte-identical, so
the dependency graph is complete, including the generated bison/flex sources. Note the *binary* hash
still varies run to run — that is the macOS linker, not parallelism; two serial builds differ from each
other too, so a binary hash cannot be used to check this.

Small in absolute terms (incremental builds, the common case, were already near-zero), but it is free
and it also covers `check-no-inheritance.sh`'s second compiler.

### 3. Cache the front end — the broadest lever

Every invocation re-parses and re-analyzes the prelude **and every imported `std::` tree**, used or not.

    KAMA_TIMING=1 ./kama check tests/parse_radix.kama
    # closure-parse=137ms prelude-parse=28ms analyze=81ms total=246ms

So **parse ≈ 2/3, analyze ≈ 1/3** for a std-heavy fixture. This lever reaches further than any other:

- ~**100 %** of the 20 s agreement phase (915 × `kama check`)
- ~**36 %** of the 82 s fixture phase
- nearly all of `check-query.sh`'s 36 s. **Now the whole guard block**, since lever 2 parallelized
  everything around it. *(Superseded — rung 1 below took this to 6 s and the guard block to 14 s. The
  figures in this bullet are the pre-rung-1 state, kept because the reasoning still explains the shape.)*
  Re-measured: **259 `kama query` processes over ~15 distinct programs** — not one
  file, as an earlier draft of this brief said. But the ratio is what matters, and one fixture dominates:
  `tests/query/complete.kama` is 98 assertions × ~210 ms ≈ **20.6 s**, of which ~63 % is re-parsing the
  same 16-unit `std` import closure. On top of that a ~28-38 ms prelude-parse floor is paid 259 times
  ≈ 8 s. Still the single most cacheable workload in the tree.
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

### Lever 3, rung 1 — the multi-query invocation ✅ SHIPPED

Everything after `idx.analyze(units)` is a pure read off the built index — 0.03-1.33 ms against a 210 ms
analysis. The only thing preventing one process from answering many questions was that every mode was its
own scalar and the dispatch if-chain `return`ed in every arm, so `--def 1:1 --type 2:2` silently answered
`--def` and dropped the rest. Now: an ordered `std::vector<Question>` filled in argv order, and one
`answerOne` lambda over it.

**Measured, back to back on one host** (baseline via `git stash`, so both legs built from source the same
way): `check-query.sh` **20.9 s → 6.0 s** standalone, the guard block **23 s → 14 s**, `./dev test`
**158 s → 140 s**. 971 pass / 0 fail on both.

Attribute the **guard block** delta to this change and nothing else. The same pair also showed the
fixture phase move 88 s → 81 s, which this change cannot touch — that is run-to-run variance, and it is
the reason the totals above should be read as "≈ −10 s, plus noise", not as −18 s.

Three properties worth keeping:

- **A single question is byte-identical to before** — no delimiter, no `ask` key, same exit codes. Checked
  by diffing every mode × fixture × `--json` combination against a stashed pre-change binary (48 on the
  final tree, 80 in a wider sweep): stdout, stderr and exit code identical throughout. The one difference
  anywhere in the surface is the usage text, which gained a sentence on purpose. Run the sweep under
  `sh -c`, not zsh — see *Measuring, correctly*; a zsh version of it reported 48 false diffs.
- **The guard derives its question list by parsing itself** (`preload`), rather than carrying a hand-written
  list that would drift silently. The scan is conservative and a miss falls through to the old one-process
  path, so **drift costs time, never correctness**. `CHECK_QUERY_NO_PRELOAD=1` disables it, and the
  invariant — preloaded output byte-identical to solo output — is what proves the batch answers the same.
- **All coordinates are validated before any answer is printed.** A partial batch that exits nonzero is
  worse than no batch: a caller checking `$?` has already consumed output that looks complete.

The guard block's floor is now `check-packages.sh` at 11.5 s, not `check-query.sh`. Anything further from
lever 3 has to come from the rungs below.

**Still open, in order:** batch mode (N programs per process — the 20 s agreement phase) then the on-disk
serialized symbol table (the fixture phase's front end, the LSP floor, user projects). Both need the
"resettable emitter" work; neither is committed. Precedent for a batched harness:
`check-lsp.sh` drives its whole assertion set against one `kama lsp` process.

### 4. Object caching — ⚠️ THE PREMISE BELOW IS WRONG. Read this first.

Everything in this section rests on "455 emitted TUs, 49 distinct — 89 % byte-identical duplicates". That
number does not survive contact with the tree, and the design built on it does not either. Verified
2026-08-09 by reading the emitter, not by re-running the sample:

- **Every emitted module `.c` has `#include "<output-stem>.gen.h"` as its second line**
  (`kama.cemit.cpp:18103`, stem from `kama.driver.cpp:5876`), and the include guard carries the stem too
  (`kama.cemit.cpp:18089`). Two fixtures built to different `-o` names therefore never emit identical bytes. The 89 %
  is only reproducible if every fixture in the sample was built to the *same* `-o` basename — which is
  almost certainly what happened, and is a measurement artifact, not a property of kama.
- **Worse, that header is whole-program.** `tests/query/complete.gen.h` is 3160 lines and declares the
  user's own types *and every monomorphized instantiation* (`Optional_completeprobe__Cell`). A stdlib
  module's TU is program-dependent **by construction**, so a *sound* cache key — content plus every
  header the compile read — gets **zero** cross-fixture hits. Guaranteed, not merely unlikely. Renaming
  the header to something fixed would restore byte-identity of the `.c` and change nothing about this:
  the header *contents* still differ, so the key still misses. That fixes the measurement, not the cache.
- **`--release` never takes the multi-file path at all** (`kama.driver.cpp:5862` folds every unit into
  one unity TU), so a per-TU object cache can only ever touch debug and wasm builds.

So the suite-speed case for this lever is gone. What survives is real but different, and belongs on a
different timeline:

| option | delivers | costs |
|---|---|---|
| **A** — the cache as designed below | cross-**run** hits (a second `./dev test` skips clang) + user-project incremental rebuilds for body-only edits | run 1 is ~42 % slower per-TU, so **cold CI gets slower**; any new declaration changes `gen.h` and invalidates every TU |
| **B** — split `gen.h` into per-module headers first | genuine cross-fixture *and* cross-project reuse | changes what kama **emits**; and a generic module's TU carries per-program instantiations, so the ceiling is only the **non-generic** slice of the stdlib |
| **C** — re-open as a 1.x "incremental rebuilds" feature | the honest framing: `kama build` recompiles the whole import closure every time, and that is a product gap | not a suite-speed lever, so it leaves the 81 s fixture phase alone |

**The one cheap measurement that would settle B**: count how many TUs in a typical closure contain no
monomorphized symbol. That is the hard ceiling on what per-module headers could ever share. Take it before
costing B.

Decision deferred to after lever 3, deliberately. The rest of this section is the original design, kept
because its *mechanics* (compile/link split, `-MD` direct-mode key, `-fdebug-prefix-map`, the guard) are
all still correct and still needed by whichever option is chosen — only its motivating number is void.

### 4 (original). Object caching — the clang 64 %

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

### Lever 4, re-measured 2026-08-09 — the numbers that decide the design

`tests/parse_radix.kama`, 24 TUs, replaying the real captured command (prepend the compiler: `--cc echo`
prints the *arguments*, not the program):

| | ms |
|---|---|
| today: one clang invocation, 24 sources | **530** |
| 24 separate `clang -c` (a fully cold cache) | **750** |
| link 24 already-built `.o` (a fully warm cache) | **34** |

Duplication re-confirmed on 40 fixtures: **276 TUs emitted, 31 distinct — 89 %**, matching the original
sample exactly. So a warm build is a 34 ms link plus a handful of compiles, against 530 ms today. Across
the 597-fixture phase that is most of its ~520 clang core-seconds.

**Ruled out: dropping `-g`.** It is on by default in every emitted build, and the obvious guess was that
debug info was a big slice. It is **7 ms of 530 ms** (1.3 %). The emitted C is simple enough that debug
info is nearly free. Don't re-chase it.

**The crux is cache-key soundness, and it is the whole design.** The compile flags include
`-I<runtimeDir> -I<dirname(input)> -I. -I<headerDir>`. The first three are constant across the entire
suite; **`headerDir` is the per-build output directory**, holding the shared generated header. So a key
over "`.c` content + flag string" gets *zero* cross-fixture hits until `headerDir` is handled, and a key
that simply drops the `-I` paths is unsound — it would reuse an object compiled against different
headers. This is exactly the problem ccache solves with its direct mode (hash the source plus every
header a previous `-MD` run reported).

The fork was settled with the user, 2026-08-09: **kama gets its own cache.** Delegating to `ccache` was
the cheap alternative — mature, correct, ~60 lines — but it leaves two build paths forever (cold per-TU
is 42 % slower, so the split cannot be the default without a cache present), needs ccache added to the
container image and CI, and gives kama's own users nothing unless they install it. kama's cache is more
code and carries the whole stale-hit risk, but it needs no dependency and it is the same machinery that
delivers **user-project incremental rebuilds** — which is half of what this lever is for. Today
`kama build` recompiles the entire import closure every time, because no object file ever exists.

**Build it in three steps, so the risk is staged and each step is falsifiable on its own:**

1. **Split compile from link.** `kama.driver.cpp` builds one `cmd` stream with the sources in the
   *middle* (`:5891`), so split it there into compile-flags and link-tail. `outStatic` (`:5958`) already
   demonstrates the per-TU loop and the archive. Keep today's single invocation as the default — this
   step changes no behavior and is verifiable by itself.
2. **The cache, behind a flag, off by default.** Key: `.c` content + the contents of every header the
   compile actually read (pass `-MD` on a miss — the compiler hands the list back as a byproduct — and
   remember it in a manifest) + normalized flags + compiler identity + a cache-format version. Turn it
   on for the suite, measure the fixture phase end to end. **The 34 ms is a microbenchmark; do not
   report a suite number until a suite run produces one.**
3. **On by default**, once `tools/check-objcache.sh` proves it: build cold, build warm, byte-compare the
   objects. Plus an escape hatch (`KAMA_NO_OBJ_CACHE=1`) and a way to clear it.

Note the objects are the thing to compare, not the binary — the macOS linker makes the binary hash vary
between two *identical* serial builds (found while measuring `make -j`).

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

**Overlapping the guard pool with the fixture fan-out** (share `gate`/`JOB_CAP` instead of running the
guards to completion first). Modelled at ~25 s. Declined for now, on two grounds: it would make the
`phase:` lines — the instrument this whole campaign steers by — meaningless right before the two levers
that most need careful measurement, and lever 3 probably deletes the win by shrinking the block to
~10 s. `tools/run-checks.sh` already buffers results and prints at one point, which is the structure
this would need, so declining costs nothing. Re-ask after lever 3.

## Suggested order

1. ~~Lever 1 (stop double-running the guards)~~ ✅ −61 s from `./dev matrix`.
2. ~~Lever 2 (parallelize the guards)~~ ✅ 61 s → 38 s. Plus `make -j`: cold build 8 s → 3 s.
3. **Lever 3 (front-end reuse)** — rung 1 ✅ shipped (multi-query invocation: `check-query` 20.9 s → 6.0 s,
   guard block 24 s → 13 s). Rungs left: batch mode (N programs per process → the 20 s agreement phase),
   then the on-disk symbol-table cache (~36 % of the fixture phase, the LSP's per-keystroke floor, user
   projects). Neither is committed — price them against the numbers below, not against the old estimates.
4. **Lever 4 (compile-to-object + `.o` cache)** — ⚠️ **its 89 %-dedup premise is void**; see the warning
   at the head of §4. Re-decide after lever 3, from the three options recorded there.

Re-run `./dev test 2>&1 | grep '^phase:'` after each and record the delta here.

Measured on a 10-core M-series host, 2026-08-09:

| | `./dev test` | guard block | native leg + `./dev check` |
|---|---|---|---|
| baseline | 184 s (970 pass) | 61 s serial | 184 + 61 = **245 s** |
| levers 1 + 2 + `make -j` | 162 s (971 pass) | 38 s parallel | 128 s (guards skipped) + 40 s = **168 s** |
| + check-query memoized | 148 s | 23 s | ~153 s |
| + lever 3 rung 1 (multi-query) | **140 s** (971 pass) | **14 s** | — |

The last row was measured back to back against its own baseline (`git stash`, rebuild, run): **158 s /
23 s** before, **140 s / 14 s** after. The 158 s does not match the 148 s recorded a row above for what
should be the same tree — which is the honest scale of run-to-run variance here, and the reason to trust
the **guard-block** column (a repeatable −9 s) over the total.

Phases now: guards 14 s · fixtures 81 s · agreement 20 s · multi-file+xfail 11 s.

**The guard block is no longer `check-query`.** Its floor is `check-packages.sh` at 11.5 s, so there is
~1.5 s left in that phase and no reason to keep aiming at it. What remains worth attacking is the 81 s
fixture phase and the 20 s agreement phase — and note the 81 s is where lever 4's premise just collapsed,
which makes lever 3's remaining rungs the only funded route to either.

`971` rather than `970` because `check-agents.sh` joined the suite — it was in `./dev check`'s glob but
not in `run_tests.sh`'s hand-written list, so no CI leg had ever run it.
