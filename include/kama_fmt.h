#ifndef KAMA_FMT_H
#define KAMA_FMT_H

// kama number-parsing binding — the FFI boundary for `std::fmt`'s deserialization side.
//
// The number->string CONVERSIONS (`kama_fmt_i64`/`_u64`/`_f64`/`_f32`/`_char` + the digit loop) moved into
// kama_runtime.h so the always-emitted prelude `Display`/`Formatter` can bind them without an opt-in header
// (see the "Number / char -> string" block there). What remains here is float PARSING (`strtod`), which only
// the deserialization path (std::json) needs — kept opt-in so non-parsing programs never `#include <stdlib.h>`.
// Included AFTER kama_runtime.h, whose `kama_string` it reuses.

#include "kama_runtime.h"
#include <stdlib.h>   // strtod (float parsing)
#include <errno.h>    // ERANGE — the checked parse distinguishes overflow from malformed

// Parse a leading float from a (NUL-terminated) kama_string — for JSON number deserialization. strtod stops
// at the first non-numeric byte, so a trailing `}`/`,`/`]` is fine. Empty/garbage yields 0.0 (the caller's
// reader sets its own error flag on a malformed token).
static inline double kama_parse_f64(kama_string* s) {
    return strtod((s && s->kama_data) ? s->kama_data : "", (char**)0);
}

// The CHECKED float parse behind `std::fmt::parse::<float64>` — the same `strtod`, but reporting WHY it
// failed rather than folding everything to 0.0. Unlike the lenient reader above, a trailing byte is an
// error here: `parse` is given a whole string and "12abc" is not a number. Status: 0 ok, 1 empty,
// 2 malformed, 3 out of range — mirroring `ParseError`'s variants so the kama side is a plain map.
static inline int32_t kama_parse_f64_ck(kama_string* s, double* out) {
    const char* p = (s && s->kama_data) ? s->kama_data : "";
    *out = 0.0;
    if (*p == '\0') return 1;
    errno = 0;
    char* end = (char*)0;
    double v = strtod(p, &end);
    if (end == p || *end != '\0') return 2;   // no digits consumed, or trailing garbage
    if (errno == ERANGE) return 3;            // overflow to +/-HUGE_VAL, or underflow to a subnormal/0
    *out = v;
    return 0;
}

#endif // KAMA_FMT_H
