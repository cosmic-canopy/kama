# Milestone 7 — a runtime narrowing cast traps, with `try cast<T>` as the fallible form

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** ROADMAP **row 1**, and the last source-VISIBLE item before the 1.0 tag. Everything below
was probed against the built compiler at **`0.9.28`** on 2026-08-17, with the command recorded. Re-run
them rather than trusting this page — every brief in this repo has been wrong somewhere load-bearing, and
the one this file replaces was wrong about its own headline number by 25x.

## What is true today

```kama
fn int32 main() { int32 big = 300; int8 a = cast<int8>(big); return cast<int32>(a); }
```
```sh
kama check … && ./a          # -> exit 44.  Silently truncated, in check AND build.
```

| probe | result |
|---|---|
| a runtime narrowing `cast` | **truncates silently** — the defect, live |
| a CONSTANT narrowing `cast` | already rejected at compile time (`rejectConstCastOverflow`, `tests/xfail/cast_const_oob*`) |
| `try cast<int8>(x)` | **Parse error: unexpected CAST, expecting NEW** — `try` is grammatically pinned to `new` (`kama.y:1519-1520`) |
| `grep -c NDEBUG include/kama_runtime.h` | **0** — a runtime check placed here survives `--release`, like the bounds check |

## The decision (taken with the user 2026-08-15; do not re-open)

**A runtime narrowing cast TRAPS**, and **`try cast<T>(x) -> Optional<T>`** is the fallible form.
Truncation needs no new spelling — mask first (`cast<uint8>(x & 0xFF)` is provably in range and never
trips). The safe cohort (Swift, Zig, Ada) traps by default and offers *named* alternatives; nobody makes
the fallible form the only form, because most narrowing casts are ones the author knows are fine.

`try` is not a new concept — [KEYWORDS.md](../KEYWORDS.md) already defines it as the non-panicking form of
an operation, yielding `Optional`. Extending it to `cast` keeps the rule one sentence.

## ⚠️ Read this before scoping: milestone 6 changed the risk profile completely

Milestone 6 made `cast<T>` **the only way to convert between numeric types**. Every implicit conversion in
the language is now a written `cast`. So this milestone does not add a check to a handful of sites — it
adds a runtime abort to **every conversion in the corpus**:

```
1,015 casts in tests lib prelude examples bench
  361 cast<int32>     100 cast<uint8>      40 cast<int64>     14 cast<float64>
  161 cast<usize>      70 cast<uint64>     29 cast<int>       12 cast<int8>
  122 cast<UnsafePtr>  41 cast<uint32>     27 cast<uint16>     7 cast<int16>
```

**This is the opposite of milestone 6's shape.** That rule was compile-time, and its migration measured
zero. This one is a *runtime* behaviour change whose cost cannot be measured statically at all: a cast
traps or not depending on the value that flows through it, so the only instrument that answers is the
suite itself, on all three legs.

Two consequences for how to sequence the work:

1. **Land the trap and run `./dev matrix` before writing `try cast`.** The failures ARE the measurement.
   Anything that aborts is either a real latent truncation bug (a finding, and the best possible outcome)
   or a place the corpus needs a mask or a `try`. Do not guess which in advance.
2. **`cast<UnsafePtr>` (122 sites) must not be touched.** It is a pointer conversion, not numeric — and
   it is the unsafe seam's one production site. Gate the new check on the target being a numeric C type,
   the same `cNumBits`/`cNumTargetWidth` test milestone 6 uses.

## The pieces that already exist

- **The range predicate.** `primIntRange` (from a declared type node) and `primIntRangeC` (from a lowered
  C type) are the same table, and `constOutOfRange` is the "provably outside" judgement. The constant half
  already calls them from `emitExpression`'s `CastNode` arm and from `constValue`'s.
- **The trap shape to copy.** `kama_bounds_fail` (`include/kama_runtime.h:352`) is `KAMA_NORETURN`, routes
  through the overridable weak `kama_panic_handler` (`:348-351`), and carries the offending values into
  the message. A `kama_narrow_fail(value, lo, hi)` sibling is the same shape.
- **`try`'s `Optional` path.** `try new` already computes an `Optional<T>` static result type; reuse it
  rather than inventing a second one. Note `new` now requires a named ctor (`try new P.make(…)`).
- **`tests/trap/`** — 24 fixtures, each a `.kama` + a `.msg`. ⚠️ **That whole directory is SKIPPED under
  `KAMA_SAN`, `KAMA_WASM` and on Windows** (UBSan intercepts the trap; node's abort codes differ). So a
  trap fixture is guarded on the native leg only — which is ROADMAP row 18's subject and is exactly why a
  probe ledger, not a green suite, is this milestone's acceptance test.

## Where the work goes

| | |
|---|---|
| the check | `emitExpression`'s `CastNode` arm (`kama.cemit.cpp:2940`), beside `rejectConstCastOverflow` at `:2979` — the constant case returns before it, so the runtime check is the `else` |
| the runtime | a `kama_narrow_fail` + a `KAMA_NARROW_*` macro pair in `include/kama_runtime.h`, next to the bounds check |
| `try cast` | `src/kama.y` — a new production beside `TRY NEW` at `:1519`. ⚠️ **This edits a RULE, so `tools/gen-grammar` must run and `docs/grammar.bnf` must be committed**, or `check-grammar` fails the matrix |
| the record | SPEC's *No undefined behavior in arithmetic* already promises "Out-of-range `float → int` **traps**"; the integer half becomes the same sentence. `docs/agents.md` and `agents/AGENTS.md` describe `cast` and want the trap named — ⚠️ `agents/AGENTS.md` is EMBEDDED, so editing it needs `./dev build` in the same commit |

## Open questions to settle with the user before writing code

1. **Does the trap fire in `--release`?** The bounds check does (`grep -c NDEBUG` is 0), and SPEC's
   arithmetic section promises traps in every build for divide-by-zero and shift-overflow. Consistency
   says yes. But this one sits on 1,015 sites rather than on array indexing, and the performance invariant
   at the top of ROADMAP is that the native/release tier stays exactly as fast. **Measure before deciding**
   — `bench/` is the instrument, and a predictable-branch range check is usually free, but "usually" is
   not a measurement.
2. **Does `cast` between two FLOAT widths trap?** `cast<float32>(someFloat64)` loses precision but is not
   out of range in the same sense. SPEC currently says nothing. Suggest: no — precision loss is not the
   same defect as a value becoming a different number.
3. **Does the sub-`int` arithmetic wrap become a trap too?** Milestone 6 recorded honestly in SPEC that
   `int8 s = 100i8; s + s` is `-56` silently, where `int32 + int32` traps. That asymmetry is now a stated
   rule, and this milestone's `kama_narrow_fail` is the machinery that could close it. It is a SEPARATE
   decision with its own migration — do not fold it in silently.

## The trap this campaign's predecessor teaches

**A green `./dev matrix` does not prove a runtime rule right.** Milestone 6 shipped two bugs that survived
a green matrix, both found by walking each branch of the rule and asking "is the thing I wrote down as a
trap actually fixed?". For a trap rule the ledger axes are: value in range / out high / out low / negative
into unsigned, crossed with source form (a literal — already rejected at compile time, a local, a field, a
call result, an element, a `comptime`) and with `cast` vs `try cast`, plus the branches that must NOT trap
(`cast<UnsafePtr>`, a widening cast, a same-type cast, a float→float cast, and every `cast` in a generic
body where the operand type is a parameter).
