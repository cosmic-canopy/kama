# Allocation — one funnel, a replaceable global allocator, allocator-aware errors, a reach-based `--no-heap`

**Status:** design, not started. Opened 2026-09-12 at `0.9.320` during KR-12 (`std::uuid`), when a
hand-written `Deserializable` had to box an error and that one box turned out to be unaccountable to every
mechanism kama has for memory: no `Allocator` saw it, `--no-heap` rejected it for merely being imported,
and no program could redirect it. This doc is deleted when the rows below ship, as the maintenance rule
for `docs/design/` says.

## Picking this up

This work follows KR-46; the handoff state, the agreed order, the per-host gate and the maintainer's
working constraints are written once, in [name-resolution.md § Picking this up](name-resolution.md#picking-this-up--on-any-machine),
and apply here unchanged. The decisions that are specific to this campaign, made by the maintainer on
2026-09-12:

- `kama_alloc` and `kama_free` are the ONLY allocation primitives. They delegate to the program's global
  allocator, which defaults to `malloc`/`free` and which a user can replace.
- EVERY allocation site is covered — emitted C, runtime headers, the OS seam, the prelude — so replacing the
  global allocator is complete. The mixed alloc/free families are a defect to remove, not a quirk to document.
- Error boxing is allocator-aware.
- `--no-heap` is reach-based, consistently.
- **No workaround in serde's error path**: a failing `deserialize` returns `Err`; a placeholder `Ok` plus the
  reader's sticky flag, or a boundary net that converts one, were both considered and rejected.

Still open, to be decided here and written down: declaration vs weak symbol for the replacement (§3), a
sized `kama_free` (§2), and a per-call error allocator (§4).

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
   failure through the reader's sticky flag with a placeholder `Ok` to dodge an allocation.

## What is true today (measured by reading, `0.9.318`; line numbers are that version's)

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

Open: whether `kama_free` is **sized** (`kama_free(p, n)`). `Allocator.deallocate` takes `bytes`, and a
size-class allocator (TLSF, a slab) wants it; most runtime frees know it already (a string's `cap`,
`sizeof(kama_ctrl)`, a buffer's tracked capacity), and the triage must confirm the rest before it is decided.

### 3. Replacing the default implementation

A program replaces the two primitives, not the eleven families that used to call libc. How it names the
replacement is the one surface decision left, to make here rather than in code:

- **A declaration** — e.g. `@globalAllocator type value Tlsf implements Allocator { … }`, at most one per
  program; the compiler emits `kama_alloc`/`kama_free` against it. Explicit and greppable (GOALS #5); the
  compiler can refuse two, and refuse a stateful one with no way to reach its state.
- **A weak link-time symbol**, as `kama_panic_handler` already is. Zero language surface, but invisible in source.

`GlobalAllocator` stays the name of the default `A`; with §2 it is already a handle onto `kama_alloc`, so it
follows the replacement for free. `Shared.adopt`'s hard-coded control block and the `SortedMap` root move
onto `A` in the same step.

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

1. §1 reach-based `--no-heap` (KR-47), then the `Handle` fix (KR-51).
2. §2 `kama_alloc`/`kama_free` as the only primitives, and the guard (KR-48; no behaviour change; mixed pairs gone).
3. §3 replacing the default implementation (KR-49; + `Shared.adopt`, `SortedMap` root).
4. §4 allocator-aware error boxing; decide the per-call question (KR-50).
5. OS seam and extern coverage completed; foreign-owned list verified against the code.

Each step: fixtures landed red first, `SPEC.md` updated in the same commit, `VERSION` bumped.
