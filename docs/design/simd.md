# Explicit SIMD — design

*In-flight design doc. **Delete this file when the work ships**, once [SPEC.md](../SPEC.md) carries the
record — see the maintenance rule at the top of [ROADMAP.md](../ROADMAP.md).*

> **Status: stage 1 of 3 has SHIPPED** (2026-08-30) — `-msimd128` on wasm, plus the two guards, §5. So
> §1b below is now a record of what *was* true, not of what is: a `--release --target wasm` build
> vectorizes. Stages 2–3 (the `Simd<T, comptime N>` type and the derived `SIMD128` flag) are what ROADMAP
> row 1 now tracks, and everything else here still stands.

> ### ►► The measurement reframes the row. Read this first.
>
> The row was scoped on the belief that kama has no SIMD story. **It has one, and it works** — on
> native, on both architectures, and it is often *better* than hand-written explicit SIMD. What it
> does not have is (a) **any SIMD at all on wasm**, for want of one compiler flag, and (b) the
> handful of operations that have **no scalar spelling** — an arbitrary shuffle, a lane mask as a
> value. Those two are the honest content of this row, and they are much smaller than `?` suggested.
>
> Everything below was produced by compiling and reading asm, never by reading a doc. The raw log,
> including the two probe-design traps that produce a convincing false negative, is reproduced in
> §1; the commands are given so it can be re-run.

---

## 1. What was measured

Host: macOS aarch64 (Apple clang). Container: Ubuntu 24.04 aarch64 — gcc 13.3, clang 18.1, emcc 6.0.1.
Compiler `0.9.117`. Reproduce with `kama transpile p.kama -o p.c && clang -std=c11 -O3 -DNDEBUG -I include -S p.c`
(`--release` is `-O3` native / `-Oz` wasm, [kama.driver.cpp:9235](../../src/kama.driver.cpp#L9235)).

### 1a. `std::math` auto-vectorizes on native, thoroughly

`c[i] = a[i] + b[i]` over `Vec4` compiles to a 16-byte-per-iteration NEON loop with no scalar residue —
the bounds checks, the `(Vec4[]){…}` compound-literal operator convention and the `InlineArray` get/set
calls all disappear:

```
LBB0_1: ldr q0, [x9, x10] ; ldr q1, [x0, x10] ; fadd.4s v0, v0, v1 ; str q0, [x8, x10]
        add x10, x10, #16 ; cmp x10, #4, lsl #12 ; b.ne LBB0_1
```

It goes further than elementwise packing. For `dot` and for `Mat4.transform`, clang emits **`ld4.4s`** —
a four-way de-interleaving load that converts the AoS array to SoA registers and computes **four dot
products (or four matrix transforms) at once**:

| kernel over `View<Vec4>` | codegen |
|---|---|
| `c[i] = a[i] + b[i]` | `fadd.4s`, unrolled ×2 |
| `c[i] = a[i] * s` | vectorized |
| `d[i] = a[i].dot(b[i])` | `ld4.4s` + `fmul.4s` + 3×`fmla.4s` — 4 at a time |
| `acc += a[i].dot(b[i])` | same SoA body; only the final reduction is scalar |
| `c[i] = m.transform(a[i])` | `ld4.4s` + 16×`fmla.4s` — 4 at a time |

x86-64 (`--target=x86_64-unknown-linux-gnu`) vectorizes the same code at the **SSE2 baseline**, with no
`-march` at all (`addps`). `-march=x86-64-v3` widens it to 256-bit `ymm`; that upside belongs to the
CPU-tuning knob already tracked in [ROADMAP_DETAIL §9](../ROADMAP_DETAIL.md#s9), not here.

### 1b. wasm gets no SIMD at all — and one flag fixes it

```
kama build p.kama --target wasm --release   ->  0 v128 instructions in the .wasm
the same generated C, emcc -Oz -msimd128    ->  18 v128 instructions
the same generated C, emcc -O3 -msimd128    ->  18 v128 instructions
```

Neither the auto-vectorized AoS path nor an explicit vector type produces a single v128 instruction
without `-msimd128`; both produce plenty with it, **including at `-Oz`**, which is what kama's wasm
release build already uses. The driver passes no `-msimd128` and never has. This is the largest
measured gap in the row and it needs no language change whatsoever.

### 1c. Explicit vectors are the *worse* tool for array kernels

`clang -O3`, aarch64, 4096 `Vec4`s × 20000 reps, kernels `noinline`, loop-invariant motion defeated,
best of 5. "scalar AoS" is what kama emits today (proven in 1a); "explicit" is hand-written
`vector_size` C with `__builtin_shufflevector`:

| kernel | scalar AoS | explicit SIMD | |
|---|---|---|---|
| cross product (shuffle-heavy) | **0.039 s** | 0.066 s | explicit is **65% slower** |
| lane-wise clamp (compare/select) | 0.020 s | 0.020 s | tie |

The scalar cross product's asm contains `ld4` and 13 `.4s` ops: clang de-interleaves and does four
cross products at once, which beats one-cross-per-register no matter how good the shuffles are. This
is the same effect [SPEC.md](../SPEC.md) already records for `Quat`'s Hamilton product.

### 1d. What auto-vectorization genuinely cannot do

| case | AoS struct | explicit vector |
|---|---|---|
| elementwise loop | `fadd.4s` | identical — **no win** |
| **single op at an ABI boundary** | 8 scalar instrs | **2** |
| **arbitrary swizzle** (`wzyx`) | not expressible | `rev64.4s; ext.16b` |
| **two-vector blend** | not expressible | `rev64.4s; trn2.4s` |
| **a lane mask as a value** | not expressible | `fcmgt.4s` |
| lane-wise select inside a loop | auto-vectorizes | tie |
| horizontal dot | `ld4` SoA wins | loses |

The ABI row is aarch64-specific: AAPCS64's HFA rule passes `struct{float x,y,z,w}` in `s0–s3` as four
*separate* registers, so an isolated `(a+b)*s` cannot pack. The x86-64 SysV ABI passes the same struct
in `xmm` pairs, where it costs almost nothing. And it only bites at a boundary the optimizer does not
inline through, which a `--release` unity-TU build makes rare.

⚠️ **This is also the probe that produced the earlier false finding**: measured at an ABI boundary, a
perfectly vectorizable type looks scalar. Two traps to know before re-measuring anything here:

1. **Never measure at an ABI boundary** — a single `Vec4` op in its own function is scalar for reasons
   that have nothing to do with the vectorizer. Measure a loop over memory.
2. **Never let the kernel be provably dead.** File-static arrays that nothing seeds and nothing reads
   back get constant-folded and dead-stored; three of five kernels here first compiled to a bare `ret`.
   Take the memory as a parameter and make the kernel externally visible.
3. On Apple's asm syntax the vector form is `fadd.4s v0, v0, v1`, **not** `fadd v0.4s`. A grep for
   `v[0-9]+\.[0-9]+s` reports zero vector registers in a fully vectorized function.

### 1e. `ext_vector_type` is a silent miscompile under gcc

```
gcc: warning: 'ext_vector_type' attribute directive ignored [-Wattributes]
     sizeof(ext_vector_type(4)) = 4      sizeof(vector_size(16)) = 16
```

gcc **ignores the attribute and leaves a plain scalar `float`** — a warning, not an error, and kama's
build does not promote it. A kama program built with `--cc gcc` would compute one lane instead of four,
silently. Since kama accepts an arbitrary `--cc`, `ext_vector_type` is disqualified as the emitted
spelling. What both compilers agree on:

| feature | gcc `vector_size` | clang `vector_size` | clang `ext_vector_type` |
|---|---|---|---|
| type, `+ - * /`, `vec * scalar`, splat init | Y | Y | Y |
| lane index `a[2]` | Y | Y | Y |
| compare → mask `a > b` | Y | Y | Y |
| `__builtin_shufflevector` | **Y** | Y | Y |
| ternary select `a>b?a:b` | n | n | Y |
| member swizzle `a.z` | n | n | Y |

The portable intersection is `vector_size` + `__builtin_shufflevector` + lane indexing + compare-to-mask,
with select spelled as mask AND/OR. gcc supporting `__builtin_shufflevector` is the pleasant surprise:
only the *type* spelling needs a per-compiler seam, not the shuffle.

### 1f. Layout

```
sizeof/alignof:  f32x2 = 8/8    f32x3 = 16/16    f32x4 = 16/16    struct{float x,y,z,w} = 16/4
```

A 3-lane vector is **16 bytes**, so `Vec3` can never be one — SPEC and
[tests/math_layout.kama](../../tests/math_layout.kama) both promise 12. And a 4-lane vector is
16-byte *aligned* where `Vec4` is 4-byte aligned today, which that same fixture pins.

---

## 2. What other languages do

| language | portable surface | width | status |
|---|---|---|---|
| **Zig** | `@Vector(N,T)` builtin + `@shuffle`/`@reduce`/`@select` | author picks N | shipped, small, degrades to scalars |
| **Swift** | `SIMD4<Float>` … in the stdlib; the geometry types **are** the vector types | fixed | shipped |
| **C#** | `Vector128/256/512<T>` **and** width-agnostic `Vector<T>` + `IsHardwareAccelerated` | both | shipped |
| **Rust** | `core::simd::Simd<T,N>` (portable) **and** `core::arch` intrinsics (per-ISA, `unsafe`) | author picks N | portable half **still unstable in 2026**, ~5 years in |
| **Java** | Vector API, species-based | runtime-preferred | **eleventh incubation**, blocked on Valhalla value types |
| **Go** | `simd/archsimd` intrinsics; portable `simd` added 1.27 | per-ISA | `GOEXPERIMENT=simd`, no compatibility promise |
| **C/clang** | `vector_size` / `ext_vector_type` | author picks N | what kama's backend already gets |

Two things fall out. The **near-consensus is a type, not a bag of intrinsics** — every language that
offers intrinsics also offers a type, and the type is the one they call portable. And **the ambitious
versions are not done**: Rust's portable half has been unstable for five years, Java's has incubated
eleven times, Go's is an experiment. The ones that shipped (Zig, Swift) shipped something small. That
is a direct argument for scope discipline, not for a `std::simd` module with a species system.

---

## 3. The design

### D1 — Turn on wasm SIMD. This is the row's biggest win and it is a flag.

`--release --target wasm` gains `-msimd128`. Nothing else changes; the already-shipped `std::math`
starts vectorizing on the one target where it does not (§1b). Runtime support is not a concern in
2026 — wasm SIMD is in every current browser and in node ≥ 16, and kama's own harness runs node in the
container. Debug builds get it too: keeping the two tiers on the same instruction set avoids a
"vectorizes only in release" class of bug report.

### D2 — The explicit surface is a **type**, not intrinsics: `Simd<T, comptime N>`

A value type, monomorphized per `(T, N)` exactly as `InlineArray<T,N>` already is, lowering to a C
typedef. The survey says type; the C backend makes it nearly free; and a bag of intrinsic free
functions would collide with GOALS *"one way to do a thing"* the moment the type appeared beside it.

```kama
Simd<float32, 4> a = Simd::<float32, 4>.splat(s: 1.0f32);
Simd<float32, 4> b = Simd::<float32, 4>.of(v: [1.0f32, 2.0f32, 3.0f32, 4.0f32]);   // from an InlineArray
Simd<float32, 4> c = a + b;                       // elementwise, C's own operator
Simd<float32, 4> d = c.shuffle::<3, 2, 1, 0>();   // comptime lane indices
Mask<float32, 4> m = c.greaterThan(r: b);         // a lane mask IS a value (§7 — it carries T)
Simd<float32, 4> e = m.select(ifTrue: c, ifFalse: b);
float32          s = c.lane::<2>();               // compile-time lane read
float32          t = c.reduceAdd();               // the one horizontal op worth having
InlineArray<float32, 4> back = c.toArray();       // store
```

### D3 — Emit `vector_size`, through a header seam

`include/kama_simd.h`, in the shape of `KAMA_FIXED_FUNCS` in
[kama_runtime.h](../../include/kama_runtime.h): one macro per operation, one `#if` per compiler family.
`ext_vector_type` is out (§1e — gcc miscompiles it silently), the ternary select is out (gcc rejects it),
member swizzles are out (only clang has them); `__builtin_shufflevector` is in, on both.
A compiler that has neither spelling gets a scalar-struct fallback in the same header — the macro seam
is what makes that possible at all, and it is why the ops are macros rather than inline C operators.

### D4 — N is what the target actually has: 128 bits, checked at registration

`sizeof(T) * N == 16`, rejected with a kama diagnostic when it does not hold.

⚠️ **Corrected 2026-08-30.** This originally said `comptime assert(cond: sizeof(T) * N == 16, …)` in the
type body, "the `Fixed<B, const F>` pattern". That does not transfer: `Fixed` is **kama source** with a
real body ([lib/std/num/fixed.kama:28](../../lib/std/num/fixed.kama#L28)), while an intrinsic's prelude
declaration is documentation-only with an **empty** body — `type value InlineArray<T, comptime N: int32> { }`
([prelude/builtin.kama:109](../../prelude/builtin.kama#L109)). A `comptime assert` in there would never
run. The check belongs in the **registration function**, as the `unsupported(...)` diagnostic
`registerFixed` already uses for `n <= 0` and for a non-`value` element
([kama.cemit.cpp:9047](../../src/kama.cemit.cpp#L9047)). It wants an `xfail` fixture, per the house rule.

128 bits is the only width **every** kama target
has: SSE2 on x86-64 baseline, NEON on aarch64, wasm128 with D1. Wider widths are not portable and are
not free — they need `-march`, which is the CPU-tuning knob in
[ROADMAP_DETAIL §9](../ROADMAP_DETAIL.md#s9) — so they wait for it and enter as a relaxed assert, not
as a second type or a species system.

### D5 — A no-SIMD target degrades; it is never a compile error

`vector_size` on a target with no unit lowers to scalar operations that are still correct, which is the
whole reason to prefer a type over intrinsics. Making it an error would fork every library that touches
`Simd`, and the MCU targets are exactly where that fork would land.

### D6 — Detection is a derived `@compileFor` flag, so *algorithm* choice stays explicit

D5 covers correctness; it does not tell an author whether the lanes are real. kama already derives
`@compileFor` flags from the resolved target — `ARCH_AARCH64`, `OS_LINUX`, `WASM`, `HOSTED`
([SPEC.md](../SPEC.md) *Conditional compilation*). Add **`SIMD128`**, derived the same way: true for
`ARCH_X86_64` and `ARCH_AARCH64`, true for wasm once D1 lands, false for a target whose triple promises
neither. A library then picks a *different algorithm* the way it already picks a platform
implementation — a `@compileFor(SIMD128)` conformance beside a `@compileFor(!SIMD128)` one, behind the
contract seam SPEC already teaches — rather than hoping the fallback is fast. Gate on the derived flag,
never on a target name; that rule is already in SPEC and applies unchanged.

### D7 — `std::math` does not change. `Simd` is a different tool, not a second way.

The evidence is against rebuilding `Vec4` on `Simd<float32,4>`: it buys **nothing** in a loop (§1d row 1),
it **loses** on the `ld4` SoA kernels that dominate real math code (§1c), it would break
`alignof(Vec4)` and the fixture that pins it, and `Vec3` cannot be a 3-lane vector at all (§1f).

That leaves the GOALS objection to answer honestly rather than wave away, because both types can add
four floats. The line is **what a lane means**:

- **`std::math`** is *geometry*. Lanes are named `x/y/z/w`, they mean different things, the operations
  are `dot`/`cross`/`transform`, and the layout is API — a WebGPU vertex stride. Arrays of it are AoS,
  which is what the GPU wants and what `ld4` already exploits.
- **`Simd<T,N>`** is a *lane batch*. Lanes are interchangeable, the operations are elementwise plus
  shuffle/mask/reduce, and the natural data layout is SoA.

They are one way to do geometry and one way to do lane work. Writing geometry with `Simd` is possible
and measurably worse; the doc that lands with this says so, so nobody has to re-derive §1c.

### D8 — `parallel_for`, `Real` and the math contracts say nothing

`parallel_for` distributes across cores and `Simd` works within one; they compose without either
knowing about the other. `Real` is a scalar numeric contract — a lane batch is not a `Real` and should
not pretend to be. §2 asked whether they must say anything; the answer is no, recorded so it is not
re-opened.

---

## 4. Rejected

- **Intrinsic free functions per operation** (`simdAdd4f`, `simdShuffle4f`, …). Two ways to do one
  thing the moment a type exists; no auto-degrade on a target without a unit; and per-ISA names are the
  part every surveyed language keeps `unsafe` or experimental.
- **Rebuilding `std::math` on the vector type** — §1c and §1f. Measured worse and source-breaking.
- **A width-agnostic `Vector<T>` (C#/Java species model).** It is the design Java has failed to ship in
  eleven attempts. It also cannot express a shuffle with fixed lane indices, which is half of what this
  row is for.
- **Making a no-SIMD target a compile error** — D5.
- **`ext_vector_type`** — §1e, a silent one-lane miscompile under gcc.
- **`-march=native` in `--release`.** It would widen native auto-vectorization for free (§1a) and
  produce binaries that crash on an older CPU. That is the CPU-tuning knob's problem, with its own
  opt-in.

---

## 5. Staged plan

| stage | what | size |
|---|---|---|
| **1** | ✅ **SHIPPED 2026-08-30.** `-msimd128` on the wasm target (D1), both tiers, at the wasm arm just above the release/debug split in [kama.driver.cpp](../../src/kama.driver.cpp). Measured after: 0 → **14** v128 ops in a real `--release --target wasm` build. It landed with **two** guards, not one — [check-simd-wasm.sh](../../tools/check-simd-wasm.sh) (the repo's first `# check-legs: wasm` guard; ⚠️ `./dev check` cannot run it) and [check-simd-native.sh](../../tools/check-simd-native.sh), because the *native* half of §1a was equally uninstrumented and had been measured wrong once already. Each compiles the same probe a second way — `-O0` native, `emcc` without the flag — and requires **zero** hits there, so a pattern that can never match fails instead of passing. Probe: [tests/support/simd_probe.kama](../../tests/support/simd_probe.kama), whose header carries the three probe-design traps | **S** |
| **2** | **`Simd<T, comptime N>`** (D2/D3/D4/D5): the intrinsic type, `include/kama_simd.h`, elementwise operators, `splat`/`of`/`toArray`, `lane`, `shuffle`, `Mask<N>` + compare + `select`, `reduceAdd`. Fixtures on native + san + wasm asserting **values**, plus one asserting the emitted C reaches the vector spelling | **L** |
| **3** | **derived `SIMD128` flag** (D6) — a small addition to target-flag derivation, plus an `xfail` for gating on a target name instead | **S** |

Stage 1 was independent and landed first, as planned: it was the measured gap, and it was a flag. Its
lasting contribution is not the flag but the two guards — the claim "kama vectorizes" now has an
instrument on every tier kama ships, which is what it lacked when it went wrong twice.

### Does this still gate the 1.0 tag?

[ROADMAP.md](../ROADMAP.md) says it must, because "a SIMD surface is API". The evidence weakens that:
the only source-breaking option was rebuilding `std::math`, and D7 rejects it on measurement. Stage 1
is not API. Stages 2–3 are **purely additive** — a new type and a new derived flag break nothing that
compiles today, so they are legal 1.x work. **Recommendation: row 1 no longer gates the tag.** The one
part that was a bad first impression regardless — a released compiler silently shipping scalar wasm —
has now shipped, so nothing here is time-pressured against the tag. The tag is the maintainer's call;
this is the reasoning, not the decision.

---

## 6. Doc corrections — all landed

The design landed the first round (native ✅ / wasm ❌, and "no explicit vector types needed" being true
only for elementwise math); **stage 1 then landed the second round**, because shipping the flag made the
wasm half true rather than merely acknowledged:

- [ENGINE_READINESS.md:66](../ENGINE_READINESS.md#L66) and [:111](../ENGINE_READINESS.md#L111) — now
  **native ✅, wasm ✅**, each naming the guard behind it. The shuffle/lane-mask gap stays.
- [SPEC.md](../SPEC.md) *Math* — "to SSE/NEON/**wasm128**" is true again; the ⚠️ paragraph drops from two
  limits to one.
- [targets.md](../targets.md) *Platform notes* — the wasm bullet now states the `-msimd128` runtime
  baseline (node ≥ 16 / any current browser), the one place kama does not target the generic baseline.
- [ROADMAP_DETAIL.md §2](../ROADMAP_DETAIL.md#s2) — the staged sizes, with stage 1 marked shipped;
  [ROADMAP.md](../ROADMAP.md) lost the wasm row entirely and renumbered.

## 7. Left open for the implementation

- ~~Whether `Mask<N>` is its own type or `Simd<bool, N>`.~~ **Settled 2026-08-30, by measurement: its
  own type, and it must carry `T`** — `Mask<T, N>`, not `Mask<N>`. The leaning was right for the right
  reason (a mask's lanes are all-ones/all-zeros bit patterns of the *element width*, not booleans), and
  that reason is exactly what rules out the bare `Mask<N>` spelling. Measured on **clang 18 (host and
  container) and gcc 13, identically** — a vector comparison's lane width follows its operand's:

  | operand | `a > b` lane size | total |
  |---|---|---|
  | `f32x4` | 4 B | 16 |
  | `f64x2` | 8 B | 16 |
  | `i16x8` | 2 B | 16 |

  So `Mask<4>` has no single C type: it is `int32x4` from a `float32` compare and `int16x8` from an
  `int16` one. `Mask<T, N>` (or an associated type off `Simd<T,N>`) is what can be lowered. Still open
  underneath it: whether the two type parameters are worth the surface, or whether the mask should be
  spelled as a member type so the pairing cannot be got wrong.
- Whether `Simd<T,N>` gets `foreach`. `InlineArray` has it; iterating a lane batch scalar-at-a-time is
  the shape the type exists to avoid, so the leaning is no.
- Integer lane types beyond `float32`/`int32` — the assert in D4 admits `float64`×2, `int16`×8,
  `uint8`×16 for free, but each needs a fixture before it is claimed.
- An operator-argument type check. Found while probing this row: `Mat4 * Vec4` — for which no operator
  exists — **passes `kama check`** and fails in the C compiler instead. Unrelated to SIMD, and it wants
  its own row.
