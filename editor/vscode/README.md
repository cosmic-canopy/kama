# kama for VS Code

Syntax highlighting, a language server (live diagnostics, go-to-definition, rename, completion)
and zero-config breakpoint debugging for the [kama](https://kama-lang.org) language (`.kama`).

## Requirements

This extension drives the **kama compiler** — install it first:

```sh
curl -fsSL https://kama-lang.org/install.sh | sh          # macOS / Linux
irm https://kama-lang.org/install.ps1 | iex                # Windows (PowerShell)
```

The extension finds it via **`kama.path`** if you set it, else a workspace-local
`out/<os>-<arch>/kama` or `./kama`, else your `PATH`.

> **The binary must be native to your OS.** The extension runs `kama lsp` as a normal host
> process, so a container-built Linux `./kama` sitting in a macOS checkout will not launch.

[CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb) is installed
automatically as an extension-pack member — it is what actually runs the debugger.

## What you get

On opening a `.kama` the extension starts `kama lsp` — a JSON-RPC server built into the compiler —
and gives you, as you type and with **no build required**:

- **live diagnostics**, including semantic errors, not just parse errors
- **hover** (kind + name) and **go-to-definition** (F12)
- **find-references** (Shift-F12) and **rename** (F2), which refuses symbols the project does not
  own; renaming a parameter also rewrites its argument labels
- **completion** and **signature help**
- the **document outline** (Ctrl-Shift-O / breadcrumbs) and **workspace symbols** (Ctrl-T)
- **semantic highlighting** layered over the TextMate grammar — the resolver knows which names are
  types, fields, locals or parameters, which a regex cannot

## Debugging (breakpoints, call stack, locals)

kama compiles to C with `#line` directives back to your `.kama`, so a debug build is
breakpoint-debuggable like any native program — and what you inspect reads as kama, not as the C.

1. Open a `.kama` file and set a breakpoint in the gutter.
2. Press **F5** (or run *"kama: Debug Current File"*). The build task runs
   `kama build ${file} -o …` (debug default: `-g -O0`), then CodeLLDB launches it.
3. Execution stops **in the `.kama` source**; Variables shows locals and params, and the Call Stack
   shows kama frames.

Values are rendered by the formatters that ship with the compiler, so a `string` shows its text, an
`Optional` shows `Some(…)` or `None`, a `DynamicArray` or `Map` shows its elements, and a `Shared`
shows `strong=N weak=M` above its pointee. A struct's fields read as you declared them. Names —
locals, the call stack, watch expressions — are put back into kama spelling by the extension, because
those come from the debug info rather than from a value.

**Debugging a project.** F5 debugs the open file. A project with its own `launch.json` needs nothing
added — if it has a `kama.json`, its configurations get the same formatters and the same demangled
names automatically:

```jsonc
{ "type": "lldb", "request": "launch", "program": "${workspaceFolder}/build/app" }
```

Use `"kama": "src/main.kama"` to point at the program explicitly when the manifest is somewhere
detection cannot find it, or `"kama": false` to opt a configuration out.

Outside VS Code, `kama demangle --lldb-init` prints the one line that loads the value formatters into
any `lldb` — which also gives a terminal `bt` readable frame names — and `kama demangle <file> -- <name>` turns a single C name (from a crash log, or a
`--keep-c` compiler error) back into its kama spelling.

For the browser target, build with `--target wasm` and debug in the browser via the emitted source maps.

## Build configuration (the status bar)

The server analyzes the program a plain `kama build` builds in that project — the resolved target's
derived flags, `BUILD_TYPE=DEBUG`, and the manifest's default flags — so the editor and the compiler
cannot disagree about which `@compileFor` declarations exist.

The **status bar** (bottom right, on a `.kama`) shows what it resolved — `⚙ HOST · DEBUG` — with the
manifest, triple and active flags in its tooltip. Click it, or run *"kama: Select Build
Configuration"*, to switch any single-select group the project declares: `TARGET`, `BUILD_TYPE`,
`OUTPUT`, and any group of your own.

The picker **writes `kama.local.json`** (the gitignored sibling of `kama.json`) rather than an editor
setting. That is deliberate: it is the same file `kama build` merges, so the editor, the CLI and F5
debugging cannot get out of step, and a Neovim user overrides configuration exactly the way you do.
Saving it re-analyzes every open buffer — no restart.

> **One configuration per server process**, pinned by the first file that resolved a manifest. Open a
> second project in the same window and it is analyzed under the first one's flags — the status-bar
> tooltip warns when the current file is outside the pinned project. Fix it with *"kama: Restart
> Language Server"*, or use one window per project.

## Settings

| setting | what it does |
|---|---|
| `kama.path` | Path to the kama compiler. Empty = search the workspace, then `PATH`. |
| `kama.trace.server` | `off` \| `messages` \| `verbose` — log JSON-RPC traffic to the output channel. |

There is deliberately **no** setting mirroring the build configuration — see above.

## Commands

| command | default key |
|---|---|
| kama: Debug Current File | <kbd>F5</kbd> |
| kama: Select Build Configuration | — |
| kama: Restart Language Server | — |

## Using a different editor?

The same server, `kama lsp`, serves Neovim, Vim, Emacs, Sublime Text, Helix, Kate and Zed —
see **[Editor setup](https://kama-lang.org/docs/editors/)**.

## Links

- [kama-lang.org](https://kama-lang.org) · [Getting started](https://kama-lang.org/docs/getting-started/)
  · [Language tour](https://kama-lang.org/docs/tour/) · [Specification](https://kama-lang.org/docs/spec/)
- [Source](https://github.com/cosmic-canopy/kama) · [Issues](https://github.com/cosmic-canopy/kama/issues)

## Contributing

Building this extension from a checkout of the kama repo — the language client is an npm dependency,
so install it first:

```sh
cd editor/vscode
npm install
ln -s "$(pwd)" ~/.vscode/extensions/kama-dev      # dev install, then reload VS Code
```

Or package and install it:

```sh
./dev ext-package                                  # from the repo root
code --install-extension editor/vscode/kama-*.vsix
```

MIT licensed. See [LICENSE](https://github.com/cosmic-canopy/kama/blob/main/editor/vscode/LICENSE).
