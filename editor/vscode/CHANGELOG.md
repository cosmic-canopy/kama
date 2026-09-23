# Changelog

Notable changes to the **kama for VS Code** extension. This file is the Changelog tab on the
[VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=cosmic-canopy.kama) and on
[Open VSX](https://open-vsx.org/extension/cosmic-canopy/kama), so it is written for people deciding
whether to install, not for contributors.

The extension version is **independent of the kama compiler's** `VERSION`: the two ship on separate
clocks, because bumping the extension on every compiler patch would push an auto-update to everyone for
a change that never touched the editor. Compiler changes are in the
[repository's history](https://github.com/cosmic-canopy/kama/commits/main).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions are
[Semantic Versioning](https://semver.org/) — the Marketplace requires a strict `major.minor.patch`.

## [0.3.12] — 2026-09-23

**First public release.** Versions before this one were development-only and were never published to
either marketplace, so everything the extension does is listed here rather than split across entries
nobody could install.

### Language support

- **Syntax highlighting** for `.kama` via a TextMate grammar, with a **semantic highlighting** layer
  over it — the compiler's resolver decides which names are types, fields, locals or parameters, which
  a regular expression cannot.
- **Live diagnostics** as you type, with no build step. These are the compiler's own errors, semantic
  ones included, not a separate linter that can disagree with it.
- **Hover**, **go-to-definition** (F12), **find-references** (Shift-F12) and **rename** (F2). Rename
  refuses symbols the project does not own, and renaming a parameter also rewrites its argument labels.
- **Completion** and **signature help**.
- **Document outline** (Ctrl-Shift-O, breadcrumbs) and **workspace symbols** (Ctrl-T).
- **Auto-import quick fix** (Ctrl-.) on a name you have not imported yet.

All of it is served by `kama lsp`, a JSON-RPC server built into the compiler — the same server behind
the Neovim, Vim, Emacs, Sublime Text, Helix, Kate and Zed setups.

### Debugging

- **Zero-config breakpoint debugging.** Set a breakpoint in a `.kama` and press F5 — no `launch.json`
  or `tasks.json` to write. kama emits C with `#line` directives back to your source.
- Values and frames read **as kama**, not as the generated C: `string`, collections, `Optional` and
  smart pointers inspect as themselves, and stack frames carry kama names.
- [CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb) ships as an
  extension-pack member and is what runs the debugger.

### Commands and settings

- Commands: **Debug Current File**, **Select Build Configuration**, **Restart Language Server**.
- Settings: `kama.path` (the compiler to use) and `kama.trace.server` (LSP tracing).
- Compiler discovery, in order: `kama.path`, then a workspace-local `out/<os>-<arch>/kama` or `./kama`,
  then `PATH`.

### Requires

The kama compiler, installed separately — see
[kama-lang.org](https://kama-lang.org). The binary must be native to your OS: the extension runs
`kama lsp` as an ordinary host process, so a container-built Linux `./kama` in a macOS checkout will
not launch.

[0.3.12]: https://github.com/cosmic-canopy/kama/releases
