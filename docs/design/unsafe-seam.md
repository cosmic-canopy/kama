# The unsafe seam — `unsafe fn`, and containing `UnsafePtr` (in-flight design)

*In-flight design doc. **Delete this file when the seam work ships**, once GOALS §3a/§3b + SPEC carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> Written out of the safety/unsafe boundary spike (`0addb7c`); findings ①–⑪ are in
> [ROADMAP.md](../ROADMAP.md) §2. Companion brief: [view-model.md](view-model.md).

## The problem, measured

GOALS §3a says the safe surface never sees a raw pointer, and that `unsafe { }` + `UnsafePtr<T>` guard the
FFI boundary. Neither holds. **Only the dereference `p[i]` is gated** — `_inUnsafe` is consulted at four
emit sites and nowhere else. Safe kama can *produce* a raw pointer (`addr(of:)`, public `dataPtr()`,
`cast<UnsafePtr<T>>` of an integer), *store* it in a field, and **call arbitrary C with it**. A double-free
needs no `unsafe` token anywhere (finding ①); `addr(of:)` plus an `UnsafePtr` field is a general
dangling-pointer factory (finding ⑧).

## The model: `unsafe` moves from the block to the function

**Decided.** The granular block is the wrong unit. Replacing it with a function-level marker:

- **`unsafe fn` replaces `unsafe { }`.** The block form goes away entirely.
- **Only a private function may be `unsafe`.** To expose capability you wrap it in a
  public/protected member — the API boundary *is* the safety boundary.
- **An `extern fn` is unsafe**, and may only be called from an `unsafe fn`.
- **`UnsafePtr` may only be produced or handled inside an `unsafe fn`**, which — combined with
  private-only — means **no public signature can mention it**.

### Why this beats the type-position rule it replaces

An earlier draft of this campaign proposed keeping blocks and adding a separate "an `UnsafePtr`-typed
expression may only appear inside `unsafe`" rule. The function-level model is strictly better:

1. **It fixes finding ⑨ by construction rather than by patching it.** Today a top-level `unsafe` block does
   `unassigned.clear()` for the *whole function* (`cemit.cpp:12501`), and `flag()`/`verifyOutsAssigned()`
   return early — so an unfilled `out` parameter passes `kama check` and the caller reads uninitialized
   stack. That is a bug precisely because the relaxation's scope (the function) does not match the
   construct's scope (the block). **When `unsafe` *is* the function, function-wide relaxation is correct.**
   The mismatch cannot exist.
2. **It makes `public fn UnsafePtr<T> dataPtr()` illegal by construction**, rather than by a case-by-case
   API review.
3. **It is greppable at the declaration** (GOALS §5) instead of buried in a body, and it is one concept
   instead of two.
4. **It removes a known emitter trap**: wrapping an assignment in `unsafe` currently changes the ctor walk's
   notion of top-level, so a real ctor-escape hole can look closed for the wrong reason.

### The tradeoff to accept deliberately

Granularity is genuinely lost — `growTo` is ~10 lines of which 2 need raw access, and the whole function
becomes unsafe. That is acceptable **only because unsafe functions are small and private by construction**,
which is the discipline you want anyway. It is the same relaxation scope as today's finding ⑨, made honest.

## Rule surface — the decision table this campaign must fill in

Each row is currently legal in safe code and needs an explicit verdict. This table is the design work.

| construct | example | verdict |
|---|---|---|
| declare an `UnsafePtr` **field** | `UnsafePtr<T> data;` in `DynamicArray` | **must stay legal** — every container and every `type extern value` in `examples/webgpu` depends on it |
| module **static** of `UnsafePtr` type | `static hardware UnsafePtr<uint32> gpio_odr;` | **must stay legal** — the MCU path depends on it |
| **read/write** such a field | `this.data` | open — presumably `unsafe fn` only |
| **pass** an `UnsafePtr` as an argument | `View.make(data: this.data, …)` | open |
| **return** an `UnsafePtr` | `dataPtr()` | private `unsafe fn` only; illegal in a public signature |
| `addr(of: x)` | `gpio_odr = addr(of: led);` | open — it *produces* a raw pointer, so presumably `unsafe fn` only |
| `cast<UnsafePtr<T>>(…)`, `cast<usize>(p)` | `cast<UnsafePtr>(0x40021000)` | must stay possible for MMIO; presumably `unsafe fn` only |
| compare two `UnsafePtr`s | `if (this.handle != null)` | open |
| **scalar-only** `extern fn` | `extern fn int32 abs(int32)` | open — subsumed for pointer cases; Rust gates these too |

## Blast radius, measured

| | |
|---|---|
| `unsafe { }` blocks in `lib/`+`prelude/` | **165** |
| of those, sitting directly in a `public fn`/`public ctor` | **61** — each needs a private `unsafe fn` helper extracted (`View.operator[]` → a private unchecked get, the `get_unchecked` shape) |
| `extern fn` declarations | **379** (188 `lib/`+`prelude/`, 160 `tests/`, 31 `examples/`) — call sites need an enclosing `unsafe fn` |
| `addr(of:)` lines in `lib/`+`prelude/` not already inside a block | **~47** |
| public `dataPtr()` call sites | **4** |
| MMIO assignments | one wrapped line each |

## Milestones

**M1 — `unsafe fn` grammar + the private-only rule.** New in the grammar: today `UNSAFE` appears at exactly
one production (`unsafe_statement : UNSAFE block`, `kama.y:1074`); there is no `unsafe fn`. Note kama's
free functions are already file-private by default (a file without `namespace` gets scope `_F<idx>`), so
"private" needs defining for free functions as well as members.

**M2 — retire the block, migrate the 165 sites.** Mechanical but wide; the 61 public-member cases need a
helper extracted. Finding ⑨ dies here without a separate fix.

**M3 — the `UnsafePtr` containment rules** per the decision table.

**M4 — `extern fn` is unsafe.** Includes the scalar-only verdict.

**M5 — fixtures.** ⚠️ **No `xfail` currently pins *"raw pointer access requires an `unsafe { }` block"*** —
only `asm_outside_unsafe` covers any gate. The two most important checks in the seam could be deleted today
and the suite would stay green. Every new rule needs its own `xfail` in the same commit.

## Closed decisions — do not reopen

**`Optional<UnsafePtr<T>>` / deleting the `null` token: killed.** Reasoning in [ROADMAP.md](../ROADMAP.md)
§2, in short: nothing null-shaped reaches a binary (deref already needs `unsafe`; the six positions the
type-keyed rule misses are all rejected by clang, making them an instance of the `kama check` gap);
`Optional` has no unwrap and cannot get one, since a generic enum rejects members; and the niche
optimisation would touch ~25 emission sites. **Containment gets the same goal for free** — all 75 genuine
`null` tokens target an `UnsafePtr`, so confining the type confines the token.

What survives as separately worthwhile: the honest FFI surface. Only **8** extern declarations return a
genuinely nullable pointer (`malloc` ×3, `kama_poller_create`, `kama_diropen`, `kama_channel_new`,
`kama_argv_new`, `kama_envp_build`).

## Open questions

**A. Is `pub unsafe fn` ever needed?** Private-only is the stronger guarantee and the reason the boundary
becomes clean. The cost is that a library cannot expose an unchecked fast path — `get_unchecked` for an ECS
or sort inner loop. Recommend holding the line and exposing a checked wrapper whose bounds check the
optimiser can hoist; revisit only with a measured hot path that needs it.

**B. What is "private" for a free function?** File-private already exists by default. Does `unsafe fn` at
module scope in a `namespace`d file need an explicit marker?

**C. Does an `unsafe fn` relax definite assignment for its whole body?** Under this model, yes — and that
is correct rather than a bug. Confirm that is intended and write it into SPEC so it is not re-filed as
finding ⑨.

**D. Ctors.** A public ctor that needs raw setup (`Box.make` → `unsafe { this.buf = malloc(...) }`) must
delegate to a private `unsafe fn` and assign the result. Check this composes with the rule that a ctor must
assign every field.
