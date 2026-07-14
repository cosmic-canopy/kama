#ifndef KAMA_FMT_H
#define KAMA_FMT_H

// kama number->string bindings — the FFI boundary for `std::fmt`.
//
// There is no number formatting on the kama surface, so this header supplies the four primitive
// conversions (signed/unsigned decimal, float32/float64) as heap-owned `string`s. Integer formatting is a
// plain digit loop; float formatting delegates to the C library's `snprintf` at a precision that
// round-trips exactly (`%.17g` for double, `%.9g` for float) — correct and bounded, so no undefined
// behavior. Pay-for-what-you-use: this header is pulled in ONLY by a module that does
// `extern "kama_fmt.h";` (i.e. `std::fmt`), so non-formatting programs never `#include <stdio.h>`.
// Included AFTER kama_runtime.h, whose `kama_string` / `kama_string_from_raw` / `kama_alloc` it reuses.

#include "kama_runtime.h"
#include <stdio.h>    // snprintf (float formatting)
#include <stdlib.h>   // strtod (float parsing)

// Reversed digit loop for an unsigned 64-bit value into `buf` (no NUL); returns the digit count.
// `0` renders as a single '0'. buf must hold >= 20 bytes.
static inline int kama_fmt_u64_digits(char* buf, uint64_t v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    int n = t, i = 0;
    while (t) buf[i++] = tmp[--t];
    return n;
}

// Unsigned decimal -> fresh heap-owned kama_string.
static inline kama_string kama_fmt_u64(uint64_t v) {
    char buf[24];
    int n = kama_fmt_u64_digits(buf, v);
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// Signed decimal -> fresh heap-owned kama_string. INT64_MIN is handled by taking the magnitude in the
// unsigned domain (`0u - (uint64_t)v`), which is well-defined two's-complement and never overflows.
static inline kama_string kama_fmt_i64(int64_t v) {
    char buf[24]; int n = 0;
    if (v < 0) {
        buf[n++] = '-';
        n += kama_fmt_u64_digits(buf + n, (uint64_t)0 - (uint64_t)v);
    } else {
        n = kama_fmt_u64_digits(buf, (uint64_t)v);
    }
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// float64 -> fresh heap-owned kama_string, at shortest-round-trip precision (%.17g).
static inline kama_string kama_fmt_f64(double v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%.17g", v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// float32 -> fresh heap-owned kama_string, at shortest-round-trip precision for single (%.9g).
static inline kama_string kama_fmt_f32(float v) {
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%.9g", (double)v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// Parse a leading float from a (NUL-terminated) kama_string — for JSON number deserialization. strtod stops
// at the first non-numeric byte, so a trailing `}`/`,`/`]` is fine. Empty/garbage yields 0.0 (the caller's
// reader sets its own error flag on a malformed token).
static inline double kama_parse_f64(kama_string* s) {
    return strtod((s && s->data) ? s->data : "", (char**)0);
}

#endif // KAMA_FMT_H
