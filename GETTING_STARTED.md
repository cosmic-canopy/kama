# Getting started with cstar

cstar is a small C-family language (no GC, named parameters) that **transpiles to portable C** and
compiles to a native binary or to WebAssembly. This is the quickest path from zero to a running,
debuggable `.cstar` program.

## 1. Install

Prebuilt packages are published for **Linux** (x64 + arm64), **macOS** (one universal binary for Intel +
Apple Silicon), and **Windows** (x64). On **\*BSD** (FreeBSD etc.) build from source — it's the same
`flex`/`bison`/`clang` toolchain (`pkg install`) and the standard `make` below.

**From a release** (recommended): download the package for your platform from the
[Releases](https://github.com/cosmic-canopy/cstar/releases) page, then:

```sh
tar xzf cstar-<os>-<arch>-vX.Y.Z.tar.gz
cd cstar-<os>-<arch>-vX.Y.Z
./install.sh /usr/local         # or any prefix on your PATH
cstar --version
```

The package ships `bin/cstar` and `include/cstar_runtime.h`; `cstar` finds the runtime header relative to
its own location, so it works from any directory.

**From source:** you need `flex`, `bison ≥ 2.7`, and `clang`. On macOS: `brew install bison flex`
(the Makefile auto-detects the keg-only bison). Then:

```sh
make
./cstar --version
```

> You also need a host **C compiler** (`clang`) on your PATH — cstar emits C and hands it to clang. For
> the WebAssembly target you need **Emscripten** (`emcc`).

## 2. Write a program

`hello.cstar`:

```cstar
fn int add(int a, int b) { return a + b; }

fn int main()
{
    int x = add(a: 40, b: 2);   // named arguments
    return x;                    // exit code
}
```

## 3. Build & run

```sh
cstar build hello.cstar -o hello    # debug build (default)
./hello; echo $?                    # -> 42

cstar build hello.cstar --release   # optimized, stripped, dead-code pruned
cstar build hello.cstar --target wasm -o hello.js   # WebAssembly
node hello.js; echo $?              # -> 42  (browser: use the .html target)
```

`cstar transpile hello.cstar -o hello.c` emits the generated C if you want to read it.

## 4. Debug in VSCode (breakpoints, call stack, locals)

cstar debug builds embed `#line` directives back to your `.cstar` and keep your variable names, so you get
real source-level debugging.

1. Install the **cstar** VSCode extension (the `.vsix` from Releases, or `editor/vscode/` from source).
   It pulls in **CodeLLDB** (`vadimcn.vscode-lldb`).
2. Copy the repo's [`.vscode/`](.vscode) (tasks + launch config) into your project, or create your own.
3. Open a `.cstar`, set a breakpoint, press **F5**.
4. Execution stops **in your `.cstar` source**; the Variables panel shows your locals and the Call Stack
   shows cstar frames.

### Debug in the browser (WebAssembly)

The same source-level debugging works for `--target wasm`. A debug wasm build emits **DWARF** plus a
`.wasm.map` source map that reference your `.cstar` (via the `#line` directives):

```sh
cstar build app.cstar --target wasm -o app.html   # debug is the default; emits app.{html,js,wasm,wasm.map}
```

1. In **Chrome/Edge**, install the **C/C++ DevTools Support (DWARF)** extension (`ms-vscode.wasm-dwarf-debugging`
   in DevTools' extension list).
2. Serve the output over HTTP (DevTools needs the `.cstar` reachable next to the artifacts), e.g.
   `python3 -m http.server` in the output directory, and open `app.html`.
3. Open **DevTools → Sources**: your `.cstar` appears in the tree. Set a breakpoint in it, reload, and
   execution stops **in the `.cstar`** with the call stack and `Scope` variables by their cstar names.

> Release wasm builds (`--release`) strip DWARF/source-map and optimize, so debug in the default build and
> ship the release one.

## Next

- Language reference: [docs/SPEC.md](docs/SPEC.md) · grammar: [docs/grammar.bnf](docs/grammar.bnf)
- Examples: [tests/](tests/) (each `.cstar` is a runnable program)
- Design goals: [GOALS.md](GOALS.md)
