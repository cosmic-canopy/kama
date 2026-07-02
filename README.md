# cstar

A small C-family language: C#-like syntax, **no garbage collector** (RAII /
deterministic destruction), and **explicit named parameters**. cstar
**transpiles to portable C**, so it runs anywhere C runs — native on every
platform, and in the browser as WebAssembly via Emscripten.

Goal: a portable, lightweight WebGPU game engine with no .NET/runtime baggage.

> Status: usable (v0.1.45). Functions, control flow, named-parameter calls, the
> **ownership type model** (`type value` copies / `type resource` moves / `type
> contract` = the polymorphic guarantee, each on the greppable `type` marker),
> **RAII** (no GC), single inheritance + **virtual dispatch**, **contracts**,
> **enums**, **generic collections** (`Array<T>`/`List<T>`/`String`, with
> `foreach` + bounds-checked `[]`), the full **smart-pointer family**
> (`Owned<T>` unique, `Shared<T>` ref-counted, `Weak<T>` non-owning) with
> **auto-deref** and the **`give`/`copy` ownership model** (by-value transfer;
> **no null** in the safe surface), **`const`-correctness**, **enforced access
> control** (private-by-default; `public`/`protected`/`friend`; per-field `public`
> on a `value`), **multi-file builds with private-by-default namespaces**, and **C FFI**
> (`extern` functions + structs, `#include`, `Ptr`/`usize`, out-params, `--link`,
> raw memory access confined to an explicit `unsafe { }` block, and `fnptr`
> callbacks) — building for **native** and **WASM**, with debug/release builds and
> **IDE breakpoint debugging**. No raw pointers / no `unsafe` outside the explicit
> FFI seam. On the road to 1.0: user-defined generics (`Map<K,V>`),
> `match`/`Optional`, and operator overloading. New? See
> **[GETTING_STARTED.md](GETTING_STARTED.md)**.

## Toolchain

The build toolchain is **containerized** for reproducibility and portability —
it works the same under **podman** (preferred) or **docker**. The image is based
on the official Emscripten SDK (emcc + node) plus bison/flex/clang for building
the compiler itself.

```sh
tools/cdev build-image      # one-time: build the cstar-dev toolchain image
tools/cdev make             # build the cstar compiler
tools/cdev test             # run the native end-to-end test suite
tools/cdev sh               # interactive shell in the toolchain
tools/cdev exec <cmd...>    # run any command in the toolchain
```

Override the engine with `CSTAR_ENGINE=docker` if you prefer docker.

A host-native build also works if you have bison ≥ 2.7, flex, and clang
(`brew install bison` on macOS — the system bison 2.3 is too old). Just run
`make`.

## Using the compiler

```sh
# Transpile cstar to C (no compiler invoked):
cstar transpile tests/arith.cstar -o arith.c

# Build a native executable:
cstar build tests/arith.cstar -o arith && ./arith

# Build a multi-file program (namespaces; files with no `namespace` are private):
cstar build graphics.cstar physics.cstar main.cstar -o app && ./app

# Build for the browser (WASM). Default output is an HTML harness:
cstar build tests/arith.cstar --target wasm          # -> arith.html + .js + .wasm
cstar build tests/arith.cstar --target wasm -o app.js # -> app.js + app.wasm (headless: `node app.js`)

# Optimized release build (stripped, NDEBUG, no #line):
cstar build tests/arith.cstar --release
```

Options: `--release`/`--debug` (default debug: `-g -O0`, debuggable), `--target native|wasm`,
`--webgpu` (link Emscripten's WebGPU port), `--cc <compiler>`, `--no-line` (omit `#line`), `--keep-c`.

### Debugging

Generated C carries `#line` directives back to the original `.cstar`, so a
native `-g` build is debuggable in lldb/gdb with breakpoints in your `.cstar`
source, and a WASM `-g -gsource-map` build steps through `.cstar` in browser
devtools.

## WebGPU

The container's Emscripten ships the `emdawnwebgpu` WebGPU port. A toolchain
smoke test lives in `tests/webgpu/` (hand-written C for now — cstar-level WebGPU
bindings need pointer/extern support, a later milestone):

```sh
tools/cdev exec tests/webgpu/build.sh   # compiles+links a WebGPU WASM module
```

## Layout

- `cstar.l`, `cstar.y` — Flex lexer and Bison grammar (the language front end)
- `cstar.ast.h`, `cstar.ast.cpp`, `cstar.forward.h` — AST
- `cstar.context.h` — parse-time context (source position + errors)
- `cstar.cemit.{h,cpp}` — the C-emitting backend
- `cstar_runtime.h` — minimal runtime included by generated C
- `cstar.driver.cpp` — CLI (`transpile` / `build`)
- `tests/`, `run_tests.sh` — end-to-end fixtures (assert on exit codes)
- `Dockerfile`, `tools/cdev` — containerized toolchain
- `docs/` — `SPEC.md` (semantics) + `grammar.bnf` (generated from `cstar.y` by `tools/gen-grammar`)
- `llms.txt`, `GOALS.md` — LLM-discovery entry point and design philosophy
