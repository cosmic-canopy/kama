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

// A built-in module — kama source belonging to a real stdlib module (today only std::memory, the
// always-in-scope smart-pointer triad), embedded so it survives a `--no-std` install. Loaded as an
// ordinary scoped unit (registers under its module, honors its `export`), plus an implicit `using` so
// its names also resolve unqualified.
//
// `name` is what a diagnostic raised inside this module's body names — `<prelude>/std/memory/shared.kama`.
// The `<` marks it synthetic (setPackageResolver skips the filesystem walk for such a unit; see the
// comment there), and the rest names WHICH module, which one shared `<prelude-module>` for all of them
// did not. An install has no such file, but a diagnostic pointing into the prelude is a compiler bug
// being reported, and then naming the module IS the content of the message.
//
// `module` is the module these declarations belong to (`std::memory`), stated rather than derived, and
// that is load-bearing in two directions. A synthetic unit has NO path, so the file→module derivation
// that replaces the `namespace` declaration (design/module-system.md §2b) cannot reach it — without
// this it would fall through to the file-private scope and `std__memory__Owned` would silently become
// `_F<n>__Owned`. And the driver needs it to know this module is ALREADY in every compilation, so an
// explicit `import std::memory` skips the disk copy rather than parsing a second one.
struct KamaPreludeModule { const char* src; const char* name; const char* module; };
extern const KamaPreludeModule KAMA_PRELUDE_MODULES[];
extern const int KAMA_PRELUDE_MODULE_COUNT;

#endif // KAMA_PRELUDE_H
