# The view model — where a view may be born (in-flight design)

*In-flight design doc. **Delete this file when the view work ships**, once GOALS §3c + SPEC carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> **Read this before the git log.** Every claim below was **probed against a built compiler**, with the
> command recorded. That is deliberate: the previous revision of this file specified a design that could
> not be built (`View.over` as an intrinsic, `View` declaring no ctor, a `Viewable` that names `View<T>`
> from the prelude), and each of those died on contact. A doc is not evidence — so this one shows its work.

## Status

| | |
|---|---|
| **Shipped — the *window*** | `borrow` opens a lexical extent; a **place** is a base plus a field chain and two places conflict iff one is a prefix of the other; overlapping `ref`/`out` arguments rejected; one `borrow` may not bind the same place twice. `a6545dc` `60ef122` `6abbb1a` |
| **Shipped — escape-check soundness** | a view ctor's borrow is matched **by argument name**, not by position; a chained derive roots where its receiver roots. `8b6d4e6` |
| **In flight — the *mint*** | this document: where a view may be born |

The window answers *how long*. The mint answers *from what*. Both are needed for
[GOALS.md](../GOALS.md) §3c to mean anything.

## What the probes established

Run these before changing any of it; they are cheap and they are the whole basis of the design.

**1. Safe kama already cannot forge a view.** Every route to an `UnsafePtr` is gated — including the
indirect one, which is the interesting case:

```kama
fn int32 main() {                                     // NOT unsafe
    DynamicArray<int32> a = DynamicArray.withCapacity(capacity: 2);
    View<int32> v = View::<int32>.make(data: a.dataPtr(), len: 999999);
}
// error: this call's result is a raw pointer, so it requires an `unsafe fn`
```

So the forge is unreachable from safe code. The type-keyed containment rule
([SPEC](../SPEC.md#what-requires-an-unsafe-fn--the-decision-table)) holds.

**2. But an `unsafe fn` can hand a bogus view *to* safe code, and its signature looks safe.**

```kama
unsafe fn View<int32> window(ref DynamicArray<int32> a) {
    return View::<int32>.make(data: a.dataPtr(), len: 999999);   // only the LENGTH is a lie
}
fn int32 main() {                                   // safe — no `unsafe` token below this line
    View<int32> v = window(a: ref a);
    foreach (int32 x in v) { … }                    // walks 999999 elements of a 1-element buffer
}
```

**This cannot be closed by any rule.** Compare the two signatures:

```kama
public unsafe fn View<T> view()                          // DynamicArray — legitimate
unsafe fn View<int32> window(ref DynamicArray<int32> a)  // the forge
```

Structurally identical — raw memory in, safe wrapper out, pointer rooted in something that outlives the
call. The only difference is whether the length is truthful, which no type system without lifetimes can
check. Rust has the same property: `Vec::as_slice` is a *safe* function with `unsafe` internals, and a
buggy one returns a bad slice with no marker anywhere.

**3. A private ctor plus `friend` already rejects an ungranted minter** — shipped machinery, zero
compiler work, verified with generics at both ends:

```kama
type view Cur<T> implements Iterator<T> {
    friend Bag[make];
    unsafe ctor make(UnsafePtr<T> d, int32 n) { … }        // private
}
unsafe fn Cur<int32> forge(UnsafePtr<int32> p) { return Cur::<int32>.make(d: p, n: 999999); }
// error: 'make' is private in 'Cur<int32>'
```

⚠️ This does **not** contradict the *"`friend`-grant sketch is rejected"* finding in the previous
revision. That rejection was about a different direction — a generic `View.over<H: Viewable<T>>(ref H host)`
reaching into the **host's** private `data` field, which is unimplementable because `H`'s only visible
members are those its bound declares. Here the grant runs the other way: the **view** grants access to
**its own** ctor. Both findings stand.

## The goal, stated honestly

Forging cannot be eliminated — probe 2 settles that, and it is a property of the unsafe surface, which is
dangerous by design, deliberately small, and greppable at declarations. So the goal is:

> **Make every mint an explicitly granted, greppable privilege, and keep the trusted set to the types
> that own the memory.**

That is an *auditability and generality* win, not a soundness win. [GOALS.md](../GOALS.md) §3c's promise
that a view "can never dangle" overclaims and must be restated as **"safe kama cannot originate a
dangling view; the trusted boundary is the `unsafe fn` set, greppable at declarations"** — still a
stronger claim than C or C++ can make.

## The model — one idea

> A `type view`'s constructor is **implicitly private**. Minting is a **grant**, carried by a contract
> marked `@viewable`: implementing a member of such a contract lets that member's body mint the view it
> returns.

Implicitly private is what makes this *hard to use incorrectly*. A view author writes no `friend` list
and cannot forget to close the door; omitting the contract fails **closed** — the ctor is simply
unreachable — rather than silently leaving a forge open.

```kama
@viewable type contract Viewable<V> for value, resource, view, intrinsic { fn V view(); }

type resource DynamicArray<T, A> implements …, Viewable<View<T>> {
    public unsafe fn View<T> view() { return View::<T>.over(at: this.data, count: this.len); }
}
```

- **The grant:** inside a method implementing a member of a `@viewable` contract, the view type that
  method returns may be minted.
- **The bidirectional half:** a view type mintable this way may mint **itself** anywhere in its own
  methods. That is what makes `View.slice` — and a future `split`/`chunks`/`first` — work with no
  extra rule.
- **`V` must be a `type view`.** kama has no kind bounds (a `type_param`'s `bounds` is a list of
  *contract* names, `kama.y:769`), so this is a check on the marked contract using `implementerKind`
  (`cemit.cpp:18524`), which already maps `ClassInfo::isBorrow` → `IK_View`.
- **Contract-level marking is sufficient** — the return type self-selects, so a member returning `int32`
  mints nothing and a whole-contract mark cannot over-grant. Member-level (`@viewable fn V view();`)
  stays available if a future contract needs the precision.

### Why the grant rides on a contract

Two rejected alternatives, recorded so they are not re-proposed:

- **Bless `View` by name in the emitter** (the previous revision's design). It does not generalize: a
  user's own `type view DmaSpan` has the identical forge and no blessing helps, so the compiler accretes
  one special case per view type — 16 of them in the stdlib alone.
- **A per-method `@viewable` marker.** Tempting because it covers every mint with no new contracts, but
  it makes the capability invisible to the type system: you cannot write a generic over "things that
  hand out a values iterator". Contracts are how kama declares capability, and the mint is a capability.

## Coverage — 16 of 16, and the gaps are real

Surveying every mint site in `lib/std` + `prelude` found 23 calls across 16 view types. **Eleven sat in
methods implementing no contract member** — which is not a flaw in the rule but a set of capabilities the
stdlib never declared:

| mint | today | contract |
|---|---|---|
| 9 iterators via `iterator()`/`iterMut()` | `Iterable<T>` / `IterableMut<T>` | mark the two `@viewable` |
| `Map.values()`, `Map.valuesMut()`, `SlotMap.values()`, `SlotMap.valuesMut()` | plain methods | new `ValuesIterable` / `ValuesIterableMut` — **two implementers each on day one** |
| `Map.entries()` | plain method | new `EntriesIterable`; `SortedMap` is the natural second |
| `BitSet.setBits()` | plain method | **`Iterable<int32>`, renamed `iterator()`** — no new contract |
| `View<T>` | public `View.make` | new `Viewable<V>` — the open set |

These are exported, fixture-covered public API (`tests/map_values.kama`, `tests/map_entries.kama`,
`tests/slot_map.kama`, `tests/bit_set.kama`), not implementation details. `BitSet`'s own comment says it
*"is not `Iterable`"* — an omission rather than a decision, since yielding set-bit indices is the obvious
iteration and the `int32` element type already rules out the C#/C++ "one `bool` per position" reading.

⚠️ `Set.iterator` (`set.kama:50`) **returns** a `MapKeyIter` by delegating to `this.m.iterator()`. It does
not mint, so it needs no grant — the rule must key on **calling the ctor**, never on returning a view.

## What this does NOT change

**The four `foreach` nominal gates stay** (`Iterable`/`IterableMut`/`Iterator`/`IteratorMut`,
`cemit.cpp:8602-8644`). They answer a different question — *"is this type declared iterable?"*, the
protocol opt-in that keeps `foreach` nominal instead of duck-typed — where `@viewable` answers *"may this
method construct this view?"*. Marking `Iterable` `@viewable` grants the mint to `iterator()`; it says
nothing about who may be `foreach`ed.

Keeping those four hardcoded is principled, and the distinction is exactly what condemns the old design:
`Iterable`/`Iterator` are the **language's own** protocol contracts — `foreach` is syntax, so what it
lowers onto is language-level, like `Deref` for `.` forwarding, `HeapOwner` for `new`, `Format` for
interpolation. `View` is a **library type**; hardcoding *that* was the mistake.

**Generator iterators stay grant-free.** The mint gate applies to `type view` only. An iterator that owns
its state — `type value`/`type resource` implementing `Iterator<T>` — borrows nothing, can dangle
nothing, and stays constructible anywhere. Already proven in tree: `Args` (`prelude/global.kama:382`,
`foreach`'d at `tests/args_env_empty.kama:17`), `IntRange` (`tests/gencontract_value.kama`), `VecIter`
(`tests/iter_vec.kama`).

## Milestones

### M1 — the attribute and the gate

`@viewable` recognized on a `type contract` (attributes on `type` declarations already parse,
`kama.y:685` — no grammar change); a marked contract's view-returning members validated to return a
`type view`; a `type view`'s ctor becomes implicitly private; the gate itself at `emitDotOnTypeCtorCall`
(`cemit.cpp:17764`), the single choke point both `X.make(…)` and `X::<T>.make(…)` route through.

Fixtures: `xfail/view_ctor_forge`, `xfail/viewable_not_marked`, `xfail/viewable_arg_not_a_view`; positive
`viewable_user_type` — a user container minting its **own** `type view` through its **own** `@viewable`
contract, which is the fixture that proves the generality claim.

### M2 — declare the missing capabilities

Mark `Iterable`/`IterableMut` `@viewable`. Add `ValuesIterable`/`ValuesIterableMut`/`EntriesIterable` in
**`lib/std/collections/`, not the prelude** — `Iterable` is floor because `foreach` over a container is
language-level, but these are not (you `foreach` the returned iterator, never the container), and keeping
them in std is what stops the floor from growing. `BitSet implements Iterable<int32>`, `setBits()` →
`iterator()`: **source-breaking at 3 sites** (`tests/bit_set.kama:31`,
`tests/import_transitive_iter.kama:10`, and the doc comment at `bit_set.kama:151`).

Fold in a cheap consistency fix: iterator **exports are inconsistent** — `map`, `slot_map` and `bit_set`
export their iterator types; `dynamic_array`, `fixed_array`, `deque` and `view` do not.

### M3 — `View<T>` and the open set

`Viewable<V>` in `prelude/global.kama` beside the iteration contracts. `View.make` → private
`ctor over(at:count:)`. `DynamicArray`/`FixedArray` declare `implements Viewable<View<T>>`. Their `slice`
becomes a **derive** — `this.view().slice(from:count:)` — which `8b6d4e6` unblocked; keep each
container's own bounds check so the panic still names what the author called. `borrow`/`parallel_for`
become nominal on `Viewable`, mirroring the `IterableMut` gate at `cemit.cpp:8602`, placed **after** the
existing `view()`-resolution checks so `xfail/parfor_noncontiguous` still matches. Resolution stays
**structural** — the contract is a gate, never a dispatch, which is what keeps the ECS path zero-dispatch.

`Viewable` is **non-boxable** (a mint protocol, not a value) and emits **no vtable**.

### M4 — `Chars`/`Split` stop being compound literals

⛔ **They cannot leave the prelude.** `string` is floor and `foreach (char c in s.chars())` works with
**no import** — a stated invariant in [SPEC.md](../SPEC.md) and all four fixtures. Moving them breaks
`--no-std` outright (the prelude is embedded in `bin/kama`; `lib/` is absent) and breaks
`registerCollection`, which calls `synthId("Chars")` unconditionally for every program using a string.

⛔ **Writing `string.chars()` in kama is also blocked** — `string` cannot declare non-contract methods in
kama source, a contract-scoped method is not callable bare (`cemit.cpp:18343`), and there is no safe
byte-pointer accessor to write the body with. That is a language change, not a refactor.

✅ **Achievable and worth doing:** give them real private ctors and have the emitter call `Chars__make(…)`
instead of writing a **positional** C compound literal (`cemit.cpp:18283`). `{ data, len, 0 }` must match
the prelude's field order and nothing checks it — reordering a field in `global.kama:315` silently
miscompiles. `stableBorrow` and the wrapper-scope temp relocation (`cemit.cpp:8677`) **stay**: they solve
a different problem — a view over a string *rvalue* — and there is no general rule to fold them into.

### M5 — the shipping record

SPEC gains the mint model and the stated limit; GOALS §3c is restated per *The goal, stated honestly*;
[FLOOR.md](../FLOOR.md) gains `Viewable`; ROADMAP finding ③ is **restated, not deleted** —
unforgeable-by-accident, still length-trusting. Then delete this file.

## Traps — each one cost a probe

| trap | consequence if rediscovered late |
|---|---|
| **`base` is a reserved keyword** (`kama.l:386`) — `over(base:count:)` does not parse, at the parameter *and* the call site | a parse error inside `lib/std/`, so nearly every fixture fails |
| **`Viewable` must not name `View<T>`** if it is prelude-resident — `global.kama:371` records that "DynamicArray/View aren't collected this early" | the contract would not resolve at all |
| **`staticOnlyInterfaces` (`cemit.cpp:10496`) never fires for a generic instance** — it is the impl-block path, and generics skip `linkBases` | the zero-vtable work must go at the three consumers (`:15109`, `:18976`, `:18965`) |
| the **`"Viewable"` template key is the bare string** only because `global.kama` has no `namespace` | a namespaced contract silently matches nothing, failing every `borrow` in the corpus |
| `implements Viewable<View<T>>` — a contract over a nested generic instance — **is probed working**; one precedent exists (`map.kama:101`) | would otherwise look risky and get designed around |
| **`Set.iterator` delegates, it does not mint** | a rule keyed on "returns a view" grants the wrong set |
| `tests/fs_raii.kama` intermittently **deadlocks the wasm leg** at 0% CPU (reproduced twice, same fixture) — pre-existing and unrelated | do not diagnose it as fallout from this work |

## Verification

`./dev matrix` at every commit. Run once into a file, then read the file — and note that
`./dev matrix > /tmp/m.log` sends **all** output there, so the harness's own task file stays empty and
looks dead when it isn't. Every `xfail` must be rejected by `kama check` as well as `build`
(`run_tests.sh:583`). The zero-vtable claim is evidenced by transpiling `tests/ecs_pattern.kama` before
and after and diffing **empty** — a comment is not evidence.
