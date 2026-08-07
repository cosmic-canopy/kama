# The contract model — conformance, hidden kinds, and what follows

*In-flight campaign doc. **Delete this file when the last campaign below ships**, once SPEC carries the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> ### ►► What this is
>
> A design review held during M2a (2026-08-04) that started as "is retro-impl dangerous?" and ended with
> a coherent model for how *every* kind declares conformance. It schedules **four campaigns**, each its
> own session, in the order given.
>
> Read the *Corrections* section before re-deriving anything — three plausible-sounding claims were
> checked against the tree and turned out to be false. **Campaign 1 is part-built — see *Status* below
> before starting anything**; several of its design points were revised once the code was written, and
> *What M3 residual actually was* records where the brief and the build disagreed. **M5 is next.**

## Status — campaign 1, as of 2026-08-06

**Shipped to `dev`.** Baseline before the campaign was native 927 / ASan 891.

| commit | what | native / ASan |
|---|---|---|
| `5f0d42d` | M0 chokepoints + **M1 `type enum X implements C { A, B; …methods… }`** | 933 / 897 |
| `f8ed0ec` | **M2** — bare `enum X` is a parse error (breaking); 99 sites migrated; 29 enum conformances folded off retro-impl; **Model C promotion deleted** | 934 / 898 |
| `81b62ce` | **M3 grammar** — `type intrinsic <…> implements C` + nested `<…> { }` sections | 934 / — |
| `bfedd9f` | **M3 emitter** — collect / validate / emit; 8 guard-rail xfails; 2 positive fixtures; tree-sitter | **944 / 908** |
| `7d4a343` | **M3 residual** prep — decouple the key from the C type (`paramListC` takes the receiver convention; emission reads the minted `cName`; `receiverTypeNode` extracted; `mangleElem` gains `char`) | 944 / — |
| `ba0bbe1` | **the re-key** — `primKey`: kama name for a scalar, cType for everything else | 944 / — |
| `e326344` | **`char` conformances** — `Format`/`Serialize`/`Deserialize`; ROADMAP §2 closed | 945 / — |
| `da44c54` | **resolved contract names** in the conformance pre-scan (a real miscompile, not just a diagnostic) | 947 / — |
| `5c6f429` | **package identity** — a duplicate conformance names both packages; both scoping fixtures | **948 / 912** |

| `cd81231` | a contract is not a cast target; **M5's `::` dropped** | 949 / — |
| `f58231a` | a cast rejects every aggregate target, not just a contract | **950 / 914** |

M3 residual closes at **native 950 / ASan 914 / wasm 888**, 0 failed, with every `tools/check-*.sh` green.

M0 was verified behaviour-neutral by **byte-identical generated C across all 585 fixtures**, and M1 by the
same diff over the 585 pre-existing ones — the promotion-timing move that was flagged as the campaign's
highest regression risk turned out to cost nothing. M3-residual's prep commit used the same gate (589
fixtures by then), and the re-key itself was verified by applying the inverse symbol rename and diffing to
zero — a stronger check than reading the diff, and it caught nothing, which was the point.

### M4 — shipped 2026-08-06

| commit | what |
|---|---|
| `4d365f1` | **prep** — the LSP's `enclosingCallable` gains an `IntrinsicImplNode` arm (it had one for the retroactive block and none for this, so completion/signature-help went dead inside these bodies); `isPrimitive` parity for `string`. Byte-identical C. |
| `ed3435d` | **prelude collapse** — `Hashable` 9→2, `Equatable` 10→1, `Comparable` 11→3 |
| `7341f1b` | **prelude Format + serde** — `Format` 13→7; `Serialize`/`Deserialize` 26 one-target blocks. `prelude/global.kama` now has **zero** `implements C for T`. |
| `90add48` | **lib** — `FromStr`×11, `FromStrRadix`×8, `Real`×2. Generated C identical, with nothing to explain away. |
| `4ad6513` | **the `string`/`Equatable` special case retires** — one atomic commit |

M4 closes at **native 950 / ASan 914 / wasm 888**, 0 failed, every `tools/check-*.sh` green. Counts are
unchanged throughout because M4 adds no behaviour: it is a change of spelling, gated on codegen.

**Three of the four pieces of machinery have now retired** — retro-impl on enums, the Model C promotion,
and the `string`-`Equatable` nominal special case. The fourth (retro-impl itself) is M6's.

**The gate that made this safe** was an order-insensitive **C function-set diff** over all 590 flat
fixtures (`kama transpile --no-line`, split into top-level definitions, sorted, compared), not a byte
diff — consolidating blocks reorders emission. Run per commit, it left exactly three deltas across the
whole milestone, each a deliberate no-op identity cast from folding the widest width into its set's
shared body: `uint64__hash`, `int64__format`, `uint64__format`. Nothing else moved. Two collateral
findings worth keeping:

- **A prelude-inlined `kama_panic_at` bakes the PRELUDE's line number into the call**, and records either
  an empty path or — in a single-unit transpile — the main file's path, so the path cannot discriminate.
  Editing the prelude shifts every one of them by exactly the number of lines added or removed. Normalize
  that before reading a codegen diff or the real signal is buried under hundreds of records.
- **`satisfiesBound` is structural first**, so most `string` code never depended on the nominal
  `Equatable` record at all. What did depend on it is the `when T: Equatable` gate —
  `DynamicArray<string>::contains`/`::indexOf` vanish without it. Verified by deleting both halves and
  diffing, which is the only way that would have been found.

**M5 is next.**

### What M3 residual actually was

The brief specified re-keying to `[kamaType][contractKey]`. Only the first half was built, and the second
half turned out to be **M5's job, not M3's**. A primitive's methods live in one flat `ClassInfo::methods`
map, so a second key would let two same-named contracts *record* two conformances but not let either
supply a method the other already named — `injectImplMethods` still rejects that, and rightly, until there
is a way to *say* which one you mean. That way is `Contract::method`, the contract-as-scope rule, M5.
Building the storage first would have created data with no reader.

What the package axis needed was smaller than a second key and different in kind: not a way to hold two
claims, but a way to **name both claimants** when a duplicate arrives. That is `_conformanceOrigin` plus a
resolver the driver installs, and it is lazy — nothing is computed unless an error fires.

Two things the brief did not anticipate:

- **`primKey` must substitute before it reads `builtInVal`.** Inside a monomorph a parameter's recorded
  type node is still the unsubstituted `T`; without the `_typeSubst` hop that `cType` and `mangleElem`
  already do, every bounded-generic call falls through to the C type and files `char` under `uint32` — so
  the fixture written to catch exactly that would have passed while being wrong.
- **The pre-scan stored BARE contract names**, which conflated two same-named contracts in different
  namespaces. Where their methods also share a name this was not a bad diagnostic but a **wrong program**:
  `tests/xfail/scoped_bound_wrong_contract.kama` compiled and ran before the fix, dispatching to the other
  namespace's method. Found by writing the namespace-axis fixture, which is the argument for writing it.

### Remaining, in order

**M4 is done** (see *Status*). What it actually cost, against the estimate, since the shape of the
collapse is the thing most likely to be misremembered:

| contract | impls | blocks after |
|---|---|---|
| `Hashable` | 9 | **2** |
| `Equatable` | 10 | **1** |
| `Comparable` | 11 | **3** |
| `Format` | 13 | 7 |
| `Serialize` | 13 | 13 |
| `Deserialize` | 13 | 13 |
| `FromStr` / `FromStrRadix` / `Real` | 21 | 21 |

The collapse is real but **concentrated**: 30 blocks become 6, and that is the whole win.
`Serialize`/`Deserialize`/`FromStr`/`Real` do not collapse at all, because each body names a
width-specific function (`writeI32`, `readF32`, `kama_sqrtf`) or a width-specific literal (a range limit),
and the set form cannot merge bodies that call different functions. They are one-target blocks and each
file now says so in prose. `Format` is the interesting middle: it collapses 13 → 7 not because its bodies
are identical but because narrow integers already widen into the widest write, so the cast on the widest
target is the identity.

1. **M5** — the contract-as-scope gate (on `MethodInfo::fromContract`, added in M0 for this) plus
   primitive→contract widening, and **no new syntax** (`::` is dropped — see *M5 has no new syntax*). The
   gate is the hard half, because bound-generic dispatch goes through the same injected methods it must
   reject on a concrete receiver. The ABI seam for widening: an intrinsic's method takes `self` **by value**
   (`isScalarRecv`, which M3 residual gave its first reader) while a vtbl slot passes `void*`, so each
   widened method needs a deref thunk — emitted only for a contract actually widened to. The flat method
   map stays flat; two contracts supplying one type the same method name stays a clean error.
2. **M6** — delete retro-impl entirely, rename what it leaves behind, close out the docs.

### Design points revised once the code existed

- **There is no orphan rule, and there will not be one.** It is a workaround for separate compilation;
  kama runs one `CEmitter` per build with every unit visible, so a duplicate claim on a (type, contract)
  pair is *detectable* — and the check that detects it already exists. What was missing is only that the
  message should name **both declaring packages**, which it now does (`5c6f429`). The designed-for relaxation is *scoped conformances*
  (a conformance scoped to its declaring namespace, resolved at the instantiation site), which is sound
  **only if the resolved conformance enters the monomorphization key** — otherwise a container built under
  one package's ordering and mutated under another's is one C struct and corrupts silently. Zero consumers
  today, so it waits for a real case. The paragraph below claiming the rule "falls out of ownership" is
  superseded.
- **Contract identity is `(owning package, namespace, name)`** — scope is implied by the import origin, so
  the *same* library imported from two origins yields two distinct contracts. Namespace is explicit sugar
  on top, not a publishing precondition.
- **Per-target specialization is a nested `<…> { … }` section**, which is what lets one block mix a shared
  body with per-target ones. The claim below that the prelude's 64 primitive impls "collapse to roughly 8–10" holds
  for the *mechanism* count, not the line count: bodies naming a different C function (`writeI32`,
  `sqrtf`) never collapse.
- **`TypeKind::Intrinsic` was renamed `TypeKind::Neutral`.** It is the neutral kind for compiler-built
  types and has nothing to do with the `type intrinsic` surface syntax; leaving the names colliding would
  have guaranteed a wrong-fix.

### Traps this campaign has already sprung

- **`tools/check-*.sh` run the HOST binary** (`build/<os>-<arch>/kama`). `tools/cdev make` updates only the
  container one, so a guard can pass against **stale** code. Run `make` on the host before believing one.
- **`_enumDeclNodes` is the LSP def-site table's only unified index over enums** — plain enums land in
  `_enums`, tagged concrete ones in `_classes`, generic ones in `_genericTypes`, and no single map holds
  all three. It reads like Model-C machinery (its comment used to say so) but deleting it breaks
  `kama.query.cpp`. Only `_enumNsCtx` was promotion-only.
- **A prelude enum's prototypes must be `static`** to match its static-inline bodies (`preludeStatic`), and
  the class-shaped prelude-definitions loop must skip variants. This is what the old
  `isRetro && isVariant` proto skip was really preventing.
- **Fixtures encode buffer coordinates.** Adding `type ` shifted `check-query`'s `spellings.kama`
  assertions, `check-lsp`'s two inline buffers (both the expectations *and* the request positions), and the
  TextMate snapshot. Regenerating a `.coverage` file must not capture stderr.
- The mandatory `;` in an enum body and `simple_type` in the intrinsic target list are each load-bearing
  for LALR(1). Zero new conflicts under `%expect 1`; the reasoning is in each production's comment.
- **The prelude is baked into the binary.** Editing `prelude/global.kama` and then running `./kama`
  compiles against the OLD prelude — `KAMA_PRELUDE_SRC` is regenerated by `make`. A prelude change that
  "does nothing" has almost certainly not been rebuilt.
- **A prim conformance's `ClassInfo::name` is its C type, and always was.** It is what `This` resolves to
  (`ScopedStr _ts(_thisType, e.target->name)`) and how the `self` parameter is spelled. So the registry key
  and the `name` deliberately differ, and `char`'s and `uint32`'s entries share a `name`. Anything that
  wants the key must not read `name`, and anything that wants a C type must not read the key.
- **Generic inference does not see through a member access or a foreach binding**, for any type — it wants
  a literal or a locally-typed value. Pre-existing and unrelated to conformances, but it shapes any fixture
  that exercises those receiver shapes: use the turbofish (`toString::<char>(x: ref m.at)`).
- **A round-trip is not a serde assertion.** Encode and decode agree whichever conformance they share, so a
  `DynamicArray<char>` round-tripped fine while writing `uint32`'s bytes. The KBIN *tag* is what
  discriminates (14 for char, 7 for uint32).
- **A `.d/` fixture with a path dependency needs its `.kama/deps/<name>` symlink committed** — the harness
  runs `kama build`, never `kama pkg install`. `tests/pkg_path_dep.d/` is the template.

### Found while building this, not fixed, tracked in [ROADMAP §2](../ROADMAP.md)

A value-producing `match` over an `enum X : IntType` does not compile — pre-existing, reproduces on a bare
`enum Color : uint8` with no contract and no `type` marker.

## Why this exists

M2a needed `int32` to satisfy a `FromStr` contract and `float64` to satisfy a `Real` contract. Both went
through **retro-impl** (`implements C for T` at top level), which is the only mechanism kama has for
giving an existing type a contract. That prompted the question of whether retro-impl is a footgun — a
module reaching into a type it does not own.

Investigating it surfaced something better: retro-impl exists to paper over **two kinds that have no
spelling**, and giving them one removes the mechanism rather than fencing it.

## The core finding — two hidden kinds

[GOALS.md](../../GOALS.md) #3c states the rule: *every declaration is `type <kind> Name`*, and lists
`type value`, `type resource`, `type view`, `type contract`. Two kinds break it:

| kind | how it is spelled | consequence |
|---|---|---|
| `enum` | its **own grammar production** (`modifiers_opt ENUM type_decl_head enum_underlying_opt enum_body`) — not part of `type`, and **no `class_base_opt`** | an enum cannot declare conformance inline, so `implements Error for MyError` is *mandatory*, not stylistic |
| `intrinsic` | **no kama spelling at all** — `TypeKind::Intrinsic` exists only inside the compiler | a primitive cannot declare conformance either, so the prelude retro-implements onto it 64 times |

Everything retro-impl is used for traces back to one of those two gaps. Give both a spelling and the
mechanism has no remaining job.

## The design

```kama
type enum MyError implements Error { NotFound, Denied }

type intrinsic <int8, int16, int32, int64, uint8, uint16, uint32, uint64>
    implements Comparable {
    public fn Ordering compareTo(ref This other) { … }      // one impl replaces eight
}

type intrinsic string implements Equatable { }              // the NATIVE `equals` satisfies it
```

**The set form is load-bearing, and it is the type-list bound that failed elsewhere.** A type list cannot
serve `sqrt` — that needs a *different C function* per width (`sqrtf` vs `sqrt`) and kama has no in-body
type branching by design. It works here because these bodies are **genuinely identical** across the set:
they use raw `<` / `==`, which stay raw C operators for all-primitive operands and never recurse. The
prelude's 64 near-identical primitive impls collapse to roughly 8–10.

**A contract is a SCOPE.** An intrinsic is decorated with methods *within the scope of a contract*, so a
contract-supplied method is not part of the intrinsic's own API:

- `(3).compareTo(other: 4)` — **rejected**; not the type's API
- `3` passed where a `Comparable` is expected — **fine**
- `sort(items: v)` — **fine**; bound dispatch
- `a < b` — **unaffected**; all-primitive comparisons stay raw C operators
- `string.equals` / `string.length` — **unaffected**; those are the type's *native* API

~~Disambiguation … uses `::`.~~ **Dropped 2026-08-05 — see *M5 has no new syntax*, below.**

#### M5 has no new syntax

`Comparable::compareTo(self: x, other: y)` is not built and will not be. The one way to reach a
contract-scoped method is to **have a contract value**, which is the idiom user types already use:

```kama
Comparable c = x;                        // a BORROW — no move, no copy of `x`
Ordering o = c.compareTo(other: y);
```

Three things settle it:

- **The zero-cost path already exists, and it is the generic bound.** `fn f<T: Comparable>(…)` monomorphizes
  to a direct `int32__compareTo(x, &y)` — no indirection at all. That is how every call in `lib/` reaches a
  contract method, and `::` would have added a second spelling for something already free. A fat-pointer
  call costs one indirect jump, which is inherent to erasing the type and is the only case `::` was faster
  than; direct calls on concrete primitives are test-only (see *Measured cost*).
- **Name collisions are an import problem, and `import` already solves them.** Two contracts named `Marker`
  from different libraries are disambiguated by `import a::{Marker as AMarker}` (per-symbol aliasing,
  [kama.y:391](../../kama.y#L391)) — at the point the ambiguity is introduced, not at every call site.
- **`::` would need machinery nothing else uses**: a contract can never appear in the `::` resolver today
  (contracts live in `_interfaces`, and the resolver only consults `_classes` / `retroTargetInfo`), and the
  `self:` argument convention exists nowhere else in the language.

What that leaves unsupported is **one type carrying two same-named methods from two contracts** — the flat
`ClassInfo::methods` map still rejects it at injection, and aliasing the *contract* names does not help
because the collision is on the *method* name. Zero occurrences in the tree; it stays a clean error, and
the flat map stays flat. Revisit only if a real case appears.

So **M5 is the gate plus widening, and nothing else**. The gate is the expensive half: it cannot simply
reject a call whose method has a non-empty `fromContract`, because bound-generic dispatch — the pervasive
idiom — goes through exactly those injected methods. It has to tell "receiver is a concrete type spelled
directly" from "receiver's type came from a substituted type parameter".

`cast<Contract>(x)` is **not** the escape hatch either: a cast produces a value, and a contract value
borrows storage a cast expression does not have. It is rejected outright ([kama.cemit.cpp](../../kama.cemit.cpp),
`CastNode`; fixture `tests/xfail/cast_to_contract.kama`) — it used to emit `((Shape)(c))` and die in the C
compiler with no kama diagnostic. The opposite direction, contract value → concrete, is `expr.as<T>()`.

**The declaration IS the anchor.** Nothing widens a primitive to a contract value today (`Comparable c =
3;` appears nowhere in `lib/`, `prelude/` or `tests/`) and the reason is now clear: a `__as_<Contract>`
vtable is emitted **from a declaration site**, and an intrinsic has none. That same gap forced the enum
"Model C" promotion — a plain enum is rebuilt into a tagged-union `ClassInfo` on the fly so it can carry
a method and a vtable. Give both kinds a spelling and the machinery has somewhere to attach.

~~**The orphan rule falls out of ownership** instead of being bolted on.~~ **Superseded — there is no
orphan rule** (see *Design points revised*). The hazard it names is real: two dependencies that have never
heard of each other both claiming the same (contract, type) pair, breaking a build neither their user nor
either author can fix. But under the whole-program view that duplicate is *directly detectable*, and the
check that detects it already existed — it only had to learn to name both packages, which it now does
(`5c6f429`). A rule restricting who may declare a conformance would forbid legal, useful cases to prevent
one the compiler can simply see.

### Four pieces of machinery retire together

1. **retro-impl** — no remaining job. **The only one left; M6.**
2. ~~**the nominal-recording special case**~~ — **retired (M4, `4ad6513`)**. `string`'s `Equatable` was
   "recorded from its built-in `equals` via `registerCollection`" *only because* an intrinsic could not
   say `implements`; it now says it, with an empty body.
3. ~~**the enum tagged-union promotion**~~ ("Model C") — **retired (M2, `f8ed0ec`)**; a full type gets a
   real `ClassInfo` from the start
4. **the missing primitive→contract path** — expressible for the first time; the *widening* half is M5

### Measured cost: one test file

Every stdlib call already goes through a bounded type parameter — `sort`, `priority_queue`,
`sorted_map`, `map`, `dynamic_array`, `fixed_array`, `deque` all call `a.compareTo(other: b)` or
`item.equals(other: …)` on a `ref T` / `ref K`. Only [tests/comparable.kama:22-36](../../tests/comparable.kama#L22)
calls `compareTo` on **concrete** locals — 9 of its 11 assertions — and rewriting those through a bound
is a better test anyway, since it exercises the supported surface.

## Corrections — do not re-derive these

Three claims that sounded right and are **false**:

- **"Retro-impl coherence is not enforced."** It is. `applyRetroactive` rejects a duplicate impl
  (``"`float64` already implements `Real`"``), a method clobbering an existing one, and an incomplete
  impl. What looks like a hole — `_retroConformances` being a `std::set` that silently dedups — is only
  the *pre-scan*, whose own comment says the real coherence check happens later.
- **"The contract `for` clause is stale now that `view` exists."** It is not. The clause expresses the
  *ownership* axis, and a view owns nothing, so grouping views with values is accurate. Moreover **there
  is no view-only contract possible**: contracts express *capabilities*, whereas a view's distinguishing
  property is a *restriction* (it may not be stored). Anything a view can do method-wise, a value can —
  so `for view` would have nothing to express.
- **"Cross-TU monomorphization divergence blocks specialization."** That hazard is real for
  separately-compiled languages (C++ templates, Rust crates) and does **not** arise here: kama runs one
  `CEmitter` per build and `collectProgram` receives prelude + built-ins + every user unit, so
  monomorphization decisions are made with whole-program visibility.

## The campaigns, in order

```
contract model  ->  full specialization  ->  const generics  ->  view-escape check
```

### 1 · Contract model

Build the design above. Open questions:

1. **Multi-method contracts** need a completeness check over the set form.
2. **Boxing cost** — `Owned<Error>` allocates; confirm the intrinsic path does not silently make that
   common.
3. **Guard rails** — a `type intrinsic` block must not declare fields; one block per (contract,
   intrinsic). The existing duplicate/clobber checks already have the right shape.
4. ~~**Migration**~~ — **done (M4)**; the measured outcome is under *Remaining*.

### 2 · Full generic specialization

**Universal, not an intrinsic workaround.** Once conformance is settled this stops being a substitute for
it and becomes what it is: polymorphism for generic *functions*, across all types.

- **Full, not partial** — a specialization's type arguments must be **fully concrete**: no type
  parameters, no bounds. Any two are then either identical (a duplicate → clean compile error, the same
  shape as the existing duplicate-operator rejection) or disjoint. No specificity lattice to define and
  nothing to prove — this is the sound half of C++'s explicit specialization. `View<int32>` qualifies;
  `View<T>` does not.
- Specializing on a **contract bound** is where overlap returns; defer unless a total order proves
  definable.
- Confirm the whole-program property (see *Corrections*) also holds on the `kama check` / LSP paths.

### 3 · Const generics → `Fixed<intBits, fracBits>`

Blocked today by three things, two of which are compiler bugs in their own right:

1. `ClassDeclarationNode::constParams` is **populated by the grammar and never read** by the emitter —
   const params on *types* are parse-only plumbing.
2. A const param **cannot be a runtime value**: `emitExpression`'s identifier branch never consults
   `_constSubst`, so `this.raw >> F` emits an undeclared C identifier and dies in the C compiler **with
   no kama diagnostic**. Silent bad codegen — worth fixing regardless of `Fixed`.
3. No **type-level selection** to map `I+F` onto a backing width, and no "next wider type" for the
   multiply. This is the genuine design question.

(`@generate(of)` is also rejected on generic types, which `Fixed16_16` uses — but that is fixable in
kama source by hand-writing a ctor.) `Fixed16_16` keeps its name meanwhile precisely to reserve `Fixed`.

### 4 · Derived view-escape check

Nothing verifies that a `view` can satisfy the contract it implements — every `isBorrow` use is at escape
sites, destructibility or isolate prep, none at the `implements` site. Reject at that site, over the
methods **actually injected** (the marker-contract pattern puts the factory in the impl rather than the
contract, so reading the contract alone would miss it): a static/ctor factory returning `This`, and
methods handing back `Owned<This>`/`Shared<This>`. `View.slice() -> This` stays legal because it borrows
the receiver.

Purely additive — no syntax, no contract re-declarations. Necessarily **partial**: boxing-idiom contracts
like `Error` have perfectly borrow-safe signatures (`fn string message()`) and stay caught later, at the
boxing site.
