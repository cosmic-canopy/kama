# cstar — repo guide for AI assistants

**Read [GOALS.md](GOALS.md) first** — the language's design philosophy. Honor it, especially:
*favor one way to do a thing, favor simplicity, favor explicit over implicit.*

## Working style

This repo vendors two guidance skills under `.claude/skills/` — apply them:
- **karpathy-guidelines**: think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail**: write the least code that works (YAGNI ladder; deletion over addition).

## Build & test

The toolchain is containerized (podman/docker). Don't install host toolchains; use the wrapper:

```sh
tools/cdev make      # build the cstar compiler (into build/)
tools/cdev test      # run the end-to-end fixtures (tests/*.cstar + .expect, exit-code asserted)
tools/cdev exec ./cstar build tests/arith.cstar            # native
tools/cdev exec ./cstar build tests/arith.cstar --target wasm   # -> .html + .js + .wasm
```

Notes:
- All build artifacts (objects + generated parser/lexer) go in `build/`. The repo root holds only
  hand-written sources. Run `make clean` when switching between host and container builds.
- The grammar needs bison ≥ 2.7 (the container has 3.8; macOS host needs `brew install bison`).
- The compiler is a tree-walking C emitter (`cstar.cemit.*`) over the Flex/Bison/AST front end
  (`cstar.l`, `cstar.y`, `cstar.ast.h`). The old LLVM backend is parked in `legacy-llvm/` (not built).
- Each language feature lands as a milestone with `tests/` fixtures verifying exit codes on native + wasm.
