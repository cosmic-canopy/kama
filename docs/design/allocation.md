# Allocation — one funnel, a replaceable global allocator, allocator-aware errors, a reach-based `--no-heap`

**Status:** §1 (KR-47) SHIPPED at `0.9.347`–`0.9.348`, the recording gap it exposed at `0.9.353`–`0.9.354`,
§5 (`Handle`, KR-51) at `0.9.355`, **sizes that tell the truth** (§2's prerequisite) at `0.9.366`, and **the layout
funnel + an aligned `Allocator`** (KR-61) at `0.9.367`, and **the whole funnel** (§2, KR-48) at `0.9.369` — verified
on Linux, wasm and Windows, and **the replaceable global allocator** (§3, KR-49) at `0.9.377`–`0.9.378`. §4 not
started. Opened 2026-09-12 at `0.9.320` during KR-12 (`std::uuid`), when a
hand-written `Deserializable` had to box an error and that one box turned out to be unaccountable to every
mechanism kama has for memory: no `Allocator` saw it, `--no-heap` rejected it for merely being imported,
and no program could redirect it. This doc is deleted when the rows below ship, as the maintenance rule
for `docs/design/` says.

## Picking this up

This campaign spans several sessions and may move between hosts, so everything a fresh session needs is in
git: this doc and the rows in `docs/ROADMAP.md`. Nothing depends on an assistant's local memory or on a
scratch directory.

**State (2026-09-17, Linux).** `dev` is `origin/dev` (`0.9.365`, the other machine's KR-1 `std::time`) plus this
campaign's UNPUSHED commits — the maintainer pushes, and pulls onto the Windows box to verify KR-48:
- the handoff doc; `0.9.366` (`deallocate` gets the size `allocate` was given: a `__size` vtable slot,
  `sizeof(ptr:)`, a bindable that releases through the box it came from); KR-61/KR-62 filed;
- `0.9.367` (KR-61: `Allocator` carries `align`, the funnel is `kama_alloc(n, align)`/`kama_free(p, n, align)`,
  every emitted site, `kama_runtime.h`, prelude and stdlib moved; the dead concrete smart-pointer macros deleted);
- `0.9.368` (found on the way: a worker spawned from two modules had its trampoline in only one TU);
- `0.9.369` (the OS seam, channel, isolate and app headers moved; `check-alloc-funnel.sh`; `KAMA_ALLOC_CHECK` on
  the san leg). Gate figures are in each commit message.
Still filed: **KR-57** (shadowing a function), **KR-58** (the Windows seam allocates a wide path per file-system
call), **KR-62** (`sizeof` of a generic instance named nowhere else). A new roadmap row takes the `Next id:`
counter at the top of `docs/ROADMAP.md`, and bumps it.

**The order changed on 2026-09-15/16 (maintainer), and KR-39 is no longer next.** Reading the emitter for
KR-39 answered its own question: devirtualizing in emission cannot reach serde, because the slot calls live in
`X__serialize(X*, Serializer* w)` — written once per program, with no backend type in sight — so the row's
premise ("`serializeJsonBuffer` constructs its backend, so the callee is knowable") holds at the ENTRY POINT and
nowhere the proof is needed. It also does not matter yet: no serde path is heap-free even with a proven callee
(every backend ctor allocates a `FixedArray` scratch or frame stacks, readers take an owned buffer, there is no
fixed-buffer writer, and every `Err` boxes). **What answers serde is this campaign's own arc:** after KR-48 every
allocation funnels through `kama_alloc`/`kama_free`, and since `0.9.377` those delegate to a global allocator the
program DECLARES — whose body `--no-heap` can then check like any other code, so a pool over program-owned
storage is provably not the system heap and the whole serde chain (scratch, buffers, error boxes) lands in it.

That settles §3's open question in favour of **the declaration**, not the weak symbol: a declaration has a body
the flag can read, where a weak symbol could only be trusted. It seemed to raise a second question — whether
`@noheap` and `--no-heap` should mean different things — and that one turned out to rest on a misreading; see
item 1 of the *Still open* list below.

**The recording gap (`0.9.353`), found planning KR-39.** The no-heap call graph could not see a call the
COMPILER writes through a vtable, and four holes fell out of that, each with a fixture that built clean at
`0.9.352`: dropping an `Owned`/`Shared`/`Weak` over a contract (the drop was a runtime MACRO, invisible to the
scanner), the same over a virtual base (`__vdrop`'s `__vt->__dtor(self)` was read but skipped as a member),
`kama_free` counting as `@heap` only when `std::process` happened to be imported, and `@noheap ~Base()` never
being enforced on subclass destructors. The fix is by construction, not per site: **a call through a struct
member in the emitted C is an allocation fact** — C has no methods, so it is always a function pointer — and a
call the emitter PROVED from a declaration says so with `KAMA_NOHEAP_SLOT`, which expands to its argument. Sound
by default, and forgetting the marker refuses a legal program instead of passing an illegal one. The six
smart-pointer `_FUNCS` macros moved into the emitter so their bodies are C the scanner reads; a `for value`
contract emits no dispatch at all (its implementations own nothing, so the slot is NULL everywhere).

**First steps next session:**

1. `git fetch && git rebase origin/dev`, `./dev build`. If the rebase brings emitter changes, re-run the matrix
   on the rebased HEAD before building on it.
2. ~~**KR-48 on Windows**~~ — closed 2026-09-17: `./dev test` (2040 fixtures) and `./dev check` (72 guards) green at
   `0.9.369` with no change to the Windows branch of `kama_os.h`. Two guards needed fixing for msys2 (gawk's `-v`
   escapes, no `python3`); the san/wasm legs are the Linux box's and already ran there.
3. ~~**KR-49**~~ — shipped `0.9.377` (`@globalAllocator`) and `0.9.378` (`Shared.adopt`, the `SortedMap` root, and
   the bare-`new` rule). The record is SPEC *Global allocator*; what the probes and the build decided is at the end
   of §3. Filed from it: **KR-65** (three raw-pointer expressions that reach clang) and **KR-66** (reaching the
   instance).
4. **KR-50** after it: prove a serde error under `--no-heap` with a declared pool, then write the per-call
   allocator verdict (§4).
5. **KR-58 on the Windows box, AFTER KR-49/KR-50** (maintainer, 2026-09-17: wait for their results, since a
   declared pool may change what a `@heap` extern means; see §2b "Meets KR-49"). The brief in §2b is otherwise ready:
   decisions 2 and 3 (stack reserve, probe cost) are pure measurement and go first.

**The agreed order** is the top of the NOW table in `docs/ROADMAP.md`: ~~KR-47 reach-based `--no-heap`~~ (shipped) →
~~the recording gap~~ (shipped `0.9.353`, and the `new`-verb gate with it at `0.9.354`) → ~~**KR-51** `Handle`~~
(shipped `0.9.355`) → ~~**KR-48** `kama_alloc`/`kama_free`~~ (shipped `0.9.369`) → ~~**KR-49**
replaceable global allocator~~ (shipped `0.9.378`) → **KR-50** allocator-aware errors → revisit **KR-39**. It was KR-39 that was to
land right after KR-47 "on the same walk", in view of tier 1 of the devirtualization ladder (KR-23); reading the
emitter retired that plan, for the reasons at the top of this doc. One half of the premise survives and is worth
keeping: the proof of "which body does this slot reach" belongs to ONE mechanism, not two. Note that the walk now
runs AFTER emission, over the C text, so it can prove what a slot reaches but cannot rewrite the call —
devirtualization must decide BEFORE emitting. KR-23 and a future KR-39 can share the RULE; they cannot share the
machinery unless the proof moves ahead of emission.

**How the maintainer wants this done** — the constraints, not suggestions:

- **Production grade. Root cause, never a workaround.** A defect found along the way is fixed where it
  lives, in its own commit, with its own `VERSION` bump and a regression fixture that fails on the previous
  compiler, and the commit cites where it was found (`— found building KR-47`). If it is too big for that,
  it becomes a KR row with its reasoning, not a library-side dodge.
- **Consistent principles.** A rule that holds in some positions and not others is the defect — `--no-heap`
  judged by reach for dispatch but per body for a direct allocation is exactly that.
- **Fixtures land RED first**, and a doc claim that something is rejected carries its `tests/xfail/` marker.
- **A design fork against a written plan goes to the maintainer** with the measured cost of each side and a
  recommendation grounded in `docs/GOALS.md`, before code.
- **The user pushes.** Commit on `dev`; do not push. Fetch and rebase onto `origin/dev` at the start of a
  session — other work lands in between.

**Gate per host.** macOS/Linux: `./dev matrix > /tmp/m.log 2>&1` once, then read the file (on Linux the wasm
leg needs emsdk's `emcc` on PATH — `. ~/emsdk/emsdk_env.sh`; a Linux host runs the san/wasm legs natively).
Windows VM: `./dev matrix` cannot pass there (no containers, and it skips the guards when the container leg
fails), so the gate is `./dev test` then `./dev check`, each into its own log, and the san/wasm legs are
reported as not run — see `docs/platforms/windows.md`. Iterate with `./dev fixture <name>…`; it does not run
a `.d` directory, so build one directly (`kama build $(find tests/xfail/<name>.d -name '*.kama' | sort)`).

The decisions that are specific to this campaign, made by the maintainer on
2026-09-12:

- `kama_alloc` and `kama_free` are the ONLY allocation primitives. They delegate to the program's global
  allocator, which defaults to `malloc`/`free` and which a user can replace.
- EVERY allocation site is covered — emitted C, runtime headers, the OS seam, the prelude — so replacing the
  global allocator is complete. The mixed alloc/free families are a defect to remove, not a quirk to document.
- Error boxing is allocator-aware.
- `--no-heap` is reach-based, consistently.
- **No workaround in serde's error path**: a failing `deserialize` returns `Err`; a placeholder `Ok` plus the
  reader's sticky flag, or a boundary net that converts one, were both considered and rejected.

Still open, to be decided and written down — in this order, because each one narrows the next:

1. ~~What `--no-heap` promises~~ — **DECIDED 2026-09-17 by the maintainer, as recommended below.** *One meaning for both spellings: the region, or the program, never reaches the
   SYSTEM heap.* This is not a new rule — it is what both ALREADY mean, and the question as first posed ("`@noheap`
   = never allocates, `--no-heap` = never reaches the system heap; the flag means the first today") was wrong about
   the code. Measured at `0.9.369`: `tests/noheap_new_bump.kama` is a `@noheap fn` that places a `new` and grows a
   `DynamicArray` in a `BumpAllocator`, and builds; SPEC *No-heap subset* makes `GlobalAllocator` the leaf for BOTH,
   and gives the reason — the system `malloc`/`free` "can block on the allocator's lock". The property kama checks
   is *no system heap*, never *no allocation*. Why keep it one meaning:
   - **GOALS #4 and the campaign's own constraint** ("a rule that holds in some positions and not others is the
     defect"): two spellings that differ only in SCOPE — a body, or what the entry points reach — must not also
     differ in MEANING. A split would make `new(allocator: pool)` legal under the flag and illegal in a `@noheap`
     body that the same program reaches, and would break the fixtures that pin today's behaviour.
   - **GOALS #3** ("Arena/pool allocators arrive as library types"): a program-owned pool is the sanctioned
     answer to "no heap", not an evasion of it — and the compiler already PROVES a pool's body is heap-free
     rather than trusting it.
   - **It is what makes the global allocator worth having and needs no exception to build.** KR-49 moved the leaf from
     `GlobalAllocator` to the funnel's DEFAULT implementation. A program that declares `@globalAllocator` over
     storage it owns then has a checked body behind `GlobalAllocator`, so boxes, containers, strings and error
     boxes become legal under both spellings — and one that declares nothing is refused exactly as today.
   - **Determinism is not lost.** The worry behind "never allocates" was an ISR or audio callback. What those
     cannot afford is an unbounded or locking allocator they did not write — the system heap — which stays
     refused. A pool the program declares is the program's code, bounded as the program makes it; its exhaustion
     is `None` from `try new` (GOALS #3d), and an infallible `new` panics as it does today.
   - **"Never allocates, even from a pool" is GENUINELY OPTIONAL, not scheduled** (the AGENTS.md verdict): no
     consumer needs it, because a region that must not allocate at all is written without an allocation, and one
     that must not TOUCH a given pool is written without that pool's handle — both already visible in source. If
     a real consumer appears, it is a separate attribute over the same reach walk, not a second meaning of this
     one.
2. ~~declaration vs weak symbol for the replacement (§3)~~ — **decided: a DECLARATION**, because the flag can
   read its body and check it, where a weak link-time symbol could only be trusted. Recorded in §3.
3. ~~A sized `kama_free(p, n)`~~ — **decided: SIZED** (2026-09-16), recorded in §2.
4. **A per-call error allocator** (§4) — likely UNNECESSARY once 1–2 land: with every allocation funnelled and
   the global allocator replaceable, errors already follow the replacement, and the per-call form is the
   source-breaking option (it changes `Serializable`/`Deserializable`). Decide it last, on evidence.

## What is wanted

1. **`--no-heap` is reach-based, consistently.** The flag proves the PROGRAM, and a program is what its
   roots reach. `0.9.295` made that true for dispatch; direct allocations are still judged per body.
2. **Two allocation primitives, replaceable.** `kama_alloc` and `kama_free` are the only way heap memory is
   obtained or released; `malloc`/`free` are their default implementation, and a program can supply its own.
3. **Every allocation goes through them.** No raw `malloc`/`free` in emitted C, the runtime headers, the OS
   seam or the prelude, so a replacement is complete rather than mostly complete — and the known gaps
   (ungated sites, extern C invisible to `--no-heap`, allocator-ignoring `Shared.adopt` and `SortedMap`
   root, mixed alloc/free families) are closed, not documented.
4. **Errors are allocator-aware.** Boxing an error into `Owned<Error>` draws from an allocator like every
   other box, instead of calling `malloc` directly.
5. **No workaround in serde's error path.** A `deserialize` that fails returns `Err`; it never smuggles a
   failure through the reader's sticky flag with a placeholder `Ok` to dodge an allocation. ✅ `Handle` was the
   one type doing exactly that (KR-51, shipped `0.9.355`): it asks `r.failed()` now, and `serialize` asks
   `w.failed()`, the boundary every generated body already had.

## What is true today (measured by reading, `0.9.318`; behaviour re-verified at `0.9.345`)

Line numbers below are `0.9.318`'s unless marked. The `--no-heap` anchors at `0.9.345`: `rejectIfNoHeap`
`src/kama.cemit.cpp:24512`, `rejectNoHeapIndirect` 24548, `checkNoHeapTransitive` 24679,
`ownedErrorTypeNode` 27351, `emitStickyErrBox` 27398, `emitPrimBoxIntoContract` 30626,
`emitEnumBoxIntoContract` 30644; `kama_alloc`/`kama_free` `include/kama_runtime.h:123`/`126`;
`kama_panic_handler` rt:436. Find a site by its function name, not its number.

**Three families, all libc underneath:**

| family | who uses it | honours `A`? |
|---|---|---|
| the `Allocator` contract / `A` parameter | `Owned`/`Shared`/`Weak` dtors, every container (`DynamicArray`, `Map`, `Deque`, `FixedArray`, `BitSet`, `SlotMap`, `SortedMap` nodes), placement `new(allocator:)` | yes |
| `kama_alloc`/`kama_calloc`/`kama_realloc`/`kama_free` (`include/kama_runtime.h:112-115`, `static inline`, block-scope `extern malloc`) | every `string` operation, `Formatter` growth, `kama_ctrl_new`, graph-serde tables, Windows argv/env/programPath | no |
| raw libc | 19 `malloc` + 3 `free` emitted by `src/kama.cemit.cpp`; ~44 calls in `include/kama_os.h`; `kama_channel.h`, `kama_isolate.h`, `kama_app.h`; `Arena`'s backing store | no |

**The raw emitted sites** (`src/kama.cemit.cpp`): bare `new` in both statement and value position (6389,
6416, 6480, 22289, 22333, 22355), fallible `new` (29111, 29197), `try new` (29288, 29295, 29300, 29368),
primitive boxing into an owning contract (29510), **error boxing into `Owned<Error>` (29527, reached by
every serde `emitStickyErrBox`)**, `parallel_for`/`parallel_spawn` bundles (5639, 5674, 5709), `spawn` and
`isolate` bundles (15186, 15247, 15274), serde `Owned<T>` reads (25664, 25713).

**Mixed families — a DEFECT, not a quirk: correct today only by the accident that all three are libc, and
wrong the moment any one is replaced.** About twenty blocks are allocated by one
family and freed by another. The pattern: a raw-`malloc` box freed by a runtime macro's `kama_free`
(`KAMA_OWNED_IFACE_FUNCS`, rt:162/216), or freed by `~Owned` → `GlobalAllocator.deallocate`. Also: graph
read shells `kama_calloc`'d and later freed through `Shared`'s `A`; `kama_capture2` buffers `realloc`'d in
`kama_os.h` and freed by `kama_free` in `process.kama`; Windows `readDir` names `malloc`'d and handed to a
`kama_string`; `BindableFunctionPtr` freeing with `kama_free` whatever `A` produced the object.

**Allocator ignored where it is named:** `Shared.adopt` gets its control block from a hard-coded
`GlobalAllocator` (`lib/std/memory/shared.kama:59`); the `SortedMap` root is a bare `new` (603, 611).

**`string` has no allocator.** It is the intrinsic `kama_string {data, len, cap}` and always uses the funnel.

**Errors are fixed to the global heap by type:** `ownedErrorTypeNode()` mangles to
`std__memory__Owned_Error_GlobalAllocator`; `Serializable.serialize` (prelude 333) and
`Deserializable.deserialize` (399) name `Owned<Error>`.

**`--no-heap` today** (`rejectIfNoHeap`, 23401): every allocation site records a fact, then rejects if the
flag OR an active `@noheap` body applies — so under the flag it rejects in every emitted body, library code
included. That is why `import { std::uuid::Uuid };` (or `hex`, or `json`) fails a `--no-heap` build that
calls nothing. Templates are exempt only because an uninstantiated one emits nothing (`_probingTemplate`).
Already reach-based under the flag: indirect dispatch (`rejectNoHeapIndirect`, 23437) and the
`GlobalAllocator` leaf (24830). The transitive walk is `checkNoHeapTransitive` (23568); under the flag its
roots are every user body. **Ungated sites:** the `isolate` handle form (15274), serde `Owned` boxes
(25664, 25713), graph shells (26716-26721, 26946). **Invisible to the walk:** any allocation inside an
extern C function — it has no body, so it records no fact.

**No replacement mechanism exists:** no hook, flag, manifest key or weak allocation symbol. There IS a
precedent for a weak runtime hook: `kama_panic_handler` (rt:425) and `kama_log_sink` (rt:1897).

**Foreign-owned memory — not replaceable, and must stay paired with its own release:**
`CommandLineToArgvW`/`LocalFree`, `GetEnvironmentStringsW`/`FreeEnvironmentStringsW`,
`getaddrinfo`/`freeaddrinfo`, `opendir`/`closedir`, `FindFirstFileW`/`FindClose`, thread stacks,
`CreateProcessW` handles, emscripten's main loop, wgpu/glfw objects, JS-side buffers.

## Proposed design

### 1. Reach-based `--no-heap` (first — independent, smallest, unblocks the rest)

The flag stops rejecting at the allocation site; every site only records its fact, and
`checkNoHeapTransitive` rejects what the program's roots reach, naming the chain — the rule dispatch
already follows. `@noheap` is unchanged: an attribute proves its own body, eagerly.

- **Roots:** `main`, every `expose fn`, every `@foreignEntry`/`@callerThread` region, interrupt handlers,
  and module-static initialisers if any can allocate. Not "every user body": an unreached helper is not
  part of the program either, and one rule is simpler than two.
- **Gate the ungated sites** (isolate handle, serde `Owned` boxes, graph shells) so their facts exist.
- **Make extern allocation visible:** an extern that allocates declares it (an attribute on the `extern fn`,
  or the runtime's own table), so a reached `Channel.make` is a fact rather than a blind spot.
- **Diagnostic** anchors on the innermost frame the author wrote, as the leaf rule already does.
- `SPEC.md` *No-heap subset* changes its sentence "the flag rejects a *direct* allocation in every body";
  every existing `noheap_*` xfail must still fail, each through a reached chain.

#### §1 settled (2026-09-15, maintainer), with what was measured — SHIPPED `0.9.347`–`0.9.348`

**Probes re-run at `0.9.345`** after a no-op rebase: uuid 4, json 28, hex 1, as recorded above.

**The call graph is read from the emitted C, not recorded per call site.** A reach rule is exactly as sound as
its edges, so the recorded graph was audited first. Method: 819 single-file fixtures transpiled with the edge
table dumped, then diffed against the calls and function references in the generated C. **1,354 missing edges
changed a verdict**, meaning the recorded graph called a function allocation-free when its C reaches an
allocation. By class:
- synthesized destructors, about 1,000: field drops inside `T__dtor`, and `Optional`/`Result` dtors. Only an
  owned LOCAL's dtor was an edge.
- graph serde helpers (`__kama_graph_readShell`/`dropBox`, `writeNode`, `visitEdges`, `__readInto`), which have
  no `_currentFunc`.
- serde calls spelled as raw strings (`serialize` → `serializeInto`, `deserialize` → `deserializeFrom`).
- the `Optional<T>.format` thunk.
- tagged templates in user code (`main` → `std::fmt::html`).
- fnptr and bindable targets (`g.fn = …__read`, `g.elemdtor = …__dtor`), including a pointer handed to C as a
  callback.

That is a pre-existing `@noheap` defect, not only a KR-47 prerequisite. `@noheap fn int32 tick(int32 n) { Box b =
Box.make(); return n; }`, where `Box` owns a `DynamicArray`, builds clean today. Recording each site through a
funnel was rejected: it is discipline around roughly 60 hand-spelled dtor calls, and it holds only while a guard
catches the next one. Reading the edges from the generated text makes them complete by construction, the way a
linker's `--gc-sections` reads relocations. The rules:
- **Capture:** the FINAL output is teed into memory as it is written. Text that only ever reaches a scratch or
  probe buffer is never an edge, which is also correct.
- **Scan:** a function is a top-level `…name(…) {`. Inside its body, every identifier that names another emitted
  function is an edge, except a member (`.x`/`->x`) and a name the body itself declares (parameter or local).
  The declaration exclusion exists because of KR-57: a C local named like a bare prelude function (`args`, 13
  corpus hits) is indistinguishable from a reference otherwise.
- **Lines:** a call site's line is the `#line` above it. `line()` always writes its directive, and the tee
  strips directives from the real stream under `--no-line`. The emitter buffers module bodies and flushes them
  later, so a line sampled at write time would be wrong.
- **What it cannot see:** functions defined by runtime MACROS (`KAMA_OWNED_IFACE_FUNCS`) and runtime C. These
  stay named leaf facts, as `GlobalAllocator` already is.
- **Scope:** the per-site `recordCallEdge` calls are deleted. All three walks over the graph gain the edges:
  no-heap, the foreign-entry static reads and `@onPanic`.

This ships as its own commit, before the flag change, with red-first fixtures.

**Roots under the flag**, read off the C like the edges:
- the C `main` (the synthesized wrapper, which calls `kama_main`)
- every `KAMA_EXPORT` body: every `expose fn`, which covers `@callerThread` and `@interrupt` because both must be
  `expose`
- `@foreignEntry` bodies, which C calls by a route no edge shows

An `@onPanic` handler is an ordinary function reached by an ordinary call, so it needs no root. Module statics
are compile-time constants. A pointer handed to C is reached through its address-take edge. A
reached user body with its own direct site reports at that site, and the flag's chain diagnostics keep today's
rule: the innermost user frame whose first hop leaves user code.

**What a `--no-heap` object promises: the reached program never allocates.** Measured: a `--release --target
embedded` object importing `std::uuid` carries `U malloc`/`U free` from bodies nothing reaches, while today's
no-heap objects reference none. The object may still NAME `malloc` in unreached code. `-ffunction-sections
-fdata-sections` go on every embedded and no-heap compile, debug included (today they are release-only), and the
board link's `--gc-sections` drops the unreached code; `docs/targets.md` says so. Stubbing allocation to a trap
(needs KR-48 first) and pruning unreached bodies from emission (L, overlaps KR-4) were the rejected alternatives.

**Extern allocation: an attribute, alone, spelled `@heap`** because it marks frees as well: `@heap extern fn
UnsafePtr kama_channel_new(…);`. Its C symbol joins `_heapSymbols`, and a CALL to it found in a body's C is an
allocation fact for that body, read by the same scan as the edges. That also covers the "ungated sites" by
construction. The isolate handle, serde `Owned` reads and graph shells emit raw `malloc(` in compiler-written
functions with no `_currentFunc`, and the prelude's `malloc` is `@heap`, so no new gate was needed. A gate's fact
keeps precedence where one exists, because it names the construct.

A runtime extern is marked when kama's C for it touches the heap on ANY target, so a verdict is the same on
every target. The audit read every variant in `include/`, following `kama_*` helpers and counting
`malloc`/`calloc`/`realloc`/`free`/`strdup`/`_strdup`/`kama_alloc`/`kama_free`/`LocalFree`/`HeapAlloc`, ….
44 symbols, 59 declarations. 23 touch the heap on every target, and 18 on some targets only, which is KR-58.
An unmarked user extern is unchecked C, as today. A table of libc names was rejected as a second mechanism.

**A consequence to know:** `Arena.make` mallocs its region through a `@heap` extern, so a `--no-heap` PROGRAM
that makes an `Arena` is refused. It had passed only because the extern was invisible. A `@noheap` region using
an arena built outside it is unchanged (`tests/noheap_arena.kama`). Under the flag a `BumpAllocator` is backed by
storage the program owns, and `tools/check-noheap.sh` cases 6c/6d pin both halves.

**A walk fact may now be unreported.** `AllocSite.reported` is true only when the `@noheap` gate refused the site
where it was written. Every other own-body fact (a flag site, a text fact) is reported by the walk at the site,
in the gate's own sentence plus why it is judged.

### 2. Two primitives: `kama_alloc` and `kama_free` (decided)

**`kama_alloc` and `kama_free` are the only two things in a kama program that obtain or release heap
memory, and they delegate to the program's GLOBAL ALLOCATOR — which defaults to `malloc`/`free` and which a
user can replace.** Everything else is built on them:

- every raw site the emitter writes (the 22 above) calls them;
- every runtime header and the OS seam (`kama_os.h`, `kama_channel.h`, `kama_isolate.h`, `kama_app.h`) calls
  them;
- `GlobalAllocator.allocate`/`deallocate` call them, instead of declaring `extern malloc`/`free` in the prelude;
- `kama_calloc` and `kama_realloc` stop being primitives — each becomes a helper over the two (allocate +
  zero; allocate + copy + free), so a replacement supplies exactly two functions and nothing escapes it.

This alone changes no behaviour and fixes every mixed alloc/free pair by construction, because there is
only one family left. After it, `malloc(`/`free(` appear in exactly one place in the tree, and a guard holds
that down (`tools/check-alloc-funnel.sh`: no `malloc`/`calloc`/`realloc`/`free`/`strdup` outside the default
implementation, across `src/` emitted strings, `include/`, `lib/` and `prelude/`; foreign-owned releases
such as `freeaddrinfo` are not those names and pass).

**DECIDED (2026-09-16): `kama_free(p, n)` is SIZED — because for kama the size is CORRECTNESS, not an
optimization.** A `malloc`-shaped allocator keeps a header and only saves a lookup, but kama's `Allocator`
contract already PROMISES `deallocate(pointer, bytes)`, and a size-class or header-free MCU pool is entitled
to trust it (Rust's `GlobalAlloc::dealloc(ptr, layout)`, Zig's `free(slice)`). With §3's decision the global
allocator IS an `Allocator`, so an unsized funnel would break that promise at the seam every allocation passes.

**Its prerequisite shipped first, at `0.9.366`: the sizes were already lying.** Measured by a header-checking
allocator (`tests/alloc_size_truth.kama`): `Owned<Base, A>`/`Shared<Base, A>` holding a derived object gave
`deallocate` `sizeof(Base)`, as did the contract upcast's `objsize`; and a `BindableFunctionPtr` bound from a
custom-allocator box released it with libc `free` (a crash). The fix is by construction — the size lives with
the dynamic type: a `__size` slot in every class vtable, **`sizeof(ptr: p)`** (SPEC) for library code, and a
bindable that releases through the box it was bound from (refused for a stateful allocator).

**The funnel carries ALIGNMENT too — shipped at `0.9.367` (was KR-61).** Measured at `0.9.366`: an arena bumped every
block to 8, so a `Simd<float32>#(4)` field (`alignof` 16) landed at an address ≡ 8 mod 16. Size and alignment are
one promise, so the contract became `allocate(bytes, align)` / `deallocate(pointer, bytes, align)` BEFORE the
funnel was built, and the funnel is `kama_alloc(n, align)` / `kama_free(p, n, align)` (plus `kama_alloc_zeroed`),
not a sized-only pair rewritten later. Beyond the fundamental alignment the default goes to the platform's aligned
allocator, whose release differs on Windows (`_aligned_free`) — which is why the FREE takes `align` as well.
`kama_calloc`/`kama_realloc` are gone. What `0.9.367` moved onto the funnel, each with its layout:
- every allocation the EMITTER writes (bare/fallible/`try` `new`, prim/enum/error boxes, `parallel_for`/`spawn`/
  `isolate` bundles and their trampolines, serde `Owned` reads, graph shells — `__kama_graph_dropBox` frees per
  node type) and every `A__allocate`/`A__deallocate` it writes;
- contract handles on either allocator free with the layout their VTABLE reports (`__size`/`__align`, and
  `__vsize`/`__valign` for a virtual-class implementer holding a derived object — whose `__dtor` is now `__vdrop`,
  fixing a slicing drop); the handles' `objsize` field is gone;
- all of `kama_runtime.h` (strings free with `cap`; the Windows argv failure path frees each slot while its
  size is known), `GlobalAllocator`, `Arena` (its buffer comes from `GlobalAllocator`, so under `--no-heap` it is
  refused through that leaf), every stdlib collection and box, and `kama_capture2` (which now returns each
  buffer's capacity to `process.kama`);
- the dead concrete `KAMA_OWNED/SHARED/WEAK_{TYPE,FUNCS}` macros (a smart-pointer class is only ever over a
  contract — measured: 0 of 826 fixtures emitted them) and the emitter arms that wrote them are deleted.

**What moved at `0.9.369` — the rest of KR-48.** Every raw call in `kama_os.h`, `kama_channel.h`, `kama_isolate.h`
and `kama_app.h`, by one of two patterns:
- **the layout is in hand where the block is released** → `kama_alloc`/`kama_free` with it: the pollers (growth is
  alloc + copy + free, sized by `cap`), the Windows `diropen` cursor, the POSIX reaper's pid array, the channel
  and its ring (the ring takes the fundamental alignment — the element's is not passed across that seam, and
  elements are copied in and out, never read in place), the isolate box, the emscripten loop record;
- **it is not** → `kama__sized_alloc`/`_zeroed`/`_strdup`/`_free`, a one-word size header in front of a funnel block,
  private to `kama_os.h`: every wide path and string (`kama__wide`, `kama__wpath`, and `kama__wfree` releases them
  all), the `readDir` pattern, the Windows command line and environment block, and the argv/envp vectors and their
  strings on both platforms — which replaced `strdup`/`_strdup`, allocators no grep for `malloc` finds. `kama__utf8`
  stays a plain `kama_alloc(n, 1)` because its result becomes a `kama_string`; `kama_envp_build` copies the entries
  it keeps into sized blocks.

**Held down two ways.** `tools/check-alloc-funnel.sh` refuses a C allocator call (`malloc`, `free`, `strdup`,
`aligned_alloc`, …) anywhere in `include/`, `prelude/`, `lib/` or the C the emitter writes, outside the funnel's own
block — self-tested on planted calls. And the san leg compiles with `KAMA_ALLOC_CHECK`: the funnel records each
block's layout in a header and panics when a release passes a different one, so a green `./dev test san` proves
every free in the corpus returned its exact size and alignment.

### 2b. The Windows seam stops allocating per path (KR-58) — brief, not started

Written 2026-09-17 on the Windows box at `0.9.369`, from reading `include/kama_os.h` and one probe. It is the Windows
box's row once KR-49/KR-50 have landed on the other machine.

**What is true today.** `kama__wpath` converts every UTF-8 path to a heap UTF-16 string (`kama__wide` → a
`kama__sized_alloc` block), and past 248 characters makes a SECOND heap block for the `GetFullPathNameW` result with
the `\\?\` prefix. POSIX passes the bytes straight through. The externs whose only heap on any target is that
conversion are the TEN below, not the eleven the row first said:

| extern | Windows body | mark today |
|---|---|---|
| `kama_open_read` / `_create` / `_append` | `kama__wopen` → `_wopen` | `@heap` |
| `kama_unlink` | `_wunlink` | `@heap` |
| `kama_mkdir` / `kama_rmdir` | `_wmkdir` / `_wrmdir` | `@heap` |
| `kama_is_symlink` / `kama_exists` | `kama__attrs` → `GetFileAttributesW` | `@heap` |
| `kama_rename` | two paths → `MoveFileExW` | `@heap` |
| **`kama_path_meta`** | `_wstat64` | ⚠️ **none — a missed mark** |

⚠️ **`kama_path_meta` is a soundness hole on Windows today.** The `@heap` audit (§1, "Extern allocation") missed it,
so `std::fs::stat` is judged heap-free: probed, a `--no-heap` program whose `main` matches on `stat(path: p)` BUILDS
and runs, while the same program calling `exists` is refused naming `kama_exists`. Nothing ties a mark to the C body
it describes, and this is the proof that the manual audit is not enough (see "Open" below).

`kama_diropen` stays `@heap` whatever this row does: its cursor is a heap block on Windows, and POSIX `opendir`
allocates. Its `<path>\*` pattern buffer can still move to the stack with the rest, for free.

**The seven one-platform externs, judged** (the row's second half) — all honest, none changes here:
- `kama_args_at`, `kama_program_invocation`/`_name`/`_path`, `kama_env_lookup`: each RETURNS a fresh owned
  `kama_string` natively; the wasm stubs return literals. Allocating is what they are for. Not stopping would take
  a borrowed-view surface, which is a language question and not this row.
- `kama_proc_spawn` (Windows): the command line and environment block are unbounded, and spawning is `@heap` on
  every target anyway (`kama_argv_new`, `kama_envp_build`). Stays.
- `kama_proc_detach` (POSIX): the reaper's pid list grows. Not a Windows task; leave it to the other boxes.

So the row narrows to: **the ten path externs stop allocating on Windows, and their marks come off** (making
`kama_path_meta`'s missing mark moot by removing the cause).

**Proposed shape: a caller-owned wide path buffer on the stack.** Zig's `sliceToPrefixedFileW` does this, returning
a `PathSpace { data: [PATH_MAX_WIDE:0]u16, len }` by value. Rust and Go allocate. A sketch, to be measured before it is
committed to:

```c
typedef struct kama__wpathbuf { wchar_t w[32767 + 8 + 1]; } kama__wpathbuf;   // NT limit + `\\?\UNC\` + NUL
static inline wchar_t* kama__wpath(const char* utf8, kama__wpathbuf* b);      // NULL + errno on failure
// kama_unlink: kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1; return _wunlink(w);
```

- The UTF-8 → UTF-16 conversion writes into `b->w` (a UTF-8 path of `n` bytes needs ≤ `n` code units). Past the
  buffer's capacity it fails with `ENAMETOOLONG`, which Win32 would refuse anyway. For a long path, convert into the
  buffer, then `GetFullPathNameW` into a second local or in place with `memmove` room. Settle which by reading the
  API's aliasing rules; do not guess.
- `kama__wfree` and its errno save/restore go away for paths (deletion). `kama__wide` stays for `proc_spawn`.
- **Frame cost:** ~64 KB per path, ~128 KB in `kama_rename`. A >4 KB frame gets `__chkstk` probing on Windows.

**Decide before code (maintainer, with measurements):**
1. **One tier vs two.** (A) The full 32K buffer in every wrapper: the simplest, one way. (B) A `MAX_PATH` stack buffer
   for the common case, falling back to the heap for a long path: REJECTED, because the marks could not come off and
   the row would not close. (C) A small buffer, with the 32K buffer only inside a `noinline` long-path helper: the same
   worst case, a small common frame, more code. **Recommend A** unless the measurements below show a cost.
2. **Is ~128 KB safe on every thread kama runs fs calls on?** Measure, don't assume: the exe's `SizeOfStackReserve`
   (`objdump -p out.exe | grep -i stackreserve`; MinGW ld and MSVC link differ), and the stack winpthreads gives
   `pthread_create(&t, NULL, …)` in `kama_isolate.h`. Worker `spawn` goes through that. Then run a probe: `exists` on a
   ~30K-character path from a spawned worker.
3. **The cost of the probe.** Time `exists` on a short path in a loop, before and after. The current path pays a
   `malloc`/`free` pair, the new one pays page probes. Bracket the timing with the clock-jump check (see
   `docs/platforms/windows.md`: this VM's timings are not Windows facts, so only a same-run ratio means anything).
4. **`ENAMETOOLONG` in `IoError`.** Check whether `lastError()` maps it to a named case or `Other(code)`. Add a case
   only if the enum already carries its POSIX siblings.

**Tests, red first:**
- A `--no-heap` flag fixture (the `tests/noheap_flag_*.d` form) whose `main` reaches `stat`, `exists`, `rename`,
  `remove`, `createDir`, `removeDir`, and a `File.open` in each `OpenMode`. RED today (it names `kama_open_read` etc.),
  green once the marks come off. First probe which of those kama functions allocate for reasons of their own (a
  `File` value, a `Result`'s error). Only a function whose sole heap is the extern belongs in the fixture, and the
  rest are recorded here.
- The marks change the no-heap verdict on EVERY target, because a mark is target-independent. So the fixture is
  also Linux's and wasm's, and the maintainer's matrix on the Linux box is part of the gate.
- `tools/check-long-path.sh` (stat/exists/rename/createDirAll/removeDirAll, and so `kama_is_symlink`) and
  `tools/check-path-unicode.sh` must stay green on Windows. Confirm the long-path probe also covers `File.open` in
  Append mode and `remove` past 260 characters, and add the cases it lacks.

**Meets KR-49 — re-read before building.** Once a program can declare `@globalAllocator`, a `@heap` extern whose
C allocates through the FUNNEL (as `kama__wpath` does, via `kama__sized_alloc`) reaches the declared pool, not the
system heap, while a `malloc` inside foreign C still reaches the system heap. §1's single `@heap` mark cannot tell
those apart. If KR-49 splits it, a program with a pool may already get `std::fs` legally under `--no-heap` without
this row. KR-58 still matters for the DEFAULT (no pool declared), which is every program today, and for an fs call
that should not allocate at all. Check what KR-49 shipped with before writing the fixture.

**Open (maintainer):** no guard ties an `@heap` mark to the header body it describes, and `kama_path_meta` shows the
manual audit misses. A guard would parse each `static inline` variant in `include/` and follow `kama_*` helpers,
probably size L. File it as its own row or accept the gap. Do not fold it into KR-58.

### 3. Replacing the default implementation

A program replaces the two primitives, not the eleven families that used to call libc.

**DECIDED (2026-09-16): a declaration** — e.g. `@globalAllocator type resource Tlsf implements Allocator { … }`, at
most one per program; the compiler emits `kama_alloc`/`kama_free` against it. Explicit and greppable (GOALS #5);
the compiler can refuse two, and refuse a stateful one with no way to reach its state. **The reason it wins is
`--no-heap`:** a declaration has a BODY the flag can walk, so a pool over storage the program owns is *proven*
allocation-free the same way any other code is — no annotation, no trust. The alternative, **a weak link-time
symbol** (as `kama_panic_handler` is), has zero language surface but is invisible in source and to the analysis,
so the flag could only take it on faith. That turns the campaign's own rule — judge what the program reaches —
into an exception at the one seam where every allocation now funnels. This is also what makes the meaning split
above (`--no-heap` = never reaches the SYSTEM heap) buildable rather than a promise on paper.

`GlobalAllocator` stays the name of the default `A`; with §2 it is already a handle onto `kama_alloc`, so it
follows the replacement for free. `Shared.adopt`'s hard-coded control block and the `SortedMap` root move
onto `A` in the same step.

#### §3 RECOMMENDED SHAPE (2026-09-17) — where the global allocator's state lives

The question: a module `static` is PER-ISOLATE (SPEC *The three sharing seams*), so a pool whose state lives in
one would be a different pool in every isolate, and a box sent over a channel would be freed into the wrong one.
**kama already has the answer, twice over, and the recommendation is to reuse both rather than invent a third:**

1. **Process-global runtime state has a precedent — the entry-TU singleton.** `setPanicHandler`'s slot,
   `setLogSink`'s and the argv stash are ONE object each: external linkage, a single definition the compiler
   emits in the entry translation unit (`isEntry` in kama.cemit.cpp), an `extern` declaration everywhere else
   (kama_runtime.h: "PROCESS-GLOBAL by contract, not per-isolate, so it must be ONE object"). The declared global
   allocator is the same kind of thing and gets the same lowering: **one instance, `<Pool> kama_global_allocator`,
   defined in the entry TU, declared `extern` in the shared header**, so every TU's `kama_alloc` reaches the same
   object and every isolate shares it — which is exactly what a heap must be.
2. **What may be shared across isolates already has a rule** — the three sharing seams: cross-isolate MUTABLE
   state is `Atomic<T>`, full stop, plus deeply immutable data. So the declared type's own fields must each be an
   `Atomic<T>`, `const`, or the raw storage it hands out (an `InlineArray` of scalars / an `UnsafePtr` region,
   reached only inside its `unsafe fn` bodies, like every allocator). The compiler checks that at the
   declaration; a pool that needs a lock-free free list writes it over `Atomic` compare-exchange, which is also
   what makes it safe to call from an ISR on a single core. Nothing new to learn, nothing to trust.

**The declaration then reads:** `@globalAllocator type resource Pool implements GlobalHeap { … }` — a `resource`,
because a process-wide instance with `Atomic` state has IDENTITY and must never be copied (see KR-63 below, which
is why this is not spelled `type value`). Construction: its `default ctor` must be a compile-time-constant init
(the module-static rule — no startup hook, no init-order fiasco, the MCU shape), since the instance is a C global.
At most one per program; a second is an error naming both.

**Contention, and per-isolate allocators (asked 2026-09-17).** One instance does not mean one lock. The DEFAULT is
the platform `malloc`, which already keeps per-thread caches, so nothing changes there. A declared pool is as
contended as it is written: a single `Atomic` compare-exchange free list is lock-free but bounces one shared word
between cores under heavy concurrent allocation. The scalable shape needs NO new language — kama's two storage
seams are exactly the two tiers of a tcache/mimalloc allocator: a module `static` is per-isolate by construction,
so it holds each isolate's uncontended cache, and the singleton's `Atomic` fields hold the shared backing a cache
refills from and a cross-isolate free returns to. **Open design point for a reference pool: there is no isolate-exit hook**, so
a dying isolate's cached blocks are stranded unless the cache is bounded or drained — decide which before a
reference pool ships. (A C thread kama did not create sees statics at their initialiser, so it simply starts with
an empty cache — safe, provided the empty-cache path goes to the shared tier.)

A PER-ISOLATE ALLOCATOR is already expressible, explicitly: everything that takes `A` (containers, `Owned<T, A>`,
`Shared<T, A>`, `new(allocator:)`) can draw from an arena per worker, and `Sendable` keeps such a block from
crossing — measured at `0.9.369`: spawning an `Owned<P, BumpAllocator>` is refused because `BumpAllocator` (for `A`)
is not `Sendable`, while `Owned<P>` builds. **Recommended NON-GOAL: a per-isolate override of the DEFAULT allocator.**
What it would redirect is exactly what crosses isolates by construction — a spawn bundle allocated in the parent is
freed in the child, strings and boxes travel over channels, a channel's ring is shared by both ends — so each
would be freed into the wrong allocator, the hazard the singleton exists to prevent; and it would make allocation
implicit (GOALS #5). What remains outside explicit per-isolate allocation is what has no `A` (`string`,
`Formatter`, `Owned<Error>`, runtime internals); if that ever matters, the answer is giving those an `A` (see
"`string` has no allocator"), not an implicit override.

**Three things the session must PROBE before building, not assume:**
- ~~**KR-63 first.**~~ (shipped `0.9.370`) A `type value` holding an `Atomic<T>` built and COPIES the cell (measured at `0.9.369`:
  `Holder k = h; k.a.store(5)` leaves `h.a` at 1), because the value check tests `destructible` and skips the
  `moveOnly` it computes. The global allocator is precisely a type whose identity must not be laundered through
  a copy, so the hole is closed before the declaration is built on top of it.
- ~~**The contract kind.**~~ **DECIDED 2026-09-17: a sibling contract, `type contract GlobalHeap for resource`**,
  with `Allocator`'s two members. `Allocator` stays `for value`. Measured at `0.9.369`: widening `Allocator` to
  `for value, resource` broke 108 of 120 container fixtures with 324 errors, all in `shared.kama`/`weak.kama`, even
  at `A = GlobalAllocator`. A bound's kind clause is what makes a container's `A` copyable, and once the clause
  admits a resource, `A` is move-only in the TEMPLATE and every `this.alloc` copy is refused. Keeping one contract
  would need new bound syntax to say "a value `Allocator`" at the 18 `A: Allocator` sites. The two are not two ways
  to do one thing (GOALS #4). `Allocator` is a HANDLE: many of them, stored in containers and copied. `GlobalHeap`
  is THE HEAP: exactly one, never stored in an `A` slot, and called only through the funnel. Neither can stand in
  for the other. The third option, having the attribute check the two members structurally with no contract, was
  rejected as an implicit promise (GOALS #5).
- **The no-heap walk.** The call graph is read from emitted C, and the funnel lives in a header it does not scan.
  With a declaration the emitted C must contain an edge `kama_alloc` → `Pool__allocate` (e.g. the entry TU defines
  `kama__global_allocate`/`_deallocate` over `kama_global_allocator`, and the header's funnel calls them under a
  define the emitter writes into every TU's preamble, as `KAMA_ONPANIC` is), and `_heapSymbols` must stop seeding `kama_alloc`/`kama_free` as heap for
  THAT program — so the pool's body, not the funnel's name, decides. Confirm a pool whose `allocate` calls
  `GlobalAllocator` is still refused (the leaf is the default implementation, which a pool must not reach).

#### §3 AS BUILT (`0.9.377`–`0.9.378`) — what the probes and the build decided

- **Construction is zero bytes, and nothing else.** Probed first: a pool with `InlineArray` storage and `Atomic`
  free-list and bump fields works from `<Pool> g = {0};` with its ctor never run. So the declaration refuses a
  ctor AND a field initializer. Either would be a promise the program never keeps, since there is no hook to run
  them. (The recommendation above said "a compile-time-constant default ctor". Zero was enough, and it is simpler.)
- **The no-heap edge, by construction.** With a declaration, every funnel FACT becomes an EDGE into
  `kama__global_allocate`: the three funnel names in `buildCallGraph`, every `rejectIfNoHeap` site (they are all
  funnel allocations, since starting a thread was never a site), and the `GlobalAllocator` leaf. The pool's body then
  decides, for `@noheap` and `--no-heap` alike, over the existing walk. A pool that reaches the funnel is a cycle
  through its own entry, refused in every build. The chain renders the entries as `GlobalHeap::allocate`.
- **`value` needs no check of its own.** `GlobalHeap` is `for resource`, so the conformance refuses it.
- **A bare `new` needs `A == GlobalAllocator`** (maintainer, 2026-09-17). A field-less custom `A` used to pass the
  "stateful" check, and its box freed through `A` a block the funnel had allocated. Measured: 0 allocations and 2
  frees through a counting `A` for one `Shared`. That is the mixed-family defect of §2 one level up, and a declared
  pool would turn it from latent to live. Any other `A` takes the placement form.
- **The fixtures prove the routing by exhaustion**, because kama code cannot reach the instance to read a count.
  That gap is KR-66.

### 4. Allocator-aware errors

Error boxing (`emitEnumBoxIntoContract`, `emitPrimBoxIntoContract`, `emitStickyErrBox`) goes through the
box's `A` — the `A3` path bare `new` into a stateful `A` already uses. With §3, `Owned<Error>` (whose `A`
is `GlobalAllocator`) then draws from the program's declared allocator, which answers "a user can replace
it" for serde and every other fallible API without touching a contract.

Open: whether a PER-CALL error allocator is wanted too (serde into an arena, errors included). That is a
contract change — `Result<T, Owned<Error, A>>` in `Serializable`/`Deserializable` — and source-breaking for
every hand-written serde, so it is decided on its merits here and not assumed.

### 5. Serde's error path stays honest

A `deserialize` that fails returns `Err`. `Handle.deserialize` (`slot_map.kama:28`), which ignores the
reader's failure today and so decodes garbage as `Ok`, is fixed to check `failed()` like every other type —
after §1, because under today's eager rule its new box would fail any `--no-heap` build importing `SlotMap`.

## Where this meets existing rows

- **KR-39 (`@noheap` cannot cross a contract slot)** is the same question asked from the other end.
  A no-heap program using serde needs three things, and this campaign supplies two of them: a flag that
  judges what is REACHED (§1), and an error box that does not hard-code `malloc` (§4). The third is
  KR-39's own better answer — prove the callee behind a contract slot when the backend is statically known
  (`serializeJsonBuffer` constructs its backend) and check that body directly — which is **tier 1 of the
  devirtualization ladder** in KR-23. §1 and KR-39 share one principle: judge what the program actually
  reaches, through a call edge or through a slot whose target is provable. They should share the reach
  walk, not grow two.
- **KR-23 (Performance — devirtualization ladder)**: tier 1 (sound static devirtualization) gains a
  correctness consumer here, not only a speed one. Its proof of "which body does this slot reach" is what
  KR-39 consumes; it is scheduled with that in mind rather than as an optimization alone.
- Reading a `string` from a `Deserializer` still allocates, by design: a no-heap consumer of serde reads
  scalars, fixed buffers and borrowed views, and `readString` stays the member that is honestly not
  `@noheap`.

## Order

1. ~~§1 reach-based `--no-heap` (KR-47), then the `Handle` fix~~ — both shipped (`0.9.348`, `0.9.355`).
2. §2 `kama_alloc`/`kama_free` as the only primitives, and the guard (KR-48; no behaviour change; mixed pairs gone).
3. ~~§3 replacing the default implementation (KR-49; + `Shared.adopt`, `SortedMap` root)~~ — shipped `0.9.378`.
4. §4 allocator-aware error boxing; decide the per-call question (KR-50).
5. OS seam and extern coverage completed; foreign-owned list verified against the code.

Each step: fixtures landed red first, `SPEC.md` updated in the same commit, `VERSION` bumped.
