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
// Every one of these is a single inlined libm call, so the indirection costs nothing at -O1 and above.
// `scalar.kama` still declares `extern "<math.h>"` alongside this header, because that declaration is
// what makes the driver append `-lm`.

#include <math.h>

#define KAMA_MATH_1(name)                                                  \
    static inline double kama_##name(double x) { return name(x); }         \
    static inline float  kama_##name##f(float x) { return name##f(x); }

#define KAMA_MATH_2(name)                                                             \
    static inline double kama_##name(double x, double y) { return name(x, y); }       \
    static inline float  kama_##name##f(float x, float y) { return name##f(x, y); }

KAMA_MATH_1(sin)
KAMA_MATH_1(cos)
KAMA_MATH_1(tan)
KAMA_MATH_1(asin)
KAMA_MATH_1(acos)
KAMA_MATH_1(atan)
KAMA_MATH_1(exp)
KAMA_MATH_1(log)
KAMA_MATH_1(log2)
KAMA_MATH_1(log10)
KAMA_MATH_1(sqrt)
KAMA_MATH_1(cbrt)
KAMA_MATH_1(floor)
KAMA_MATH_1(ceil)
KAMA_MATH_1(round)
KAMA_MATH_1(trunc)
KAMA_MATH_1(fabs)

KAMA_MATH_2(pow)
KAMA_MATH_2(fmod)
KAMA_MATH_2(atan2)
KAMA_MATH_2(hypot)

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

#endif
