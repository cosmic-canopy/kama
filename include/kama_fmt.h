#ifndef KAMA_FMT_H
#define KAMA_FMT_H

// kama number-parsing binding — the FFI boundary for `std::fmt`'s deserialization side.
//
// The number->string CONVERSIONS (`kama_fmt_i64`/`_u64`/`_f64`/`_f32`/`_char` + the digit loop) moved into
// kama_runtime.h so the always-emitted prelude `Display`/`Formatter` can bind them without an opt-in header
// (see the "Number / char -> string" block there). What remains here is float PARSING (`strtod`/`strtof`), which
// only `std::fmt::parse` and the JSON reader need — kept opt-in so non-parsing programs never `#include <stdlib.h>`.
// Included AFTER kama_runtime.h, whose `kama_string` it reuses.

#include "kama_runtime.h"
#include <stdlib.h>   // strtod / strtof (float parsing)
#include <errno.h>    // ERANGE — the checked parse distinguishes overflow from malformed
#include <float.h>    // DBL_MAX / FLT_MAX — ERANGE alone does not say WHICH way the value left the range

// Is `p[0..n)` a float spelled the way `std::fmt::parse` reads one — Rust's grammar: an optional sign, then
// `inf` / `infinity` / `nan` in any case, or digits with an optional `.` fraction and an optional exponent, with
// at least one digit before the exponent (`1.`, `.5` and `1e3` are numbers; `.`, `1e` and `e3` are not)? strtod
// takes more than that, and none of it is a number to `parse`: leading whitespace (SPEC: `" 7"` fails), a hex
// float (`0x1p3`), a NaN payload (`nan(12)`). Read over the LENGTH, so a NUL inside the string is not an end.
static inline int kama__float_spelling(const char* p, size_t n) {
    size_t i = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) ++i;
    static const char* const words[] = { "inf", "infinity", "nan" };
    for (size_t w = 0; w < sizeof words / sizeof words[0]; ++w) {
        size_t k = 0;
        while (words[w][k] && i + k < n && (p[i + k] | 0x20) == words[w][k]) ++k;   // ASCII case-fold
        if (!words[w][k] && i + k == n) return 1;
    }
    size_t digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') { ++i; ++digits; }
    if (i < n && p[i] == '.') { ++i; while (i < n && p[i] >= '0' && p[i] <= '9') { ++i; ++digits; } }
    if (!digits) return 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        if (i < n && (p[i] == '+' || p[i] == '-')) ++i;
        if (!(i < n && p[i] >= '0' && p[i] <= '9')) return 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') ++i;
    }
    return i == n;
}

// The CHECKED float parse behind `std::fmt::parse::<float64>` — strtod, behind the strict spelling above, and
// reporting WHY it failed rather than folding everything to 0.0. Status: 0 ok, 1 empty, 2 malformed, 3 out of
// range — mirroring `ParseError`'s variants so the kama side is a plain map.
//
// OUT OF RANGE IS OVERFLOW ONLY. strtod raises ERANGE in both directions, and this used to refuse both — so a
// subnormal it had read exactly (`5e-324`, `2.2250738585072009e-308`) was "out of range" (peer KPG-13). A value
// below the smallest subnormal rounds to zero and is `Ok(0.0)`, as in Rust and Go, and as the kama literal
// `1e-400` already is (maintainer ruling, 2026-10-01). A value past the largest finite one has no float
// spelling, and reporting it beats folding it to an infinity nobody wrote — `inf` must be written to be read.
// The string's bytes are NUL-terminated (see kama_string), so strtod stops at the length; it must END there.
static inline int32_t kama_parse_f64_ck(kama_string* s, double* out) {
    const char* p = (s && s->kama_data) ? s->kama_data : "";
    const size_t n = s ? s->kama_len : 0;
    *out = 0.0;
    if (n == 0) return 1;
    if (!kama__float_spelling(p, n)) return 2;
    errno = 0;
    char* end = (char*)0;
    double v = strtod(p, &end);
    if (end != p + n) return 2;
    if (errno == ERANGE && (v > DBL_MAX || v < -DBL_MAX)) return 3;
    *out = v;
    return 0;
}

// The float32 twin: strtof rounds ONCE, to the nearest float32. Going through a double and then narrowing rounds
// twice — `"1.00000005960464483"` came out 1 where strtof gives 1.00000012 — and the narrowing turned an
// overflow into `Ok(inf)` while the same text refused for float64.
static inline int32_t kama_parse_f32_ck(kama_string* s, float* out) {
    const char* p = (s && s->kama_data) ? s->kama_data : "";
    const size_t n = s ? s->kama_len : 0;
    *out = 0.0f;
    if (n == 0) return 1;
    if (!kama__float_spelling(p, n)) return 2;
    errno = 0;
    char* end = (char*)0;
    float v = strtof(p, &end);
    if (end != p + n) return 2;
    if (errno == ERANGE && (v > FLT_MAX || v < -FLT_MAX)) return 3;
    *out = v;
    return 0;
}

#endif // KAMA_FMT_H
