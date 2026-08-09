# kama — repo guide for AI assistants

This repo builds **the kama compiler itself**. That makes it two codebases at once, and which one
you are in decides which tools help:

- **~34k lines of C++** (`kama.l`, `kama.y`, `kama.cemit.*`, `kama.driver.cpp`, `kama_runtime.h`) —
  the compiler. Ordinary C++ work; `kama query` cannot see any of it.
- **~28k lines of kama** across `lib/std/`, `tests/`, `prelude/`, `examples/`, `bench/` — the
  stdlib and the fixture corpus. Here the guidance in `docs/agents.md` applies, and
  `kama query --search` beats grep because it answers from what the compiler resolved.

**Read [docs/GOALS.md](docs/GOALS.md) first** — the language's design philosophy. Honor it,
especially: *favor one way to do a thing, favor simplicity, favor explicit over implicit.*

## Working style

This repo vendors two guidance skills under `.claude/skills/` — apply them:
- **karpathy-guidelines**: think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail**: write the least code that works (YAGNI ladder; deletion over addition).

## Verify the claim before you build on it

The house rule, learned the hard way and worth more than any other line here: **a doc is not
evidence.** Run it.

`docs/SPEC.md` stated that `foreach (char c in s)` "is a type error — the byte/codepoint distinction
is enforced". It was not enforced, and the loop silently yielded one bogus `char` per UTF-8 byte.
Nobody caught it because every fixture used one of the two *correct* spellings, and because
`tests/idioms_kama_way.kama` compiles the docs' **positive** examples only — a prose claim that
something is rejected has no guard unless a `tests/xfail/` fixture proves it.

So: compile the snippet, run the command, read the emitted C. Every brief in this repo has been
wrong somewhere load-bearing.

## Build & test

**Use `./dev` — not `make` or `tools/cdev` directly.** Run `./dev help` for the full list. Every
test task builds the binary it is about to test, which is the one thing you cannot get right by
hand reliably (see the trap below).

```sh
./dev build          # build the compiler for this host
./dev test           # native fixture suite
./dev test san       # ASan/UBSan  (container — macOS has no LeakSanitizer)
./dev test wasm      # wasm/node   (container)
./dev matrix         # test all + every tools/check-*.sh guard — the pre-commit gate
```

Notes:
- **The stale-binary trap.** Build artifacts are platform-scoped (`build/<os>-<arch>/`), so a host
  build and a container build coexist — switching needs no `make clean`. The cost is that building
  one and testing the other passes *silently* against an old compiler. `./dev` exists to make that
  unrepresentable; if you bypass it, rebuild for the platform you are about to test on.
- The repo root holds only hand-written sources plus `./kama`, a symlink to whichever platform built
  last. Anything that must get the *native* binary regardless (the test harness, the
  `tools/check-*.sh` guards, the VS Code extension) resolves `build/<os>-<arch>/kama` directly — in a
  shell script, source `tools/kama-bin.sh` rather than hardcoding a path.
- The grammar needs bison ≥ 2.7 (the container has 3.8; macOS host needs `brew install bison`).
- The compiler is a tree-walking C emitter (`kama.cemit.*`) over the Flex/Bison/AST front end
  (`kama.l`, `kama.y`, `kama.ast.h`). (An early LLVM backend was removed; C emission is the only backend.)
- Each language feature lands as a milestone with `tests/` fixtures verifying exit codes on native + wasm.
- A `tools/check-*.sh` is **glob-enrolled** by `./dev check`, so a new guard joins the gate with no
  list to update. A new negative claim in the docs wants a `tests/xfail/` fixture in the same commit.

## Where things are written down

`docs/ROADMAP.md` is a **plan, not a changelog** — delete a shipped item once its record lands in
the right place (`docs/SPEC.md` for language surface, `docs/packages.md` / `docs/editors.md` /
`docs/targets.md` for workflow, the git log for *why*). Its *Working order* table is the authority
on what to do next.

`agents/AGENTS.md` is **not** this file: it is the snippet kama *ships* to user projects, and it
must stay free of this repo's own working preferences. `docs/agents.md` documents that surface.
