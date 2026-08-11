#ifndef KAMA_LOG_H
#define KAMA_LOG_H

// The `std::log` runtime seam — the FFI boundary for leveled/tagged diagnostics. Pulled in only by a program
// that `import std::log`s (`extern "kama_log.h";`), so non-logging programs pay nothing.
//
// Three concerns, kept separate (the Rust log/tracing shape): a runtime FILTER decides whether a record is
// enabled, a swappable SINK outputs it, and the facade builds the message. This header holds the filter, the
// sink slot, and the default console sink. It is modeled on the setPanicHandler slot, not a `Logger`
// contract: a kama resource can't be a module-static (no static-teardown seam) and a `Ptr<Logger>` to an
// interface isn't dispatchable, so kama never calls a fnptr through a module-static — it calls the extern
// `kama_log_dispatch`, which invokes the C-held slot.
//
// Requires kama_runtime.h (included first by the emitter): `kama_string` and `kama_print_write`. The config
// source is the PROCESS-GLOBAL `KAMA_LOG` env (getenv), so every translation unit / isolate reads the same
// value into its own module-scoped static — argv is a module-scoped static (per the concurrency model) and
// isn't visible outside main's TU, so the `--log` flag is bridged into KAMA_LOG once in main (see
// kama_log_init_args). getenv/setenv/isatty are declared at BLOCK scope (the kama_runtime convention), so
// <stdlib.h>/<unistd.h> never leak and the header stays freestanding clean.

#include <stdint.h>

// ---- Sink slot ---------------------------------------------------------------
// A replaceable pointer (NOT set-once — the default console sink is the C fallback below, and a user override
// must win): register your own before spawning isolates, then it is read-only. ABI: the kama `LogSink` fnptr
// is `void(LogLevel, string, string)`; LogLevel is a plain enum (compatible with int32_t), string is
// kama_string — matching this typedef. dispatch synthesizes borrowed kama_strings (cap==0) for it, valid for
// the duration of the call.
typedef void (*kama_log_sink_fn)(int32_t level, kama_string tag, kama_string msg);
// EXTERNAL LINKAGE (single definition in the entry TU, emitted by the compiler — see `isEntry` in
// kama.cemit.cpp). The sink is process-global: `setLogSink` writes it from the std::log TU while a log call
// dispatches from wherever it appears (main / any module), so a per-TU `static` slot would read an empty copy
// and always fall back to the console default. One object fixes it for every TU. (Same class as the panic
// hook; the LEVEL/TAG filter cache below stays per-TU — each TU derives identical state from the env.)
extern kama_log_sink_fn kama_log_slot;
static inline void kama_set_log_sink(kama_log_sink_fn s) { kama_log_slot = s; }

// ---- Level names / config parsing --------------------------------------------
// Levels: error=0 (most severe) .. trace=4 (most verbose); off=-1 suppresses all. A call at level L is
// enabled when L <= threshold(tag). Default threshold = Info(2). Config grammar (from `--log`, else
// `KAMA_LOG`): `warn,audio=debug,net=trace` — a leading bareword sets the global threshold, each `tag=level`
// overrides one tag.

// name == b (b is a NUL-terminated literal), comparing the first n bytes of `name` then requiring b's end.
static int kama_log_streq(const char* name, int n, const char* b) {
    int i = 0;
    for (; i < n; i++) { if (b[i] == '\0' || name[i] != b[i]) return 0; }
    return b[i] == '\0';
}
static int kama_log_level_of(const char* s, int n) {
    if (kama_log_streq(s, n, "off"))   return -1;
    if (kama_log_streq(s, n, "error")) return 0;
    if (kama_log_streq(s, n, "warn"))  return 1;
    if (kama_log_streq(s, n, "info"))  return 2;
    if (kama_log_streq(s, n, "debug")) return 3;
    if (kama_log_streq(s, n, "trace")) return 4;
    return -2;   // unknown -> ignored (a typo never silently breaks the global level)
}

static int  kama_log_parsed = 0;
static int  kama_log_global = 2;     // Info
static char kama_log_tag_name[16][32];
static int  kama_log_tag_level[16];
static int  kama_log_ntags = 0;
static int  kama_log_color = 0;

static void kama_log_parse_spec(const char* spec) {
    int i = 0;
    while (spec[i]) {
        int start = i;
        while (spec[i] && spec[i] != ',') i++;
        int end = i;
        int eq = -1;
        for (int j = start; j < end; j++) { if (spec[j] == '=') { eq = j; break; } }
        if (eq < 0) {                                   // bareword -> global threshold
            int lv = kama_log_level_of(spec + start, end - start);
            if (lv != -2) kama_log_global = lv;
        } else {                                        // tag=level -> per-tag override
            int nameLen = eq - start;
            int lv = kama_log_level_of(spec + eq + 1, end - (eq + 1));
            if (lv != -2 && nameLen > 0 && nameLen < 32 && kama_log_ntags < 16) {
                for (int k = 0; k < nameLen; k++) kama_log_tag_name[kama_log_ntags][k] = spec[start + k];
                kama_log_tag_name[kama_log_ntags][nameLen] = '\0';
                kama_log_tag_level[kama_log_ntags] = lv;
                kama_log_ntags++;
            }
        }
        if (spec[i] == ',') i++;
    }
}

// The `--log` -> env bridge, run ONCE in main (where argv is valid) when a program imports std::log — the
// emitter emits a call to this right after kama_args_init. It mirrors a `--log VALUE` / `--log=VALUE` runtime
// flag into the process env (KAMA_LOG), so the filter has a single PROCESS-GLOBAL config source that every
// translation unit / isolate reads uniformly — argv itself is a module-scoped static (per the concurrency
// model), so it is NOT visible outside main's TU. `--log` overwrites KAMA_LOG (the flag is primary; the env
// is the secondary layer). Non-logging programs never call this and pay nothing.
static inline void kama_log_init_args(int argc, char** argv) {
    const char* spec = 0;
    for (int a = 1; a < argc; a++) {
        const char* arg = argv[a];
        if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'l' && arg[3] == 'o' && arg[4] == 'g') {
            if (arg[5] == '\0' && a + 1 < argc) { spec = argv[a + 1]; break; }
            if (arg[5] == '=') { spec = arg + 6; break; }
        }
    }
    if (!spec) return;
#if defined(_WIN32)
    { extern int _putenv_s(const char*, const char*); (void)_putenv_s("KAMA_LOG", spec); }
#else
    { extern int setenv(const char*, const char*, int); (void)setenv("KAMA_LOG", spec, 1); }
#endif
}

// The baked project default (from the manifest `log` section), seeded in main right AFTER kama_log_init_args
// when the manifest declares one. `setenv(..., overwrite=0)` is deliberate: it only fills KAMA_LOG when it is
// not already set, so precedence stays `--log` (overwrite=1) > a pre-existing KAMA_LOG env > this baked
// default > the hardcoded Info floor. It writes the same process-global env every TU reads, so the multi-TU
// static-copy hazard that a runtime default slot would have is sidestepped. Embedded has no env -> no-op.
static inline void kama_log_set_default(const char* spec) {
    if (!spec || !spec[0]) return;
#if !defined(KAMA_TARGET_EMBEDDED)
#if defined(_WIN32)
    { extern char* getenv(const char*); extern int _putenv_s(const char*, const char*);
      if (!getenv("KAMA_LOG")) (void)_putenv_s("KAMA_LOG", spec); }
#else
    { extern int setenv(const char*, const char*, int); (void)setenv("KAMA_LOG", spec, 0); }
#endif
#else
    (void)spec;
#endif
}

// Parse the config ONCE (lazy) from the process-global `KAMA_LOG` env (fed by `--log` via kama_log_init_args,
// or set directly). Cache the stderr isatty for the default sink's color. Embedded has no env/tty -> defaults.
static void kama_log_ensure_parsed(void) {
    if (kama_log_parsed) return;
    kama_log_parsed = 1;
#if !defined(KAMA_TARGET_EMBEDDED)
    extern char* getenv(const char*);
    const char* spec = getenv("KAMA_LOG");
    if (spec) kama_log_parse_spec(spec);
#if defined(_WIN32)
    { extern int _isatty(int); kama_log_color = _isatty(2) ? 1 : 0; }
#else
    { extern int isatty(int); kama_log_color = isatty(2) ? 1 : 0; }
#endif
#endif
}

static int kama_log_threshold(const char* tag, size_t tagLen) {
    for (int i = 0; i < kama_log_ntags; i++) {
        const char* nm = kama_log_tag_name[i];
        size_t j = 0; int ok = 1;
        for (; j < tagLen; j++) { if (nm[j] == '\0' || nm[j] != tag[j]) { ok = 0; break; } }
        if (ok && nm[tagLen] == '\0') return kama_log_tag_level[i];
    }
    return kama_log_global;
}

// The filter — kama binds this as `extern fn bool kama_log_enabled(int32 level, Ptr<int8> tag, usize tagLen)`.
// Strings cross the seam as borrowed byte spans (the floor `print` discipline), never by value, so no kama
// ownership is transferred through the facade.
static inline bool kama_log_enabled(int32_t level, const char* tag, size_t tagLen) {
    kama_log_ensure_parsed();
    return (int)level <= kama_log_threshold(tag, tagLen);
}

// ---- Default console sink ----------------------------------------------------
// `[LEVEL] tag: msg\n` to stderr (fd 2 via kama_print_write, so it routes to the weak kama_log_sink on
// embedded), ANSI level color on a tty. No libc: prefix strings are literals written by an inline strlen.
static const char* kama_log_prefix(int level, int color) {
    if (color) {
        switch (level) {
            case 0: return "\x1b[31m[ERROR]\x1b[0m ";   // red
            case 1: return "\x1b[33m[WARN]\x1b[0m ";    // yellow
            case 2: return "\x1b[32m[INFO]\x1b[0m ";    // green
            case 3: return "\x1b[36m[DEBUG]\x1b[0m ";   // cyan
            case 4: return "\x1b[90m[TRACE]\x1b[0m ";   // bright black
            default: return "\x1b[32m[INFO]\x1b[0m ";
        }
    }
    switch (level) {
        case 0: return "[ERROR] ";
        case 1: return "[WARN] ";
        case 2: return "[INFO] ";
        case 3: return "[DEBUG] ";
        case 4: return "[TRACE] ";
        default: return "[INFO] ";
    }
}
static inline void kama_log_puts(const char* s) {
    size_t n = 0; while (s[n]) n++;
    kama_print_write(2, s, n);
}

// The sink entry — kama binds this as
// `extern fn void kama_log_dispatch(int32 level, Ptr<int8> tag, usize tagLen, Ptr<int8> msg, usize msgLen)`.
// A registered sink wins (fed borrowed kama_strings valid for the call); otherwise the C console default
// runs. All byte spans are borrowed (the caller still owns them) — read only, never freed here.
static inline void kama_log_dispatch(int32_t level, const char* tag, size_t tagLen,
                                     const char* msg, size_t msgLen) {
    if (kama_log_slot) {
        kama_log_slot(level, kama_string_lit(tag, tagLen), kama_string_lit(msg, msgLen));
        return;
    }
    kama_log_ensure_parsed();
    kama_log_puts(kama_log_prefix((int)level, kama_log_color));
    if (tagLen) { kama_print_write(2, tag, tagLen); kama_print_write(2, ": ", 2); }
    if (msgLen) kama_print_write(2, msg, msgLen);
    kama_print_write(2, "\n", 1);
}

#endif  // KAMA_LOG_H
