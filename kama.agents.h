#ifndef KAMA_AGENTS_H
#define KAMA_AGENTS_H

// The agent-guidance files, embedded into the compiler binary at build time so `kama agents` works
// from any install — including `--no-std`, which ships only bin/kama (see install.sh). Same
// mechanism and the same reason as the prelude (kama.prelude.h): nothing to resolve at runtime,
// nothing to add to the release payload, nothing that can go missing.
//
// The definitions live in build/kama.agents.gen.cpp, generated from agents/ by
// tools/embed_agents.sh. User docs: docs/agents.md.
//
// The CONTENT is KAMA_AGENTS_MD and nothing else. Every stub is a POINTER to it — either the
// documented `@AGENTS.md` import or one line telling an agent to go read it — so a project has one
// file to edit and there is nothing to keep in sync. tools/check-agents.sh enforces that mechanically.

// agents/AGENTS.md — the cross-tool standard file, read natively by most agent tools.
extern const char* KAMA_AGENTS_MD;

// agents/skill/SKILL.md — the richer, on-demand form. Separate because a skill loads only when it is
// relevant, where AGENTS.md is paid for on every turn: different budgets, so different content.
extern const char* KAMA_AGENTS_SKILL;

// A per-tool pointer file, for the tools that read neither AGENTS.md nor a skill.
//   name — the key `kama agents --tool <name>` takes (the stub file's basename)
//   dest — where `install` writes it, relative to the project root. Declared by the stub file's own
//          first line, `<!-- dest: … -->`, which the generator strips: the path a tool reads is a
//          fact about that tool, so it belongs beside the tool's text and not in a C++ table that
//          someone adding a stub would have to find.
//   src  — the pointer text itself
struct KamaAgentStub { const char* name; const char* dest; const char* src; };
extern const KamaAgentStub KAMA_AGENT_STUBS[];
extern const int KAMA_AGENT_STUB_COUNT;

#endif // KAMA_AGENTS_H
