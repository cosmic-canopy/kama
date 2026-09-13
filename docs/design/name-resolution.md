# Name resolution and visibility at every type position (KR-46)

**Status:** measured, not started — **the next work item** (maintainer, 2026-09-12). Found building KR-12
at `0.9.320`. This is the working doc for the campaign: the probe grid it was measured with, the results,
the root causes, and the plan. Deleted when KR-46 ships, once `SPEC.md` § Modules and the `tests/xfail/`
fixtures carry the record.

## Picking this up — on any machine

This campaign spans several sessions and may move between hosts, so everything a fresh session needs is in
git: this doc, [allocation.md](allocation.md), and the rows in `docs/ROADMAP.md`. Nothing depends on an
assistant's local memory or on a scratch directory.

**State at handoff (2026-09-12).** `dev` at `0.9.320`. The work that surfaced all of this shipped:
`0.9.318` (a foreign member's return type resolves in its own file), `0.9.319` (`.as<T>()` across units),
`0.9.320` (`std::uuid`, KR-12). They passed the native suite (1702/0) and all 68 guards on the Windows VM.
**Picked up on Linux (Ubuntu 24.04, 2026-09-13).** `./dev matrix` at `0.9.320` is green there — native
1702/0, san 1703/0, wasm 1673/0, 68 guards — which is the first sanitizer and wasm run for `0.9.318`–`0.9.320`.
A Linux host runs those legs natively now (`dev`, 12b14faa), so the gate below needs no container there. The
grid was re-run on that host and **no cell moved**. One precision for the results table: its turbofish
**cq** "ACC — runs" is `z::<geo::HidVal>()` (X15); T19's `deserializeJsonBuffer::<geo::HidVal>` is refused,
but only because its template also spells the private type in the local's declared type, so the turbofish
there is never the thing judged.

**The agreed order** is the top of the NOW table in `docs/ROADMAP.md`: **KR-46** (this doc) → **KR-47**
reach-based `--no-heap` → **KR-51** `Handle` → **KR-48** `kama_alloc`/`kama_free` → **KR-49** replaceable
global allocator → **KR-50** allocator-aware errors.

**How the maintainer wants this done** — the constraints, not suggestions:

- **Production grade. Root cause, never a workaround.** A defect found along the way is fixed where it
  lives, in its own commit, with its own `VERSION` bump and a regression fixture that fails on the previous
  compiler, and the commit cites where it was found (`— found building KR-46`). If it is too big for that,
  it becomes a KR row with its reasoning, not a library-side dodge.
- **Consistent principles.** A rule that holds in some positions and not others is the defect (that is
  what this row is). The same goes for `--no-heap` in KR-47.
- **Fixtures land RED first**, and a doc claim that something is rejected carries its `tests/xfail/` marker.
- **The user pushes.** Commit on `dev`; do not push. Fetch and rebase onto `origin/dev` at the start of a
  session — other work lands in between.

**First steps for KR-46:**

1. `git fetch && git rebase origin/dev`, `./dev build`.
2. Re-run the grid from the appendix on the current compiler and diff against the results table below —
   some cells may have moved. Record what changed here.
3. Then the plan below, step 1 (fixtures), private-name ACCEPTED cells first.

**Gate per host.** macOS/Linux: `./dev matrix > /tmp/m.log 2>&1` once, then read the file (on Linux the wasm
leg needs emsdk's `emcc` on PATH — `. ~/emsdk/emsdk_env.sh`). Windows VM:
`./dev matrix` cannot pass there (no containers, and it skips the guards when the container leg fails), so
the gate is `./dev test` then `./dev check`, each into its own log, and the san/wasm legs are reported as
not run. See `docs/platforms/windows.md` for driving that shell.

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

## Appendix — the probe harness, verbatim

Recreate it anywhere: make a scratch directory (e.g. `.scratch/imports/`, which is gitignored), write the
files below into it, put the `geo` module from **How it was measured** at `geo/lib.kama`, then

```sh
ROOT=$(pwd); . tools/kama-bin.sh; export KAMA; bash .scratch/imports/runall.sh && bash .scratch/imports/table.sh
```

from the repo root. `runall.sh` regenerates `probes/` from `probes.txt` and runs every cell (`JOBS=4` parallel);
`table.sh` prints the `check/build` grid. `xprobes.sh` writes the follow-up programs into `xprobes/`; run
each with `bash run1.sh xprobes/<name>`. A probe binary is named `p.exe` on every host.

### `probes.txt`

```text
=== T01_local_decl_init
fn int32 main() { @T@ x = @V@; return 0; }
=== T03_arg1_optional
fn int32 main() { Optional<@T@> x = Optional::None; return 0; }
=== T04_arg1_result
fn int32 main() { Result<int32, @T@> r = Result::Ok(value: 1); return 0; }
=== T05_arg2_opt_result
fn int32 main() { Optional<Result<int32, @T@>> r = Optional::None; return 0; }
=== T06_arg1_dynarray
#import std::collections::DynamicArray
fn int32 main() { DynamicArray<@T@> a = DynamicArray.empty(); return 0; }
=== T07_param
fn int32 f(@T@ e) { return 0; }
fn int32 main() { return 0; }
=== T08_param_nested
fn int32 f(Optional<@T@> e) { return 0; }
fn int32 main() { return f(e: Optional::None); }
=== T09_return_nested
fn Optional<@T@> f() { return Optional::None; }
fn int32 main() { return 0; }
=== T10_return_nested_called
fn Optional<@T@> f() { return Optional::None; }
fn int32 main() { return match (f()) { case Some(value: v): 1; case None: 0; }; }
=== T11_field_value
type value W { public @T@ e; public ctor make() { this.e = @V@; } }
fn int32 main() { W w = W.make(); return 0; }
=== T12_field_nested_uncalled
type value W { public Optional<@T@> e; public ctor make() { this.e = Optional::None; } }
fn int32 main() { return 0; }
=== T13_field_resource_owned
type resource W { Owned<@R@> p; public ctor make(Owned<@R@> p) { this.p = give p; } }
fn int32 main() { return 0; }
=== T14_generic_bound_fn
fn int32 g<X: @C@>(X x) { return x.cm(); }
fn int32 main() { return 0; }
=== T15_generic_bound_type
type value G<X: @C@> { X x; }
fn int32 main() { return 0; }
=== T16_implements
type value W implements @C@ { int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }
fn int32 main() { W w = W.make(); return w.cm() - 1; }
=== T17_implements_generic_arg
type contract Holds<X> for value { fn int32 cm(); }
type value W implements Holds<@R@> { int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }
fn int32 main() { return 0; }
=== T18_turbofish_fn
fn int32 z<X>() { return 0; }
fn int32 main() { return z::<@T@>(); }
=== T19_turbofish_deser
#import std::serialization::text::json::deserializeJsonBuffer
fn int32 main() { string s = "{\"v\":1}"; Result<@R@, Owned<Error>> g = deserializeJsonBuffer::<@R@>(src: give s); return 0; }
=== T20_cast
unsafe fn int32 f(UnsafePtr p) { UnsafePtr w = cast<UnsafePtr>(cast<UnsafePtr<@R@>>(p)); return 0; }
fn int32 main() { return 0; }
=== T21_sizeof
fn int32 main() { usize n = sizeof(@R@); return 0; }
=== T22_as_downcast
fn int32 main() { Result<int32, Owned<Error>> r = Result::Err(error: DeError::Malformed); return match (r) { case Ok(value: v): 0; case Err(error: e): match (e.as<@T@>()) { case Some(value: x): 1; case None: 0; }; }; }
=== T24_variant_expr
fn int32 main() { Result<int32, Owned<Error>> r = Result::Err(error: @V@); return 0; }
=== T25_ctor_call
fn int32 main() { int32 n = @R@.make().v; return n - 1; }
=== T26_static_fn_call
fn int32 main() { int32 n = @R@::stat(); return 0; }
=== T27_inlinearray_elem
fn int32 f(InlineArray<@T@>#(2) a) { return 0; }
fn int32 main() { return 0; }
=== T28_owned_param
fn int32 f(Owned<@R@> p) { return 0; }
fn int32 main() { return 0; }
=== T30_new_expr
fn int32 main() { Owned<@R@> p = new @R@.make(); return 0; }
=== T31_extern_fn_sig
extern fn int32 kama_probe_nope(@R@ x);
fn int32 main() { return 0; }
=== T32_generate_field
@generate(Serializable, Deserializable)
type value W { @field public @R@ inner; public ctor make(@R@ inner) { this.inner = inner; } }
fn int32 main() { return 0; }
=== T33_static_var
static Optional<@T@> gs;
fn int32 main() { return 0; }
=== T34_method_return_nested
type value W { public int32 a; public ctor make() { this.a = 1; } public fn Optional<@T@> m() { return Optional::None; } }
fn int32 main() { W w = W.make(); return w.a - 1; }
=== T35_user_generic_arg
type value Box<X> { public X x; }
fn int32 f(Box<@T@> b) { return 0; }
fn int32 main() { return 0; }
=== T36_fnptr_sig
fnptr int32 Cb(@R@ x);
fn int32 main() { return 0; }
=== T37_generic_fn_body_uncalled
fn int32 g<X>(X x) { Optional<@T@> o = Optional::None; return 0; }
fn int32 main() { return 0; }
=== T38_generic_fn_body_called
fn int32 g<X>(X x) { Optional<@T@> o = Optional::None; return 0; }
fn int32 main() { return g(x: 1); }
=== F01_fn_call
fn int32 main() { int32 n = @F@(); return n - 1; }
=== F02_const_read
fn int32 main() { isize k = @K@; return 0; }
=== F03_const_in_arraysize
fn int32 f(InlineArray<int32>#(@K@) a) { return 0; }
fn int32 main() { return 0; }
```

### `gen.sh`

```bash
#!/bin/bash
# Generate probes/<probe>__<case>/{main.kama,geo/lib.kama} from probes.txt. Cases:
#  a  totally unknown          b  exported by geo, not imported     bq qualified geo::X, not imported (should be OK)
#  c  private in geo, bare     cq private, qualified geo::HidX      d  imported (control)
#  bs std::uuid UuidError unimported (T/V only)                     bsq std::uuid::UuidError qualified (should be OK)
set -u
cd "$(dirname "$0")"
rm -rf probes; mkdir probes
declare -A T V R F K C I
T[a]=Zork;        V[a]=Zork::Bad;          R[a]=ZorkVal;      F[a]=zorkFn;      K[a]=ZORKK;      C[a]=ZorkC;      I[a]="geo::Shown"
T[b]=ShownErr;    V[b]=ShownErr::Bad;      R[b]=ShownVal;     F[b]=shownFn;     K[b]=SHOWNK;     C[b]=ShownC;     I[b]="geo::Shown"
T[bq]=geo::ShownErr; V[bq]=geo::ShownErr::Bad; R[bq]=geo::ShownVal; F[bq]=geo::shownFn; K[bq]=geo::SHOWNK; C[bq]=geo::ShownC; I[bq]="geo::Shown"
T[c]=HidErr;      V[c]=HidErr::Bad;        R[c]=HidVal;       F[c]=hidFn;       K[c]=HIDK;       C[c]=HidC;       I[c]="geo::Shown"
T[cq]=geo::HidErr; V[cq]=geo::HidErr::Bad; R[cq]=geo::HidVal; F[cq]=geo::hidFn; K[cq]=geo::HIDK;  C[cq]=geo::HidC; I[cq]="geo::Shown"
T[d]=ShownErr;    V[d]=ShownErr::Bad;      R[d]=ShownVal;     F[d]=shownFn;     K[d]=SHOWNK;     C[d]=ShownC;     I[d]="geo::Shown, geo::ShownErr, geo::ShownVal, geo::shownFn, geo::SHOWNK, geo::ShownC"
T[bs]=UuidError;  V[bs]=UuidError::InvalidLength; R[bs]=; F[bs]=; K[bs]=; C[bs]=; I[bs]="geo::Shown, std::uuid::Uuid"
T[bsq]=std::uuid::UuidError; V[bsq]=std::uuid::UuidError::InvalidLength; R[bsq]=; F[bsq]=; K[bsq]=; C[bsq]=; I[bsq]="geo::Shown, std::uuid::Uuid"
name=; body=; extra=
flush() {
  [ -z "$name" ] && return
  for cs in a b bq c cq d bs bsq; do
    skip=0
    for p in R F K C; do eval "val=\${$p[$cs]}"; if [ -z "$val" ] && printf '%s' "$body" | grep -q "@$p@"; then skip=1; fi; done
    [ $skip = 1 ] && continue
    # T23 matches on ShownErr — for std cases, match on Uuid.parse instead
    b="$body"
    case $cs in bs|bsq) b="${b//Shown.parse(n: 0 - 1)/Uuid.parse(text: \"x\")}"; b="${b//::Bad/::InvalidLength}"; b="${b//::Worse(at: a)/::InvalidCharacter(at: a)}";; esac
    b="${b//@T@/${T[$cs]}}"; b="${b//@V@/${V[$cs]}}"; b="${b//@R@/${R[$cs]}}"; b="${b//@F@/${F[$cs]}}"; b="${b//@K@/${K[$cs]}}"; b="${b//@C@/${C[$cs]}}"
    d=probes/${name}__$cs; mkdir -p $d/geo; cp geo/lib.kama $d/geo/
    imp="${I[$cs]}"; [ -n "$extra" ] && imp="$imp$extra"
    printf 'import { %s };\n%s' "$imp" "$b" > $d/main.kama
  done
}
while IFS= read -r line; do
  case "$line" in
    "=== "*) flush; name=${line#=== }; body=; extra=;;
    "#import "*) extra="$extra, ${line#\#import }";;
    *) body="$body$line"$'\n';;
  esac
done < probes.txt
flush
ls probes | wc -l
```

### `run1.sh`

```bash
#!/bin/bash
# run1.sh <probe dir> : kama check, kama build, run; one summary line to <dir>/result
d=$1; cd "$d" || exit
ck=$("$KAMA" check main.kama geo/lib.kama 2>&1); ckrc=$?
bd=$("$KAMA" build main.kama geo/lib.kama -o p.exe 2>&1); bdrc=$?
ckline=$(printf '%s\n' "$ck" | grep -m1 -E "error|OK" | sed 's/^.*main.kama:/main:/; s/^.*lib.kama:/lib:/')
if [ $bdrc = 0 ]; then ./p.exe >/dev/null 2>&1; bl="BUILT run_rc=$?"
elif printf '%s' "$bd" | grep -q "clang failed"; then bl="C_ERROR: $(printf '%s\n' "$bd" | grep -m1 'error:' | sed 's/^.*error: //')"
else bl="KAMA_ERR($bdrc): $(printf '%s\n' "$bd" | grep -m1 -E 'error|rror' | sed 's/^.*main.kama:/main:/; s/^.*lib.kama:/lib:/')"; fi
printf '%s\tcheck(rc=%s): %s\tbuild: %s\n' "$(basename $d)" "$ckrc" "$ckline" "$bl" > result
```

### `runall.sh`

```bash
#!/bin/bash
# Re-run everything:  MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'pushd /c/Users/matt/Documents/kama >/dev/null; ROOT=$(pwd); . tools/kama-bin.sh; export KAMA; bash .scratch/imports/runall.sh'
cd "$(dirname "$0")"; bash gen.sh
ls -d probes/*/ | xargs -P ${JOBS:-4} -n1 bash run1.sh
cat probes/*/result > results.tsv; wc -l results.tsv
```

### `table.sh`

```bash
#!/bin/bash
# Condense results.tsv: per cell  <check>/<build>  where check: ok|K ; build: run|K|C
# Sorted by `sort`, not gawk's `asorti` — Debian/Ubuntu ship mawk as `awk`, which has no asorti.
cd "$(dirname "$0")"
awk -F'\t' '{
  split($1,p,"__"); ck = ($2 ~ /rc=0/) ? "ok" : "K"
  if ($3 ~ /BUILT/) b="run"; else if ($3 ~ /C_ERROR/) b="C"; else b="K"
  print p[1] "\t" p[2] "\t" ck "/" b
}' results.tsv | sort | awk -F'\t' '
  function row(  i, v) { printf "%-32s", pos; for (i = 1; i <= n; i++) { v = cell[cols[i]]; printf "%-8s", (v == "" ? "-" : v) }; print "" }
  BEGIN { n = split("a b bq c cq d bs bsq", cols, " "); printf "%-32s", "position"; for (i = 1; i <= n; i++) printf "%-8s", cols[i]; print "" }
  $1 != pos { if (pos != "") row(); pos = $1; split("", cell) }
  { cell[$2] = $3 }
  END { if (pos != "") row() }'
```

### `xprobes.sh`

```bash
#!/bin/bash
# Second batch: targeted follow-ups. Writes xprobes/<name>/main.kama (+geo copy); run via run1.sh like probes/.
cd "$(dirname "$0")"; rm -rf xprobes; mkdir xprobes
mk() { mkdir -p xprobes/$1/geo; cp geo/lib.kama xprobes/$1/geo/; cat > xprobes/$1/main.kama; }
# (1) the reported repro shape: a Result instance that ALREADY exists in the program (geo's Shown.parse)
for t in ShownErr:b Zork:a HidErr:c geo::HidErr:cq; do n=${t%%:*}; c=${t##*:}
mk X01_existing_instance_init__$c <<K
import { geo::Shown };
fn int32 main() { Result<Shown, $n> r = Shown.parse(n: 1); return 0; }
K
mk X02_existing_instance_okctor__$c <<K
import { geo::Shown };
fn int32 main() { Result<Shown, $n> r = Result::Ok(value: Shown.make()); return 0; }
K
mk X03_existing_instance_param__$c <<K
import { geo::Shown };
fn int32 f(Result<Shown, $n> r) { return 0; }
fn int32 main() { return 0; }
K
mk X04_bound_unused__$c <<K
import { geo::Shown };
fn int32 g<X: ${n/Err/C}>(X x) { return 0; }
fn int32 main() { return g(x: 1); }
K
mk X05_implements_arg_called__$c <<K
import { geo::Shown };
type contract Holds<X> for value { fn int32 cm(); }
type value W implements Holds<$n> { public int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }
fn int32 main() { W w = W.make(); return w.cm() - 1; }
K
mk X06_cast_direct__$c <<K
import { geo::Shown };
fn int32 main() { int32 k = cast<int32>(cast<$n>(0)); return 0; }
K
mk X07_sizeof_plain__$c <<K
import { geo::Shown };
fn int32 main() { usize k = sizeof($n); return 0; }
K
done
mk X01_existing_instance_init__d <<'K'
import { geo::Shown, geo::ShownErr };
fn int32 main() { Result<Shown, ShownErr> r = Shown.parse(n: 1); return 0; }
K
mk X08_implements_qualified_imported__d <<'K'
import { geo::Shown, geo::ShownC };
type value W implements geo::ShownC { public int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }
fn int32 main() { W w = W.make(); return w.cm() - 1; }
K
mk X09_hex_decode__b <<'K'
import { std::encoding::hex::decode, std::collections::DynamicArray };
fn int32 main() { Result<DynamicArray<uint8>, DecodeError> r = decode(text: "00"); return 0; }
K
mk X09_hex_decode__d <<'K'
import { std::encoding::hex::decode, std::encoding::hex::DecodeError, std::collections::DynamicArray };
fn int32 main() { Result<DynamicArray<uint8>, DecodeError> r = decode(text: "00"); return 0; }
K
mk X10_uuid_param__bs <<'K'
import { std::uuid::Uuid };
fn int32 f(Result<Uuid, UuidError> r) { return 0; }
fn int32 main() { return 0; }
K
mk X11_uuid_return__bs <<'K'
import { std::uuid::Uuid };
fn Result<Uuid, UuidError> f() { return Uuid.parse(text: "x"); }
fn int32 main() { return 0; }
K
mk X12_uuid_field__bs <<'K'
import { std::uuid::Uuid };
type value W { public Result<Uuid, UuidError> r; public ctor make() { this.r = Uuid.parse(text: "x"); } }
fn int32 main() { W w = W.make(); return 0; }
K
mk X13_private_qualified_variant_run__cq <<'K'
import { geo::Shown };
fn int32 main() { Result<int32, Owned<Error>> r = Result::Err(error: geo::HidErr::Worse(at: 7)); return 0; }
K
mk X14_private_fn_as_bound_qualified__cq <<'K'
import { geo::Shown };
type value R2 implements geo::ShownC { public int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }
fn int32 g<X: geo::HidC>(X x) { return x.cm(); }
fn int32 main() { return 0; }
K
mk X15_private_turbofish_used__cq <<'K'
import { geo::Shown };
fn usize z<X>() { return sizeof(X); }
fn int32 main() { return cast<int32>(z::<geo::HidVal>()) - 4; }
K
# (3) which FIRST type argument hides an unresolved second one?
i=0
for first in "int32" "string" "Shown" "geo::Shown" "Optional<int32>" "DynamicArray<int32>" "LocalV" "Zork"; do i=$((i+1))
mk Y0${i}_result_first_arg__a <<K
import { geo::Shown, std::collections::DynamicArray };
type value LocalV { public int32 a; }
fn int32 f(Result<$first, Zork> r) { return 0; }
fn int32 main() { return 0; }
K
done
mk Y09_optional_user_then_unknown__a <<'K'
import { geo::Shown };
type value LocalV { public int32 a; }
type value Pair<A, B> { public A a; public B b; }
fn int32 f(Pair<LocalV, Zork> r) { return 0; }
fn int32 main() { return 0; }
K
mk Y10_unknown_first__a <<'K'
import { geo::Shown };
fn int32 f(Result<Zork, int32> r) { return 0; }
fn int32 main() { return 0; }
K
mk Y11_local_decl_result_userfirst__a <<'K'
import { geo::Shown };
type value LocalV { public int32 a; }
fn int32 main() { Optional<Result<LocalV, Zork>> r = Optional::None; return 0; }
K
```

