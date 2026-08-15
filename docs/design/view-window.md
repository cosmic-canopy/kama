# The view window — where a minted view may live (in-flight design)

*In-flight design doc. **Delete this file when the work ships**, once SPEC + GOALS carry the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> **Read this before the git log.** Every claim below was **probed against the built compiler on
> 2026-08-15**, with the command recorded. That is not ceremony — the *previous* campaign shipped a row
> asserting it closed findings ④⑤⑥, and it closed none of them, because the "closes" column of a plan got
> copied forward as a result. This file shows its work so that cannot happen twice.

## Status

| | |
|---|---|
| **Shipped — the mint** | a `type view`'s ctor is private; minting is a grant carried by a `@viewable` contract; `main` may not be `unsafe`; a mint protocol emits no vtable. `3f45d89`…`ede7dac`. Closes ③ (restated) and retires ⑧ |
| **Shipped — the window, partially** | `borrow` opens a lexical extent; a **place** is a base plus a field chain, conflicting iff one is a prefix of the other; overlapping `ref`/`out` arguments rejected. `a6545dc` `60ef122` `6abbb1a` `8b6d4e6` |
| **OPEN — this document** | `borrow` is **opt-in**, so none of ④⑤⑥ is closed |

## What the probes established

### 1. All three findings are live heap-use-after-frees

Built with `--cc "clang -fsanitize=address -fno-omit-frame-pointer -g"`; all three report
`ERROR: AddressSanitizer: heap-use-after-free`.

```kama
// ⑤ resize invalidation
View<int32> v = d.view();
d.add(item: 3); d.add(item: 4); d.add(item: 5);   // reallocs out from under `v`
return v[0];

// ④ reseat through a `ref View<T>` parameter (crosses a function boundary)
fn void reseat(ref View<int32> v, ref DynamicArray<int32> src) { v = src.view(); }
{ DynamicArray<int32> inner = …; reseat(v: ref v, src: ref inner); }   // `inner` dies here
return v[0];

// ⑥ aliasing in one argument list
fn int32 bad(ref DynamicArray<int32> d, View<int32> v) { d.reserve(n: 64); return v[0]; }
return bad(d: ref d, v: d.view());
```

**Every one of them rides the same spelling: a `View<T>` held as a plain local or parameter.** `borrow`
already makes each of them expressible *safely*; nothing requires it.

### 2. The migration surface is 19 locals, and only 2 are outside `tests/`

```sh
grep -rnE '^\s*(const )?View<[^>]+>\s+[a-zA-Z_][a-zA-Z0-9_]*\s*=' --include='*.kama' .   # -> 19
```

17 in `tests/`; **2 in `lib/std/collections/sort.kama:115,117`**. Nothing in `prelude/`, `examples/`,
`bench/`. This is a *small* source-breaking change — much smaller than the mint's was.

### 3. ⚠️ `borrow` CANNOT express the two `sort.kama` locals

This is the load-bearing finding, and it is why "make `borrow` mandatory" is not implementable as stated.

`borrow_binding` is `primary_expression AS IDENTIFIER` ([kama.y](../../src/kama.y)), and the emitter
**always calls `host.view()`** on the bound expression. So a *derive* cannot be bound:

```kama
borrow v.slice(from: 1, count: 2) as w { … }
// error: `std::collections::View<int32>` has no nullary `.view()`, so it cannot be borrowed
//        — a `borrow` host must be a container that can hand out a `View<T>`
```

And `sort.kama`'s locals are exactly that shape — a derive from a by-value view **parameter**:

```kama
fn void intro<T, C: Order<T>>(View<T> items, ref C by, int32 depth) {
    View<T> left  = items.slice(from: 0, count: store);              // :115
    View<T> right = items.slice(from: store + 1, count: n - store - 1);  // :117
```

These are **already safe**: `items` is a by-value parameter, so its root outlives the frame, and there is
no nameable container in scope to mutate. A rule that rejects them would be rejecting the safe case.

### 4. Zero `ref View<T>` / `out View<T>` parameters exist

```sh
grep -rnE '(ref|out) (const )?View<' --include='*.kama' .   # -> NONE outside xfail
```

Finding ④'s cross-function form has **no legitimate user in the corpus**, so banning it is free.

### 5. ⑥'s mechanism is precise and narrow

`checkArgOverlap(placePath(argExpr), p.name)` runs **only** for `p.byRef && !p.isConst`
([kama.cemit.cpp:11506](../../src/kama.cemit.cpp)). In `bad(d: ref d, v: d.view())` the second argument is
a *by-value* `View<int32>`, so no check runs on it — and `placePath(d.view())` returns `{}` anyway,
because an invocation "is not a place" ([:11904](../../src/kama.cemit.cpp)).

So the existing place machinery is right; it simply never sees the view argument as a borrow of `d`.

## The reframe — one row, three separable pieces

"Make `borrow` mandatory" is a single line in the ROADMAP and three changes of very different cost. Two
are cheap and independent; only the third is source-breaking.

| | piece | closes | cost | source-breaking |
|---|---|---|---|---|
| **W1** | reject `ref` / `out` parameters of view type | ④ (cross-function form) | tiny — zero corpus uses | no |
| **W2** | an argument that is a view **rooted in** another argument's place conflicts with it | ⑥ | small — a `viewRootPlace()` helper feeding the existing `checkArgOverlap` | no |
| **W3** | **the window rule** — a view local's root must already be lifetime-bounded | ⑤, ④ (intra-function) | the real work | **yes** |

W1 and W2 can land first, each green alone, each with its own `xfail`. They also shrink W3: with ④'s
cross-function form gone, W3 only has to reason within one frame.

### W3 — the rule, as the probes shape it

Not *"every view local must be `borrow`-bound"* — probe 3 shows that would reject the safe `sort.kama`
case with no legal spelling available. The rule the evidence actually supports:

> A local of `type view` is legal iff its **root is already lifetime-bounded**: a `borrow` binding, or a
> by-value view parameter. A view minted from a **nameable container** is not bounded, and must go
> through `borrow`.

Checked against all 19 sites:

- `View<int32> v = d.view();` — root is the container `d`, nameable and mutable ⇒ **must become `borrow`**
  (the 17 `tests/` sites, and the shape all of ⑤ rides)
- `View<T> left = items.slice(…);` — root is the parameter `items`, already bounded ⇒ **stays legal**
  (the 2 `sort.kama` sites)
- `View<int32> mid = whole.slice(…);` where `whole` came from `borrow xs as whole` ⇒ **stays legal**, so
  `borrow` needs no derive-binding form (`tests/sort_slice.kama:10`)

The derive-rooting machinery already exists — `8b6d4e6` made a chained derive root where its receiver
roots — so W3 is a rule over information the emitter already computes, not new analysis.

**Open, decide with a probe not an argument:** whether a view local rooted in a by-value view *parameter*
should also require the caller to have borrowed. It cannot dangle within the frame, but the *caller* may
have minted it unbounded. If W3 makes every container-mint go through `borrow`, the caller cannot — which
would make this question answer itself. Verify that before adding a rule for it.

## Milestones

1. **W1** — reject `ref`/`out` view parameters. `xfail/view_ref_param`. Green alone.
2. **W2** — view-root aliasing in argument lists. `xfail/view_arg_aliases_ref` (finding ⑥'s exact repro).
   Green alone.
3. **W3** — the window rule **plus** its migration in one commit (sweep-then-land: the rule and the 17
   fixtures it would reject land together, or the tree red-lines). Fixtures: `xfail/view_local_unbounded`
   (⑤'s repro), positive `view_derive_from_param` pinning that `sort.kama`'s shape stays legal.
4. **The record** — SPEC's `View<T>` section gains the window rule; GOALS §3c is revisited (⑤ closing
   strengthens what safe kama can claim); ROADMAP row 1 is deleted and ④⑤⑥ struck in §2. Delete this file.

## Traps — each already cost something

| trap | consequence |
|---|---|
| **`borrow` always calls `host.view()`** — it cannot bind a derive | a rule phrased as "everything must be `borrow`-bound" has no legal spelling for `sort.kama` |
| **A green `./dev matrix` proves nothing here.** ④⑤⑥ survived the entire mint campaign, all three legs, 34 guards | the ASan leg runs, but only over the fixtures that EXIST — and no fixture held a view across a mutation. Write the `xfail` **first** |
| the ⑥ repro needs `reserve(n:)`, not `reserve(capacity:)` | a wrong argument name reads as "the rule caught it" |
| `placePath` returns `{}` for an invocation, and `{}` never conflicts | a `viewRootPlace` that forgets to unwrap the receiver silently checks nothing |
| the prelude is compiled INTO the binary | `./dev build` after any `prelude/global.kama` edit |

## Verification

- `./dev matrix > /tmp/w<N>.log 2>&1; tail -5 /tmp/w<N>.log`, then grep the same file.
- **Every fixture in this campaign must also be run under ASan**, not just built:
  `kama build f.kama -o f --cc "clang -fsanitize=address -fno-omit-frame-pointer -g"`. The three repros in
  probe 1 are the acceptance test — each must stop compiling, and that is a stronger claim than a green
  suite, which they already passed.
- Each `xfail` must be rejected by `kama check` as well as `build` (`run_tests.sh`).
