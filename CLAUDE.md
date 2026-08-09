# kama — repo guide for AI assistants

**Read [GOALS.md](docs/GOALS.md) first** — the language's design philosophy. Honor it, especially:
*favor one way to do a thing, favor simplicity, favor explicit over implicit.*

## Working style

This repo vendors two guidance skills under `.claude/skills/` — apply them:
- **karpathy-guidelines**: think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail**: write the least code that works (YAGNI ladder; deletion over addition).

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
