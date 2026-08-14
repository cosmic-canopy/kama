# The unsafe seam — `unsafe fn`, and containing `UnsafePtr` (in-flight design)

*In-flight design doc. **Delete this file when the seam work ships**, once GOALS §3a/§3b + SPEC carry the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> Written out of the safety/unsafe boundary spike (`0addb7c`); findings ①–⑪ are in
> [ROADMAP.md](../ROADMAP.md) §2. Companion briefs: [view-model.md](view-model.md) ·
> the contract `for` clause, now shipped ([SPEC.md](../SPEC.md#the-contract-for-clause--which-kinds-may-implement-it-)).

## The problem, measured

GOALS §3a says the safe surface never sees a raw pointer, and that `unsafe { }` + `UnsafePtr<T>` guard the
FFI boundary. Neither holds. **Only the dereference `p[i]` is gated** — `_inUnsafe` is consulted at four
emit sites and nowhere else (`kama.cemit.cpp:1993`, `:2037`, `:2694`, and `:3915`, which is not a gate).
Safe kama can *produce* a raw pointer (`addr(of:)`, public `dataPtr()`, `cast<UnsafePtr<T>>` of an
integer), *store* it in a field, and **call arbitrary C with it**. A double-free needs no `unsafe` token
anywhere (finding ①); `addr(of:)` plus an `UnsafePtr` field is a general dangling-pointer factory
(finding ②).

## The model: `unsafe` marks the body, not the caller

**Decided.** The granular block is the wrong unit — but so was the containment rule an earlier draft of
this brief bolted onto it, and the reason is worth stating because it is what changed:

**`unsafe fn` has two possible meanings, and the earlier draft used both at once.** Rust's `unsafe fn`
means *"**calling** this is dangerous — the caller must uphold an invariant"*; that meaning is what forces
containment (private-only, callable only from another `unsafe fn`). C#'s `unsafe` means *"this **body**
does dangerous things"* — it is Rust's `unsafe {}` block moved to function granularity, and safe code
calls such a method freely. The draft adopted the C# meaning (it is what "when `unsafe` **is** the
function, function-wide relaxation is correct" argues) while applying Rust's containment on top. kama is a
C-family language with C#-like syntax; the C# meaning is the one that belongs here, and taking it cleanly
makes the campaign several times cheaper and strictly more honest.

So:

- **`unsafe fn` replaces `unsafe { }`.** The block form goes away entirely.
- **An `unsafe fn` may be called from anywhere.** No propagation, no caller obligation. The function's
  *signature* is the safe boundary, so a caller needs no permission.
- **Visibility is ordinary.** `public unsafe fn` is legal. `private` is still `private` when an unchecked
  path genuinely must not escape — it is simply not what carries the guarantee.
- **An expression, declaration, or binding whose TYPE is or contains `UnsafePtr<T>` may only occur inside
  an `unsafe fn`.** One rule, covering the body and the signature both. A public function may name it —
  and is then `unsafe`, greppable at the declaration.

  ⚠️ **Key on the type, not the spelled token.** An earlier wording said "produced, handled, or *named*",
  which is token-based and leaks: `match (a.allocate(bytes: n)) { case Some(value: p): … }` never spells
  `UnsafePtr`, yet `p` is one. It does not occur in the corpus today (every call goes through `unwrapPtr`
  plus an explicit `cast`), but it is writable, and "the rule only looks clean because nothing exercises
  it" is finding ⑧'s exact shape. The emitter has the type at every one of these positions; use it.
- **An `extern fn` carries no marker**; it is bodiless, so there is nothing in it to be unsafe. *Calling*
  one is the unsafe operation, and requires an enclosing `unsafe fn`.
- **A contract MEMBER carries no marker either, for the same reason** — a contract member is bodiless, and
  `unsafe` describes a body. See *contract members are conduits* below.

**The keyword stays `unsafe`.** It is exactly C#'s, in the syntax family kama is modeled on; it is what
every reader, tool and LLM already maps to "raw memory lives here"; and it is already reserved
(`kama.l:455`), already in tree-sitter and both editor highlighters. `trusted` (D's spelling, and a
literal fit for "the body is unsafe, the boundary is verified") reads as reassurance where a warning is
wanted; `raw` under-describes the extern-call case, where no pointer appears.

### ⚠️ "No public signature can mention `UnsafePtr`" is FALSE — the prelude disproves it

The earlier draft's containment rule cannot be implemented, and the counterexamples are two of kama's own
extension points. A **contract is public-only** by definition (GOALS §3c), and two name a raw pointer in a
contract member:

```kama
type contract HeapOwner<T> for resource { ctor adopt(UnsafePtr<T> raw); }          // global.kama:27
type contract Allocator for value {                                                // global.kama:71
    fn Optional<UnsafePtr> allocate(usize bytes);
    fn void deallocate(UnsafePtr pointer, usize bytes);
}
```

`HeapOwner` is the `new T(args)` extension point, implemented by `Owned`, `Shared`, and user types
(`tests/rc_iface.kama`). `Allocator` is what every container takes as `A: Allocator = GlobalAllocator`. An
allocator cannot be expressed without naming raw memory, so this is not fixable by redesign. Under the
one-rule form above, both members stay unmarked and every *implementation* of them is `unsafe`.

GOALS §3a's promise is therefore restated honestly: **the safe surface never *silently* exposes a raw
pointer.** Every position where one appears is marked `unsafe` at the declaration, which is the greppable
guarantee GOALS §5 actually asks for. Do not re-derive the stronger claim; it was measured false here.

### Contract members are conduits — contained on both sides, with no extra rule

Neither member gets an `unsafe` marker, and **a contract cannot usefully specify one**: under this model a
contract constrains signatures, not bodies, and everything it *could* force is already forced by the
member's own types. Nor is a call rule needed. Both ends are closed by the one type rule above:

- **Implementer side** — the implementation's own signature names `UnsafePtr`, so it must be an
  `unsafe fn`. All four real conformances move: `Owned.adopt` (`owned.kama:19`), `Shared.adopt`
  (`shared.kama:61`), and the user-type examples `Rc.adopt` / `Box.adopt` in `tests/`.
- **Caller side** — invoking either member means handling a value whose type contains `UnsafePtr`. The real
  shape is `cast<UnsafePtr<V>>(unwrapPtr(o: this.alloc.allocate(…)))` (`slot_map.kama:199`, `map.kama:349`);
  `deallocate` takes an `UnsafePtr` argument outright.

So `Allocator` is safe to **declare** and safe to **name in a bound** — which is what lets every container
keep `A: Allocator = GlobalAllocator` in a perfectly safe signature — while neither member can be *invoked*
outside an `unsafe fn`. Greppability is carried by the type's own name, which is why `Ptr<T>` was renamed
`UnsafePtr<T>` in the first place.

```kama
// 1. SAFE — naming the contract as a bound. No UnsafePtr value exists.
type resource Map<K: Hashable + Equatable<K>, V, H: Hasher = DefaultHasher,
                  A: Allocator = GlobalAllocator> { … }

// 2. The IMPLEMENTER is forced unsafe by its OWN signature — `HeapOwner` said nothing.
type resource Owned<T, A: Allocator = GlobalAllocator> implements Deref<T>, HeapOwner<T> {
    UnsafePtr<T> p;
    public unsafe ctor adopt(UnsafePtr<T> raw) { this.p = raw; return this; }
}

// 3. The CALLER is forced unsafe too: it cannot take what `allocate` returns,
//    nor supply what `deallocate` wants, without an UnsafePtr in hand.
unsafe fn void growTo(int32 nc) {
    this.values = cast<UnsafePtr<V>>(unwrapPtr(o: this.alloc.allocate(bytes: …)));
    this.alloc.deallocate(pointer: cast<UnsafePtr>(old), bytes: …);
}
```

And the case the token-based wording missed — note the *deref* was never the leak (`p[0]` is the one thing
gated even today); **acquisition and propagation** is:

```kama
fn stashIt(ref Arena a) {                          // NOT marked unsafe — a token rule cannot tell
    match (a.allocate(bytes: 64)) {
        case Some(value: p): { this.buf = p; }     // `p` is an UnsafePtr. The word never appears.
        case None: { }
    }
}
```

Combined with an `UnsafePtr` field — which stays legal — that is finding ②'s dangling-pointer factory,
reachable from a function carrying no marker. Hence: key on the type.

> **Terminology.** This file says **contract member**, never "slot". `slot` is a *keyword* — uninitialized
> storage filled by an `out` argument — and has nothing to do with contracts. SPEC used to carry the
> collision, calling a contract's requirements its "slots"; that is now reconciled at the source (SPEC
> § *Uninitialized storage* states `slot` means only the keyword, and the `type view` conformance rule says
> "`ctor` member"). The one other load-bearing use is the emitted **vtable slot**, which is always spelled
> with `vtable`/`vtbl`. Keep it that way.

### What this buys

1. **It fixes finding ⑨ by construction rather than by patching it.** Today a top-level `unsafe` block does
   `unassigned.clear()` for the *whole function* (`kama.cemit.cpp:12501`), and `flag()`/`verifyOutsAssigned()`
   return early — so an unfilled `out` parameter passes `kama check` and the caller reads uninitialized
   stack. That is a bug precisely because the relaxation's scope (the function) does not match the
   construct's scope (the block). When `unsafe` *is* the function, function-wide relaxation is correct.
2. **It is greppable at the declaration** (GOALS §5) instead of buried in a body, and it is one concept
   instead of two.
3. **It removes a known emitter trap**: wrapping an assignment in `unsafe` currently changes the ctor
   walk's notion of top-level (`kama.cemit.cpp:11923`), so a real ctor-escape hole can look closed for the
   wrong reason.

### The tradeoff to accept deliberately

**A third of the public stdlib API will read `public unsafe fn` — 241 of 712 public members in
`lib/`+`prelude/`.** That is the honest price and it belongs here rather than as a surprise at migration
time. Two things make it acceptable: those 241 members touch raw memory *today*, so the marker relocates
existing unsafety to the declaration rather than adding any; and the alternative (Rust's meaning, with
private-only containment) buys a cleaner-looking API by requiring a private helper extracted from every one
of the same 241 members, which is ceremony that hides where the raw work is.

Granularity is also genuinely lost — `growTo` is ~10 lines of which 2 need raw access, and the whole
function becomes unsafe. That is the same relaxation scope as today's finding ⑨, made honest.

## Rule surface — the decision table, settled

| construct | example | verdict |
|---|---|---|
| declare an `UnsafePtr` **field** | `UnsafePtr<T> data;` in `DynamicArray` | **legal** — every container and every `type extern value` in `examples/webgpu` depends on it |
| module **static** of `UnsafePtr` type | `static hardware UnsafePtr<uint32> gpio_odr;` | **legal** — the MCU path depends on it |
| **read/write** such a field | `this.data` | `unsafe fn` only |
| **pass** an `UnsafePtr` as an argument | `View.over(base: this.data, …)` | `unsafe fn` only |
| **return** an `UnsafePtr` | `dataPtr()` | `unsafe fn` only |
| a signature naming `UnsafePtr` | `fn UnsafePtr<T> dataPtr()` | legal **iff that function is `unsafe`** |
| a **contract MEMBER** naming `UnsafePtr` | `ctor adopt(UnsafePtr<T> raw)` | legal, **no marker** — bodiless; the implementer and the caller are each forced `unsafe` by their own types |
| bind an `UnsafePtr` **without spelling it** | `match (a.allocate(…)) { case Some(value: p): … }` | `unsafe fn` only — the rule keys on the **type**, not the token |
| `addr(of: x)` | `gpio_odr = addr(of: led);` | `unsafe fn` only — it *produces* a raw pointer |
| `cast<UnsafePtr<T>>(…)`, `cast<usize>(p)` | `cast<UnsafePtr>(0x40021000)` | `unsafe fn` only; stays possible for MMIO |
| compare two `UnsafePtr`s | `if (this.handle != null)` | `unsafe fn` only |
| **declare** an `extern fn` | `extern fn int32 abs(int32)` | no marker |
| **call** an `extern fn`, **scalar-only included** | `kama_sqrt(x: this)` | `unsafe fn` only |
| inline `asm(…)` | | `unsafe fn` only (already gated, `kama.cemit.cpp:2694`) |
| **call** an `unsafe fn` | | unrestricted |
| definite assignment inside an `unsafe fn` | | locals relaxed; **`out` params still verified** |

**No scalar-only exemption.** `extern fn int32 kama_close_socket(isize fd)` is scalar by type and a
double-free primitive by effect; so are `kama_app_exit` and `kama_ws_close`. **87 of the 188**
`lib/`+`prelude/` extern declarations name no pointer, and exempting them would leave finding ① open in
its handle-closing shape. A type-based carve-out is not sound.

**DA relaxes locals but not `out` parameters**, and that split is load-bearing: relaxation exists because
raw stores are invisible to the DA walker, whereas filling an `out` is a contract with the *caller*. Once
safe code may call an `unsafe fn` — which is the whole point of the model — relaxing `out` would reopen
finding ⑨ immediately. Write this into SPEC so it is not re-filed.

**Ctors.** A public ctor needing raw setup becomes `public unsafe ctor` — no private-helper delegation.
Still to check during M2: that this composes with "a ctor must assign every field", and with the ctor
walk's treatment of `unsafe` at `kama.cemit.cpp:11923`.

## Blast radius, measured

| | |
|---|---|
| `unsafe { }` blocks in `lib/`+`prelude/` | **165** |
| `extern fn` declarations | **379** (188 `lib/`+`prelude/`, 160 `tests/`, 31 `examples/`) |
| **`extern fn` call sites in `lib/`+`prelude/`** | **274** — **201 inside a `public fn`/`ctor`**, 73 in free functions, **0** in private/protected |
| distinct **public members** containing ≥1 extern call | **130** |
| distinct **free functions** containing ≥1 extern call | **44** |
| **exported** free functions calling an extern directly | **21** — all of `std::fmt`'s `i32Str`…, `fs::stat`/`remove`/`readDir`, `time::monotonicNow`, `io::lastError`, `log`, `app::run` |
| private members in `lib/`+`prelude/` **today** | **5**, against **743** public |
| public members that gain `unsafe` under this model | **241 of 712 — 34% of the public stdlib API** |
| `addr(of:)` lines, corpus-wide | **133** (~47 in `lib/`+`prelude/` not already inside a block) |
| public `dataPtr()` call sites | **4** |
| MMIO assignments | one wrapped line each |

The earlier draft's radius counted only the 165 blocks and the 61 of them sitting in public members. It
never counted **where the extern call sites live**, which is the larger half: 201 of 274 sit in a public
member. Under the model chosen here those are keyword additions; under the rejected one they would each
have been a helper extraction, into a stdlib that contains **five** private members in total.

**The 44 free functions are why free `unsafe fn` must exist.** `std::math`, `std::net` and `std::fs` are
deliberately free-function-shaped, and requiring a host type purely to satisfy a visibility rule would be
arbitrary. The 21 exported ones keep a safe wrapper — which is the seam doing its job, not a cost:

```kama
export { i32Str };                          // safe public API
extern fn string kama_i32_str(int32 v);     // bodiless, no marker
unsafe fn string i32StrRaw(int32 v) { return kama_i32_str(v: v); }   // free, unsafe
fn string i32Str(int32 v) { return i32StrRaw(v: v); }
```

**Performance — to measure, not to assume.** `unsafe fn` bodies are small, and the emitter already has
`static inline` machinery (`_emitStaticInlineFn`, `kama.cemit.cpp:14280`; prelude methods emit that way at
`:10290`). Release inlines a single-caller static for free. `-O0` is the tier where a real call would
appear, and it is kama's iteration tier — so measure on the `foreach` path against
`tools/check-ecs-zero-dispatch.sh` and `bench/` before adding any `always_inline`.

## Milestones

**M1 — `unsafe fn` grammar.** Today `UNSAFE` appears at exactly one production (`unsafe_statement : UNSAFE
block`, `kama.y:1074`) and is **not** in the `modifier` list (`kama.y:770-789`), so `unsafe fn` does not
parse. `FunctionDeclarationNode`/`ClassMethodDeclarationNode` gain an `isUnsafe` flag; `_inUnsafe`
(`kama.cemit.h:1075`) becomes "the enclosing function is unsafe". Surfaces that follow the keyword:
`docs/grammar.bnf`, `docs/KEYWORDS.md`, `tree-sitter-kama/grammar.js` (+ regenerated parser),
`tree-sitter-kama/queries/highlights.scm`, `editor/vscode/syntaxes/kama.tmLanguage.json`,
`editor/zed/languages/kama/highlights.scm`.

**M2 — retire the block, mark the 241 members.** Delete `unsafe_statement`; migrate the 165 blocks by
marking their enclosing function. Finding ⑨ dies here, together with the DA split above and the ctor-walk
check.

**M3 — the `UnsafePtr` naming rule** per the table. Includes moving `HeapOwner.adopt` and
`Allocator.allocate`/`deallocate`, whose implementations all become `unsafe`, and their conformances in `Owned`, `Shared`,
`BumpAllocator`/`GlobalAllocator` and user types (`tests/rc_iface.kama`) with them — these are user-facing
extension points, so the change is API-visible and belongs in the same commit as its SPEC note.

**M4 — the `extern fn` call gate**, at `kama.cemit.cpp:14079` where `_funcs.find(resolveFunc(…))` succeeds
(`FuncSig::node` already retains the decl, and `CEmitter::isExtern` is at `:10869`). Secondary resolution
sites needing the same check: `:7146`, `:9021`, `:16305`, `:16730`. No scalar exemption.

**M5 — fixtures.** ⚠️ **No `xfail` currently pins *"raw pointer access requires `unsafe`"*** — only
`asm_outside_unsafe` covers any gate, so the two most important checks in the seam could be deleted today
and the suite would stay green. Each new rule needs its own `xfail` in the same commit: raw deref outside
an `unsafe fn` (read **and** store); an extern call outside one; a **scalar-only** extern call outside one;
`addr(of:)` outside; `cast<UnsafePtr<T>>` outside; `UnsafePtr` named in a non-`unsafe` signature (parameter
**and** return); **an `UnsafePtr` bound without being spelled** — the `match (a.allocate(…)) { case
Some(value: p): … }` shape, which is the one the token-based wording missed; an `unsafe fn` leaving an
`out` param unfilled. Plus a **positive** fixture: a safe function that declares `A: Allocator` as a bound
and never invokes either member must still compile.

## Closed decisions — do not reopen

**`Optional<UnsafePtr<T>>` / deleting the `null` token: killed.** Reasoning in [ROADMAP.md](../ROADMAP.md)
§2, in short: nothing null-shaped reaches a binary (deref already needs `unsafe`; the six positions the
type-keyed rule misses are all rejected by clang, making them an instance of the `kama check` gap);
`Optional` has no unwrap and cannot get one, since a generic enum rejects members; and the niche
optimisation would touch ~25 emission sites. **The naming rule gets the same goal for free** — all 75
genuine `null` tokens target an `UnsafePtr`, so confining the type confines the token.

What survives as separately worthwhile: the honest FFI surface. Only **8** extern declarations return a
genuinely nullable pointer (`malloc` ×3, `kama_poller_create`, `kama_diropen`, `kama_channel_new`,
`kama_argv_new`, `kama_envp_build`).

**`pub unsafe fn` is not a question any more.** It was one only under Rust's meaning; under C#'s, a public
`unsafe fn` with a safe signature is an ordinary public API implemented with raw memory — which is what
`View.operator[]` and `DynamicArray.add` genuinely are.

**"What is private for a free function?" dissolves.** Visibility is not the containment mechanism, so a
free `unsafe fn` needs no answer to it.

## Definition of done

GOALS §3a/§3b state the C# meaning, the one naming rule, and the honest form of the raw-pointer promise.
SPEC gains the rule table and the DA split. Every negative claim has an `xfail`. `./dev matrix` green,
including the sanitizer leg. Then this file is deleted.
