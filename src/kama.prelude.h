#ifndef KAMA_PRELUDE_H
#define KAMA_PRELUDE_H

// The built-in kama sources, embedded into the compiler binary at build time so a `--no-std`
// install (which ships only bin/kama — see install.sh) still carries the language core. The
// definitions live in build/kama.prelude.gen.cpp, generated from prelude/*.kama by
// tools/embed_prelude.sh. See prelude/global.kama (the implicit global-namespace prelude) and
// prelude/std/memory/*.kama (the always-in-scope smart-pointer triad).

// The global-namespace prelude source (Optional/Result, the contracts, the primitive conformances,
// Chars/Split). Parsed once, collected before user code with an empty scope so its names resolve
// unqualified everywhere.
extern const char* KAMA_PRELUDE_SRC;

// A namespaced built-in module — kama source that declares its own `namespace` (e.g. std::memory).
// Loaded as an ordinary namespaced unit (registers under its scope, honors its `export`), plus an
// implicit `using` so its names also resolve unqualified.
struct KamaPreludeModule { const char* src; };
extern const KamaPreludeModule KAMA_PRELUDE_MODULES[];
extern const int KAMA_PRELUDE_MODULE_COUNT;

#endif // KAMA_PRELUDE_H
