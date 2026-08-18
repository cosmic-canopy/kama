# Milestone: a runtime narrowing cast traps — `try cast<T>` and `truncate<T>` are the escapes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** ROADMAP **row 1**. Every number below was re-measured at **`0.9.29`** on 2026-08-18,
*after* the isize campaign landed. ⚠️ The version of this file that preceded it was wrong about **both** of
its headline numbers, so re-run the census rather than trusting this page:
`python3` over the corpus with a paren-matched `cast<...>(...)` extractor — the shipped one lived at
`/tmp/casts.py`; rewrite it, it is 30 lines.

## What is true today

```kama
fn int32 main() { int32 big = 300; int8 a = cast<int8>(big); return cast<int32>(a); }
```
```sh
kama build … && ./a          # -> exit 44.  Silently truncated, in --debug AND --release.
```

| probe | result |
|---|---|
| a runtime narrowing `cast` | **truncates silently** — the defect, live |
| a CONSTANT narrowing `cast` | already rejected (`rejectConstCastOverflow`, `tests/xfail/cast_const_oob*`) |
| `cast<int32>(1.0e30)` — float→int out of range | **already TRAPS, in BOTH builds** (`-fsanitize-trap=float-cast-overflow`, `kama.driver.cpp:7244`) |
| `int32 a = INT32_MAX; a + 1` | traps in both builds |
| `try cast<int8>(x)` | Parse error — `try` is grammatically pinned to `new` (`kama.y`, the `TRY NEW` production) |
| `grep -c NDEBUG include/kama_runtime.h` | **0** |

**The release question is already settled by precedent, not opinion:** `float-cast-overflow` IS an
out-of-range cast and it already traps in `--release`. The integer→integer narrowing cast is its exact
sibling. ⚠️ The driver comment at `:7239` claims signed overflow *"WRAPS in release"* via `-fwrapv`; the
binary **traps** in both. Comment and compiler disagree — confirm during implementation.

## The decision (taken with the user; do not re-open)

| | |
|---|---|
| a runtime narrowing `cast` | **traps** |
| in `--release`? | **yes, every build** |
| fallible form | **`try cast<T>(x)` → `Optional<T>`** |
| truncating form | **`truncate<T>(x)`** — a NEW keyword |
| `bitcast` | unchanged: same-width only |

What each verb **preserves** — the rule to document:

| verb | preserves | width rule | can fail? |
|---|---|---|---|
| `cast<T>(x)` | the **value** | any | **yes — traps** |
| `truncate<T>(x)` | the **low bits** | target narrower or equal | no |
| `bitcast<T>(x)` | **all the bits** | **same width** | no |

**`truncate` is required, not optional** — the predecessor brief claimed "truncation needs no new spelling,
mask first", and that is FALSE for a signed target: `cast<int8>(x & 0xFF)` yields 0..255, outside `int8`,
so it would itself trap. Proven: negative `int8`/`int16` round-trip through KBIN **today** (`exit 55`)
*because* `cast<int8>(cast<uint8>(200))` silently truncates to −56 — working code that the trap breaks.
Every language that traps ships a named truncating form (Swift `truncatingIfNeeded:`, Zig `@truncate`,
C# `unchecked`); none routes users through the unsigned peer. Zig deliberately keeps `@truncate` and
`@bitCast` separate, which is the precedent for not overloading `bitcast`.

## The measurement (re-derived at 0.9.29, post-isize)

```
1,077 cast sites in tests lib prelude examples bench
  208  non-numeric target (UnsafePtr 198 + generic T/Pair/CompareFn/Shape) — MUST NOT be touched
  130  constant operand — already compile-time rejected, no runtime check
  ---
  869  numeric target;  ~739 with runtime exposure
```

Operand shape of the 869: 322 bare local/param · 130 literal · 126 arithmetic · 101 `.length()`/`.count()`
· 79 other call · 75 field · 34 element.

⚠️ **`cast<UnsafePtr>` (198 sites) must not be touched** — a pointer conversion, not numeric, and the unsafe
seam's one production site. Gate on the target being a numeric C type (`cNumBits`/`cNumTargetWidth`).

## Where the work goes

**1. Runtime** — `include/kama_runtime.h`: `kama_narrow_fail(value, lo, hi)` + a `KAMA_NARROW_*` macro pair
beside `kama_bounds_fail`. Copy its shape exactly: `KAMA_NORETURN`, routes through the overridable weak
`kama_panic_handler`, carries the offending values into the message. Keep the file's 0 `NDEBUG` guards.

**2. The check** — `emitExpression`'s `CastNode` arm in `src/kama.cemit.cpp`, as the `else` after
`rejectConstCastOverflow` (which returns first for constants).

- Gate on a numeric target via `cNumBits`/`cNumTargetWidth`.
- Elide provably-lossless conversions with `typeOfExpr` — the same helper milestone 6 uses. A widening or
  same-type cast emits a bare C cast and no check. The two big buckets each need only a **one-sided**
  comparison.
- ⚠️ **`typeOfExpr` returns `""` for an unknown source — emit the FULL check, never a guess.** This is the
  `4232be0` lesson. See ROADMAP row 2: `""` is common and is its own open gap.
- Reuse `primIntRange`/`primIntRangeC`/`constOutOfRange`; do not restate the range table.

**3. `truncate<T>(x)`** — new keyword in `src/kama.l` beside `{"bitcast", BITCAST}`, a grammar production,
an emitter arm. Simplest shape reuses `CastNode` with a flag. Rejects a **widening** target (Zig's rule).

**4. `try cast<T>(x)`** — new production in `src/kama.y` beside `TRY NEW`; reuse `try new`'s existing
`Optional<T>` static-result computation. ⚠️ **Edits a grammar RULE → run `tools/gen-grammar` and commit
`docs/grammar.bnf`**, or `check-grammar` fails the matrix.

**5. Corpus migration** — land the trap, then let `./dev matrix` find the sites. Do not pre-migrate. Two
are already identified:

| file | sites | fix |
|---|---|---|
| `lib/std/serialization/binary/binary.kama` `readI8`/`readI16`/`readI32` | 3 | `truncate<int8>`/`truncate<int16>` — currently `cast<int8>(cast<uint8>(…))`, working code that needs the wrap |
| `lib/std/serialization/json/json.kama` `readI8`/`readI16` | 2 | `try cast` — a JSON `300` into an `int8` field must error, not abort a deserializer |

`lib/std/fmt/parse.kama` (4) and `backing.kama`'s `clampWide` (2) are provably in range; `backing.kama`'s
`fromWide` (2) *should* trap — `clampWide` is the offered alternative.

## Verification

`./dev matrix > /tmp/matrix.log 2>&1; tail -5 /tmp/matrix.log` — **run once, read the file.**

⚠️ **A green matrix does not prove a runtime rule right** — milestone 6 shipped two bugs past one. Walk a
**probe ledger** and run each branch:

- **traps:** in range / out high / out low / negative into unsigned — crossed with source form (local,
  field, call result, element, `comptime`) and with `cast` vs `try cast` vs `truncate`.
- **must NOT trap:** `cast<UnsafePtr>`, widening, same-type, float→float, every `truncate`, and every cast
  in a generic body where the operand type is a parameter.
- **release parity:** every trap fixture also built `--release`.
- **perf:** run `bench/` before and after — the ROADMAP invariant is that native/release stays exactly as
  fast. A one-sided predictable branch is usually free; measure it.

⚠️ `tests/trap/` is **skipped under `KAMA_SAN`, `KAMA_WASM` and on Windows**, so trap fixtures guard the
native leg only. The probe ledger, not a green suite, is the acceptance test.

## Record when it ships

SPEC's *No undefined behavior in arithmetic* gains the integer half of its existing float sentence; add
`truncate` to the conversion-verb table beside `cast`/`bitcast` (SPEC and **KEYWORDS.md**, whose keyword row
now lists `bitcast`). `docs/agents.md` + `agents/AGENTS.md` describe `cast` and want the trap named —
⚠️ `agents/AGENTS.md` is **EMBEDDED**, so editing it needs `./dev build` in the same commit. Bump `VERSION`.
Delete this file and the ROADMAP row from **both** ROADMAP.md and ROADMAP_DETAIL.md.
