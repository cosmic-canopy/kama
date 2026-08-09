# kama

> **The memory-safe, no-GC systems language that keeps traditional OOP — and compiles to
> readable, portable C.**

**A C-family language with C#-like syntax, no garbage collector, and no runtime beside the compiled
binary — it transpiles to portable C.** So it runs anywhere C runs: native on every platform, and in
the browser as WebAssembly.

> **TL;DR** — kama gives you modern ergonomics (generics, sum types + exhaustive `match`,
> operator overloading, RAII, smart pointers) over a **deterministic, no-GC** memory model,
> and compiles to readable C you can build and debug like any C program. **Status: the
> language is feature-complete, on the road to 1.0** (see [VERSION](VERSION)).
>
> ```sh
> tools/cdev make                       # build the compiler (containerized toolchain)
> tools/cdev exec ./kama build hello.kama -o hello && ./hello   # native
> tools/cdev exec ./kama build hello.kama --target wasm         # -> hello.html + .js + .wasm
> ```
> New here? Start with **[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)**.

## What makes kama kama

- **No GC, deterministic lifetimes (RAII).** Destruction is scope-driven; allocation is
  explicit in the generated C. No stop-the-world, no hidden runtime.
- **Memory-safe by construction, without a borrow checker.** No raw pointers or `null` in
  the safe surface — heap and buffers are reached only through **smart pointers**
  (`Owned`/`Shared`/`Weak`) and **collections** (`FixedArray`/`DynamicArray`/`string`), all bounds-checked
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
in **[docs/GOALS.md](docs/GOALS.md)**; the forward plan in **[docs/ROADMAP.md](docs/ROADMAP.md)**.

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
  deliberate: kama does **not** statically enforce Rust's aliasing-exclusivity. *(Concurrency is
  shared-nothing and shipped — isolates, typed channels, structured `scope`, `Atomic<T>` and
  `parallel_for`, on native and wasm — so data races are prevented by construction rather than by
  a borrow checker, and there is no `async` colouring; see [Concurrency](docs/SPEC.md#concurrency-).)*
  It's a different point on the safety/effort curve — aimed at the OOP-shaped,
  borrow-checker-weary middle — not a superset of Rust.

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

# Build the project entry (from kama.json "main") and run it in one step:
kama run                      # native-only; `kama run <file>` also works
```

Options: `--release`/`--debug` (default debug: `-g -O0`), `--target <name-or-triple>`
(`HOST` (default), `MACOS`, `WINDOWS`, `LINUX`, `WASM`, `EMBEDDED`, or an `<arch>-<os>-<abi>` triple —
cross-compiling works with `--cc "zig cc"`), `--select GROUP=VALUE` (build-configuration groups, e.g.
`OUTPUT=STATIC` for a static library), `--define NAME` (a `@compileFor` flag), `--webgpu`
(link Emscripten's WebGPU port), `--cc <compiler>`, `--no-line` (omit `#line`), `--keep-c`.
Targets, cross-compilation and toolchain setup: **[targets & toolchains](docs/targets.md)**.

For multi-file / dependency projects — a `kama.json` manifest, `kama pkg add`/`install`,
the lockfile, and `kama run` — see the **[packages quickstart](docs/packages.md)**.

## Debugging

Generated C carries `#line` directives back to the original `.kama`, so a native `-g` build
is debuggable in lldb/gdb with breakpoints in your `.kama` source, and a WASM
`-g -gsource-map` build steps through `.kama` in browser devtools. A VSCode extension
(`editor/vscode/`) ships a CodeLLDB launch config and Build tasks.

## Editor support

The compiler is its own language server: `kama lsp` serves diagnostics, hover, go-to-definition,
find-references, project-wide rename, completion, signature help and semantic highlighting. VS Code has a
packaged extension; Neovim, Vim, Emacs, Sublime Text, Helix and Kate need a few lines of config —
see **[editor setup](docs/editors.md)**.

## Layout

- `kama.l`, `kama.y` — Flex lexer and Bison grammar (the language front end)
- `kama.ast.h`, `kama.forward.h` — AST
- `kama.cemit.{h,cpp}` — the C-emitting backend
- `kama_runtime.h` — the small runtime included by generated C
- `kama_os.h` — cross-platform OS/IO bindings header (POSIX + Windows), pulled in only by `std::io`/`std::fs`/`std::net`
- `lib/std/` — the self-hosted standard library (`memory`, `collections`, `io`, `fs`, `net`)
- `kama.driver.cpp` — CLI (`transpile` / `build`)
- `tests/`, `run_tests.sh` — end-to-end fixtures (assert on exit codes)
- `examples/` — worked programs (e.g. `httpd/`, a static-file server in Kama)
- `Dockerfile`, `tools/cdev` — containerized toolchain
- `./dev serve` — preview the site locally, served by the Kama-written `examples/httpd` (dogfooding; → http://localhost:8080)
- `docs/` — `SPEC.md` (language reference), `tour.md` (the guided introduction),
  `grammar.bnf` (generated from `kama.y`), `TYPE_MODEL.md`, `KEYWORDS.md`, `FLOOR.md`,
  `ROADMAP.md` (what's next), `editors.md`, `packages.md`, `targets.md`, `mcu.md`,
  `*_READINESS.md` (per-domain gap analyses), `benchmarks/`, and `design/` (in-flight design
  notes — deleted once the work ships and its record lands in the docs above)
- `site/`, `tools/site/` — kama-lang.org: static assets plus the generator that renders the
  docs above into the published site (`./dev site`, `./dev serve`)
- `llms.txt`, `docs/GOALS.md` — LLM-discovery entry point and design philosophy

MIT licensed — see [LICENSE](LICENSE).
