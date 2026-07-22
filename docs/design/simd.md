# SIMD backend for `std::math` — campaign kickoff

**Status:** **M0 (measure) done — regroup pending a decision.** See **[§ M0 findings](#-m0-findings-2026-07-22)**
at the bottom: the headline is that clang **already auto-vectorizes** the math when it inlines, and the thing
actually blocking SIMD in shipped binaries is **the multi-TU build model (no cross-module inlining)**, not
the emitter. Read the findings before picking a design option — they change the recommendation.

This is the **last Tier-0 engine-readiness item** ([ENGINE_READINESS.md](../ENGINE_READINESS.md));
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

---

## ► M0 findings (2026-07-22)

M0 landed a permanent `math` bench workload (`bench/src/kama/math.kama` + sibling ports in all 10 comparison
languages — a 2×10⁶-iteration hot loop over `Vec4` add/sub/scale, `dot`, `Mat4*Vec4`, `Mat4*Mat4`, and the
`Quat` Hamilton product; all inputs are small integer-valued float32 so every intermediate is exactly
representable and the exit-code checksum is bit-identical across the float32 and float64 backends — the
fairness gate is green across all 11 languages). Then it did the Step-0 measurement: `objdump` of the `-O3`
build (aarch64 container) + a wall-clock matrix. **The result reframes the campaign.**

### Headline: the layout is already SIMD-friendly; the *build model* is what blocks SIMD

When the math ops **inline into a hot loop**, clang at `-O3` **already auto-vectorizes them with zero emitter
changes** — the kama-generated C, compiled as one TU, has an all-packed hot loop (63 packed NEON FP ops, 0
scalar) and runs **at parity with plain `-O3` C**. But **`./kama build` never gets there**, because it
compiles each module as a **separate translation unit with no LTO**. So the `std::math` ops can't inline into
the user's loop — they stay **out-of-line scalar function calls**. Proof, same generated C, aarch64, 2×10⁶
iters, 10-run avg:

| build of the *same* kama-generated C | `kama_main` hot loop | time |
|---|---|---|
| **multi-TU, no LTO (what `./kama build` ships)** | **0 packed · 19 scalar · 16 `bl` calls** | **25 ms/run** |
| multi-TU **+ `-flto`** (cross-module inline) | fully inlined, 0 calls | 8 ms/run |
| single-TU (all inlined) | 63 packed · 0 scalar | 8 ms/run |
| hand-written C, `-O3` | (SLP/loop-vectorized) | 4 ms/run |

So the shipped native math is ~6× C. **The single cause is missing cross-module inlining** (~3×, 25→8 ms):
the ops never inline, so they never vectorize and pay call overhead. **This is the dominant, highest-leverage
issue and it is NOT a SIMD-codegen problem.** Fix inlining and native math lands **at parity with C** — proven
by isolating the one remaining cost, which turns out to be a *benchmark artifact*, not a real cost (inlined,
2×10⁶ iters, 10-run avg):

| inlined variant | time |
|---|---|
| per-iteration `float→int` cast + full safety traps (**the bench**) | 8 ms |
| **pure-FP loop (1 cast total) + full safety traps** | **3 ms** |
| pure-FP loop, no traps | 3 ms |
| hand C `-O3` (per-iter cast) | 5 ms |

The residual "2×" I first attributed to the safety traps (`-fsanitize=float-cast-overflow,signed-integer-overflow`,
driver ~690–697) is **almost entirely the per-iteration `float→int` cast** the checksum needs — an artifact of
the *benchmark's* exit-code mechanism, not of real math. **A pure-float loop with all safety traps on is 3 ms —
at parity with (here, faster than) C.** Real engine math (transforms, physics — all float, no per-iter int
cast) pays a **near-zero** safety-trap tax. So once inlined, kama math **is on par with C**; there is no second
tax to pay.

Corroboration sitting right next to it: **wasm kama (16 ms) is *faster* than native kama (26 ms)** in the
cross-language matrix below — because the wasm path (`transpile` → one `.c` → `emcc -O3`) **inlines
everything** and omits the native traps. The fix is demonstrated by an existing path.

### Per-op `-O3` codegen (out-of-line library symbols, aarch64)

What each op compiles to when *not* inlined (i.e. what the shipped multi-TU build actually calls):

| op | out-of-line codegen | note |
|---|---|---|
| `Vec4 +`, `-`, `dot` | **scalar** (4×) | AArch64 **HFA ABI** passes a `Vec4` as 4 separate `s` registers, so the standalone op can't cheaply pack. A `vector_size(16)` type would pass it in one `q` register → one packed instruction. |
| `Vec4 * scalar` | **packed** (1×) | splat + one `fmul.4s`. |
| `Vec3 +`, `dot` | scalar (3×) | 3-wide; also HFA. |
| `Mat4.transform` (M·v) | **packed** (4× `fmla.4s`) | passed by memory → contiguous loads pack naturally. |
| `Mat4 * Mat4` | **packed** (16×) | 4 transforms. |
| `Quat * Quat` (Hamilton) | **scalar** (16×) | the shuffled ± sign pattern defeats SLP even when inlined — a genuine gap where hand-written SIMD (lane shuffles) would help. |
| `Mat4.inverse`/`determinant`/`lookAt`, `Quat.slerp`/`toMat*`/`fromEuler`/`nlerp` | scalar | complex ops; auto-vec leaves them scalar. |

Note the two ops that already vectorize out-of-line (`Mat4.transform`, `Mat4*Mat4`) are the ones passed **by
memory**; the ones that stay scalar (`Vec4`/`Vec3`/`Quat`) are the small HFAs passed **in registers**.

### Cross-language wall-clock matrix (aarch64 container, median, `math` = 2×10⁶ iters)

| lang | native time | vs C | notes |
|---|---|---|---|
| C | 4.99 ms | 1.0× | baseline |
| C++ | 5.24 ms | 1.05× | |
| Rust | 6.06 ms | 1.2× | |
| **kama (native, as shipped)** | **26.34 ms** | **5.3×** | multi-TU, no inline + safety traps (see above) |
| **kama→wasm** | **16.2 ms** | — | single-TU + inline, no native traps → beats native kama; **11× faster than hand JS (178 ms)** |
| Go | 77.2 ms | 15× | |
| C# (JIT) | 109.5 ms | 22× | |
| Java (JIT) | 114.4 ms | 23× | |
| Lua | 3885 ms | 780× | |
| Python | 4383 ms | 880× | |

All 11 backends produce the identical checksum (fairness gate ✓). kama already beats every managed/interpreted
language; the whole opportunity is the **5.3× gap to the C/C++/Rust cluster**, and ~3× of that is inlining.

### Re-evaluated design options

- **The cheapest, highest-leverage change is not in the original three options: fix cross-module inlining of
  the hot math ops.** Either (a) turn on `-flto` for `--release` native builds (the wasm path already proves
  the payoff), or (b) emit the small hot `std::math` ops as `static inline` in the shared generated header so
  they inline without LTO, or (c) whole-program single-TU release builds. This is a **build/emitter-packaging**
  change, ~3×, and it **unlocks the auto-vectorization that is already latent** — no SIMD codegen required.
- **Option 1 (lean on `-O3` auto-vec)** is *validated for inlined code* — once the ops inline, clang packs
  them. So after the inlining fix, Option 1 may be almost the whole story for `Vec4`/`Mat4`.
- **Option 2 (`vector_size(16)` primitive)** still has real, narrower value the auto-vectorizer won't give:
  (i) the **out-of-line / ABI-boundary** `Vec4` path (pass-in-one-register, guaranteed packing regardless of
  inlining or loop shape), and (ii) it makes the win **not depend on the optimizer noticing** a vectorizable
  loop. Its value is much smaller *after* inlining is fixed — so decide it on the re-measure.
- **Option 3 (`@simd` attribute)** — same op-lowering cost as Option 2, scoped; only worth it if we want
  opt-in rather than rewriting the `std::math` types.
- **`Quat`** is the one op that stays scalar even inlined — a real, contained candidate for explicit SIMD
  (lane shuffles) whichever option wins.

### Recommendation (for the regroup)

1. **First, fix inlining** (LTO on release, or `static inline` hot-op emission). Cheapest, ~3×, unlocks the
   latent auto-vec. Re-run the `math` bench to confirm native kama collapses toward the C cluster.
2. **Then re-measure and decide** whether a `vector_size` primitive (Option 2) is still worth it for the
   residual out-of-line/ABI gap + `Quat`, or whether Option 1 (now-inlined auto-vec) is enough.
3. The safety-trap ~2× is a separate conversation (not this campaign).

**Decision needed:** which inlining mechanism (LTO vs `static inline` headers vs single-TU), and whether to
pursue the `vector_size` primitive now or gate it on the post-inlining re-measure.
