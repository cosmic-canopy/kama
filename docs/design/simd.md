# SIMD backend for `std::math` — campaign kickoff

**Status:** not started. This is the **last Tier-0 engine-readiness item** ([ENGINE_READINESS.md](../ENGINE_READINESS.md));
math types + `InlineArray` already ship. Pure **performance** optimization — bit-identical results, **no API
change**. Prepared as a running start for a fresh session.

## Goal

Make `std::math`'s `Vec`/`Mat`/`Quat` use hardware SIMD as a pure implementation swap behind the unchanged,
SIMD-ready-layout API. A user keeps writing `Vec4 c = a + b`; it lowers to one SSE2/NEON/wasm128 instruction
instead of four scalar ops. Nothing about semantics, exit codes, or the surface changes.

## Current state (verified facts — don't re-explore)

- **Types:** `lib/std/math/{vec,mat,quat,scalar}.kama`. `Vec2/3/4`, `Mat2/3/4`, `Quat` — all **float32 value
  types**. `Vec4` = 4×float32 = **16 B** (a SIMD lane); `Vec3` = 12 B (**must pad to 16**); `Mat4` =
  `InlineArray<Vec4,4>` = 64 B. Operators are ordinary **scalar kama bodies** (`operator+` builds a result
  field-by-field: `Vec4.of(x: this.x + r.x, …)`); no intrinsics today.
- **Fixtures:** `tests/math_{vec,mat,quat,scalar,ctor,chain,lib}.kama` + `opindex_vec` — **exact-value** checks
  (Pythagorean triples, identity matrices). These are the CORRECTNESS gate; they pass **scalar or SIMD
  identically** and **cannot detect** whether SIMD fired (that needs asm/bench).
- **Emitter:** `cType()` (~234–313; `float32` → `"float"` at ~302); `emitStruct()` field emission at ~9352
  (`cType(f.type) << " " << f.name`). **No `vector_size` / `@simd` / SIMD infra exists** — clean slate.
- **Build opt levels** (`kama.driver.cpp` ~662–682): native release/bench = **`-O3`**, wasm = `-Oz`,
  debug/test = **`-O0`**. SIMD codegen is an `-O` concern, so the fast path is the optimized build; `-O0` stays
  scalar (and test exit codes are identical either way).
- **Verification harness:** `bench/run all` (heavy ~4.5 GB image — `bench/run build-image` once) measures
  time/RSS/size at `-O3` → `docs/benchmarks/RESULTS.md`. Plus `objdump`/asm inspection to confirm actual SIMD
  instructions. This is a DIFFERENT loop than the fixture-green one.

## ► Step 0 — MEASURE FIRST (before writing any emitter code)

At `-O3`, clang's auto-vectorizer **already** vectorizes the simple contiguous `Vec4` ops with zero changes.
So the first task is to build the math at `-O3` and `objdump` the hot ops (`Vec4` `+`/`-`/`*`, `dot`,
`Mat4*Vec4`, `Mat4*Mat4`) and see **what is already SIMD and what isn't**. This sizes the real work — likely
only `Vec3` (3-wide → no auto-vec), matrix ops, and reductions need explicit help; the easy `Vec4` paths may be
free. Don't build the feature before knowing how much the compiler already gives us (karpathy/ponytail: measure,
then write the least code that closes the gap).

## Design options (decide AFTER Step 0)

1. **Lean on `-O3` auto-vectorization + layout guarantees.** Minimal/no emitter change; guarantee via
   contiguous + aligned layout (maybe `aligned(16)`), verify by bench/asm. Cheapest if auto-vec already covers
   the hot paths.
2. **First-class SIMD primitive type.** A compiler-lowered `vector_size(16)` type (construct / lane-access /
   arithmetic / horizontal-reduce); rewrite `Vec4` (and padded `Vec3`) on top. Guaranteed SIMD, general (users
   could use it directly), but the largest change — the ops become whole-vector ops in the library.
3. **`@simd` annotation.** Mark a math type; the emitter lays it out as a vector and lowers its elementwise ops
   to vector ops. Opt-in, rides the `@`-attribute infra; the op-lowering is the tricky part.

**Portability is largely free:** a `vector_size` type lowers to scalar transparently on a target without SIMD,
so the "scalar fallback" needs no hand-written path, and there's **no runtime CPU dispatch** for the baseline
(SSE2 on x86-64, NEON on ARM64, SIMD128 on wasm are all baseline). Going *wider* (AVX2/512) would need
`-march`/runtime dispatch — a later, optional step.

## Guardrails

- **Exact-value math fixtures MUST stay bit-identical** (triple-green native/SAN/WASM) — the correctness gate.
- Every commit **ASan/UBSan-clean** (north star); math is used everywhere.
- **No API churn** — the layout was designed SIMD-ready; the surface is untouched.
- **`Vec3` pads to 16 B** — the unused 4th lane must not corrupt `dot`/`length` (mask or zero it).
- **Deferred (not this campaign):** the `f64`/`DVec` family, rotors, AVX/runtime-dispatch.

## Milestones (draft — refine after Step 0)

- **M0 Measure** — `-O3` asm baseline of the hot ops + a `bench/run` baseline number; pick the approach above.
- **M1** — storage + `Vec4` ops guaranteed-SIMD; fixtures bit-identical; record the bench delta.
- **M2** — padded `Vec3` + `Mat` ops + reductions (`dot`/`length`).
- **M3** — `Quat`; wasm SIMD128 parity (emcc `-msimd128`); asm + bench confirmation; docs (ENGINE_READINESS
  Tier-0 → ✅, SPEC note, RESULTS.md refresh).
