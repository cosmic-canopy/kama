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

// Parse a leading float from a (NUL-terminated) kama_string — for JSON number deserialization. strtod stops
// at the first non-numeric byte, so a trailing `}`/`,`/`]` is fine. Empty/garbage yields 0.0 (the caller's
// reader sets its own error flag on a malformed token).
static inline double kama_parse_f64(kama_string* s) {
    return strtod((s && s->data) ? s->data : "", (char**)0);
}

#endif // KAMA_FMT_H
