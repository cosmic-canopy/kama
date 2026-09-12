# kama — repo guide for AI assistants

This repo builds **the kama compiler itself**. That makes it two codebases at once, and which one
you are in decides which tools help:

- **~34k lines of C++** in `src/` (`kama.l`, `kama.y`, `kama.cemit.*`, `kama.driver.cpp`) plus the
  shipped runtime headers in `include/` (`kama_runtime.h`, `kama_os.h`, …) — the compiler. Ordinary
  C++ work; `kama query` cannot see any of it.
- **~28k lines of kama** across `lib/std/`, `tests/`, `prelude/`, `examples/`, `bench/` — the
  stdlib and the fixture corpus. Here the guidance in `docs/agents.md` applies, and
  `kama query --search` beats grep because it answers from what the compiler resolved.

**Read [docs/GOALS.md](docs/GOALS.md) first** — the language's design philosophy. Honor it,
especially: *favor one way to do a thing, favor simplicity, favor explicit over implicit.*

## Working style

This repo vendors two guidance skills under `.claude/skills/` — apply them:
- **karpathy-guidelines**: think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail**: write the least code that works (YAGNI ladder; deletion over addition).

**YAGNI stops at the language surface.** Both skills govern how code is written, not what kama offers.
kama is a general-purpose systems language, and a feature is decided on its merits against GOALS.md, never
deferred because no consumer in the corpus asks for it yet: the corpus cannot ask for what the language has
not made available. "No consumer yet" is not a verdict. A surface item is *scheduled*, *genuinely optional*
(say why a consumer never needs it), or a *non-goal* (say what answers the need instead), and each verdict
is written down in ROADMAP_DETAIL. Ponytail's ladder still applies to the implementation of whatever is
decided: least code, deletion over addition. Learned the expensive way: the read-only place was refused
three times as "the corpus asks for it nowhere" and then blocked the first external package.

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

**Run a test task ONCE, into a file, then read the file.** These take minutes, not seconds — `./dev
matrix` builds the compiler and runs the whole corpus plus every guard. Re-running it to see a
different slice of the same output is pure waste:

```sh
./dev matrix > /tmp/matrix.log 2>&1; tail -5 /tmp/matrix.log   # then grep the SAME file for details
```

**Bump `VERSION` in any commit that changes `src/`, `include/`, `prelude/`, `lib/`, `agents/` or
`seed/`** — a patch bump is the default; those are the six trees that end up inside the binary. The
last two read as documentation and are not: they are *embedded*, so editing them changes what
`kama agents` and `kama seed` write. `kama --version` is the first
thing a bug report carries, and it is useless when two different compilers claim the same number:
VERSION sat at `0.9.5` across dozens of emitter-changing commits before `tools/check-version.sh`
started holding it down. Docs-only and test-only commits need no bump — they produce an identical
compiler. The Makefile appends `+g<short-sha>` to whatever the file says, so a forgotten bump still
leaves a binary you can identify; that is the backstop, not permission to skip it. Tagging the repo is
a separate, later act — this is only the number the binary reports.

Notes:
- **Per-platform notes live in `docs/platforms/`** — one page per host that needs more than `./dev`.
  Only [windows.md](docs/platforms/windows.md) exists so far, because macOS and Linux are where this
  repo is built daily and have nothing to say that this file does not. On Windows read it FIRST: msys2
  setup, why the language server needs a snapshot of the compiler rather than the build itself, and the
  list of things that are true there and nowhere else (`long` is 32-bit; `system()` runs cmd.exe, which
  has no `/dev/null`; a running executable is locked; git writes symlinks as text files). Every entry
  on that list cost a wrong diagnosis before someone wrote it down.
- **The stale-binary trap.** Build artifacts are platform-scoped (`out/<os>-<arch>/`), so a host
  build and a container build coexist — switching needs no `make clean`. The cost is that building
  one and testing the other passes *silently* against an old compiler. `./dev` exists to make that
  unrepresentable; if you bypass it, rebuild for the platform you are about to test on.
- **The layout, which is the one kama teaches** (`kama seed` gives a project the same shape):
  `src/` compiler sources · `include/` the runtime headers that SHIP (generated C includes them; an
  install puts them in `<prefix>/include`) · `lib/std/` the stdlib · `out/<os>-<arch>/` every build
  artifact · `.scratch/` gitignored, for throwaway language probes and local benchmark logs. The root
  holds docs, config, `./dev`, `run_tests.sh`, and `./kama`, a symlink to whichever platform built
  last. Anything that must get the *native* binary regardless (the test harness, the
  `tools/check-*.sh` guards, the VS Code extension) resolves `out/<os>-<arch>/kama` directly — in a
  shell script, source `tools/kama-bin.sh` rather than hardcoding a path.
- **Nothing a build generates belongs in the worktree.** `kama build` writes only into `dirname(-o)`,
  and `run_tests.sh` gives every fixture its own dir under a `mktemp -d`. `tools/check-clean-tree.sh`
  holds that down. If a stray artifact ever appears, find what wrote it — **do not add a pattern to
  `.gitignore`**. That file used to carry ~30 lines of blanket globs and `!` rescues for exactly this,
  and one of the rescues was missing, so a hand-written `examples/httpd/public/index.html` sat
  untracked and invisible for months.
- The grammar needs bison ≥ 2.7 (the container has 3.8; macOS host needs `brew install bison`).
- The compiler is a tree-walking C emitter (`kama.cemit.*`) over the Flex/Bison/AST front end
  (`kama.l`, `kama.y`, `kama.ast.h`). (An early LLVM backend was removed; C emission is the only backend.)
- Each language feature lands as a milestone with `tests/` fixtures verifying exit codes on native + wasm.
- A `tools/check-*.sh` is **glob-enrolled** by `tools/run-checks.sh`, which drives the guards for both
  `./dev check` and `run_tests.sh`, so a new guard joins the gate with no list to update. A new negative
  claim in the docs wants a `tests/xfail/` fixture in the same commit.
- The guards run **in parallel**, so a guard must: work in a private `mktemp -d`, never write into the
  worktree, never `cd` outside a subshell, and reach the compiler through `$KAMA` (the runner exports an
  absolute one) rather than the `./kama` symlink — which `check-no-inheritance.sh` repoints while it
  builds. A guard that cannot honor that says `# check-heavy: yes` in its own header and is then run
  alone. The other marker is `# check-legs: native san` (default `native`) for a guard that must also run
  on the sanitizer or wasm leg. Both live in the guard, not in a list, so enrollment stays automatic.

## Where things are written down

`docs/ROADMAP.md` is a **plan, not a changelog**, and it is **the ordered list only** — one row per
item, a one-line summary, and a link. **It is the authority on what to do next; read it first and you
should not need anything else to pick up work.** The reasoning behind each row lives in
`docs/ROADMAP_DETAIL.md`, one section per topic. Keep prose out of ROADMAP.md — that is what took the
single-file version to 1,279 lines and made "what is next" unanswerable without reading all of it;
`tools/check-roadmap.sh` now holds the split down (line ceiling, every row resolves, no orphaned
section). Delete a shipped item from **both** files once its record lands in the right place
(`docs/SPEC.md` for language surface, `docs/packages.md` / `docs/editors.md` / `docs/targets.md` for
workflow, the git log for *why*).

**Cite a row by its `KR-<n>` id.** The id is permanent — assigned once, never reused, never renumbered —
so it is safe to write in a commit message, a note or another doc, and deleting a shipped row leaves a
gap rather than shifting anything. A new row takes one more than the highest id present. (Rows used to be
numbered by position, which silently re-pointed every `row N` in prose each time one was deleted, and made
"find the row by its TEXT, never its number" a standing instruction to every reader. That instruction is
retired: the id is now the reliable handle.)

`agents/AGENTS.md` is **not** this file: it is the snippet kama *ships* to user projects, and it
must stay free of this repo's own working preferences. `docs/agents.md` documents that surface.
