# Getting started with kama

This is the quickest path from zero to a running, debuggable `.kama` program.

## 1. Install

Prebuilt packages are published for **Linux** (x64 + arm64), **macOS** (one universal binary for Intel +
Apple Silicon), and **Windows** (x64). On **\*BSD** (FreeBSD etc.) build from source — it's the same
`flex`/`bison`/`clang` toolchain (`pkg install`) and the standard `make` below.

**One-line install** (recommended) — installs into `~/.kama`, no admin:

```sh
# macOS / Linux
curl -fsSL https://kama-lang.org/install.sh | sh
```
```powershell
# Windows (PowerShell)
irm https://kama-lang.org/install.ps1 | iex
```

The installer detects your OS/arch and whether a C compiler is present: if so it grabs the small build;
if not, it grabs a self-contained build that bundles `zig cc`, so `kama build` works with nothing else
installed. Then add `~/.kama/bin` to your PATH (the installer prints the line) and:

```sh
kama --version
kama update            # self-update to the latest release (kama update --version vX.Y.Z to pin)
```

Flags: `--no-std` (or `KAMA_NO_STD=1`) skips the standard library; `KAMA_VERSION=vX.Y.Z` installs a
specific release.

**Manual install:** download the package for your platform from the
[Releases](https://github.com/cosmic-canopy/kama/releases) page, extract it, and add its `bin/` to your
PATH — `kama` finds its runtime header and stdlib relative to the binary, so it runs from any directory.
Packages are named `kama-<platform>-vX.Y.Z` — `linux-x64`, `linux-arm64`, `macos-universal`,
`windows-x64` — with a `-bundled` variant that carries its own C compiler:

```sh
tar xzf kama-linux-x64-vX.Y.Z.tar.gz -C ~/.kama --strip-components=1
export PATH="$HOME/.kama/bin:$PATH"
kama --version
```

**From source:** you need `flex`, `bison ≥ 2.7`, and `clang`. On macOS: `brew install bison flex`
(the Makefile auto-detects the keg-only bison). Then:

```sh
make
./kama --version
```

> kama emits C and hands it to a C compiler: `clang`, `gcc` or `cc` on your PATH, or the `zig cc` the
> bundled package carries. For the WebAssembly target you need **Emscripten** (`emcc`).

## 2. Write a program

`hello.kama`:

```kama
import { core::println };

fn int32 add(int32 a, int32 b) { return a + b; }

fn int32 main()
{
    string name = "kama";
    int32 x = add(a: 40, b: 2);        // every argument is named at the call site
    println(s: "hello from ${name}: ${x}");
    return x;                          // the exit code
}
```

`${…}` splices a value into the string as the program runs. The compiler turns the template into direct
formatting calls, so no format string is parsed at runtime. `println` comes from module `core`, and the
import line names it — a file uses only what it declares or imports. The
[tour](tour.md) covers the rest of the language in one read.

## 3. Build & run

```sh
kama build hello.kama -o hello    # debug build (default)
./hello; echo $?                    # prints "hello from kama: 42", then the exit code 42

kama build hello.kama --release   # optimized, stripped, dead-code pruned
kama build hello.kama --target wasm -o hello.js   # WebAssembly
node hello.js; echo $?              # -> 42  (browser: use the .html target)
```

`kama transpile hello.kama -o hello.c` emits the generated C if you want to read it.

## 4. Start a project

One file needs no ceremony. The moment you want a second one, a dependency, or a name your editor can
rename across files, you want a **project** — a directory with a `kama.json`:

```sh
kama seed myapp        # asks for a name, a version, a kind, and whether to write AGENTS.md
cd myapp
kama run kama.json     # builds the project's entry (src/app.kama) and runs it
```

`kama seed` writes the manifest, a starter source file, a `.gitignore` and a README stub. It prompts only
when it has a terminal, so `kama seed myapp --yes` (or any script, or CI) takes the defaults instead.
`--kind library` and `--kind monorepo` give you the other two shapes.

Build output lands under `out/<triple>/<debug|release>/`, which is the one line the generated
`.gitignore` needs. Dependencies, workspaces and the lockfile: **[packages quickstart](packages.md)**.

## 5. Set up your editor

The compiler is its own language server, so you get live diagnostics, hover, go-to-definition,
find-references, project-wide rename, completion, signature help, an outline, workspace symbol search and
an auto-import quick fix in any editor with an LSP client — VS Code, Neovim, Vim, Emacs, Sublime Text,
Helix, Kate and Zed. Each is a few lines pointing at `kama lsp`: see **[editor setup](editors.md)**.

VS Code has a packaged extension that also brings syntax highlighting, a build-configuration picker, and
the F5 debugging below.

## 6. Debug in VSCode (breakpoints, call stack, locals)

kama debug builds embed `#line` directives back to your `.kama`, and the extension puts your own names
and values back on top of them, so you get real source-level debugging: a `string` shows its text, an
`Optional` shows `Some(…)`/`None`, a container shows its elements, and locals and frames read as you
wrote them rather than in the C the compiler emitted.

1. Install the **kama** VSCode extension (the `.vsix` from Releases, or `editor/vscode/` from source).
   It auto-installs **CodeLLDB** (`vadimcn.vscode-lldb`) and wires up **F5** — no `launch.json`/`tasks.json`
   to copy.
2. Open a `.kama`, set a breakpoint, press **F5** — or run **"kama: Debug Current File"** from the
   command palette. The extension builds a debug binary and launches it.
   The ▶ button in the Run and Debug view is a different route: it runs a `launch.json`
   configuration and cannot invoke an extension command, so it only does anything once your project
   has one. It does not need a kama-specific key — a configuration in a project with a `kama.json`
   gets the same formatters and the same demangled names ([Editors](editors.md) § *Debugging*).
3. Execution stops **in your `.kama` source**; the Variables panel shows your locals and the Call Stack
   shows kama frames.

> Prefer to build from a task? The repo's [`.vscode/tasks.json`](../.vscode/tasks.json) has debug and
> release build tasks for the current file. There is deliberately no `launch.json` beside it: a launch
> configuration can only name a program, so it cannot load the value formatters or start the name layer,
> and a hand-written one is a strictly worse session that looks like the real thing.

### Debug in the browser (WebAssembly)

The same source-level debugging works for `--target wasm`. A debug wasm build emits **DWARF** plus a
`.wasm.map` source map that reference your `.kama` (via the `#line` directives):

```sh
kama build app.kama --target wasm -o app.html   # debug is the default; emits app.{html,js,wasm,wasm.map}
```

1. In **Chrome/Edge**, install the **C/C++ DevTools Support (DWARF)** extension (`ms-vscode.wasm-dwarf-debugging`
   in DevTools' extension list).
2. Serve the output over HTTP (DevTools needs the `.kama` reachable next to the artifacts), e.g.
   `python3 -m http.server` in the output directory, and open `app.html`.
3. Open **DevTools → Sources**: your `.kama` appears in the tree. Set a breakpoint in it, reload, and
   execution stops **in the `.kama`** with the call stack and `Scope` variables by their kama names.

> Release wasm builds (`--release`) strip DWARF/source-map and optimize, so debug in the default build and
> ship the release one.

## Next

- The language in one read: [the tour](tour.md)
- Language reference: [docs/SPEC.md](SPEC.md) · grammar: [docs/grammar.bnf](grammar.bnf)
- Projects, dependencies and publishing: [packages](packages.md)
- Editor setup: [docs/editors.md](editors.md)
- Examples: [tests/](../tests/) (each `.kama` is a runnable program)
- Design goals: [GOALS.md](GOALS.md)
