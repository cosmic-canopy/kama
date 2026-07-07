# Getting started with kama

This is the quickest path from zero to a running, debuggable `.kama` program.

## 1. Install

Prebuilt packages are published for **Linux** (x64 + arm64), **macOS** (one universal binary for Intel +
Apple Silicon), and **Windows** (x64). On **\*BSD** (FreeBSD etc.) build from source — it's the same
`flex`/`bison`/`clang` toolchain (`pkg install`) and the standard `make` below.

**From a release** (recommended): download the package for your platform from the
[Releases](https://github.com/cosmic-canopy/kama/releases) page, then:

```sh
tar xzf kama-<os>-<arch>-vX.Y.Z.tar.gz
cd kama-<os>-<arch>-vX.Y.Z
./install.sh /usr/local         # or any prefix on your PATH
kama --version
```

The package ships `bin/kama` and `include/kama_runtime.h`; `kama` finds the runtime header relative to
its own location, so it works from any directory.

**From source:** you need `flex`, `bison ≥ 2.7`, and `clang`. On macOS: `brew install bison flex`
(the Makefile auto-detects the keg-only bison). Then:

```sh
make
./kama --version
```

> You also need a host **C compiler** (`clang`) on your PATH — kama emits C and hands it to clang. For
> the WebAssembly target you need **Emscripten** (`emcc`).

## 2. Write a program

`hello.kama`:

```kama
fn int add(int a, int b) { return a + b; }

fn int main()
{
    int x = add(a: 40, b: 2);   // named arguments
    return x;                    // exit code
}
```

## 3. Build & run

```sh
kama build hello.kama -o hello    # debug build (default)
./hello; echo $?                    # -> 42

kama build hello.kama --release   # optimized, stripped, dead-code pruned
kama build hello.kama --target wasm -o hello.js   # WebAssembly
node hello.js; echo $?              # -> 42  (browser: use the .html target)
```

`kama transpile hello.kama -o hello.c` emits the generated C if you want to read it.

## 4. Debug in VSCode (breakpoints, call stack, locals)

kama debug builds embed `#line` directives back to your `.kama` and keep your variable names, so you get
real source-level debugging.

1. Install the **kama** VSCode extension (the `.vsix` from Releases, or `editor/vscode/` from source).
   It auto-installs **CodeLLDB** (`vadimcn.vscode-lldb`) and wires up **F5** — no `launch.json`/`tasks.json`
   to copy.
2. Open a `.kama`, set a breakpoint, press **F5**. The extension builds a debug binary and launches it.
3. Execution stops **in your `.kama` source**; the Variables panel shows your locals and the Call Stack
   shows kama frames.

> Prefer to wire it yourself? The repo's [`.vscode/`](.vscode) has an equivalent `tasks.json` + `launch.json`.

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

- Language reference: [docs/SPEC.md](docs/SPEC.md) · grammar: [docs/grammar.bnf](docs/grammar.bnf)
- Examples: [tests/](tests/) (each `.kama` is a runnable program)
- Design goals: [GOALS.md](GOALS.md)
