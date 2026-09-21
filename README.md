<p align="center">
  <img src="site/assets/logo.png" alt="kama" width="160" height="160">
</p>

<h1 align="center">kama</h1>

<p align="center">
  <b>鎌 · Japanese farming tool, and weapon</b><br>
  The memory-safe, no-GC systems language that keeps traditional OOP —
  and compiles to readable, portable C.
</p>

<p align="center">
  <a href="https://kama-lang.org"><b>kama-lang.org</b></a> ·
  <a href="https://kama-lang.org/docs/getting-started/">Getting started</a> ·
  <a href="https://kama-lang.org/docs/tour/">Tour</a> ·
  <a href="https://kama-lang.org/docs/spec/">Spec</a>
</p>

## Install

```sh
curl -fsSL https://kama-lang.org/install.sh | sh     # macOS / Linux
```

```powershell
irm https://kama-lang.org/install.ps1 | iex          # Windows (PowerShell)
```

Then:

```sh
kama build hello.kama -o hello && ./hello     # native
kama build hello.kama --target wasm           # -> hello.html + .js + .wasm
```

**A C-family language with C#-like syntax, no garbage collector, and no runtime beside the compiled
binary — it transpiles to portable C.** So it runs anywhere C runs: native on every platform, and in
the browser as WebAssembly.

> **TL;DR** — kama gives you modern ergonomics (generics, sum types + exhaustive `match`,
> operator overloading, RAII, smart pointers) over a **deterministic, no-GC** memory model,
> and compiles to readable C you can build and debug like any C program. **Status: the
> language is feature-complete, on the road to 1.0** (see [VERSION](VERSION)).
>
> New here? Start with **[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)**.
> Building the compiler itself instead? See **[Toolchain](#toolchain)** below.

## What makes kama kama

- **No GC, deterministic lifetimes (RAII).** Destruction is scope-driven; allocation is
  explicit in the generated C. No stop-the-world, no hidden runtime.
- **Memory-safe by construction, without a borrow checker.** No raw pointers or `null` in
  the safe surface — heap and buffers are reached only through **smart pointers**
  (`Owned`/`Shared`/`Weak`) and **collections** (`FixedArray`/`DynamicArray`/`string`), all bounds-checked
  and RAII-managed. Use-after-move is a compile error. Raw pointers live only inside an
  explicit, greppable `unsafe fn` at the C/FFI seam.
- **Ownership is the type axis.** Every type declares its kind: `type value` (owns nothing,
  copies), `type resource` (owns or has identity, moves), `type view` (borrows, stack-only),
  `type enum` (a sum type), `type contract` (an interface), or `type intrinsic` (gives a built-in
  a contract). One greppable `type` marker, parallel to `fn`.
- **Opt-in, compile-time serialization.** `@generate(Serializable, Deserializable)` and a mark on
  every field — JSON and three binary formats, object graphs included, no runtime reflection.
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

Building the compiler needs bison ≥ 2.7, flex and clang (`brew install bison` on macOS — the
system bison is too old). `./dev` is the one entry point; every test task builds the binary it is
about to test:

```sh
./dev build          # build the compiler for this host
./dev test           # the native fixture suite
./dev test wasm      # the wasm/node suite (in the container on macOS)
./dev matrix         # every leg plus every guard — the pre-commit gate
./dev help           # the full list
```

The sanitizer and wasm legs run in a container — **podman** (preferred) or **docker**
(`KAMA_ENGINE=docker`) — built from the official Emscripten SDK image plus bison/flex/clang.

## Using the compiler

```sh
# Start a project (manifest, starter source, .gitignore, README). Interactive on a terminal;
# a script or CI gets the defaults. --kind library|monorepo for the other shapes:
kama seed myapp && cd myapp && kama run kama.json

# Transpile kama to C (no C compiler invoked):
kama transpile hello.kama -o hello.c

# Build a native executable:
kama build hello.kama -o hello && ./hello

# Build several loose files as one program (each file's `export { … }` is its public surface):
kama build graphics.kama physics.kama main.kama -o app && ./app

# Build for the browser (WASM). Default output is an HTML harness:
kama build hello.kama --target wasm            # -> hello.html + .js + .wasm
kama build hello.kama --target wasm -o app.js  # -> app.js + app.wasm (headless: node app.js)

# Optimized release build (stripped, NDEBUG, no #line):
kama build hello.kama --release

# Build the project entry (from kama.json "entry") and run it in one step:
kama run kama.json            # native-only; `kama run <file>` also works

# Type-check without invoking a C compiler (what an editor or CI wants):
kama check kama.json
```

Options: `-o <path>`, `-j <jobs>`, `--release`/`--debug` (default debug: `-g -O0`), `--target <name-or-triple>`
(`HOST` (default), `MACOS`, `WINDOWS`, `LINUX`, `WASM`, `EMBEDDED`, or an `<arch>-<os>-<abi>` triple —
cross-compiling works with `--cc "zig cc"`), `--select GROUP=VALUE` (build-configuration groups, e.g.
`OUTPUT=STATIC` for a static library), `--define NAME` (a `@compileFor` flag), `--webgpu`
(link Emscripten's WebGPU port), `--cc`/`--cxx <compiler>`, `--link <lib>`, `--shared`, `--no-heap`
(prove the program never allocates), `--dev` (dev-dependencies on the import path), `--no-line` (omit
`#line`), `--keep-c`, `--no-cache`.
Targets, cross-compilation and toolchain setup: **[targets & toolchains](docs/targets.md)**.

For multi-file / dependency projects — `kama seed`, the `kama.json` manifest, `kama pkg add`/`install`,
the lockfile, and `kama run` — see the **[packages quickstart](docs/packages.md)**. Inside a project,
build output collects under `out/<triple>/<debug|release>/` rather than beside your sources.

## Debugging

Generated C carries `#line` directives back to the original `.kama`, so a native debug build
is debuggable in lldb/gdb with breakpoints in your `.kama` source, and a debug wasm build steps
through `.kama` in browser devtools. The VS Code extension (`editor/vscode/`) adds F5 through
CodeLLDB with kama's own names and values in the Variables panel, and `kama demangle` turns a
generated symbol from a crash log back into its kama name.

## Editor support

The compiler is its own language server: `kama lsp` serves diagnostics, hover, go-to-definition,
find-references, project-wide rename, completion, signature help, semantic highlighting, an outline,
workspace symbols and an auto-import quick fix. VS Code has a packaged extension; Neovim, Vim, Emacs,
Sublime Text, Helix, Kate and Zed need a few lines of config — see **[editor setup](docs/editors.md)**.

## Layout

- `src/` — the compiler itself (C++):
  - `kama.l`, `kama.y` — Flex lexer and Bison grammar (the language front end)
  - `kama.ast.h`, `kama.forward.h` — AST
  - `kama.cemit.{h,cpp}` — the C-emitting backend
  - `kama.comptime.cpp` — the compile-time evaluator
  - `kama.lsp.cpp`, `kama.query.cpp` — the language server and `kama query`
  - `kama.driver.cpp` — the CLI: `build`, `run`, `check`, `transpile`, `seed`, `pkg`, `publish`,
    `toolchain`, `update`, `query`, `lsp`, `demangle`, `stats`, `agents`
- `include/` — the headers that SHIP: generated C `#include`s them, and an install puts them in
  `<prefix>/include`. Not compiler sources — a different audience entirely.
  - `kama_runtime.h` — the small runtime included by generated C
  - `kama_os.h` — cross-platform OS/IO bindings (POSIX + Windows), pulled in only by the modules that use it
  - one header per runtime seam a module needs (`kama_log.h`, `kama_time.h`, `kama_channel.h`, …)
- `prelude/` — the always-in-scope floor, embedded in the binary ([FLOOR.md](docs/FLOOR.md))
- `lib/std/` — the self-hosted standard library: `app`, `ascii`, `collections`, `concurrent`, `digest`,
  `encoding`, `fmt`, `fs`, `gpu`, `io`, `log`, `math`, `memory`, `net`, `num`, `path`, `process`,
  `ptr`, `random`, `serialization`, `time`, `uuid`
- `out/` — every build artifact, under `out/<os>-<arch>/` so a host and a container build coexist.
  The same `out/` a `kama seed` project gets, which is the point ([targets.md](docs/targets.md)).
- `.scratch/` — gitignored, for throwaway language probes and local benchmark logs
- `tests/`, `run_tests.sh` — end-to-end fixtures (assert on exit codes); `tools/check-*.sh` — the guards
- `examples/` — worked programs (e.g. `httpd/`, a static-file server in Kama)
- `bench/` — the cross-language benchmarks behind [docs/benchmarks](docs/benchmarks/RESULTS.md)
- `editor/`, `tree-sitter-kama/` — the VS Code extension, per-editor configs, and the grammar
- `seed/`, `agents/` — what `kama seed` and `kama agents` write, embedded in the binary
- `mcu/` — bare-metal startup and board support ([mcu.md](docs/mcu.md))
- `Dockerfile`, `tools/cdev`, `./dev` — the build entry point and its container
- `./dev serve` — preview the site locally, served by the Kama-written `examples/httpd` (dogfooding; → http://localhost:8080)
- `docs/` — `SPEC.md` (language reference), `tour.md` (the guided introduction),
  `grammar.bnf` (generated from `src/kama.y`), `TYPE_MODEL.md`, `KEYWORDS.md`, `FLOOR.md`,
  `ROADMAP.md` (what's next), `editors.md`, `packages.md`, `targets.md`, `mcu.md`,
  `*_READINESS.md` (per-domain gap analyses), and `benchmarks/`
- `site/`, `tools/site/` — kama-lang.org: static assets plus the generator that renders the
  docs above into the published site (`./dev site`, `./dev serve`)
- `llms.txt`, `docs/GOALS.md` — LLM-discovery entry point and design philosophy
- `docs/agents.md`, `agents/` — the AI-agent surface (`kama query`) and the `AGENTS.md` kama ships

MIT licensed — see [LICENSE](LICENSE).
