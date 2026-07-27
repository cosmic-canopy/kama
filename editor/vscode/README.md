# kama — VSCode extension

Syntax highlighting, bracket/comment support, breakpoint debugging, and live
diagnostics (a language server) for the [kama](../../README.md) language (`.kama`).

## Install

From this folder (install the JS deps first — the language client is an npm dep):

```sh
npm install
# symlink into your VSCode extensions (dev install)
ln -s "$(pwd)" ~/.vscode/extensions/kama-0.1.0
# then reload VSCode
```

Or package + install (`vsce package` bundles `node_modules`, so run `npm install` first):

```sh
npm install
npm i -g @vscode/vsce
vsce package
code --install-extension kama-0.1.0.vsix
```

## Live diagnostics (language server)

On opening a `.kama`, the extension starts the kama language server (`kama lsp`,
a JSON-RPC server over stdio built into the compiler) and shows errors/warnings
inline as you type — no build required. It uses the same `kama` binary as the
debugger (workspace-local `./kama` if present, else `PATH`; build it with `make`).
Hover, go-to-definition, and completion arrive in later milestones — the same
server gains them and this client needs no change.

## Debugging `.kama` (breakpoints, call stack, locals)

kama compiles to C with `#line` directives back to your `.kama`, and locals keep
their kama names — so a debug build is breakpoint-debuggable like any native
program. This extension uses **[CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb)**
(`vadimcn.vscode-lldb`, installed automatically as an extension pack member).

1. Open a kama project in VSCode (the repo ships a `.vscode/` with the tasks +
   launch config — see [`/.vscode`](../../.vscode)).
2. Open a `.kama` file and set a breakpoint in the gutter.
3. Press **F5** (or run *"kama: debug current file"*). The **Build Debug** task
   runs `kama build ${file} -o … ` (default debug: `-g -O0`), then CodeLLDB
   launches the binary.
4. Execution stops at your breakpoint **in the `.kama` source**; the Variables
   panel shows locals/params and the Call Stack shows kama frames.

Notes:
- Object fields appear as `self->field` and `this` as `self` (the C lowering);
  fully inspectable. Prettier formatters are a future nicety.
- `kama` must be on your `PATH` (build it with `make`, then symlink/copy to a
  `PATH` dir, or adjust the task's command).
- For the browser/WASM target, build with `--target wasm` and debug in the
  browser via the emitted source maps.
