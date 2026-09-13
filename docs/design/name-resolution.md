# Name resolution and visibility at every type position (KR-46)

**Status:** measured, not started — **the next work item** (maintainer, 2026-09-12). Found building KR-12
at `0.9.320`. This is the working doc for the campaign: the probe grid it was measured with, the results,
the root causes, and the plan. Deleted when KR-46 ships, once `SPEC.md` § Modules and the `tests/xfail/`
fixtures carry the record.

## The rule, and where it breaks

`SPEC.md` § Modules: a name is spelled in a file only if that file declares it, imports it, or it comes from
the prelude; a name leaves its file only through that file's `export { … };`. A kama program must never
fail in the C compiler, and a private name must never be reachable.

**Today the rule holds only when the name IS the whole type.** A bad name nested as a type argument, in an
expression-level type operand, or in a handful of unwalked positions reaches clang, draws an unrelated
message, or — worst — compiles.

## How it was measured

Every position below was compiled once per case, under both `kama check` and `kama build` (and run when it
built), against a two-file program: `main.kama` + a local module `geo/lib.kama`.

**Cases** — what the placeholder name is:

| case | name | correct outcome |
|---|---|---|
| **a** | totally unknown (`Zork`) | kama error: unknown |
| **b** | exported by `geo`, but not imported (`ShownErr`) | kama error: not imported |
| **bq** | exported, spelled qualified, not imported (`geo::ShownErr`) | **legal** — builds and runs |
| **c** | private in `geo`, spelled bare (`HidErr`) | kama error: not imported / not exported — never a hint to import it |
| **cq** | private, spelled qualified (`geo::HidErr`) | kama error: not exported by `geo/lib.kama` |
| **d** | imported (control) | **legal** |
| **bs** / **bsq** | `std::uuid::UuidError` with only `Uuid` imported, bare / qualified | error / legal |

**The module** (`geo/lib.kama`) — every `Shown*` is exported, every `Hid*` is its private twin:

```kama
export { Shown, ShownErr, ShownVal, shownFn, SHOWNK, ShownC };

comptime isize SHOWNK = 3;
comptime isize HIDK = 4;

type contract ShownC for value { fn int32 cm(); }
type contract HidC for value { fn int32 cm(); }

type enum ShownErr implements Error {
    Bad, Worse(isize at);
    public const fn string message() { return "shown"; }
}
type enum HidErr implements Error {
    Bad, Worse(isize at);
    public const fn string message() { return "hid"; }
}

@generate(Serializable, Deserializable)
type value ShownVal {
    @field public int32 v;
    public ctor make() { this.v = 1; }
    public static fn int32 stat() { return 5; }
}
@generate(Serializable, Deserializable)
type value HidVal {
    @field public int32 v;
    public ctor make() { this.v = 2; }
    public static fn int32 stat() { return 6; }
}

type value Shown {
    public int32 v;
    public ctor make() { this.v = 1; }
    public ctor Result<Shown, ShownErr> parse(int32 n) {
        if (n < 0) { return Result::Err(error: ShownErr::Bad); }
        return Result::Ok(value: Shown.make());
    }
}

fn int32 shownFn() { return 1; }
fn int32 hidFn() { return 2; }
```

**The positions** — each `main.kama` is `import { geo::Shown };` (plus the case's imports) followed by one
template. Placeholders: `@T@` a type, `@V@` a variant expression (`X::Bad`), `@R@` a record type
(`ShownVal`/`HidVal`), `@F@` a function, `@K@` a constant, `@C@` a contract.

| id | template |
|---|---|
| T01 local | `fn int32 main() { @T@ x = @V@; return 0; }` |
| T03 `Optional<>` | `fn int32 main() { Optional<@T@> x = Optional::None; return 0; }` |
| T04 `Result<,>` | `fn int32 main() { Result<int32, @T@> r = Result::Ok(value: 1); return 0; }` |
| T05 depth 2 | `fn int32 main() { Optional<Result<int32, @T@>> r = Optional::None; return 0; }` |
| T06 container | `DynamicArray<@T@> a = DynamicArray.empty();` (imports `DynamicArray`) |
| T07 param | `fn int32 f(@T@ e) { return 0; }` |
| T08 nested param | `fn int32 f(Optional<@T@> e) { return 0; }` called with `Optional::None` |
| T09/T10 nested return | `fn Optional<@T@> f() { return Optional::None; }`, uncalled / matched on |
| T11 field | `type value W { public @T@ e; public ctor make() { this.e = @V@; } }` |
| T12 nested field | `type value W { public Optional<@T@> e; … }`, never constructed |
| T13 `Owned<>` field | `type resource W { Owned<@R@> p; public ctor make(Owned<@R@> p) { this.p = give p; } }` |
| T14 fn bound | `fn int32 g<X: @C@>(X x) { return x.cm(); }` |
| T15 type bound | `type value G<X: @C@> { X x; }` |
| T16 `implements` | `type value W implements @C@ { … public fn int32 cm() { … } }` |
| T17 contract type arg | `type contract Holds<X> for value { fn int32 cm(); }` + `type value W implements Holds<@R@> { … }` |
| T18 turbofish | `fn int32 z<X>() { return 0; }` + `return z::<@T@>();` |
| T19 serde turbofish | `deserializeJsonBuffer::<@R@>(src: give s)` |
| T20 `cast<>` | `cast<UnsafePtr<@R@>>(p)` in an `unsafe fn` |
| T21 `sizeof` | `usize n = sizeof(@R@);` |
| T22 `.as<>()` | `match (e.as<@T@>()) { … }` on a boxed `DeError` |
| T24 variant expr | `Result::Err(error: @V@)` |
| T25 ctor call | `int32 n = @R@.make().v;` |
| T26 static call | `int32 n = @R@::stat();` |
| T27 `InlineArray<>` | `fn int32 f(InlineArray<@T@>#(2) a)` |
| T28 `Owned<>` param | `fn int32 f(Owned<@R@> p)` |
| T30 `new` | `Owned<@R@> p = new @R@.make();` |
| T31 extern sig | `extern fn int32 kama_probe_nope(@R@ x);` |
| T32 `@generate` field | `@generate(Serializable, Deserializable) type value W { @field public @R@ inner; … }` |
| T33 static | `static Optional<@T@> gs;` |
| T34 method return | `public fn Optional<@T@> m()` on a type |
| T35 user generic arg | `type value Box<X> { public X x; }` + `fn int32 f(Box<@T@> b)` |
| T36 fnptr | `fnptr int32 Cb(@R@ x);` |
| T37/T38 generic body | `fn int32 g<X>(X x) { Optional<@T@> o = Optional::None; … }`, uncalled / called |
| F01 call | `int32 n = @F@();` |
| F02 constant | `isize k = @K@;` |
| F03 const-generic | `fn int32 f(InlineArray<int32>#(@K@) a)` |

Follow-ups that sharpened it: the reported repro against an instance that already exists
(`Result<Shown, X> r = Shown.parse(n: 1)`), the same as parameter and field; `Result<F, Zork>` with every
kind of first argument (`int32`, `string`, a user type, a qualified type, `Optional<int32>`,
`DynamicArray<int32>`) — **every one hides the unknown second argument**; a user `Pair<LocalV, Zork>`;
`std::encoding::hex::decode` with `DecodeError` unimported; a private qualified variant, bound and turbofish
that actually RUN.

## Results (`0.9.320`)

**KD** correct kama diagnostic · **Ki** kama error, unrelated message · **C** passes `check`, fails in clang ·
**ACC** compiled (and ran). `check` and `build` agree except where marked ⚠. Every **bq** and **d** control
built and ran, except F03 and T16 (below).

| position | a | b | c | cq | bs |
|---|---|---|---|---|---|
| local / param / field / fnptr param / `@generate` field | KD | KD, wrong hint ¹ | KD, suggests importing a PRIVATE name | KD | KD, wrong hint ¹ |
| **`Result<Shown, X> r = Shown.parse(…)`**, hex `decode` | **C** `undeclared identifier 'Result_geo__Shown_Zork'` | **C** | **C** | **C** | **C** |
| `Result<…, X>` as param, return, field (any first arg) | C | C | C | C | C |
| `Optional<X>`, `Optional<Result<int32, X>>` | Ki *"`Optional` has no variant `None`"* | Ki | Ki | KD | Ki |
| `Result<int32, X> r = Result::Ok(…)` | Ki *"resolves to no known function"* | Ki | Ki | KD | Ki |
| `DynamicArray<X> a = DynamicArray.empty()` | Ki *"cannot tell which `DynamicArray` to construct"* | Ki | Ki | KD | Ki |
| param `Owned<X>`, `InlineArray<X>#(2)`, user `Box<X>` | C | C | C | KD | C |
| field `Owned<X>` in a resource | C | C | C | KD | – |
| `static Optional<X> gs;` | C `unknown type name 'Optional_UuidError'` | C | C | KD | C |
| `cast<X>(0)`, `cast<UnsafePtr<X>>(p)`, `sizeof(X)` | C | C | C | KD | – |
| const-generic `#(K)` | C | C | C | C | – |
| bound `<T: X>` | KD | KD | KD | **ACC** when never called | – |
| `implements X` | KD, but names no name | same | same | same | – |
| **`implements Holds<X>`** | **ACC** | **ACC** | **ACC** | **ACC** | – |
| turbofish `z::<X>()`, `deserializeJsonBuffer::<X>` | Ki *"only valid on a generic function"* | Ki | Ki | **ACC** — runs | Ki |
| `e.as<X>()` in a match | Ki *"subject's type could not be resolved"* | Ki | Ki | KD | Ki |
| variant expression `X::Bad` | KD | KD | KD | **ACC** — `geo::HidErr::Worse(at: 7)` runs | KD |
| ctor `X.make()`, static `X::stat()` | KD / Ki | same | same | KD | – |
| `Optional<geo::HidErr>` inside a generic body | Ki | Ki | Ki | ⚠ `check` KD, **`build` ACC** | Ki |
| function call, constant read | KD (the call message names no name) | KD | KD | KD | – |
| `extern fn` signature | ACC — skipped on purpose (below) | ACC | ACC | ACC | – |

¹ The hint is built from a generic instance's mangled key: *"it lives in `Result_std::uuid::Uuid_std::uuid`;
add `import { Result_std::uuid::Uuid_std::uuid::UuidError };`"*.

Also broken for LEGAL code: `implements geo::ShownC` is refused as an unknown contract unless `ShownC` is
also imported bare, and `InlineArray<int32>#(geo::SHOWNK)` fails in C.

## Root causes (`src/kama.cemit.cpp`, `0.9.320`)

1. **Type arguments are never walked.** `checkTypeResolves` (~2268) inspects the outer type and returns early
   when `cType(t) != name` — a generic instance always mangles to SOME name, so it passes whatever its
   arguments are; its own comment says the argument "is still not walked here". `checkDeclaredTypes` (~2707)
   and `checkBodyLocals` (~2791) check the outer name only.
2. **Reach is judged as a side effect of emission.** `checkReach` (~2589) is called from the resolver
   (~577, 2687, 2993, 4401, 22654, 22986), so a name that is never emitted — a bound, a turbofish argument, a
   qualified variant, an unused declaration — is never judged. Inside a generic instance it is skipped
   (`_nsCtx.unitPath != refFile`, ~2663; `_refUnit` null), which is the `check`/`build` disagreement:
   `check` probes the template, `build` emits the instance.
3. **Positions with no check at all:** `static` declarations (not in the `checkDeclaredTypes` walk);
   `sizeof`/`cast` operands; `#(K)` arguments; a contract's type arguments in `implements` (~24317 checks the
   contract name only); generic-function bounds before instantiation (~18202).
4. **Turbofish** falls through to an unrelated message when a type argument fails (~22743).
5. **Hints:** `namespaceOfType` (~660) splits every `_classes`/`_enums` key on its last `__`, instance keys
   included, and does not filter unexported keys.

## Plan

1. **Fixtures first, RED.** One `tests/xfail/` fixture per failing cell, private-name ACCEPTED cells first
   (they are the correctness hole). Each carries a `.msg` substring naming the bad name, and a row in
   `tests/xfail/DIAGNOSTIC_LINES` (`KAMA_UPDATE_DIAG_LINES=1 ./dev test`). Multi-file cases are
   `tests/xfail/<name>.d/` with the `geo` module above. The **bq**/**d** controls — including the two LEGAL
   spellings that fail today (`implements geo::ShownC`, `#(geo::SHOWNK)`) — become a positive fixture, so the
   fix cannot over-refuse.
2. **One walk.** A single recursive resolver over a type node — outer name, every type argument, `#(…)`
   arguments — that resolves each name and judges its reach from the file that wrote it, called from EVERY
   type position: declarations (including `static`), signatures, fields, bounds, `implements` and its
   arguments, turbofish, `cast`/`sizeof`, `.as<>()`, `new`, qualified variant and ctor expressions. It runs
   in collection, independent of what is emitted, so `check` and `build` cannot disagree; `checkReach`'s
   emission-time callers then become redundant and are deleted rather than kept beside it.
3. **Generic bodies** are judged once, at the template, from the template's file — the instance skip at
   ~2663 goes.
4. **Hints** come from the declaration table (declaring file + export list), never from a mangled key, and
   never suggest importing a name that is not exported.
5. **Turbofish** reports the failing type argument itself.
6. **`SPEC.md` § Modules** states the rule for nested positions explicitly, each claim with its
   `<!-- xfail: … -->` marker (check-doc-claims).

**Open — decide during the work, and write the answer here:** the `extern fn` signature skip (~2856) is
deliberate — an extern names C types owned by its header (`tests/callback_qsort.d`), which kama must pass
through verbatim. But a name that IS a kama declaration (qualified, or found in the program's tables) in an
extern signature could still be judged for reach without breaking that seam. Whether to, and how a C typedef
is told apart from a misspelled kama type, is the one design question here.

## Size and risk

L. The walk itself is modest; the risk is breadth — every type position in the grammar, and the corpus
(~28k lines of kama) proving no LEGAL spelling is refused. `kama check` runs over every fixture in the
suite's analysis-agreement phase, which is the net for that: it caught the 33-fixture regression the
`0.9.318` scope fix first introduced.
