# Conditional compilation via decl-level tags (`@when`) — design of record

**Status: PREPARED, not built.** Kickoff brief for a fresh session, the campaign after const-eval (6a/6b
all shipped — [[next-session-const-eval]]). This is the **structure** axis that pairs with const-eval's
**value** axis: const-eval bakes compile-time *values* (tables, sizes); this feature selects which whole
*declarations* exist for a given build. Roadmap of record: ROADMAP.md §5 (the "Platform-specific
compilation" bullet).

## The one hard constraint (user, sharpened 2026-07-24)

**DECL-LEVEL TAGGING ONLY. No in-body branching, no statement gating, no `#ifdef`/`static if`/`comptime-if`
soup.** The user rejects "slop together platform-specific logic in a tangled web of ifdefs" as ugly. The
mechanism is: **tag a whole declaration; the toolchain keeps or drops it for the active build.** Variance
lives at a declaration boundary (fn / type / enum / field / static / impl), never scattered mid-body. This
is *selective compilation of tagged decls*, not conditional *logic*.

## The core realization — ONE primitive, not two

The user described two flavors: **build-mode** (`DEBUG`/`RELEASE`) and **platform** (`WINDOWS`/`WASM`/…). They
are the *same* primitive: **keep-or-drop a declaration based on the active flag set.**

- **Build-mode** is that gate on any decl: a debug-only assert fn, an extra validation field, verbose logging
  — present in one build, absent in another.
- **Platform** is that same gate applied to *implementations behind a contract*: a platform-agnostic
  `contract` defines the seam; N concrete impls each gated for a platform; exactly one survives. That is the
  already-decided **tag-type abstraction boundary** ([[platform-abstraction-preference]]) — expressed with
  the *same* tag, not a second mechanism. This directly answers open-Q5 ("don't create two overlapping
  platform mechanisms"): there is one gate; "platform" is a *usage pattern* (contract + gated impls) on it.

So the whole campaign is **one attribute + one prune pass**, far simpler than the const-eval interpreter.

## Motivating examples

```kama
// build-mode: a debug-only helper, gone entirely in release (no symbol, no call site allowed)
@when(DEBUG) fn void traceState(int32 s) { … }

// platform via the tag-type contract seam — exactly one impl survives per build; both satisfy `Clock`
contract Clock { fn uint64 nowMillis(); }
@when(WASM)     type value WasmClock     implements Clock { fn uint64 nowMillis() { … } }
@when(NATIVE)   type value NativeClock   implements Clock { fn uint64 nowMillis() { … } }

// a debug-only extra field / a release-only fast path, etc. — same gate, any taggable decl
```

A gated-out decl is **removed before any collect/emit pass sees it** — as if it were never written. No C
`#ifdef` ever reaches the emitted output; the Kama compiler does the selection.

## Keyword / naming — the one real snag

The natural name is **`@when(FLAG)`** — it parallels `fn … when [T: Bound]` (both mean "present only
when…"), which is a genuine consistency win. **But `when` is already a keyword** (the structural bound gate,
kama.l:407 / kama.y:1255), and attributes lex as `AT IDENTIFIER` (kama.y:1071) — so `@when` currently lexes
as `AT WHEN` and won't parse. Two ways out, decide in-session:
- **Keep `@when`** and add `attribute : AT WHEN LPAREN … RPAREN` (or allow the WHEN token in attribute-name
  position). Small grammar tweak; preserves the nice `when` parallel. **Lean: this.**
- **Rename to `@cfg(…)`** (Rust-familiar, no collision) or `@build(…)`. Avoids the tweak; loses the parallel.

`@target(…)` (for platform) is NOT a keyword, so it lexes fine — but per the "one primitive" realization it
should be, at most, a *reserved-flag spelling* of `@when`, not a separate mechanism. Lean: ship `@when` only;
revisit a `@target` alias later if readability wants it.

## Flag model — reproducible builds (mirror the const-eval ethos)

const-eval's whole discipline was *deterministic → reproducible builds*. Conditional compilation must keep
that: **the active flag set comes from the explicit build invocation, NOT ambient environment.**

- **Built-in flags**, auto-derived from existing driver state (kama.driver.cpp): `NATIVE` / `WASM` /
  `EMBEDDED` from `--target` (line ~517), `DEBUG` / `RELEASE` from `--release` (line ~522). One canonical
  source, already reproducible.
- **User flags** via a new repeatable **`--define NAME`** on `kama build` (reproducible, greppable in the
  build command). Platform/OS flags the user cares about (`WINDOWS`/`MAC`/`LINUX`/`XBOX`) live here in v1 —
  note the current `--target` is a *backend* axis (native/wasm/embedded), NOT an OS axis, so OS/platform is a
  separate dimension the user selects explicitly (open-Q, below).
- **Explicitly NOT** ambient env vars (they make a build depend on shell state — the reproducibility hazard
  the const-eval campaign was careful to avoid).

Flags are **Kama-compiler-level** (they drive dropping decls during collection), *not* C preprocessor
defines — nothing `#ifdef`-shaped reaches the emitted C. (`-DKAMA_TARGET_EMBEDDED` at kama.driver.cpp:748 is
a separate, C-level thing for the freestanding entry; don't conflate.)

## Boolean logic — keep it "tagging," not "logic"

To honor "just tagging," v1 is a **flag-set membership test**, not an expression evaluator:
- `@when(FLAG)` — present iff FLAG is active.
- Allow a leading **`!`** (`@when(!RELEASE)` = debug-or-other) — hugely useful, trivial.
- Allow an **all-of** list `@when(DEBUG, LINUX)` (comma = AND) if wanted — still not an expression grammar.
- **Defer** full boolean expressions (`&&`/`||`/parens). If they ever prove necessary, open-Q4's idea —
  reuse the just-shipped **comptime bool evaluator** to fold `@when(<comptime-bool-expr>)` — is the clean
  path (a flag is a compile-time bool), but that is *more than tagging* and is explicitly out of v1 scope.

## Semantics of "drop"

Dropping is **literal**: the decl is removed, its symbol never exists. A reference from *kept* code to a
*dropped* decl is a normal unresolved-symbol error — **the user must gate both sides** (the caller too, or a
kept fallback with the same name gated for the complementary flag). This is simple and predictable; document
it. (The platform contract seam makes this natural: callers depend on the *contract*, and exactly one impl is
always present, so there is nothing dangling.)

## Seam map (grounded 2026-07-24)

- **Attribute AST** — reuse `AttributeNode` (kama.ast.h:560, `name` + `SharedArgumentList args`); no new node.
- **Grammar** — attribute list at kama.y:1067-1083 (`@name` / `@name(args)`). Decls that ALREADY accept an
  `attribute_list`: free fn (kama.y:628), module `static`/`comptime` (344/346), `type` (517), `enum` (1450),
  class field (1313). Decls that DO NOT yet: **class methods (1316), ctors/dtors, extern fn (603), comptime
  free fn (606), and `implements C for T` blocks (527)**. v1 covers the already-attributed kinds; the
  platform contract seam needs `@when` on **`type`** (already OK) — so a contract + gated *types* works
  in v1 without touching the method/impl grammar. Gating individual methods / impl blocks is a Stage-2
  grammar add. **Lexer:** if keeping `@when`, teach the attribute rule to accept the `WHEN` token
  (every `_opt` must set `$$` — bison trap).
- **The prune pass (the heart)** — a single new pass that runs at the TOP of `collectProgram`
  (kama.cemit.cpp ~13582, before `collectSignatures`/`collectClasses`) and **filters each unit's
  `codeDeclarationList`**, dropping any top-level decl whose `@when(...)` is inactive. Because it runs before
  every collect pass, nothing downstream needs per-pass guarding — the elegant single seam. (Stage 2 extends
  the filter into class member lists for gated fields/methods.)
- **Flag evaluation** — a small `flagActive(name)` over the active-flag `std::set<std::string>` built once in
  the driver/emitter setup from `--target` + `--release` + `--define`. Thread the active-flag set into the
  `CEmitter` (a new config field, like `_noHeapActive`/target plumbing at kama.driver.cpp:517).
- **Driver** — add repeatable `--define NAME` parsing (kama.driver.cpp flag loop ~526-543); derive built-in
  flags from `target`/`release` (517/522) and pass the set to the emitter.
- **`declAttrPrefix`** (kama.cemit.cpp:10383) currently errors on unknown attributes — it must learn to treat
  `@when` (and any gate attribute) as known-and-consumed (the prune pass already handled it; the emitter
  should not re-error). Keep `@when` out of the C `__attribute__((…))` output entirely.
- **Contract/impl selection** — with types gated by `@when`, the existing contract machinery
  (`_retroConformances` kama.cemit.h:489, collect/inject loop ~13623-13668, `classSatisfiesBound` ~7786)
  needs NO change: dropped impls simply never register. That is the payoff of unifying platform into the gate.

## Staged plan (checkpoint commits, like 6b-3)

- **Stage 1 — the `@when` decl gate.** Attribute-name accommodation for `@when`; the prune pass over
  top-level decls; `--define` + built-in flags (`DEBUG`/`RELEASE`/`NATIVE`/`WASM`/`EMBEDDED`); `declAttrPrefix`
  accepts `@when`. Fixtures: a `@when(DEBUG)` fn present under `--debug` / absent under `--release` (assert via
  exit code + a transpile-grep that the symbol is gone); an unknown-flag / dropped-symbol-referenced xfail.
- **Stage 2 — platform via the contract seam.** Document + fixture the pattern (a `contract` + `@when(WASM)`/
  `@when(NATIVE)` gated `type` impls, exactly one selected). Decide the platform/OS flag vocabulary + how set
  (`--define WINDOWS` vs a `--platform` axis vs host detection). Optionally extend attributes to methods /
  `implements` blocks for finer gating. `!`-negation and comma-AND if wanted.
- **Stage 3 — docs + polish.** SPEC section ("Conditional compilation"), KEYWORDS/attribute reference, ROADMAP
  §5 flip, MCU/ENGINE/WEB readiness rows. Reconcile with [[platform-abstraction-preference]] wording so the
  tag-type platform story and `@when` are described as one mechanism.

## Open questions for the session (with leans)

1. **`@when` vs `@cfg`/`@build`** — lean **keep `@when`** (parallels `fn … when`), pay the small
   attribute-lexing tweak.
2. **Flag source** — lean **built-in (from `--target`/`--release`) + explicit `--define`**, NO ambient env
   (reproducibility).
3. **Boolean depth** — lean **membership + leading `!`** (+ optional comma-AND); defer expressions; if ever
   needed, fold via the comptime bool evaluator.
4. **Platform/OS vocabulary** — how are `WINDOWS`/`MAC`/`LINUX`/`XBOX` set? `--define` (v1, explicit) vs a new
   `--platform` axis vs host auto-detection. Lean **`--define` for v1**; a dedicated axis later. Note
   `--target` (native/wasm/embedded) is orthogonal (backend, not OS).
5. **Gate granularity in v1** — top-level decls only (types/fns/statics/enums/fields) vs also methods/impl
   blocks. Lean **top-level for v1** (the platform contract seam works via gated *types* without touching the
   method/impl grammar); methods/impls in Stage 2.
6. **`@target` alias?** — keep a distinct `@target(...)` spelling for platform readability, or just use
   `@when` with platform flags? Lean **`@when` only** (one way to do a thing); revisit if readability asks.

## Verification

Per-stage fixtures under `tests/` whose exit code / presence depends on the active flags: build the SAME
`.kama` twice (`--debug` vs `--release`, or two `--define`s) and assert different behavior + a **transpile-grep**
(a `tools/check-*.sh`, precedent `tools/check-embedded.sh`/`tools/check-comptime.sh`) that a gated-out
symbol is truly absent from the emitted C — proving selection happens in the Kama compiler, not via a C
`#ifdef`. Negative fixtures: a reference to a dropped symbol (clean unresolved error), an unknown/malformed
flag. Green on native + ASan + wasm (the wasm leg doubles as the `@when(WASM)`/`@when(NATIVE)` platform test).
