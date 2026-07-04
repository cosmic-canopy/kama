# cstar

**A C-family language with C#-like syntax, no garbage collector, and no runtime — it
transpiles to portable C.** So it runs anywhere C runs: native on every platform, and in
the browser as WebAssembly.

> **TL;DR** — cstar gives you modern ergonomics (generics, sum types + exhaustive `match`,
> operator overloading, RAII, smart pointers) over a **deterministic, no-GC** memory model,
> and compiles to readable C you can build and debug like any C program. **Status: the
> language is feature-complete (v0.1.75), on the road to 1.0.**
>
> ```sh
> tools/cdev make                       # build the compiler (containerized toolchain)
> tools/cdev exec ./cstar build hello.cstar -o hello && ./hello   # native
> tools/cdev exec ./cstar build hello.cstar --target wasm         # -> hello.html + .js + .wasm
> ```
> New here? Start with **[GETTING_STARTED.md](GETTING_STARTED.md)**.

## What makes cstar cstar

- **No GC, deterministic lifetimes (RAII).** Destruction is scope-driven; allocation is
  explicit in the generated C. No stop-the-world, no hidden runtime.
- **Memory-safe by construction, without a borrow checker.** No raw pointers or `null` in
  the safe surface — heap and buffers are reached only through **smart pointers**
  (`Owned`/`Shared`/`Weak`) and **collections** (`Array`/`List`/`string`), all bounds-checked
  and RAII-managed. Use-after-move is a compile error. Raw pointers live only inside an
  explicit, greppable `unsafe { }` block at the C/FFI seam.
- **Ownership is the type axis.** Every type is a `type value` (owns nothing, copies), a
  `type resource` (owns/has identity, moves), or a `type contract` (an interface). One
  greppable `type` marker, parallel to `fn`.
- **Modern, explicit surface.** Named parameters only, monomorphized generics (with
  turbofish `f::<T>()`), tagged unions with exhaustive `match` (no `switch`, no fallthrough),
  operator overloading, `Optional`/`Result` instead of exceptions or `null`.
- **Explicit over implicit.** Named args, explicit `ref`/`out`, explicit casts, a keyword or
  operator on every declaration and dangerous operation — readable at a glance by a human, a
  tool, or an LLM.

The full language reference lives in **[docs/SPEC.md](docs/SPEC.md)**; the design philosophy
in **[GOALS.md](GOALS.md)**; the forward plan in **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Toolchain

The build toolchain is **containerized** for reproducibility — same result under **podman**
(preferred) or **docker**. The image is the official Emscripten SDK (emcc + node) plus
bison/flex/clang for building the compiler itself.

```sh
tools/cdev build-image      # one-time: build the cstar-dev toolchain image
tools/cdev make             # build the cstar compiler
tools/cdev test             # run the end-to-end test suite
tools/cdev exec <cmd...>    # run any command in the toolchain
tools/cdev sh               # interactive shell in the toolchain
```

Override the engine with `CSTAR_ENGINE=docker`. A host-native build also works with bison
≥ 2.7, flex, and clang (`brew install bison` on macOS — the system bison is too old); just
run `make`.

## Using the compiler

```sh
# Transpile cstar to C (no C compiler invoked):
cstar transpile hello.cstar -o hello.c

# Build a native executable:
cstar build hello.cstar -o hello && ./hello

# Build a multi-file program (files with no `namespace` are file-private):
cstar build graphics.cstar physics.cstar main.cstar -o app && ./app

# Build for the browser (WASM). Default output is an HTML harness:
cstar build hello.cstar --target wasm            # -> hello.html + .js + .wasm
cstar build hello.cstar --target wasm -o app.js  # -> app.js + app.wasm (headless: node app.js)

# Optimized release build (stripped, NDEBUG, no #line):
cstar build hello.cstar --release
```

Options: `--release`/`--debug` (default debug: `-g -O0`), `--target native|wasm`, `--webgpu`
(link Emscripten's WebGPU port), `--cc <compiler>`, `--no-line` (omit `#line`), `--keep-c`.

## Debugging

Generated C carries `#line` directives back to the original `.cstar`, so a native `-g` build
is debuggable in lldb/gdb with breakpoints in your `.cstar` source, and a WASM
`-g -gsource-map` build steps through `.cstar` in browser devtools. A VSCode extension
(`editor/vscode/`) ships a CodeLLDB launch config and Build tasks.

## Layout

- `cstar.l`, `cstar.y` — Flex lexer and Bison grammar (the language front end)
- `cstar.ast.h`, `cstar.forward.h` — AST
- `cstar.cemit.{h,cpp}` — the C-emitting backend
- `cstar_runtime.h` — the small runtime included by generated C
- `cstar.driver.cpp` — CLI (`transpile` / `build`)
- `tests/`, `run_tests.sh` — end-to-end fixtures (assert on exit codes)
- `Dockerfile`, `tools/cdev` — containerized toolchain
- `docs/` — `SPEC.md` (language reference), `grammar.bnf` (generated from `cstar.y`),
  `TYPE_MODEL.md`, `KEYWORDS.md`, `ROADMAP.md`
- `llms.txt`, `GOALS.md` — LLM-discovery entry point and design philosophy

MIT licensed — see [LICENSE](LICENSE).
