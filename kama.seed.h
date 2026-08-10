#ifndef KAMA_SEED_H
#define KAMA_SEED_H

// The `kama seed` project templates, embedded into the compiler binary at build time so seeding works
// from any install — including `--no-std`, which ships only bin/kama (see install.sh). Same mechanism
// and the same reason as the prelude (kama.prelude.h) and the agent files (kama.agents.h).
//
// The definitions live in build/kama.seed.gen.cpp, generated from seed/ by tools/embed_seed.sh.
//
// Four named externs rather than a table, unlike KAMA_AGENT_STUBS. That table exists so adding a tool
// touches no C++; these four are four ROLES the C++ names one by one, a fifth would be a code change
// anyway, and a missing one becomes a LINK error instead of a silent gap. Where a template lands is not
// a property of the file either — the project kind decides (src/app.kama for an executable,
// src/<name>.kama for a library) — so there is no dest declaration to carry.
//
// Two tokens are substituted at write time, both chosen to be legal kama identifiers so that every
// seed/*.kama COMPILES AS-IS and joins the whole-corpus grammar oracle (tools/check-treesitter.sh):
//
//   KAMA_SEED_NAME    the package name as written in kama.json  (`@acme/geo`, `my-app`)
//   KAMA_SEED_IDENT   the namespace / import identifier derived from it  (`geo`, `my_app`)

extern const char* KAMA_SEED_APP;         // seed/app.kama    — the executable entry
extern const char* KAMA_SEED_LIB;         // seed/lib.kama    — the library surface
extern const char* KAMA_SEED_GITIGNORE;   // seed/gitignore   — dotless in the repo, .gitignore on disk
extern const char* KAMA_SEED_README;      // seed/README.md   — the README stub

#endif // KAMA_SEED_H
