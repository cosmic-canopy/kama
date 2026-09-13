# Name resolution and visibility at every type position (KR-46)

**Status:** in progress. Plan step 1 (fixtures) is DONE and measured; **next is plan step 2, the one
walk**. Found building KR-12 at `0.9.320`. This is the working doc for the campaign: the probe grid, the
results, the root causes, the fixtures and the plan. Deleted when KR-46 ships, once `SPEC.md` § Modules and
the `tests/xfail/` fixtures carry the record.

## Picking this up — on any machine

This campaign spans several sessions and may move between hosts, so everything a fresh session needs is in
git: this doc, [allocation.md](allocation.md), and the rows in `docs/ROADMAP.md`. Nothing depends on an
assistant's local memory or on a scratch directory.

**State at handoff (2026-09-13, Linux).** `dev` at `0.9.321` (`fa77056f`), gate green on every leg: native
1703/0, san 1704/0, wasm 1674/0, 68 guards. Since the Windows handoff at `0.9.320`:

- `12b14faa` — a Linux host runs the san/tsan/msan/wasm/linux legs natively (the container would build into
  the same `out/Linux-<arch>/` and replace the host binary). First san/wasm run of `0.9.318`–`0.9.320`: green.
- `b26d5e36` — the grid re-measured on Linux: no cell moved.
- `01eec6e8` (`0.9.321`) — found building KR-46: nested checked arithmetic emitted C that doubled per level
  (a 38-term sum crashed clang). Fixed at the emitter; not part of KR-46's scope.
- `fa77056f` — the KR-46 fixtures: generated, every template proven legal, **119 of 162 red**. They are NOT
  in the tree — each lands with the fix slice that turns it green, so every commit keeps the gate green.
  Their generator is `genfix.py` in the appendix; the **Fixtures** section says what they found.

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

**First steps next session:**

1. `git fetch && git rebase origin/dev`, `./dev build`.
2. Recreate the scratch tools: extract `genfix.py` from the appendix into `.scratch/imports/`
   (`awk '/^### \`genfix.py\`/{f=1} f&&/^\`\`\`python/{b=1;next} b&&/^\`\`\`$/{exit} b' docs/design/name-resolution.md > .scratch/imports/genfix.py`),
   then `python3 .scratch/imports/genfix.py .` writes all 162 fixtures + the two controls into `tests/`.
3. Classify them on the current compiler with `classify.sh` (appendix) — expect **12 ACCEPTED, 36 C, 71 MSG,
   43 green**. A different count means the compiler moved; record it here before changing anything.
4. `rm -rf tests/xfail/reach_* tests/reach_controls_*` before any gate run that is not meant to be red, and
   re-generate for the slice being worked. Then the plan below, step 2, slice 1.

**Gate per host.** macOS/Linux: `./dev matrix > /tmp/m.log 2>&1` once, then read the file (on Linux the wasm
leg needs emsdk's `emcc` on PATH — `. ~/emsdk/emsdk_env.sh`). Windows VM:
`./dev matrix` cannot pass there (no containers, and it skips the guards when the container leg fails), so
the gate is `./dev test` then `./dev check`, each into its own log, and the san/wasm legs are reported as
not run. See `docs/platforms/windows.md` for driving that shell. Iterate with `./dev fixture <name>…` — it
runs `tests/xfail/<name>` too, but NOT a `.d` directory; `classify.sh` is the inner loop for those.

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

## Fixtures (plan step 1) — generated, measured RED at `0.9.321`

`genfix.py` (appendix) writes them from the templates above, one per (position, case), with ONE `.msg`
rule per case so every position is held to the same diagnostic:

| case | fixture | `.msg` must contain |
|---|---|---|
| a | `tests/xfail/reach_<pos>_unknown.kama` (single file — the analysis-agreement leg covers it) | `` unknown type `Zork` `` / `` unknown contract `ZorkC` `` / the value name |
| b | `tests/xfail/reach_<pos>_unimported.d/` (minimal `geo`) | `` add `import { geo::ShownErr };` `` — the hint must be right |
| c, cq | `tests/xfail/reach_<pos>_private[_qualified].d/` | `` `HidErr` is not exported by `` — never an import hint |
| bs | `tests/xfail/reach_result_{local,param,return,field}_std_unimported.kama`, `reach_hex_decode_…` | `` add `import { std::uuid::UuidError };` `` |

Positive controls: `tests/reach_controls_imported.d/` and `tests/reach_controls_qualified.d/` — every
position in one program (each fails with its own exit code). Each template was first built LEGALLY on its
own (`genfix.py <root> --controls <dir>`, 39/39 build and return 0), so no rejection fixture passes by
accident of a broken template. `cast_direct` has no legal spelling (an enum is not castable) and so no
control; `extern_sig` has only the qualified one (see the extern decision below).

**Measured on `0.9.321`:** 162 fixtures — **12 compile** (`implements_arg` a/b/c/cq; `bound_fn`,
`bound_type`, `turbofish`, `turbofish_deser`, `variant_expr`, `generic_body`, `generic_body_called`,
`extern_sig` — all cq), **36 fail in C**, **71 carry the wrong message**, 43 already right. The imported
control builds and runs; the qualified one is refused at `implements geo::ShownC` (and `#(geo::SHOWNK)`
still fails in C behind it).

**What the fixtures showed that the results table above did not:**

- The serde turbofish with a private qualified type COMPILES once isolated — T19's template also spelled the
  type in the local's declared type, which is what refused it. Likewise a generic TYPE's bound (`bound_type`
  cq), not only a function's.
- Several cells the table scores KD carry the wrong message: a bound naming a bad contract reports "`X` has
  no bound providing `cm`" (`bound_fn` a/b/c); `implements` reports "unknown contract in implements" and
  names nothing; an unimported function or constant reports "call to unknown function" / "cannot resolve"
  with no import hint (`fn_call`, `const_read` b/c).
- The **b** hint is mangled for a LOCAL module too, not only for std: `import { Result_geo::Shown_geo::ShownErr }`
  (`local_unimported` — `geo` already builds a `Result<Shown, ShownErr>`), and every bare **c** hint
  suggests importing the private name.
- `xprobes.sh` below had a bug: `n=${t%%:*}` cut `geo::HidErr:cq` at the FIRST colon, so every X-series
  **cq** probe spelled `geo`, not `geo::HidErr`. Those cq results were harness artifacts; the fixtures replace
  them.
- The analysis-agreement leg of `run_tests.sh` skips `.d` fixtures on the belief that files are checked one
  at a time; `kama check` takes several files, and all 24 existing `tests/xfail/*.d` are refused by it. It
  should cover them, so the generic-body `check`/`build` disagreement is asserted in both directions.

## Plan

1. **Fixtures, RED — DONE** (see **Fixtures**). Each lands with the slice below that turns it green, with its
   row in `tests/xfail/DIAGNOSTIC_LINES` (`KAMA_UPDATE_DIAG_LINES=1 ./dev test`; read that diff).
2. **One walk**, grown out of `checkDeclaredTypes` — it already visits every declaration once, before
   emission (called at the tail of `collectProgram`), sets `_nsCtx` per unit and binds type/const params, so
   it is the single visit; nothing new goes beside it. In commit slices, each gated green:
   1. **Nested type arguments + hints.** Replace the head-only `check` lambda with a recursive
      `checkTypeNode`: keep its head rules (`rejectBareCChar`, `This`/`Base`, const param in type position,
      `tp`, `checkQualifiedExport`, `checkNoLeak`, `rejectMintProtocolValue`), judge the head by RESOLVING the
      name (not `cType(t) != name`, which waves every generic instance through), recurse into `genericArgs`
      and `qualifierGenericArgs`, and resolve a `#(K)` identifier as a VALUE judged for reach
      (`constArgValue` literals skip). Rewrite `namespaceOfType` from the declaration table (`declFileOf` +
      `_exported`): no instance keys, never a private name. One message per case (the **Fixtures** table).
      Turns green: the param/return/field/local/static/`Owned`/`InlineArray`/user-generic C cells, the
      `Optional`/`Result`/`DynamicArray` MSG cells, all hint cells, `const_generic_arg`.
   2. **Declaration positions not walked today:** `static` declarations; `implements` contract AND its type
      arguments (the contract-name check is at ~24360, and names nothing); class base types; bounds on
      function, type and method type params, before any instantiation (~18245 today, instantiation-time
      only; `bound_fn` a/b/c's "`X` has no bound providing `cm`" is ~13307 firing first); `fnptr`
      signatures. Keep the walk in step with `buildPositions` step (2) in `kama.query.cpp`.
   3. **Body positions.** Replace `checkBodyLocals` (export-only, over `collectBindings`) with a full statement
      + expression walk — `collectBindingsExpr` (~1681 in `kama.query.cpp`) is NOT exhaustive (no
      `SizeofNode`, `BitcastNode`, turbofish args, `ObjectCreationNode::type`), so it cannot be reused as is.
      Judge: local declared types (full `checkTypeNode`), `Cast`/`Bitcast`/`Sizeof`/`AsDowncast` types,
      `new`, turbofish args, and the qualified head of a value expression (`geo::HidErr::Bad`, `X::stat()`,
      `X.make()`) via `checkReach(qualified=true)`. A generic METHOD's own type params must be bound here —
      their absence is why `checkBodyLocals` stayed export-only. Turbofish (~22787) then reports the failing
      argument, not "only valid on a generic function".
   4. **Generic bodies once, at the template** — remove the instance skips in `checkReach` (~2653 and
      ~2685) only once slice 3 covers those positions; then delete each emission-time `checkReach` caller
      (~577, ~3015, ~4442, ~22697, ~23029) whose positions the walk provably covers (its fixtures stay green
      with the call removed). A bare function call or constant read needs local-scope knowledge the walk
      does not have: a caller kept for that stays with a comment saying so, and this doc records it.
   Also, in the slice that first needs it: extend the analysis-agreement leg of `run_tests.sh` (~1071) to
   `.d` fixtures (`kama check` over all their files, or their `kama.json`) — measured: all 24 existing
   `tests/xfail/*.d` are already refused by it.
3. **`SPEC.md` § Modules** states the rule for nested positions explicitly, each claim with its
   `<!-- xfail: … -->` marker (check-doc-claims).
4. Re-run the grid at the end (every a/b/c/cq/bs cell KD, every bq/bsq/d cell runs), paste the table into
   the commit that deletes this doc, and delete KR-46 from `ROADMAP.md` and `ROADMAP_DETAIL.md` §2.

**`extern fn` — decided (2026-09-13):** keep the skip for an UNQUALIFIED name — an extern names C types owned
by its header (`tests/callback_qsort.d`), and a bare spelling there is the literal C name. A QUALIFIED name is
never a C spelling, so it is judged for reach like any other position (`reach_extern_sig_private_qualified`;
the qualified control carries `geo::ShownVal`). Line numbers in this section are `0.9.321`.

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


### `genfix.py`

Run from the repo root: `python3 .scratch/imports/genfix.py .` writes every fixture above into `tests/`;
`python3 .scratch/imports/genfix.py . --controls <dir>` writes the legal twin of each template instead.

```python
#!/usr/bin/env python3
"""Generate the KR-46 xfail fixtures from the probe grid (docs/design/name-resolution.md).

One fixture per (position, case). Case `a` is a single file; b/c/cq are `.d` directories carrying a
minimal `geo` module; `bs` (std, single file) only for the positions of the reported repro.
Writes into tests/xfail/ and prints the names it wrote.
"""
import os, sys

ROOT = sys.argv[1]
XF = os.path.join(ROOT, "tests", "xfail")
CONTROLS = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--controls" else None

# kind -> (exported twin, private twin) declarations for geo/lib.kama
DECLS = {
    "T": ("type enum ShownErr implements Error {\n    Bad, Worse(isize at);\n    public const fn string message() { return \"shown\"; }\n}",
          "type enum HidErr implements Error {\n    Bad, Worse(isize at);\n    public const fn string message() { return \"hid\"; }\n}"),
    "R": ("@generate(Serializable, Deserializable)\ntype value ShownVal {\n    @field public int32 v;\n    public ctor make() { this.v = 1; }\n    public static fn int32 stat() { return 5; }\n}",
          "@generate(Serializable, Deserializable)\ntype value HidVal {\n    @field public int32 v;\n    public ctor make() { this.v = 2; }\n    public static fn int32 stat() { return 6; }\n}"),
    "F": ("fn int32 shownFn() { return 1; }", "fn int32 hidFn() { return 2; }"),
    "K": ("comptime isize SHOWNK = 3;", "comptime isize HIDK = 4;"),
    "C": ("type contract ShownC for value { fn int32 cm(); }", "type contract HidC for value { fn int32 cm(); }"),
}
SHOWN_T = {"T": "ShownErr", "R": "ShownVal", "F": "shownFn", "K": "SHOWNK", "C": "ShownC"}
HID_T   = {"T": "HidErr",   "R": "HidVal",   "F": "hidFn",   "K": "HIDK",   "C": "HidC"}
ZORK    = {"T": "Zork",     "R": "ZorkVal",  "F": "zorkFn",  "K": "ZORKK",  "C": "ZorkC"}

# (fixture stem, placeholder kind, extra imports, what the cell is, template). `@X@` is the name, `@V@` a variant of it.
POS = [
    ("local",                 "T", [], "a local's declared type", "fn int32 main() { @X@ x = @V@; return 0; }"),
    ("optional_local",        "T", [], "a type argument of a local (`Optional<X>`)", "fn int32 main() { Optional<@X@> x = Optional::None; return 0; }"),
    ("result_local",          "T", [], "the second type argument of a local (`Result<int32, X>`)", "fn int32 main() { Result<int32, @X@> r = Result::Ok(value: 1); return 0; }"),
    ("nested_local",          "T", [], "a depth-2 type argument of a local", "fn int32 main() { Optional<Result<int32, @X@>> r = Optional::None; return 0; }"),
    ("container_local",       "T", ["std::collections::DynamicArray"], "a container's element type (`DynamicArray<X>`)", "fn int32 main() { DynamicArray<@X@> a = DynamicArray.empty(); return 0; }"),
    ("param",                 "T", [], "a parameter type", "fn int32 f(@X@ e) { return 0; }\nfn int32 main() { return 0; }"),
    ("optional_param",        "T", [], "a parameter's type argument", "fn int32 f(Optional<@X@> e) { return 0; }\nfn int32 main() { return f(e: Optional::None); }"),
    ("optional_return",       "T", [], "a return type's type argument, never called", "fn Optional<@X@> f() { return Optional::None; }\nfn int32 main() { return 0; }"),
    ("optional_return_called","T", [], "a return type's type argument, called", "fn Optional<@X@> f() { return Optional::None; }\nfn int32 main() { return match (f()) { case Some(value: v): 1; case None: 0; }; }"),
    ("field",                 "T", [], "a field type", "type value W { public @X@ e; public ctor make() { this.e = @V@; } }\nfn int32 main() { W w = W.make(); return 0; }"),
    ("optional_field",        "T", [], "a field's type argument, never constructed", "type value W { public Optional<@X@> e; public ctor make() { this.e = Optional::None; } }\nfn int32 main() { return 0; }"),
    ("owned_field",           "R", [], "an `Owned<X>` field of a resource", "type resource W { Owned<@X@> p; public ctor make(Owned<@X@> p) { this.p = give p; } }\nfn int32 main() { return 0; }"),
    ("bound_fn",              "C", [], "a generic function's bound, never called", "fn int32 g<X: @X@>(X x) { return x.cm(); }\nfn int32 main() { return 0; }"),
    ("bound_type",            "C", [], "a generic type's bound", "type value G<X: @X@> { X x; }\nfn int32 main() { return 0; }"),
    ("implements",            "C", [], "an `implements` contract", "type value W implements @X@ { int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }\nfn int32 main() { W w = W.make(); return w.cm() - 1; }"),
    ("implements_arg",        "R", [], "a type argument of an `implements` contract", "type contract Holds<X> for value { fn int32 cm(); }\ntype value W implements Holds<@X@> { int32 a; public ctor make() { this.a = 1; } public fn int32 cm() { return this.a; } }\nfn int32 main() { W w = W.make(); return w.cm() - 1; }"),
    ("turbofish",             "T", [], "a turbofish type argument", "fn int32 z<X>() { return 0; }\nfn int32 main() { return z::<@X@>(); }"),
    ("turbofish_deser",       "R", ["std::serialization::text::json::deserializeJsonBuffer"], "a serde turbofish type argument", "fn int32 main() { string s = \"{\\\"v\\\":1}\"; deserializeJsonBuffer::<@X@>(src: give s); return 0; }"),
    ("cast_arg",              "R", [], "a `cast<UnsafePtr<X>>` type argument", "unsafe fn int32 f(UnsafePtr p) { UnsafePtr w = cast<UnsafePtr>(cast<UnsafePtr<@X@>>(p)); return 0; }\nfn int32 main() { return 0; }"),
    ("sizeof",                "R", [], "a `sizeof` operand", "fn int32 main() { usize n = sizeof(@X@); return 0; }"),
    ("as_downcast",           "T", [], "an `.as<X>()` downcast target", "fn int32 main() { Result<int32, Owned<Error>> r = Result::Err(error: DeError::Malformed); return match (r) { case Ok(value: v): 0; case Err(error: e): match (e.as<@X@>()) { case Some(value: x): 1; case None: 0; }; }; }"),
    ("variant_expr",          "T", [], "a variant expression", "fn int32 main() { Result<int32, Owned<Error>> r = Result::Err(error: @V@); return 0; }"),
    ("ctor_call",             "R", [], "a construction", "fn int32 main() { int32 n = @X@.make().v; return n - 1; }"),
    ("static_call",           "R", [], "a static call", "fn int32 main() { int32 n = @X@::stat(); return 0; }"),
    ("inline_array_param",    "T", [], "an `InlineArray<X>#(2)` parameter's element type", "fn int32 f(InlineArray<@X@>#(2) a) { return 0; }\nfn int32 main() { return 0; }"),
    ("owned_param",           "R", [], "an `Owned<X>` parameter", "fn int32 f(Owned<@X@> p) { return 0; }\nfn int32 main() { return 0; }"),
    ("new_expr",              "R", [], "a `new` expression", "fn int32 main() { Owned<@X@> p = new @X@.make(); return 0; }"),
    ("generate_field",        "R", [], "a field of a `@generate` type", "@generate(Serializable, Deserializable)\ntype value W { @field public @X@ inner; public ctor make(@X@ inner) { this.inner = inner; } }\nfn int32 main() { return 0; }"),
    ("static_decl",           "T", [], "a `static`'s type argument", "static Optional<@X@> gs;\nfn int32 main() { return 0; }"),
    ("method_return",         "T", [], "a method return type's type argument", "type value W { public int32 a; public ctor make() { this.a = 1; } public fn Optional<@X@> m() { return Optional::None; } }\nfn int32 main() { W w = W.make(); return w.a - 1; }"),
    ("user_generic_arg",      "T", [], "a user generic's type argument", "type value Box<X> { public X x; }\nfn int32 f(Box<@X@> b) { return 0; }\nfn int32 main() { return 0; }"),
    ("fnptr",                 "R", [], "an `fnptr` parameter type", "fnptr int32 Cb(@X@ x);\nfn int32 main() { return 0; }"),
    ("generic_body",          "T", [], "a local inside a generic function, never called", "fn int32 g<X>(X x) { Optional<@X@> o = Optional::None; return 0; }\nfn int32 main() { return 0; }"),
    ("generic_body_called",   "T", [], "a local inside a generic function, called", "fn int32 g<X>(X x) { Optional<@X@> o = Optional::None; return 0; }\nfn int32 main() { return g(x: 1); }"),
    ("fn_call",               "F", [], "a function call", "fn int32 main() { int32 n = @X@(); return n - 1; }"),
    ("const_read",            "K", [], "a constant read", "fn int32 main() { isize k = @X@; return 0; }"),
    ("const_generic_arg",     "K", [], "a const-generic `#(K)` argument", "fn int32 f(InlineArray<int32>#(@X@) a) { return 0; }\nfn int32 main() { return 0; }"),
    ("cast_direct",           "T", [], "a `cast<X>` target", "fn int32 main() { int32 k = cast<int32>(cast<@X@>(0)); return 0; }"),
    ("extern_sig",            "R", [], "an `extern fn` signature", "extern fn int32 kama_probe_nope(@X@ x);\nfn int32 main() { return 0; }"),
]
# The reported repro: a Result instance geo already builds, named with an unimported error type.
SHOWN = "type value Shown {\n    public int32 v;\n    public ctor make() { this.v = 1; }\n    public ctor Result<Shown, ShownErr> parse(int32 n) {\n        if (n < 0) { return Result::Err(error: ShownErr::Bad); }\n        return Result::Ok(value: Shown.make());\n    }\n}"
POS.append(("existing_instance", "T", ["geo::Shown"], "a type argument of a `Result` instance that already exists",
            "fn int32 main() { Result<Shown, @X@> r = Shown.parse(n: 1); return 0; }"))

CASES = {
    "a":  ("unknown",           "a name declared nowhere"),
    "b":  ("unimported",        "a name `geo` exports that this file does not import"),
    "c":  ("private",           "a name private to `geo/lib.kama`, spelled bare"),
    "cq": ("private_qualified", "a name private to `geo/lib.kama`, spelled qualified"),
}
# `extern fn` keeps its C seam for an unqualified name (a header typedef is not a kama type), so only the
# qualified private spelling is a cell there.
ONLY = {"extern_sig": {"cq"}}

def variant(kind, name):
    return name + "::Bad" if kind == "T" else name

def msg_for(kind, case, stem):
    if case == "a":
        word = {"T": "type", "R": "type", "C": "contract"}.get(kind)
        if word and stem not in ("variant_expr", "ctor_call", "static_call"):
            return f"unknown {word} `{ZORK[kind]}`"
        return f"`{ZORK[kind]}`"
    if case == "b":
        return f"add `import {{ geo::{SHOWN_T[kind]} }};`"
    return f"`{HID_T[kind]}` is not exported by"

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f: f.write(text)


# The reported repro, against std: `Uuid` imported, its error type not. Single files — std needs no fixture module.
STD = [
    ("result_local", "a local's `Result` type argument", "fn int32 main() { Result<Uuid, @X@> r = Uuid.parse(text: \"x\"); return 0; }"),
    ("result_param", "a parameter's `Result` type argument", "fn int32 f(Result<Uuid, @X@> r) { return 0; }\nfn int32 main() { return 0; }"),
    ("result_return", "a return type's `Result` type argument", "fn Result<Uuid, @X@> f() { return Uuid.parse(text: \"x\"); }\nfn int32 main() { return 0; }"),
    ("result_field", "a field's `Result` type argument", "type value W { public Result<Uuid, @X@> r; public ctor make() { this.r = Uuid.parse(text: \"x\"); } }\nfn int32 main() { W w = W.make(); return 0; }"),
]
def gen_std():
    out = []
    for stem, what, tmpl in STD:
        fx = f"reach_{stem}_std_unimported"
        write(os.path.join(XF, fx + ".kama"),
              f"// KR-46: {what}, naming `UuidError` without importing it (only `Uuid` is). Must be rejected with the import that fixes it.\n"
              "import { std::uuid::Uuid };\n" + tmpl.replace("@X@", "UuidError") + "\n")
        write(os.path.join(XF, fx + ".msg"), "add `import { std::uuid::UuidError };`")
        out.append(fx)
    fx = "reach_hex_decode_std_unimported"
    write(os.path.join(XF, fx + ".kama"),
          "// KR-46: a `Result` type argument naming `DecodeError` without importing it. Must be rejected with the import that fixes it.\n"
          "import { std::encoding::hex::decode, std::collections::DynamicArray };\n"
          "fn int32 main() { Result<DynamicArray<uint8>, DecodeError> r = decode(text: \"00\"); return 0; }\n")
    write(os.path.join(XF, fx + ".msg"), "add `import { std::encoding::hex::DecodeError };`")
    out.append(fx)
    return out

# Positive controls: every position in ONE program, imported (d) or qualified-and-unimported (bq).
import re
RENAME = ["main", "W", "f", "g", "z", "G", "Box", "Cb", "Holds", "gs", "kama_probe_nope"]
def gen_positive(qualified):
    parts, calls, imps = [], [], {"geo::Shown"}
    for stem, kind, imports, what, tmpl in POS:
        if stem == "cast_direct": continue                      # no legal spelling exists
        if stem == "extern_sig" and not qualified: continue     # a bare name there is a C spelling
        name = ("geo::" if qualified else "") + SHOWN_T[kind]
        if not qualified: imps.add("geo::" + SHOWN_T[kind])
        imps.update(i for i in imports if not i.startswith("geo::"))
        if stem == "existing_instance" and not qualified: imps.add("geo::ShownErr")
        body = tmpl.replace("@V@", variant(kind, name)).replace("@X@", name)
        for r in RENAME:
            body = re.sub(r"(?<![\w:])" + r + r"(?=\s*[<(;{ .:])", f"{r}_{stem}", body)
        parts.append(f"// {what}\n{body}")
        calls.append(f"main_{stem}()")
    kinds = ["T", "R", "F", "K", "C"]
    lib = "export { Shown, " + ", ".join(SHOWN_T[k] for k in kinds) + " };\n\n" + \
          "\n\n".join(DECLS[k][0] + "\n\n" + DECLS[k][1] for k in kinds) + "\n\n" + SHOWN + "\n"
    how = "qualified and never imported (`geo::ShownErr`)" if qualified else "imported"
    main = (f"// KR-46 positive control: every position the reach fixtures reject, spelled LEGALLY — {how}.\n"
            "// The fix must not over-refuse any of these; each position's function must return 0.\n"
            f"import {{ {', '.join(sorted(imps))} }};\n\n" + "\n\n".join(parts) +
            "\n\n// Each position fails with its own exit code, so a red run names the position.\nfn int32 main() {\n"
            + "".join(f"    if ({c} != 0) {{ return {i + 1}; }}\n" for i, c in enumerate(calls)) + "    return 0;\n}\n")
    d = os.path.join(ROOT, "tests", "reach_controls_" + ("qualified" if qualified else "imported") + ".d")
    write(os.path.join(d, "main.kama"), main)
    write(os.path.join(d, "geo", "lib.kama"), lib)
    write(os.path.join(d, "expect"), "0\n")
    return os.path.basename(d)

written = []
if CONTROLS:
    for stem, kind, imports, what, tmpl in POS:
        if stem == "cast_direct": continue   # no legal spelling: an enum is not castable at all
        d = os.path.join(CONTROLS, stem)
        name = SHOWN_T[kind]
        body = tmpl.replace("@V@", variant(kind, name)).replace("@X@", name)
        if stem == "extern_sig": body = body.replace("kama_probe_nope(ShownVal x)", "kama_probe_nope(int32 x)")
        imps = list(dict.fromkeys(imports + ["geo::" + name]))
        shown, hid = DECLS[kind]
        exports = [name] + (["Shown"] if stem == "existing_instance" else [])
        extra = "\n\n" + SHOWN if stem == "existing_instance" else ""
        write(os.path.join(d, "geo", "lib.kama"), f"export {{ {', '.join(exports)} }};\n\n{shown}\n\n{hid}{extra}\n")
        write(os.path.join(d, "main.kama"), f"import {{ {', '.join(imps)} }};\n" + body + "\n")
    sys.exit(0)
for stem, kind, imports, what, tmpl in POS:
    for case, (suffix, desc) in CASES.items():
        if stem in ONLY and case not in ONLY[stem]: continue
        if stem == "existing_instance" and case == "a": imports_here = ["geo::Shown"]
        else: imports_here = list(imports)
        name = {"a": ZORK, "b": SHOWN_T, "c": HID_T, "cq": {k: "geo::" + v for k, v in HID_T.items()}}[case][kind]
        body = tmpl.replace("@V@", variant(kind, name)).replace("@X@", name)
        correct = "rejected, naming it" if case != "b" else "rejected with the import that fixes it"
        head = f"// KR-46: {what}, naming {desc}. Must be {correct}.\n"
        imp = f"import {{ {', '.join(imports_here)} }};\n" if imports_here else ""
        fx = f"reach_{stem}_{suffix}"
        needs_geo = case != "a" or stem == "existing_instance"
        if not needs_geo:
            write(os.path.join(XF, fx + ".kama"), head + imp + body + "\n")
            write(os.path.join(XF, fx + ".msg"), msg_for(kind, case, stem))
        else:
            d = os.path.join(XF, fx + ".d")
            shown, hid = DECLS[kind]
            exports = [SHOWN_T[kind]]
            extra = ""
            if stem == "existing_instance":
                exports.insert(0, "Shown"); extra = "\n\n" + SHOWN
            lib = f"export {{ {', '.join(exports)} }};\n\n{shown}\n\n{hid}{extra}\n"
            write(os.path.join(d, "geo", "lib.kama"), lib)
            write(os.path.join(d, "main.kama"), head + imp + body + "\n")
            write(os.path.join(d, "msg"), msg_for(kind, case, stem))
        written.append(fx)
written += gen_std()
written += [gen_positive(False), gen_positive(True)]
print("\n".join(written))
```

### `classify.sh`

The inner loop for the reach fixtures (`./dev fixture` cannot run a `.d`): builds each one the way the xfail
leg does and prints `ACCEPTED` (compiled), `C` (failed in the C compiler), `MSG` (refused without its `.msg`)
or `green`. Run from the repo root after `genfix.py`; `sort | uniq -c` on column 1 gives the counts.

```bash
#!/bin/bash
# classify.sh — one line per tests/xfail/reach_* fixture: <verdict> <name>
ROOT=$(pwd); . tools/kama-bin.sh
w=$(mktemp -d)
for f in tests/xfail/reach_*; do
  case $f in *.msg) continue;; esac
  n=${f##*/}; n=${n%.kama}; n=${n%.d}
  if [ -d "$f" ]; then m=$f/msg; files=$(find "$f" -name '*.kama' | sort); else m=tests/xfail/$n.msg; files=$f; fi
  "$KAMA" build $files -o "$w/$n" >/dev/null 2>"$w/$n.err"; rc=$?
  if [ $rc -eq 0 ]; then v=ACCEPTED
  elif grep -q "clang\|error generated" "$w/$n.err"; then v=C
  elif ! grep -qF "$(cat "$m")" "$w/$n.err"; then v=MSG
  else v=green; fi
  echo "$v $n"
done
rm -rf "$w"
```
