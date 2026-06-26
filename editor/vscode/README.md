# cstar — VSCode extension

Syntax highlighting, bracket/comment support, and breakpoint debugging for the
[cstar](../../README.md) language (`.cstar`).

## Install

From this folder:

```sh
# symlink into your VSCode extensions (dev install)
ln -s "$(pwd)" ~/.vscode/extensions/cstar-0.1.0
# then reload VSCode
```

Or package + install:

```sh
npm i -g @vscode/vsce
vsce package
code --install-extension cstar-0.1.0.vsix
```

## Debugging `.cstar` (breakpoints, call stack, locals)

cstar compiles to C with `#line` directives back to your `.cstar`, and locals keep
their cstar names — so a debug build is breakpoint-debuggable like any native
program. This extension uses **[CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb)**
(`vadimcn.vscode-lldb`, installed automatically as an extension pack member).

1. Open a cstar project in VSCode (the repo ships a `.vscode/` with the tasks +
   launch config — see [`/.vscode`](../../.vscode)).
2. Open a `.cstar` file and set a breakpoint in the gutter.
3. Press **F5** (or run *"cstar: debug current file"*). The **Build Debug** task
   runs `cstar build ${file} -o … ` (default debug: `-g -O0`), then CodeLLDB
   launches the binary.
4. Execution stops at your breakpoint **in the `.cstar` source**; the Variables
   panel shows locals/params and the Call Stack shows cstar frames.

Notes:
- Object fields appear as `self->field` and `this` as `self` (the C lowering);
  fully inspectable. Prettier formatters are a future nicety.
- `cstar` must be on your `PATH` (build it with `make`, then symlink/copy to a
  `PATH` dir, or adjust the task's command).
- For the browser/WASM target, build with `--target wasm` and debug in the
  browser via the emitted source maps.
