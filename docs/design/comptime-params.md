# Where compile-time arguments live — a deferred design

*Design doc for a decision, not for work in flight. It records a question that must be answered before
the 1.0 tag because every option is source-breaking, together with the measurements that say it is not
urgent. **Delete this file when the decision is made** and put the outcome in
[SPEC.md](../SPEC.md); if the answer is "no change", the reasoning moves to
[ROADMAP_DETAIL.md §2](../ROADMAP_DETAIL.md#s2) as a declared non-goal.*

> **Status.** Change **A shipped** — `const N: int32` is now `comptime N: int32`. Changes **B** and
> **C** are deferred, and the evidence in §4 argues they may never be needed. Nothing is blocked on
> this: the SIMD campaign that raised it compiles identically either way.

---

## 1. The question

kama has two axes for compile-time work, and they are spelled with two keywords:

| axis | spelling | what it is |
|---|---|---|
| compile-time **arguments** | the generic parameter list `<…>` | entries are **type** parameters or `comptime` **value** parameters |
| compile-time **execution** | `comptime` | `comptime fn` runs at build time; `comptime` constants bake its result; `comptime assert` checks an invariant per instantiation |

Zig fuses the two: a type is an ordinary comptime *value* of type `type`, so `fn max(comptime T: type,
a: T, b: T) T` **is** Zig's generics, and there is no `<…>` syntax at all. Rust splits them the way
kama does — `<T>` and `<const N: usize>` in one parameter list, and no comptime parameters.

Both are coherent. The question is whether kama should move part of the way toward Zig.

## 2. The three changes, which are separable

They are usually proposed together and have very different merits.

### A — rename `const N: int32` → `comptime N: int32` ✅ SHIPPED

A rename that removes an inconsistency rather than adding a mechanism. `const` had carried **three**
unrelated meanings: a compile-time value parameter, an immutable local/class constant, and `const fn`
const-correctness. `comptime` carried exactly one. The generic parameter was the single place where a
*compile-time* concept wore the *immutability* keyword, and afterwards the split is clean:

- **`const` = immutable** — constants, `const fn`
- **`comptime` = compile-time** — `comptime fn`, `comptime` constants, `comptime assert`, and now
  compile-time parameters

It also keeps one spelling across functions and types, which is the property change C gives up:

```kama
fn int32 shifted<comptime S: int32>(int32 x)      // function
type value InlineArray<T, comptime N: int32>      // type — identical spelling
```

### B — allow comptime parameters of non-integral type

Today the grammar restricts a comptime parameter to `integral_type`
([kama.y](../../src/kama.y), the `COMPTIME IDENTIFIER COLON integral_type` arm). B would admit a
float, an `InlineArray`, or a struct. It is the only one of the three that adds capability rather than
syntax: it is what would let a **consumer** write an API whose argument must be compile-time constant,
rather than that being a power only the compiler has over its own intrinsics.

⚠️ **The constraint on B is mangling, and Rust is the evidence.** A value in `<…>` participates in a
type's identity, so `Simd<float32, 4>` and a hypothetical `Pattern<[3,2,1,0]>` would both have to
encode their arguments into a C symbol name. **Rust has shipped const generics since 2021 and still
restricts them to integers, `bool` and `char`** for exactly this reason. So B is hard *as a generic
parameter* and easy *as a function parameter* — where the value does not name a type and needs no
mangle. Which pushes B toward C.

### C — move a function's comptime values from `<…>` into `(…)`

```kama
fn int32 shifted<comptime S: int32>(int32 x) { … }   // today
int32 y = shifted::<3>(x: 2);

fn int32 shifted(comptime int32 S, int32 x) { … }    // C
int32 y = shifted(S: 3, x: 2);
```

The appeal is real and it is ergonomic: compile-time and runtime arguments read in one place, in
declaration order, and `comptime` becomes a per-parameter marker instead of a separate list. It is
also what unlocks B cheaply, per the note above.

## 3. What C would have to solve

**The type use site.** A type has no `(…)` list, so under C a type and a function spell the same
concept two ways — the thing A had just fixed — unless a type gets its own comptime list too. Then the
question is what the *use* site looks like, since a type's arguments appear in type position:

```kama
type value InlineArray<T> [comptime N: int32] { }
InlineArray<int32>[4] buf;                       // a candidate spelling
```

Brackets are the natural second delimiter, and they are **not** free: `when [T: Serialize]` clauses
already use them in declaration position ([kama.y](../../src/kama.y), `method_when_opt`). Any proposal
here must show the grammar has no conflict, not assert it. The alternative is Zig's answer — generic
types *are* calls, `ArrayList(u8)` — which removes the collision by removing `<…>` for types
altogether, and that is a bigger change than C.

**Inference, or its explicit replacement.** Today a comptime argument can be *inferred* from another
argument's type:

```kama
fn int32 sumN<comptime N: int32>(InlineArray<int32, N> a)
sumN(a: v);          // N = 3, inferred from v's type — never written
```

Under C, either that argument is written out (`sumN(N: 3, a: v)`) or the language gains "a parameter
you may omit when it is inferable", which is new machinery and a new rule. ⚠️ **Zig is the cautionary
case and also the reassuring one.** It started fully explicit — `max(i32, x, y)` — found it too
verbose in practice, and added `anytype` so a parameter's type could be inferred at the call. That is
a second mechanism retrofitted to recover inference. **But it applies to *type* parameters, and C
keeps types in `<…>` with inference intact.** Only comptime *values* move, and §4 shows how few of
those exist.

## 4. Why it may not be needed — the measurement

Counted across `prelude/`, `lib/std/`, `examples/` and `tests/` at `0.9.118`:

| | comptime-parameterised **types** | comptime-parameterised **functions** |
|---|---|---|
| `prelude/` | 1 — `InlineArray<T, comptime N>` | 0 |
| `lib/std/` | 1 — `Fixed<B, comptime F>` | **0** |
| `examples/` | 0 | 0 |
| `tests/` | 9 | 2 files |

Three things fall out, and together they are the argument for deferring:

1. **There is not one comptime-parameterised function in shipped kama code.** The entire surface C
   would move is test fixtures written to exercise the feature. C's migration cost is therefore
   near zero — and so is its benefit.
2. **The two real customers are both types** — `InlineArray` and `Fixed` — and a type keeps its
   parameters in `<…>` under every option. C does not touch them.
3. **B's motivating customer is being served another way.** The case that raised this question was a
   SIMD shuffle's lane pattern, which must be compile-time because *the hardware instruction encodes
   it* (a constant pattern is `rev64.4s` + `ext.16b`; a runtime one spills to the stack and rebuilds
   the vector with four scalar loads). That is now enforced by the compiler for `Simd`'s own methods —
   the same intrinsic knowledge it already has about `InlineArray`'s `get`/`set`/`length`. B would
   generalise that power to user code, and the first user who wants it is someone writing a SIMD
   wrapper — which the intrinsic already provides.

⚠️ **Two honest counterweights, so this is not read as a closed case.** Low stdlib usage is not low
value: the natural comptime customers are MCU register maps, DSP kernels and fixed-point math, and
kama's stdlib is not that kind of code — the count is accurate for this repo and understates it for
users. And `InlineArray` is one declaration that is load-bearing everywhere, so "two customers"
undercounts the reach badly.

## 5. Recommendation

**Defer B and C; revisit when a real customer appears.** The trigger to watch for is a user — or an
MCU/DSP library in this repo — wanting a compile-time parameter that is *not* an integer, or wanting
to require a constant argument on their own API. Either is B, and B is what makes C worth its cost. If
neither appears before the 1.0 tag, close this as a declared non-goal in
[ROADMAP_DETAIL §2](../ROADMAP_DETAIL.md#s2) rather than leaving it open indefinitely.

**Decide before 1.0 regardless.** Every option here is source-breaking, and ROADMAP's rule is that
source-breaking changes land before the tag or wait for 2.0. Deferring the *work* is fine; carrying
the *question* past the tag is not.

## 6. Rejected outright

- **Adding `comptime` parameters *beside* the generic parameter list**, leaving both. That is two
  spellings for one capability — `fn f<comptime N: int32>()` and `fn f(comptime int32 N)` — and the
  reason Rust has never added comptime parameters despite Zig demonstrating them. If C happens, the
  `<…>` value form goes away in the same change.
- **Adopting Zig's model wholesale** — types as first-class comptime values, generic types as
  functions returning types. It would rewrite every generic in `lib/std`, all of SPEC's generics
  chapter, `when [T: …]`, contract bounds and the monomorphisation machinery. That is a different
  language, not a refactor. And it would not have solved the case that raised the question: **Zig
  holds that model and still spells shuffle as `@shuffle(E, a, b, mask)` with the pattern as a
  comptime *value*.** A variable-length index list wants to be a value under either design.
- **Variadic generics**, which is the only thing that would let a turbofish carry a shuffle pattern
  (`shuffle::<3,2,1,0>()` cannot serve `Simd<int8,16>`, which needs sixteen indices). Rust, Zig and
  Swift all avoided them; nothing in this repo needs them; and the pattern-as-value form makes the
  question moot.
