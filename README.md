# kama

> **The memory-safe, no-GC systems language that keeps traditional OOP — and compiles to
> readable, portable C.**

**A C-family language with C#-like syntax, no garbage collector, and no runtime — it
transpiles to portable C.** So it runs anywhere C runs: native on every platform, and in
the browser as WebAssembly.

> **TL;DR** — kama gives you modern ergonomics (generics, sum types + exhaustive `match`,
> operator overloading, RAII, smart pointers) over a **deterministic, no-GC** memory model,
> and compiles to readable C you can build and debug like any C program. **Status: the
> language is feature-complete (v0.1.75), on the road to 1.0.**
>
> ```sh
> tools/cdev make                       # build the compiler (containerized toolchain)
> tools/cdev exec ./kama build hello.kama -o hello && ./hello   # native
> tools/cdev exec ./kama build hello.kama --target wasm         # -> hello.html + .js + .wasm
> ```
> New here? Start with **[GETTING_STARTED.md](GETTING_STARTED.md)**.

## What makes kama kama

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

## Why kama over…

- **…C** — everything C targets, plus safety by default: no `null`, bounds-checked indexing,
  RAII, and compile-time use-after-move — without leaving C. The output *is* readable C, so kama
  drops into an existing C codebase one file at a time.
- **…C++** — one obvious construct per idea instead of five, safety on by default instead of
  opt-in, no exception overhead, smaller binaries — and portable-C output that slots into
  toolchains (certified, embedded, legacy) where modern C++ can't go. Most of C++'s expressive
  OOP, minus the footguns and the decades of syntax accretion.
- **…C# / interpreted languages** — the same OOP model and ergonomics with **no GC**
  (deterministic latency, no stop-the-world) and **no runtime** (tiny deploys, real embedded,
  wasm without a multi-MB runtime). AOT-native speed, and it runs where a managed runtime never
  will.
- **…Rust** — kama keeps the memory-safety wins that bite in practice — no dangling, no
  use-after-free, no use-after-move, no `null`, no leaks, bounds checks — at a fraction of the
  cognitive cost, and hands back the **traditional OOP toolkit** (single inheritance + virtual
  dispatch, contracts for substitutability) that Rust declines to provide. The trade is
  deliberate: kama does **not** statically enforce Rust's aliasing-exclusivity or data-race
  freedom today. *(The planned concurrency model is shared-nothing — data-race freedom by
  construction rather than by a borrow checker; see the [roadmap](docs/ROADMAP.md). The 1.0 core
  is single-threaded.)* It's a different point on the safety/effort curve — aimed at the
  OOP-shaped, borrow-checker-weary middle — not a superset of Rust.

kama's white space is the combination no one else occupies: **traditional OOP + no-GC/RAII +
transpiles to portable C.** Rust and Zig drop OOP; C# and Swift carry a GC/runtime; C++ keeps the
footguns. That triangle is the reason to reach for kama.

## Toolchain

The build toolchain is **containerized** for reproducibility — same result under **podman**
(preferred) or **docker**. The image is the official Emscripten SDK (emcc + node) plus
bison/flex/clang for building the compiler itself.

```sh
tools/cdev build-image      # one-time: build the kama-dev toolchain image
tools/cdev make             # build the kama compiler
tools/cdev test             # run the end-to-end test suite
tools/cdev exec <cmd...>    # run any command in the toolchain
tools/cdev sh               # interactive shell in the toolchain
```

Override the engine with `KAMA_ENGINE=docker`. A host-native build also works with bison
≥ 2.7, flex, and clang (`brew install bison` on macOS — the system bison is too old); just
run `make`.

## Using the compiler

```sh
# Transpile kama to C (no C compiler invoked):
kama transpile hello.kama -o hello.c

# Build a native executable:
kama build hello.kama -o hello && ./hello

# Build a multi-file program (files with no `namespace` are file-private):
kama build graphics.kama physics.kama main.kama -o app && ./app

# Build for the browser (WASM). Default output is an HTML harness:
kama build hello.kama --target wasm            # -> hello.html + .js + .wasm
kama build hello.kama --target wasm -o app.js  # -> app.js + app.wasm (headless: node app.js)

# Optimized release build (stripped, NDEBUG, no #line):
kama build hello.kama --release
```

Options: `--release`/`--debug` (default debug: `-g -O0`), `--target native|wasm`, `--webgpu`
(link Emscripten's WebGPU port), `--cc <compiler>`, `--no-line` (omit `#line`), `--keep-c`.

## Debugging

Generated C carries `#line` directives back to the original `.kama`, so a native `-g` build
is debuggable in lldb/gdb with breakpoints in your `.kama` source, and a WASM
`-g -gsource-map` build steps through `.kama` in browser devtools. A VSCode extension
(`editor/vscode/`) ships a CodeLLDB launch config and Build tasks.

## Layout

- `kama.l`, `kama.y` — Flex lexer and Bison grammar (the language front end)
- `kama.ast.h`, `kama.forward.h` — AST
- `kama.cemit.{h,cpp}` — the C-emitting backend
- `kama_runtime.h` — the small runtime included by generated C
- `kama.driver.cpp` — CLI (`transpile` / `build`)
- `tests/`, `run_tests.sh` — end-to-end fixtures (assert on exit codes)
- `Dockerfile`, `tools/cdev` — containerized toolchain
- `docs/` — `SPEC.md` (language reference), `grammar.bnf` (generated from `kama.y`),
  `TYPE_MODEL.md`, `KEYWORDS.md`, `ROADMAP.md`
- `llms.txt`, `GOALS.md` — LLM-discovery entry point and design philosophy

MIT licensed — see [LICENSE](LICENSE).
