#ifndef KAMA_MATH_H
#define KAMA_MATH_H

// kama scalar-math binding — the FFI boundary for `std::math`'s libm surface.
//
// Why a shim at all, when `<math.h>` already declares every one of these: `std::math` exports C's OWN
// names (`sin`/`sinf`, `pow`/`powf`, `floor`/`floorf`), and a kama wrapper cannot share its extern's
// name — inside `namespace std::math` a `fn float64 sin(float64)` whose body called `sin(x: x)` would
// resolve to itself and recurse. An `extern fn` also keeps its literal C symbol, so it cannot be
// `export`ed under a namespace either. Renaming the SEAM is the way out: kama binds `kama_sin` and
// publishes it as `sin`. Same shape `kama_fmt.h` uses for the number->string direction.
//
// Each is one inlined call, so the indirection costs nothing at -O1 and above. The driver appends `-lm` for a
// program that externs this header (or `<math.h>`).
//
// A bare-metal build (KR-341) has neither <math.h> nor a libm. There the exact seven are the compiler's builtins, one
// instruction where the FPU has it and otherwise a call to the plain name (`sqrtf`), which the entry unit defines, weak,
// over musl's (the block at the end of this file); the other fourteen are musl's, as under `reproducible-float`.

#if defined(KAMA_TARGET_EMBEDDED)
#include "kama_libm.h"
#define KAMA_MATH_1(name)                                                                  \
    static inline double kama_##name(double x) { return __builtin_##name(x); }             \
    static inline float  kama_##name##f(float x) { return __builtin_##name##f(x); }
#define KAMA_MATH_2(name)                                                                             \
    static inline double kama_##name(double x, double y) { return __builtin_##name(x, y); }       \
    static inline float  kama_##name##f(float x, float y) { return __builtin_##name##f(x, y); }
#else
#include <math.h>

#define KAMA_MATH_1(name)                                                  \
    static inline double kama_##name(double x) { return name(x); }         \
    static inline float  kama_##name##f(float x) { return name##f(x); }

#define KAMA_MATH_2(name)                                                             \
    static inline double kama_##name(double x, double y) { return name(x, y); }       \
    static inline float  kama_##name##f(float x, float y) { return name##f(x, y); }
#endif

// Exact, by IEEE 754 (`sqrt`) or by construction, so every libm gives the same bits: always libm's,
// which a compiler inlines to one instruction where the target has it.
KAMA_MATH_1(sqrt)
KAMA_MATH_1(floor)
KAMA_MATH_1(ceil)
KAMA_MATH_1(round)
KAMA_MATH_1(trunc)
KAMA_MATH_1(fabs)
KAMA_MATH_2(fmod)

// Not required correctly rounded, so each libm rounds these its own way in the last bit: native Windows
// and wasm disagree on 343,547 of 5.8 million inputs, a Windows CPU without FMA3 disagrees with one that
// has it, and a release wasm build with a debug one. Under `reproducible-float`, which promises the same
// bits on every target, the driver defines KAMA_REPRODUCIBLE_FLOAT and they come from musl compiled into
// the program instead (kama_libm.h), on every target, wasm included. Without the key, the platform's libm,
// which is as fast as C's because it is C's.
#if defined(KAMA_REPRODUCIBLE_FLOAT) || defined(KAMA_TARGET_EMBEDDED)
#include "kama_libm.h"
#define KAMA_LIBM_1(name)                                                           \
    static inline double kama_##name(double x) { return kama__musl_##name(x); }     \
    static inline float  kama_##name##f(float x) { return kama__musl_##name##f(x); }
#define KAMA_LIBM_2(name)                                                                     \
    static inline double kama_##name(double x, double y) { return kama__musl_##name(x, y); }   \
    static inline float  kama_##name##f(float x, float y) { return kama__musl_##name##f(x, y); }
#else
#define KAMA_LIBM_1(name) KAMA_MATH_1(name)
#define KAMA_LIBM_2(name) KAMA_MATH_2(name)
#endif

KAMA_LIBM_1(sin)
KAMA_LIBM_1(cos)
KAMA_LIBM_1(tan)
KAMA_LIBM_1(asin)
KAMA_LIBM_1(acos)
KAMA_LIBM_1(atan)
KAMA_LIBM_1(exp)
KAMA_LIBM_1(log)
KAMA_LIBM_1(log2)
KAMA_LIBM_1(log10)
KAMA_LIBM_1(cbrt)
KAMA_LIBM_2(pow)
KAMA_LIBM_2(atan2)
KAMA_LIBM_2(hypot)

// The lane-batch trio: `sqrt`/`floor`/`ceil` on a `Simd<float32|float64>#(N)`. The compiler emits one
// instance per float lane batch beside its KAMA_SIMD_FUNCS (kama_runtime.h), passing the width-matched
// libm name (`sqrtf` for a float lane, `sqrt` for a double one). Written as PER-LANE LOOPS for the same
// reason every KAMA_SIMD_FUNCS member is: both gcc and clang fold the loop to the vector instruction
// (`fsqrt.4s`, `frintm.4s`, `frintp.4s`; `f32x4.sqrt/floor/ceil` on wasm) and neither has a portable
// elementwise builtin. ⚠️ `sqrt` folds only under `-fno-math-errno`, which the driver passes on every C
// compile: with errno live, the call is unsinkable and stays a per-lane libm call. Measured — see
// ROADMAP_DETAIL §2. It lives here and not in kama_runtime.h because that header is freestanding and
// these need `<math.h>`; a program that never calls them pays nothing — the functions are static inline.
#define KAMA_SIMD_MATH(T, N, NAME, SQRT, FLOOR, CEIL)                             \
static inline NAME NAME##__sqrt(const NAME* self) {                              \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (T)SQRT((*self)[i]); return r;  \
}                                                                               \
static inline NAME NAME##__floor(const NAME* self) {                             \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (T)FLOOR((*self)[i]); return r; \
}                                                                               \
static inline NAME NAME##__ceil(const NAME* self) {                              \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (T)CEIL((*self)[i]); return r;  \
}

#undef KAMA_MATH_1
#undef KAMA_MATH_2
#undef KAMA_LIBM_1
#undef KAMA_LIBM_2

#endif

// The plain names a bare-metal program's builtins and musl's fourteen call when no instruction does the job, defined once,
// weak, over musl's exact ones: the entry unit defines KAMA__MATH_DEFINE and includes this header again, so the block sits
// outside the include guard. Weak, so a board that links a libm keeps its own.
#if defined(KAMA_TARGET_EMBEDDED) && defined(KAMA__MATH_DEFINE) && !defined(KAMA__MATH_DEFINED)
#define KAMA__MATH_DEFINED 1
#define KAMA__MATH_WEAK_1(name)                                                                    \
    __attribute__((weak)) double name(double x) { return kama__musl_exact_##name(x); }             \
    __attribute__((weak)) float name##f(float x) { return kama__musl_exact_##name##f(x); }
KAMA__MATH_WEAK_1(sqrt)
KAMA__MATH_WEAK_1(floor)
KAMA__MATH_WEAK_1(ceil)
KAMA__MATH_WEAK_1(round)
KAMA__MATH_WEAK_1(trunc)
KAMA__MATH_WEAK_1(fabs)
__attribute__((weak)) double fmod(double x, double y) { return kama__musl_exact_fmod(x, y); }
__attribute__((weak)) float fmodf(float x, float y) { return kama__musl_exact_fmodf(x, y); }
__attribute__((weak)) double scalbn(double x, int n) { return kama__musl_exact_scalbn(x, n); }
#undef KAMA__MATH_WEAK_1
#endif
