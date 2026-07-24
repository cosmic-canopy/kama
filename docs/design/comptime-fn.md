# const-eval 6b-3 — compile-time functions (`comptime fn`) — design of record

**Status: PREPARED, not built.** Kickoff brief for a fresh session. Prereqs shipped: 6b-1 (const-param
arithmetic), **6b-2 (named `comptime` constants — local/module/type, the `comptime` keyword)**. This
milestone adds the *evaluation-time* axis for **functions**: run a bounded, pure function at compile time
to bake a `static const` value or table (CRC / gamma / trig LUTs, permutation constants, precomputed
masks). Roadmap of record: ROADMAP.md §5.

## Goal & motivating example

```kama
comptime fn InlineArray<uint8, 256> crcTable() {
    InlineArray<uint8, 256> t = [0; 256];
    for (int32 i = 0; i < 256; i = i + 1) {
        uint8 c = cast<uint8>(i);
        for (int32 k = 0; k < 8; k = k + 1)
            c = ((c & 1) != 0) ? cast<uint8>((c >> 1) ^ 0x8C) : cast<uint8>(c >> 1);
        t[i] = c;
    }
    return t;
}

comptime InlineArray<uint8, 256> CRC = crcTable();   // baked: static const uint8_t CRC[256] = { … };
```

The table is computed by the compiler and emitted as a C `static const` aggregate — zero runtime cost,
sits in `.rodata` / flash. This is the payoff 6b-2 set up (`comptime` values), now fed by *computation*.

## Keyword — settled

**`comptime fn`.** Extends the 6b-2 `comptime` axis (`const`=runtime-immutable, `static`=runtime-associated,
`comptime`=compile-time). `const fn` is already taken (= const-*method*). A comptime fn is **necessarily
`static`** — it has no runtime `this` to read at compile time — so no extra marker is needed; a top-level
`comptime fn` is the natural form, and a `comptime fn` inside a type is an associated compile-time function.

## The core new subsystem — a bounded AST interpreter

There is **no compile-time evaluator today beyond `constValue`** (kama.cemit.cpp ~4002, an int64 folder over
literals + arithmetic). 6b-3 is a genuinely new component: a small tree-walking **interpreter** that is a
*sibling* of the emitter (it produces *values*, not C text). Minimum viable subset:

- **Values**: integers (all widths, with correct narrow-type wrap — watch the int64-fold caveat), floats
  (`float32`/`float64`), `bool`, `char`, and **fixed-size `InlineArray<T,N>`** (the table carrier). No heap,
  no `string`, no smart pointers, no `resource` — value data only (mirrors the module-static type gate).
- **Statements**: local `let`/`const`/`comptime` decls, assignment, `if`/`else`, `for`/`while`, `foreach`
  over a fixed array, `return`. Fixed-size **array element writes** (`t[i] = …`) — the table-fill primitive.
- **Expressions**: arithmetic / bitwise / comparison / logical / ternary / cast, array index reads,
  `sizeof`/`alignof`, references to other `comptime` constants and **calls to other `comptime fn`s**.
- **Out of scope v1**: pointers, FFI, I/O, allocation, dynamic collections, recursion depth beyond the
  budget, generics over the comptime fn itself (start monomorphic / concrete).

Represent a comptime value as a small tagged union (`int64` + width tag, `double`, `bool`, or a
`std::vector<Value>` for a fixed array). Evaluate the fn body against an environment of locals; on `return`,
hand the Value back to the caller (a `comptime` constant initializer or another comptime fn).

## Two safety rails (the real work — not the interpreter mechanics)

1. **Purity check.** A comptime fn must be deterministic and effect-free: no I/O, no reads of mutable
   module `static`s (reading another `comptime` constant is fine), no `new`/`spawn`/FFI, no calls to
   non-comptime fns. Enforce structurally (reject disallowed nodes during eval or a pre-pass), so the same
   inputs always bake the same output → **reproducible builds**. (Note the harness parallel: `Date.now()`
   /`Math.random()` are banned in workflow scripts for the same reason.)
2. **Step / branch budget.** A bounded step counter (and/or recursion-depth cap) so a runaway loop can't
   hang the compiler. Exceeding it is a clean `unsupported`-style diagnostic naming the fn (cf. C++
   constexpr-step limit, Zig `@setEvalBranchQuota`). Pick a generous default (e.g. 1e6 steps) + a way to
   raise it later if a real LUT needs more.

## Dual-use decision (settle first)

**Recommended: dual-use, like C++ `constexpr`.** A `comptime fn` is comptime-*evaluated* when its args are
compile-time-known (or when it's called in a `comptime`/const-generic context), and otherwise emitted as an
ordinary runtime C function. This is more useful (one definition, both worlds) and matches a well-trodden
model. Alternative (simpler, more restrictive): *always* compile-time-only — a call outside a comptime
context is an error. Lean dual-use unless the interpreter's runtime-fallback emission proves costly; decide
in the first design pass.

## Seam map (grounded)

- **Lexer** kama.l: `comptime` is **already reserved** (6b-2). No lexer change.
- **Grammar** kama.y: `function_modifier_opt` (line ~598) is **currently empty** — the clean attach point.
  Add a `COMPTIME`-carrying modifier (or a dedicated `comptime_opt` before `FN`) on the free-function rule
  (line 606) and, if type-scoped comptime fns are in v1, on the method rule (~1309). Set a flag on
  `FunctionDeclarationNode` / `ClassMethodDeclarationNode`. Every `_opt` must set `$$` (bison trap).
  Regenerate grammar.bnf; `comptime` already highlights (tmLanguage), so syntax-drift stays green.
- **AST** kama.ast.h: add `bool isComptime` to `FunctionDeclarationNode` (line ~ the `isRef`/`attributes`
  block) and, if in v1, `ClassMethodDeclarationNode`.
- **Evaluator** (new): `kama.comptime.{h,cpp}` (or a section of kama.cemit) — the interpreter above. It
  needs read access to `_comptimeFns` (a registry of comptime-fn AST by name), `_moduleConsts`/`_typeConsts`
  (6b-2 registries) for constant reads, and the class table for array shapes.
- **Wiring into 6b-2**: extend `constValue`/`constArgN` (kama.cemit.cpp ~4002/4086) and the `comptime`
  constant emit paths so a `comptime` constant initialized by a **call** to a comptime fn evaluates the fn
  and bakes the result. For a scalar result this is the existing "bake a literal" path; for an **array**
  result, emit a C initializer list (`static const T name[N] = { v0, v1, … };`) — new emit, precedent in
  `emitArrayLiteral` (~6233) / `KAMA_FIXED_TYPE` (~13831) for the C array shape.
- **Registration/order**: gather comptime-fn declarations in `collectSignatures`/`collectClasses` (same
  early phase as 6b-2 constants), so a `comptime` constant's initializer can call one before the collection
  pre-pass. Declaration-order + no forward/cyclic (same rule as 6b-2).

## Staged plan (checkpoint commits, like 6b-2)

- **Stage 1** — parse + register `comptime fn` (grammar/AST/`_comptimeFns`), reject a non-pure body early.
  A comptime fn that's never comptime-called still emits as a normal runtime fn (dual-use groundwork).
- **Stage 2** — the interpreter for **scalar** returns (int/float/bool) + the step budget; wire into a
  `comptime` constant initialized by a comptime-fn call (`comptime int32 X = f();` bakes a literal). Fixture.
- **Stage 3** — **array** returns + element writes + loops → bake `static const T tbl[N] = {…}`; the CRC/LUT
  flagship fixture. Transpile-grep the baked table.
- **Stage 4** — calls between comptime fns; purity-check hardening; budget-exceeded + impurity diagnostics
  (negative fixtures). Docs (SPEC "Compile-time constants" → add a `comptime fn` subsection; KEYWORDS note;
  ROADMAP §5 flip to shipped; MCU/ENGINE readiness rows).

## Open design questions for the session

1. **Dual-use vs comptime-only** (above) — lean dual-use.
2. **Value model** — tagged union shape; how narrow-int wrap is tracked (the int64-fold caveat) so a
   `uint8` table entry wraps at 256 exactly as runtime would.
3. **Budget default + override syntax** — a fixed constant first; a per-fn override (`@steps(…)`?) later.
4. **Type-scoped comptime fns in v1?** — or module-level free `comptime fn` only first (smaller). Lean
   free-only for v1.
5. **Float determinism** — pin evaluation to the same IEEE semantics the target uses (host `double` is
   fine for `float64`; be careful with `float32` rounding of intermediate results).

## Verification

Fixtures per stage (`tests/comptime_fn_*.kama` + `.expect`) whose exit code depends on a comptime-computed
value/table; the flagship bakes a real LUT and the program indexes it. Green on **native + ASan + wasm**
(`tools/cdev test`, `KAMA_SAN=1`, `KAMA_WASM=1`). Transpile-grep the emitted C to confirm the table is a
baked `static const` initializer (no runtime fill). Negative fixtures: an impure comptime fn and a
budget-exceeding loop each produce a clean diagnostic.
