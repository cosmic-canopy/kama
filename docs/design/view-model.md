# The view model — finishing `type view` (in-flight design)

*In-flight design doc. **Delete this file when the view work ships**, once GOALS §3c + SPEC carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> Written out of the safety/unsafe boundary spike (`0addb7c`). Every number below was measured, not
> estimated; the eleven findings it refers to are in [ROADMAP.md](../ROADMAP.md) §2. Companion briefs:
> [contract-kinds.md](contract-kinds.md) (**a prerequisite** — `Viewable` and the iterator contracts
> cannot be spelled without it) · [unsafe-seam.md](unsafe-seam.md).

## The model, in one paragraph

**A view is a window into another type.** It owns nothing, destroys nothing, and it is only meaningful
while the window is open. It does not exist independently — it is always *paired* with the thing it
looks into. **A borrowing iterator is a view**: not by analogy but literally, since `b743f80` made all
17 of them `type view`. An iterator is a window plus a cursor.

(A *generating* iterator — `Args`, `IntRange` — borrows nothing and is correctly a `type value`. The
distinction matters when spelling the contracts; see [contract-kinds.md](contract-kinds.md).)

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

**The decision: kama picks (c).** `foreach` already *is* (c) — it scopes an iterator lexically.
Generalising that to the whole kind is not a new concept, it is making the two members of one kind behave
the same way.

## Why (c) is nearly free *for kama specifically*

Scored against Rust's four borrow abilities:

| Rust ability | lost? |
|---|---|
| return an interior reference | **No** — kama already allows the useful subset, rooted at `this` or a `ref` param. That is `d.view()`, `v.slice()`, `operator[] -> ref T`. |
| relate two lifetimes in one signature | **No measurable loss** — needs a returned borrow derived from one of two params. Zero demand in the corpus. |
| disjoint mutable borrows (`split_at_mut`) | **No** — served by built-ins: `parallel_for` splits into disjoint sub-views internally, plus `View.swap` / `sort`. |
| **store a borrow in a struct** | **Yes — and it is already lost.** `view_field` forbids it. |

kama has *already paid* Rust's expressiveness price via those shipped `xfail` fixtures; it simply has not
been collecting Rust's guarantee in return. Going lexical does not take expressiveness away — it takes away
the *workaround*.

And the workaround is measurable: the **18 `type view` declarations** in `lib/`+`prelude/` carry **37**
`UnsafePtr` fields, of which **11 are `modsp` mutation-counter pointers and 26 are borrowed data
pointers**. That is "store a borrow in a struct", performed 26 times, with a raw pointer standing in for
the borrow the language will not let them name. Those fields are the same shape as finding ②.

> ⚠️ Earlier drafts of this file said "25 borrowed raw-pointer fields across 18 declarations", and
> [ROADMAP.md](../ROADMAP.md) has said 17, 18 and 19 `type view` declarations in different places. Settled
> by counting: **18 declarations = 17 borrowing iterators + `View<T>` itself**; the 19 was a grep counting
> a comment line in `view.kama`. And separating `modsp` from the data pointers is the useful half of the
> number, because a `modsp` points at a single `int32`, not a range, so it can never become a view.

## Measured impact of going lexical

| measurement | value |
|---|---|
| `.view()` / `.slice()` call sites | **59** — 48 `tests/`, 9 `lib/`, 2 `examples/` |
| non-test sites | **11** — **8 mints** (4 of them on a *field*) and **3 derives** from a `View<T>` parameter, which stay free |
| `View` **locals** outside `tests/` | **2**, both `sort.kama:115,117`, and both *derived from a `View<T>` parameter* |
| long-lived views in `lib/` / `prelude/` / `examples/` / `bench/` | **zero** |
| `foreach` sites | **141** (37 `lib/`, 2 `prelude/`, 100 `tests/`, 1 `examples/`, 1 `bench/`) + **9** `parallel_for` |
| `examples/webgpu` | holds **no kama `View`** — `WGPUStringView` is a C extern struct |

## The rules

- **Minting** a view *from a container* — requires a lexical window: `borrow`, `foreach`, or
  `parallel_for`. **There is no statement-scoped temporary.**
- **Deriving** a view *from a view* (`v.slice(...)`) — free. Same window; `parallel_for` depends on it.
- **Passing** a view to a callee — free. A callee's frame is strictly shorter than its caller's, so **the
  free-standing `View<T>` parameter survives unchanged** — which is most of the real usage
  (`read(into: View<uint8>)` across `std::net`/`io`/`fs`, all of `sort`).
- **Storing** — still forbidden. No relaxation.

Gating the *mint* rather than checking the *use* is what closes the cross-function case. Finding ⑥ was
`bad(d: ref d, v: d.view())`; inside a borrow block `d` is unnameable, so `ref d` is rejected where you
would write it, and no depth of indirection reconstructs the pair. This is also why lexical beats a
call-site root check: shadowing cannot rot into dead code the way finding ⑧'s guard did.

**No statement-scoped temporary, and the reason is not just uniformity.** An earlier draft recommended
allowing `integrate(xs: xf.view(), …)` on the grounds that "its window is provably the statement". The
window is — but the *container* is not protected for that statement, and finding ⑥'s
`bad(d: ref d, v: d.view())` **is a single statement**. Allowing the temporary would therefore have
required a second rule (no other mention of the mint's root anywhere in the same statement). Requiring the
block needs no such companion.

## Syntax

`borrow` is purely lexical — **zero codegen, zero runtime cost.**

```kama
// mint + name the window. `xf` is unnameable until the closing brace.
borrow xf as v {
    integrate(xs: v, dt: 2.0f32);
    sort(items: v.slice(from: 0, count: n));    // derive — free
    // xf.add(item: …);                          // ERROR: `xf` is borrowed by `v` for this block
}

// multi-binding: one level, not one per component array
borrow transforms as t, velocities as vel { movement(t: t, v: vel); }

// `foreach` IS a window — it names the ELEMENTS instead of the view
foreach (ref Transform t in xf) { … }
foreach (K k in map) { /* map.remove(key: k) — ERROR: `map` is borrowed by the loop */ }
```

Two consequences of a block being a *statement*, both benign:

```kama
// a value that must escape is an ordinary pre-initialized local.
// NOT a `slot` — SPEC rule 1: "Only an `out` argument fills a slot. Not an assignment."
int32 ticks = 0;
borrow ts as t { ticks = tickAll::<Timer>(items: t); }

// a `slot` DOES compose, when the fill is an `out` argument: SPEC rule 3 accepts a fill in
// "a nested block that always runs", and a `borrow` block is unconditional.
slot File f;
borrow d as v { openInto(src: v, dst: out f); }
```

And the block hoists out of a loop, so the common stdlib shape gets shorter rather than longer —
`pump()`, `streams.kama:52`:

```kama
borrow scratch as s {
    while (true) {
        Result<usize, IoError> rr = from.read(into: s);
        …
        Result<Unit, IoError> wr = writeAll(w: to, bytes: s.slice(from: 0, count: k));   // derive
        …                                                                                 // `return`
    }                                                                                     // from inside
}                                                                                         // is fine
```

## How a type opts in — `Viewable`

```kama
type contract Viewable<T> for value, resource, view, intrinsic { fn View<T> view(); }

type resource DynamicArray<T> implements Viewable<T> {
    UnsafePtr<T> data;  int32 len;
    public unsafe fn View<T> view() { return View.over(base: this.data, count: this.len); }
}
```

The `view()` member resolves **structurally**, not through contract dispatch, or it boxes — `emitForeachIterator`
(`kama.cemit.cpp:8366`) already does exactly that for `iterator()`/`iterMut()`, which is the precedent.

**Two compiler-checked facts carry the design. Neither is convention:**

1. **`View.over(base:count:)` is an intrinsic, not a ctor**, legal in exactly two positions: the body of a
   `Viewable<T>.view()` conformance, and `View`'s own `slice`. **`View` declares no ctor at all** —
   `View.make(UnsafePtr, int32)` ceases to exist — so a forged view is *unrepresentable* rather than
   discouraged. That is what actually kills finding ③, and it is needed precisely because under
   [unsafe-seam.md](unsafe-seam.md)'s model a public ctor would otherwise be callable from safe code.
2. **`.view()` may appear only as the subject of a `borrow`/`foreach`.** The member is public so `borrow` can
   resolve it; a direct `View<T> v = xf.view();` is the mint-outside-a-window error. The minting rule is
   enforced at the call site, not at the declaration.

### ⚠️ The `friend`-grant sketch is rejected — it cannot be implemented

An earlier draft proposed that "`Viewable` tags the host and grants the view access to nominated private
fields", reusing `friend Type[members];`. `friend` is real enforcement (four `xfail`s pin it), but **a
grant is useless unless the grantee can name the member, and `View<T>` cannot**:

```kama
public ctor over<H: Viewable<T>>(ref H host) { this.data = host.data; }
//                                                       ^^^^^^^^^ does not resolve
```

`H`'s only visible members are those its bound `Viewable<T>` declares, and `Viewable` is a **contract** —
public-only — so it may not declare an `UnsafePtr` member without violating containment. The decision *not
to parameterise the view by its host* is what makes friendship unimplementable here; there is no
`host.data` to grant. `friend` fits one named type reaching one named member, not a generic view over an
open set of hosts.

### Kind, and why it is not the constraint

`Viewable` is **`for value, resource, view, intrinsic`** — anything with storage may opt in. Not
`for resource`: that would exclude `View<T>` itself (a `type view`, and `Viewable` — which is how
`ViewIter` windows a `View`) and `string` (a `type intrinsic`). A `type value` with inline storage is a
legitimate host too — a `Vec4` viewed as `View<float32>` is a real engine idiom, and nothing unsound
follows, because the host is unnameable for the window's extent and so cannot be copied or moved while it
is open. `enum` is excluded: a tagged union's payload is not a contiguous run.

**The real constraint is that a `borrow` host must be a *place* that outlives the block** — a local,
field, parameter, or element, never a temporary (`borrow makeVec() as v { }`); precedent
`tests/xfail/addr_of_temporary.kama`. That holds at every kind, which is why the kind gate is not the
mechanism.

**Do NOT parameterise the view by its host** (`View<T, Host>`). Binding the host into the type would give
"which" for free, but it infects every signature — `read(into: View<uint8>)` across `std::net`/`io`/`fs`
and all of `sort` would have to name a host — and pushed to soundness it converges on lifetimes with extra
steps, which GOALS §3e declines. Keep `View<T>`: `Viewable` + the `View.over` intrinsic supply *no
forging*, lexical scope supplies *how long*. That division is the whole design.

## `foreach` is a borrow scope

**One model, two spellings.** `borrow` opens a window and gives it a **name**; `foreach`/`parallel_for`
open a window and give you its **elements**. They are not alternatives — you cannot substitute one for the
other — so requiring both would be saying it twice, and they compose: inside `borrow xf as v { … }`, a
`foreach (… in v)` is a derive.

The container becomes unnameable inside a `foreach` body. **That closes finding ⑧ by the same rule that
closes ④⑤⑥**, and *retires* the dead compile-time invalidation guard at `kama.cemit.cpp:17611` (which
requires `isIntrinsicColl`, admitting only `string`/`BindableFunctionPtr`/`InlineArray`, none of which has
an `add`) rather than reviving it.

Requiring `foreach` to *nest inside* a `borrow` was considered and rejected: every `foreach` over a
container mints (`map.iterator()`, `s.chars()`, `arr.iterMut()`), so it would cost **141 `foreach` sites +
9 `parallel_for`** and buy no safety, since `foreach` already has exactly the extent a `borrow` would give
it.

**`foreach` is NOT desugared into `borrow`.** It stays a parallel construct sharing the model, so
`emitForeachIterator`'s monomorphized zero-dispatch path is untouched — `tools/check-ecs-zero-dispatch.sh`
guards it.

## Making the host unnameable

`borrow d as v { … }` marks the binding **borrowed**, with a dedicated diagnostic — not shadowing, not
`const`. The error message is the entire UX of this feature: *"`xf` is borrowed by `v` for this block"*
beats *"unknown identifier `xf`"*.

**Field mints need `const fn`, which makes it a hard prerequisite rather than an independent item.** Four
of the eight non-test mints are on a field — `binary.kama:67`, `json.kama:55`, `streams.kama:180`,
`streams.kama:212`. Banning the *place* `this.buf` does not stop `this.someMethod()` from reallocating it,
so the rule is: **inside `borrow this.f as v`, only a `const fn` may be called on `this`.** That is the
`const fn` item in [ROADMAP.md](../ROADMAP.md), and this work depends on it.

There are no `ref` locals in kama, so for a plain local the block is airtight with no further rule: there
is no way to have pre-made an alias to the host.

## ECS, and what the corpus says

[tests/ecs_pattern.kama](../../tests/ecs_pattern.kama) is the data-oriented shape the language is meant to
carry. A system is a free function over a `View<T>` parameter with `foreach`/`parallel_for` inside — that
part is unchanged, and it is the majority of the code. What changes is the three call sites, which gain a
block:

```kama
borrow xf as v {
    integrate(xs: v, dt: 2.0f32);         // was :74
    integratePar(xs: v, dt: 0.0f32);      // was :75  — parallel_for inside
}
```

The owning `DynamicArray`s live in the world, not the systems — the standard architecture (Bevy hands a
system its queries per run), so the model matches rather than fights it. Archetype/chunk iteration is
`slice()`, which is a derive and therefore free.

**WebGPU** holds no kama `View` at all: `wgpuQueueWriteBuffer(data: cast<UnsafePtr>(addr(of: angle)),
size: 4)` hands C a raw address plus a length, and under the unsafe seam that line's enclosing function
becomes `unsafe fn` — the correct marking for a genuine, short-lived FFI hand-off. The place a view *would*
appear is a mapped buffer range, which map/unmap makes **inherently** a lexical window.

The residual risk is an **async** mapped range — a window opened by a callback and closed later, which no
lexical block can span. kama has no async/await today, so it does not arise; re-ask if one lands.

## What this closes, and what it does not

Closes, from the eleven: **③** (no forgeable `View.over` — views only come from a `Viewable`), **④**
(nothing outlives the block to reseat onto), **⑤**, **⑥**, and **⑧** (retired, not revived).

⚠️ **Does not close, and must not be assumed to:** **①/②** need the `UnsafePtr` naming rule, which is
[unsafe-seam.md](unsafe-seam.md). **⑨** is that brief's too. **⑦** (`reserve()` reallocs without bumping
`mods`) is *de-fanged* here — the container is unnameable during iteration, so the exploit path closes
lexically — but the counter is still simply wrong, and it remains a real stdlib fix covering what the
lexical rule cannot see. **⑩** (uninstantiated generic bodies unchecked) and **⑪** (silent narrowing cast)
are unrelated.

## The raw-pointer fields that stay

The 26 borrowed data pointers are the honest measure of what the model is missing, and the answer is
explicit: **they stay unsafe internals.** The field *declaration* is legal per the unsafe seam's table;
every *touch* moves inside an `unsafe fn`. `Map`/`SlotMap` hold parallel arrays sharing one `cap` and
`Deque` is a ring buffer, so neither is expressible as a contiguous view — that is a property of the data
structures, not a gap in the language.

Converting the ~9 iterators that *could* hold a `View<T>` instead is **explicitly out of scope**: each
drops one pointer rather than all of them, and `next()` currently skips bounds checking because the loop
already proved `pos < len`, where `View.operator[]` would re-check. `-O3` will likely fold that; debug will
not. **Measure on the `foreach` path before converting anything**, since
`tools/check-ecs-zero-dispatch.sh` guards that loop.

## Definition of done

[contract-kinds.md](contract-kinds.md) has shipped (this cannot be spelled before it). GOALS §3c states
that kama picked (c) and the rules match it. SPEC gains a **Views** section with the window model and the
mint/derive/pass/store table. Every negative claim has an `xfail`: mint outside a `borrow`/`foreach`;
naming the host inside a `borrow`; mutating the container inside a `foreach` body (finding ⑧'s missing
fixture); calling a non-`const fn` on `this` inside `borrow this.f as v`; `View.make` no longer existing;
`View.over` outside a `Viewable.view()` body; a `Viewable` conformance whose `view()` is missing; and
`borrow` over a temporary. Then this file is deleted.
