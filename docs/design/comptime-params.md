# Compile-time arguments — `#(…)`

*Design of record for [ROADMAP.md](../ROADMAP.md) row 1. The question this file used to ask — where a
function's compile-time values live — is **decided**; what remains is the migration. Delete this file
once the campaign lands and the surface is recorded in [SPEC.md](../SPEC.md).*

> **Status.** Change **A shipped** (`const N: int32` → `comptime N: int32`). The rest is **decided,
> 2026-08-31, not yet implemented**: compile-time *values* leave the generic list, and `<…>` becomes
> types-only. Source-breaking, so it lands before the 1.0 tag.

---

## 1. The decision

kama has two axes for compile-time work: compile-time **arguments** (the generic parameter list) and
compile-time **execution** (`comptime fn`, `comptime` constants, `comptime assert`). Today the
argument list mixes two different things — a type parameter and a value parameter — in one pair of
angle brackets. It stops doing that.

```kama
type value InlineArray<T> comptime(int32 N) { … }     // definition — the greppable keyword
fn int32 shifted(int32 x) comptime(int32 S) { … }     // definition — always the LAST list

InlineArray<int32>#(4) buf;                           // use — terse
Simd<float32>#(4) v;
int32 y = shifted(x: 2)#(3);
```

Three delimiters, three non-overlapping meanings:

| | holds | spelled at definition | spelled at use |
|---|---|---|---|
| `<…>` | compile-time **types** | `<T: Bound>` | `<int32>` |
| `comptime(…)` / `#(…)` | compile-time **values** | `comptime(int32 N)` | `#(4)` |
| `(…)` | runtime values | `(int32 x)` | `(x: 2)` |

### The four settled points

1. **Placement — `comptime(…)` is always the last list.** After `<…>` for a type (which has no
   runtime list), after the runtime `(…)` for a function.
2. **Definition vs use are spelled differently, deliberately.** The keyword `comptime` at
   definitions; the symbol `#(…)` at uses. Definitions are rare and want to be greppable — `grep -rn
   comptime` finds every compile-time parameter in a codebase. Uses are frequent and want to be
   terse. This is Rust's split (`const N: usize` at the declaration, `::<3>` at the use), and it is
   principled *because* it is declaration-vs-use: different audiences, different frequencies, and the
   compiler always knows which position it is in. ⚠️ Contrast the split this design **rejects** — one
   spelling for types and another for functions — which is arbitrary, because that is the same
   audience in the same position.
3. **Type set — Rust's.** The eight integral types, plus `bool` and `char`. Trivial mangling, no
   identity rules owed. See §5 for what is deliberately left out and why.
4. **Arguments are positional** (`#(4, 2)`), matching what `<…>` does today rather than the named
   runtime-argument convention. Defaults carry over unchanged: `comptime(int32 N = 4)`.

## 2. Why — types and values stop sharing a list

The rationale is **not** brevity at the call site. It is that one list carrying two kinds of thing
forces every reader, and every part of the compiler, to ask which kind each entry is.

`::<…>` is unaffected in its main job and gets narrower and more honest. Measured across
`prelude/`, `lib/std/`, `examples/`, `tests/` and `bench/` at `0.9.127`:

| | sites |
|---|---|
| total `::<` turbofish | 346 |
| pure **type** turbofish — untouched | 287 |
| carrying a bare integer — affected | 59 |

⚠️ Many of the 59 are *mixed*, and those **unmix** rather than disappear:

```kama
Fixed::<int16, 8>.one()        // today — a type and a value sharing one list
Fixed::<int16>#(8).one()       // after — each in its own
```

A second consequence worth stating: a `comptime(…)` group is *separate*, so it can be **omitted when
inferable**, exactly as the turbofish is omitted today. `sumN(a: v)` still infers `N` from `a`'s
type. Any design that merged compile-time values into the runtime parameter list would have had to
invent "a parameter you may omit when it is inferable" — the machinery Zig retrofitted as `anytype`.
Keeping the group separate is what avoids that.

## 3. Grammar work

- **New token `#`.** It is free mid-line today: [kama.l:102](../../src/kama.l#L102) is
  `reserved_preprocessor ^[ \t]*#.*`, **anchored to line start**. ⚠️ Narrow that rule to
  `#region|#endregion`, or a line that happens to begin with `#(` is swallowed whole.
- **Definition arms.** A `comptime_params_opt` after `RPAREN` in the `function_declaration` arms
  ([kama.y:875, 902, 925](../../src/kama.y#L875)), and after `type_decl_head`
  ([kama.y:786](../../src/kama.y#L786)). `type_param` loses its `COMPTIME` arm
  ([kama.y:983](../../src/kama.y#L983)); the parameter type there widens from `integral_type` to
  integers + `bool` + `char`.
- **The method slot.** `method_when_opt` already sits directly after `)`
  ([kama.y:1845](../../src/kama.y#L1845)). Fix an order for `(…) comptime(…) when [ … ] { }` — both
  start with reserved keywords, so either parses, but one must be chosen and written down.
- **Use-site `#(…)`** is a postfix on both a type name and a call expression, so it must compose with
  chaining (`shifted(x: 2)#(3).method()`) and nest inside a type argument list
  (`Map<string, InlineArray<int32>#(4)>`, where `genericDepth` governs `>` vs `>>` lexing).
- **Build the grammar and count bison conflicts — DONE 2026-09-02, and it is CLEAR.** A prototype
  carrying all of the above over the full grammar reports exactly the baseline's single shift/reduce
  conflict (the dangling `else`, already declared `%expect 1` at [kama.y:273](../../src/kama.y#L273)) and
  generates a real 6944-line parser. The measurement also settles **the method slot below**:
  `comptime(…)` binds before `when [ … ]`. ⚠️ Three traps make this easy to get wrong — `/usr/bin/bison`
  is 2.3 on macOS and cannot parse this grammar at all (while printing nothing matching `conflict`);
  `%expect 1` silences bison's default report, so `-Wcounterexamples` is required; and a run that wrote
  no parser lies the same way. Use the Makefile's bison and check the generated line count.

## 4. Checked — not a hazard

**Use-before-declare in a signature is already the rule and breaks nothing.**
`fn T max<T>(T a, T b)` ([tests/generic_fn.kama:4](../../tests/generic_fn.kama#L4)) uses `T` as the
return type *before* `<T>` declares it — kama's return type precedes the name, so no list placed
after the name can precede it, and that has always been true. The parser only builds nodes; name
binding happens in the emitter, which has the whole signature before it analyzes any of it. So
`fn int32 sumN(InlineArray<int32>#(N) a) comptime(int32 N)` is an emitter ordering choice, not a
language constraint.

## 5. The type ladder — what is left out, and why

Admitting a wider set is **purely additive** (every program that compiles still compiles), so none of
it gates the 1.0 tag and none of it needs deciding now.

| candidate | status | note |
|---|---|---|
| the eight integral types | **in** — ships today | |
| `bool`, `char` | **in** — this campaign | trivial mangling, Rust's set |
| `InlineArray<T, N>` of an admissible scalar | later | needs an element-list symbol encoding; the SIMD shuffle-pattern case |
| `float32` / `float64` | later | ⚠️ needs an identity rule Rust refused to write: is `Foo#(0.0)` the same type as `Foo#(-0.0)`, and is `NaN` legal? Bitwise identity is the defensible answer. The real customers are MCU/DSP — fixed-point scaling, gamma, tolerances |
| `string` literals | later | symbol length + a hashing scheme. C++ allows it; Rust does not |
| a structural `type value` | later | where C++ landed after 20 years |
| `type` itself as a value | **rejected** | that is Zig wholesale — see §7 |

⚠️ **A correction that prices this ladder honestly.** An earlier draft of this file argued
non-integral values are cheap as *function* parameters because "the value does not name a type and
needs no mangle". Not so: [tests/constgen_value_fn.kama:8](../../tests/constgen_value_fn.kama#L8)
monomorphizes per constant, so the emitted C symbol **already encodes the value today**. What a
function parameter escapes is *type-identity* mangling, not symbol mangling — the float identity rule
and the array encoding are owed either way.

## 6. Migration surface

| surface | sites |
|---|---|
| `InlineArray<…>` uses | 119 |
| `Simd<…>` / `Mask<…>` uses | 78 |
| `Fixed<…>` uses | 25 |
| value-carrying turbofish | 59 |
| comptime-param declarations | 39 (1 `prelude/`, 1 `lib/std/`, 37 `tests/`) |

**~340 sites (approximate — the categories overlap), plus every user's code.** Re-measured at `0.9.140`:
`InlineArray` 140, `Simd`/`Mask` 78, `Fixed` 25, value-carrying turbofish 64 of 355, comptime-param
declarations 34. The `InlineArray` and turbofish rows grew in thirteen days, which is the concrete form of
"the cost only rises". This is the largest
source break kama takes before 1.0, and it is payable only before the tag — which is why the row sits
at the top of the NOW list rather than in the middle of it.

⚠️ **Do not re-derive "the migration is cheap" from a count of comptime-parameterised *functions*.**
There are none in shipped kama code, and an earlier draft used that to argue the change was nearly
free. It measures the wrong thing: the cost is concentrated in the *use sites of the two
comptime-parameterised types*, `InlineArray` and `Simd`, which are load-bearing everywhere.

## 7. Rejected

- **`_comptime(…)` as a glued use-site marker.** A leading underscore *starts an identifier*
  ([kama.l:161](../../src/kama.l#L161)), so `_comptime` would have to become a second reserved word
  beside `comptime` — two keywords for one concept. It also collides with the prelude's own
  private-member convention (`_parts`, `_nparts` in
  [prelude/global.kama:632](../../prelude/global.kama#L632)), and forbidding the space would need a
  post-parse source-position check, which is a formatter's job (`kama fmt`), not the grammar's.
- **`::comptime(…)`.** Coherent — `::` already means qualification in all three of its jobs (module
  path, associated item, turbofish) — but it is the language's most overloaded operator already, and
  the spelling is longer than what it replaces.
- **Moving compile-time values into the runtime `(…)` list** (`fn f(comptime int32 S, int32 x)`).
  Kills omission-when-inferable, as above; and a type has no `(…)` list, so it would force either a
  second delimiter for types or two spellings for one concept.
- **A `[…]` list for a type's comptime values.** The bracket is *not* actually taken —
  `when_clause` is `WHEN LEFT_BRACKET …` ([kama.y:1763](../../src/kama.y#L1763)), always preceded by
  its keyword — so the conflict an earlier draft asserted does not exist. Dropped for a better
  reason: `#(…)` gives types and functions one spelling, and brackets would give them two.
- **Comptime parameters *beside* the generic list, leaving both.** Two spellings for one capability,
  and the reason Rust never added comptime parameters despite Zig demonstrating them.
- **Zig's model wholesale** — types as first-class comptime values, generic types as functions
  returning types. It would rewrite every generic in `lib/std`, all of SPEC's generics chapter,
  `when [T: …]`, contract bounds and the monomorphisation machinery. That is a different language,
  not a refactor. And it would not have solved the case that raised the question: **Zig holds that
  model and still spells shuffle as `@shuffle(E, a, b, mask)` with the pattern as a comptime
  *value*.**
- **Variadic generics** — the only thing that would let a turbofish carry a shuffle pattern
  (`shuffle::<3,2,1,0>()` cannot serve `Simd<int8,16>`, which needs sixteen indices). Rust, Zig and
  Swift all avoided them; the pattern-as-value form makes the question moot.
