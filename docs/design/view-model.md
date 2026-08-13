# The view model — finishing `type view` (in-flight design)

*In-flight design doc. **Delete this file when the view work ships**, once GOALS §3c + SPEC carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> Written out of the safety/unsafe boundary spike (`0addb7c`). Every number below was measured, not
> estimated; the eleven findings it refers to are in [ROADMAP.md](../ROADMAP.md) §2.

## The model, in one paragraph

**A view is a window into another type.** It owns nothing, destroys nothing, and it is only meaningful
while the window is open. It does not exist independently — it is always *paired* with the thing it looks
into. An **iterator is a view**: not by analogy but literally, since `b743f80` made all 17 borrowing
iterators `type view`. An iterator is a window plus a cursor.

That is the sentence to teach, and it is the sentence the current implementation cannot back up.

## The two questions a view type must answer

The trouble with `type view` today is that it answers one of these and pretends to answer the other.

1. **Which** — this view is a window into *that* container.
2. **How long** — the window is open *until when*.

Rust answers both with one lifetime annotation, which is why they look like one question. They are
separable, and separating them is what makes this tractable without lifetimes.

For **how long**, there are exactly three answers available to any language:

| | mechanism | sound? | cost |
|---|---|---|---|
| a | the programmer promises | C++ `span`, **kama today** | no | free |
| b | the type system proves it | Rust `&[T]` + lifetimes | yes | annotations in every signature; the machinery [GOALS.md](../GOALS.md) §3e declines |
| c | lexical scope bounds it | `foreach`, the `borrow` block | yes | expressiveness |

**kama documents (b) and implements (a).** GOALS §3c promises a view "can never dangle"; the escape rules
deliver something much weaker. That gap is the defect — in the docs as much as in the code — and it is why
the type has been hard to reason about: the stated contract and the real one differ, so there is no stable
mental model to hold.

**The decision: kama picks (c).** `foreach` already *is* (c) — it scopes an iterator lexically. Generalising
that to the whole kind is not a new concept, it is making the two members of one kind behave the same way.

## Why (c) is nearly free *for kama specifically*

The usual objection to lexical scoping is lost expressiveness. Scored against Rust's four abilities:

| Rust ability | lost? |
|---|---|
| return an interior reference | **No** — kama already allows the useful subset, rooted at `this` or a `ref` param. That is `d.view()`, `v.slice()`, `operator[] -> ref T`. |
| relate two lifetimes in one signature | **No measurable loss** — needs a returned borrow derived from one of two params. Zero demand in the corpus. |
| disjoint mutable borrows (`split_at_mut`) | **No** — served by built-ins: `parallel_for` splits into disjoint sub-views internally, plus `View.swap` / `sort`. |
| **store a borrow in a struct** | **Yes — and it is already lost.** `view_field` forbids it. |

kama has *already paid* Rust's expressiveness price via those shipped `xfail` fixtures; it simply has not
been collecting Rust's guarantee in return. Going lexical does not take expressiveness away — it takes away
the *workaround*.

And the workaround is measurable: **18 `type view` declarations in `lib/`+`prelude/` carry 25 borrowed
raw-pointer fields**, because `View<T>` may not be a field — not even of another view. That is "store a
borrow in a struct", performed 25 times, with a raw pointer standing in for the borrow the language will
not let them name. Those fields are the same shape as finding ⑧.

## Measured impact of going lexical

| measurement | value |
|---|---|
| `.view()` / `.slice()` call sites | **60** — 48 `tests/`, 10 `lib/`, 2 `examples/` |
| of the 12 non-test sites, how many are **temporaries in argument position** | **12** (`w.write(bytes: this.scratch.view())`, `from.read(into: scratch.view())`, …) — window = the statement |
| `View` **locals** outside `tests/` | **2**, both `sort.kama:115,117`, and both *derived from a `View<T>` parameter* |
| long-lived views in `lib/` / `prelude/` / `examples/` / `bench/` | **zero** |
| `examples/webgpu` | holds **no kama `View`** — `WGPUStringView` is a C extern struct |

So the disruption is 17 fixture sites and nothing else.

## The rules, as they stand

- **Minting** a view *from a container* — gated. This is the only new restriction.
- **Deriving** a view *from a view* (`v.slice(...)`) — free. Same window; `parallel_for` depends on it.
- **Passing** a view to a callee — free, in any direction that cannot outlive the window. A callee's frame
  is strictly shorter than its caller's, and the existing escape rules already stop a view being stored or
  returned, so **the free-standing `View<T>` parameter survives unchanged** — which is most of the real
  usage (`read(into: View<uint8>)` across `std::net`/`io`/`fs`, all of `sort`).
- **Storing** — still forbidden, with one proposed relaxation (spike D).

Gating the *mint* rather than checking the *use* is what closes the cross-function case. Finding ⑥ was
`bad(d: ref d, v: d.view())`; inside a borrow block `d` is unnameable, so `ref d` is rejected where you
would write it, and no depth of indirection reconstructs the pair — you would still have to name the
container inside the block to start the chain. This is also why lexical beats a call-site root check:
shadowing cannot rot into dead code the way finding ⑧'s guard did.

## What this closes, and what it does not

Closes, from the eleven: **③** (no free `View.make` — views only come from a `Viewable`), **④** (nothing
outlives the block to reseat onto), **⑤**, **⑥**, and **⑧**'s guard becomes unnecessary rather than dead.

⚠️ **Does not close, and must not be assumed to:** **①/②** need `UnsafePtr` containment, which is separate
work. **⑨** (one `unsafe` block disables definite assignment for the whole function) is **not** covered and
**must land with or before containment** — containment pushes more code inside `unsafe` blocks and would
otherwise widen it. **⑩** (uninstantiated generic bodies unchecked) and **⑪** (silent narrowing cast) are
unrelated.

## Open spikes — the tuning still to do

**A. How does a type opt in?** The working sketch, and it hangs together:

- A view has **one ctor, taking the `Viewable` host** — never a raw pointer and a length. That is what kills
  the forged-view OOB (finding ③) *by construction* rather than by a `friend` grant patched onto a leaky
  ctor. `View.make(UnsafePtr, int32)` simply ceases to exist.
- **`Viewable` tags the host and grants the view access to nominated private fields.** kama already has the
  mechanism — `friend Type { members };` — so this is a contract that implies a friend grant, not new
  machinery.
- **`Viewable` carries the minting call** in its interface (`fn View<T> view()`), so "how you get a view" is
  one named thing. It must resolve *structurally*, not through contract dispatch, or it boxes — `foreach`
  already does exactly that for `iterator()`/`iterMut()` (`emitForeachIterator`), which is the precedent.

Two consequences to settle:

1. **The view's ctor touches the host's `UnsafePtr` fields**, so it must be a private `unsafe fn` under
   [unsafe-seam.md](unsafe-seam.md). That is the correct coupling — minting a window *is* the unsafe act,
   performed once, in one audited place, instead of 25 raw fields scattered across 18 types.
2. **Do NOT parameterise the view by its host** (`View<T, Host>`). Binding the host into the type would give
   "which" for free, but it infects every signature — `read(into: View<uint8>)` across `std::net`/`io`/`fs`
   and all of `sort` would have to name a host — and pushed to soundness it converges on lifetimes with
   extra steps, which GOALS §3e declines. Keep `View<T>`: `Viewable` supplies *no forging*, lexical scope
   supplies *how long*. That division is the whole design.

**B. Temporary, or block-only?** This single question decides the corpus cost. If `.view()` survives as a
statement-scoped temporary in argument position, **all 12 non-test sites are unaffected** and only local
bindings need a block. If `borrow` is the sole path, those 12 move too. Recommend allowing the temporary;
its window is provably the statement.

> **ECS settles this.** [tests/ecs_pattern.kama](../../tests/ecs_pattern.kama) is the data-oriented shape
> the language is meant to carry, and every one of its three system call sites is already a temporary:
> ```kama
> fn void integrate(View<Transform> xs, float32 dt) { foreach (ref Transform t in xs) { … } }
> integrate(xs: xf.view(), dt: 2.0f32);          // :74
> integratePar(xs: xf.view(), dt: 0.0f32);       // :75  — parallel_for inside
> tickAll::<Timer>(items: ts.view());            // :81  — generic over a contract bound
> ```
> A system is a free function over a `View<T>` parameter, and `foreach`/`parallel_for` inside it is already
> a lexical block. **With temporaries allowed, ECS changes by zero lines.** Without them, the idiom becomes
> nested `borrow` blocks — and a multi-component SoA system
> (`movement(t: transforms.view(), v: velocities.view())`) would need one level of nesting per component
> array, which is the shape that gets written most often in a real engine.

**C. How does the compiler mint one?** `borrow d as v { … }` must make `d` unusable inside the block. Three
candidate mechanisms — shadow the name outright, mark the binding `const` for the extent, or mark it
*borrowed* with a dedicated diagnostic. The third gives the best error message; the first is the least code.
Also open: does `foreach` become sugar for `borrow`, or stay a parallel construct?

**D. Composition — "view-in-view" was the wrong framing; it is NOT an independent decision.** Dropped as a
standalone item.

Ask what the nested view windows. In `MyIter<T> { View<T> src; int32 pos; }`, `src` is *not* a window into
`MyIter` — `MyIter` is not a thing one can window. Both are windows onto the same container. So the field is
not a nested view at all: **it is the outer view's representation**, and "an iterator is a view plus a
cursor" already said that. Calling it composition conflated *being* a window with *how a window is
implemented*.

Genuine nesting does exist and is already allowed: `v.slice(...)` is a sub-window whose extent is contained
in its parent's, returned as a value rather than stored.

So the question collapses into spike A: **is `View<T>` itself `Viewable`?** It arguably already is — it
hands out `ViewIter`/`ViewIterMut` and implements `Iterable`/`IterableMut` — in which case `ViewIter`
windows a `View`, which transitively windows the container, and the chain of custody is well-formed. Decide
it there, not separately.

The measurement stands and still argues against a refactor: it would fit only ~9 of the 18 iterators
(`Map`/`SlotMap` hold parallel arrays sharing one `cap`, so views would store the length 2–3×; `Deque` is a
ring buffer a contiguous view cannot model), each of the 9 drops one pointer rather than all of them
(`modsp` points at a single `int32`, not a range), and `next()` currently skips bounds checking because the
loop already proved `pos < len` — `View.operator[]` would re-check. `-O3` will likely fold that, debug will
not. **Measure on the `foreach` path before converting anything**, since
`tools/check-ecs-zero-dispatch.sh` guards that loop.

**E. What does a library author write instead of a raw pointer?** The 25 borrowed raw-pointer fields are the
honest measure of what the model is missing. Spike D covers ~9; the parallel-array and ring-buffer cases
need an answer that is not "keep using `UnsafePtr`", or an explicit decision that they stay unsafe internals.

**F. Does anything legitimately want a view to outlive a block?** The corpus says no (table above), and the
two workloads most likely to break it do not:

- **ECS** — see spike B. Systems take a `View<T>` *parameter* and iterate it inside `foreach`/`parallel_for`;
  the owning `DynamicArray`s live in the world, not the systems. That is the standard architecture (Bevy
  hands a system its queries per run), so the model matches rather than fights it. Archetype/chunk
  iteration is `slice()` — deriving from a view, which is free.
- **WebGPU** — [examples/webgpu/triangle.kama](../../examples/webgpu/triangle.kama) holds **no kama `View`
  at all**. GPU work is FFI work: `wgpuQueueWriteBuffer(data: cast<UnsafePtr>(addr(of: angle)), size: 4)`
  hands C a raw address plus a length. Under `UnsafePtr` containment that line moves inside `unsafe { }`,
  which is the correct marking — it is a genuine, short-lived FFI hand-off, and a good illustration that
  containment marks such code rather than banning it. The place a view *would* appear is a mapped buffer
  range, which this example never uses; if it ever does, map/unmap is **inherently** a lexical window, so
  the design fits it.

The residual risk is an **async** mapped range — a window opened by a callback and closed later, which no
lexical block can span. kama has no async/await today, so it does not arise; re-ask if one lands.

## Definition of done

GOALS §3c states which of (a)/(b)/(c) kama picked, and the rules match it. SPEC gains a **Views** section
with the window model and the mint/derive/pass/store table. Every negative claim has an `xfail` fixture —
including the ones that should already exist for the raw-pointer `unsafe` gates. Then this file is deleted.
