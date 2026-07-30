# Editors

kama ships a language server in the compiler itself. `kama lsp` speaks JSON-RPC over stdio and takes no
arguments, so wiring up an editor is a few lines pointing at it — **one server, thin clients**. Every
editor below gets the same features, because they all read the same server.

VS Code additionally has a packaged extension ([`editor/vscode/`](../editor/vscode/README.md)) with syntax
highlighting, F5 debugging and a build-configuration picker. Everything else on this page is configuration
you paste into your own dotfiles.

## What you get

Advertised by `kama lsp` today, in every editor:

| feature | LSP method |
|---|---|
| live diagnostics as you type (semantic, not just parse errors) | `textDocument/publishDiagnostics` |
| hover — kind and type | `textDocument/hover` |
| go to definition | `textDocument/definition` |
| find references | `textDocument/references` |
| rename, project-wide | `textDocument/rename` (+ `prepareRename`) |
| document outline / breadcrumbs | `textDocument/documentSymbol` |
| project-wide symbol search | `workspace/symbol` |
| completion (triggers: `.` and `::`), including argument labels | `textDocument/completion` |
| signature help (triggers: `(` and `,`) | `textDocument/signatureHelp` |
| semantic highlighting | `textDocument/semanticTokens/full` |

Rename refuses a symbol the project does not own (a `std` or dependency declaration), and renaming a
parameter also rewrites its argument labels at every call site.

## Syntax colouring differs per editor — read this before filing a bug

LSP features are identical everywhere. **Colouring is not**, because it comes from three different places
and not every editor has all three:

| editor | LSP features | syntax colour |
|---|---|---|
| **VS Code** | all | TextMate grammar (shipped in the extension) **+ semantic tokens** |
| **Sublime Text** | all | the same TextMate grammar — see below |
| **Neovim** | all | semantic tokens only |
| **Vim** (coc.nvim) | all | semantic tokens only |
| **Emacs** (eglot) | all | semantic tokens only |
| **Kate** | all | semantic tokens only |
| **Helix** | all | **none yet** — Helix colours only from tree-sitter |
| **Zed** | — | **no extension possible yet** — a Zed extension needs a tree-sitter grammar |

Each section below says whether its snippet was **verified against a running editor** or is documented from
the editor's own configuration reference. We would rather tell you which is which than imply we tested
everything.

Semantic tokens colour what the *resolver* concluded — types, fields, locals, parameters, enum members —
which is strictly more accurate than a regex grammar, but it only covers identifiers. Keywords, strings,
numbers and comments come from the editor's own grammar, so in a semantic-tokens-only editor those stay
uncoloured.

**A tree-sitter grammar is the next milestone (M7)**; it is what unlocks Helix and Zed, and full colouring
in Neovim. Until then the table above is the honest state.

## Getting the compiler on `PATH`

Every snippet below runs the bare command `kama`. If you installed via the toolchain manager, that is
already on your `PATH` (`~/.kama/bin/kama` selects the right version per project). Otherwise use an
absolute path in the snippet.

> **The binary must be native to your OS.** The editor runs `kama lsp` as an ordinary host process, so a
> container-built binary (e.g. a Linux `./kama` produced by `tools/cdev make` on macOS) will not launch.

## Build configuration — the same file in every editor

The server analyzes the program a plain `kama build` in that project builds: the resolved target's derived
flags, the `BUILD_TYPE`, and the manifest's default flags. So the editor and the compiler agree about which
`@compileFor` declarations exist.

To override it, edit **`kama.local.json`**, the gitignored sibling of `kama.json`
([packages.md](packages.md#local-overrides--kamalocaljson)):

```jsonc
{ "select": { "TARGET": { "RPI": { "default": true } },
              "BUILD_TYPE": { "RELEASE": { "default": true } } } }
```

There is deliberately no editor setting for this, in any editor. One mechanism means the editor, the CLI
and a debug launch cannot disagree. VS Code's status-bar picker just writes this file for you.

Two things to know:

- **The server must be told when that file changes.** It asks your editor to watch `**/*.kama`,
  `**/kama.json` and `**/kama.local.json` when the editor supports it (see the note under Neovim).
- **One configuration per server process**, pinned by the first file that resolved a manifest. Open a
  second project in the same window and it is analyzed under the first one's flags. Declare a shared flag
  universe in the root manifest, or use one window per project.

---

## Neovim

*Verified against Neovim 0.12.4 — attaches, registers all three file watchers, and answers hover,
definition, references, rename, outline and semantic tokens.*

Requires Neovim 0.11+ for `vim.lsp.config`. Put this in `init.lua`:

```lua
vim.filetype.add({ extension = { kama = 'kama' } })

vim.lsp.config['kama'] = {
  cmd = { 'kama', 'lsp' },
  filetypes = { 'kama' },
  root_markers = { 'kama.json', '.git' },
}
vim.lsp.enable('kama')
```

On older Neovim, via [`nvim-lspconfig`](https://github.com/neovim/nvim-lspconfig), the same three fields go
into `require('lspconfig.configs').kama = { default_config = { … } }` followed by
`require('lspconfig').kama.setup{}`.

> **File watching is off on Linux and BSD** — not our choice: Neovim advertises
> `didChangeWatchedFiles.dynamicRegistration` as `false` there on purpose, because its watcher backends are
> too limited ([`vim/lsp/protocol.lua`](https://github.com/neovim/neovim/blob/master/runtime/lua/vim/lsp/protocol.lua)).
> The server honours that and does not register. The practical cost: editing `kama.json` or
> `kama.local.json` does not re-resolve the configuration until you restart the client (`:LspRestart`).
> On macOS and Windows it works.

## Vim (coc.nvim)

*Documented from coc.nvim's `languageserver` reference — **not** verified against a running Vim.*

In `:CocConfig` (`coc-settings.json`):

```json
{
  "languageserver": {
    "kama": {
      "command": "kama",
      "args": ["lsp"],
      "filetypes": ["kama"],
      "rootPatterns": ["kama.json", ".git"]
    }
  }
}
```

And teach Vim the filetype, in `~/.vim/ftdetect/kama.vim` or your `vimrc`:

```vim
autocmd BufRead,BufNewFile *.kama set filetype=kama
```

## Emacs (eglot)

*Verified against GNU Emacs 30.2 — connects, registers the file watcher, and answers hover, definition and
outline.*

eglot is built in from Emacs 29. In your init file:

```elisp
(add-to-list 'auto-mode-alist '("\\.kama\\'" . prog-mode))
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(kama-mode . ("kama" "lsp"))))
```

If you use a dedicated major mode rather than `prog-mode`, name it in both places. `M-x eglot` in a `.kama`
buffer starts the server; `eglot-ensure` in a mode hook makes it automatic.

lsp-mode users: register with `lsp-register-client` and `(lsp-stdio-connection '("kama" "lsp"))`.

## Sublime Text

*Documented from the LSP package's client reference — **not** verified end-to-end against a running
Sublime. Installing the LSP package from a bare git clone did not bring the client up here; install it
through Package Control instead, which is the supported route.*

Install the **LSP** package (via Package Control), then in *Preferences → Package Settings → LSP →
Settings*:

```json
{
  "clients": {
    "kama": {
      "enabled": true,
      "command": ["kama", "lsp"],
      "selector": "source.kama"
    }
  }
}
```

**Colouring: reuse the TextMate grammar, do not rewrite it.** Sublime loads TextMate grammars as XML
plists, not as JSON, so the shipped file needs a format conversion — but only a format one, since the two
are the same structure. On macOS that is one command:

```sh
ST=~/Library/Application\ Support/Sublime\ Text/Packages/User
plutil -convert xml1 editor/vscode/syntaxes/kama.tmLanguage.json -o "$ST/kama.tmLanguage"
# ⚠️ Required. The grammar carries no `fileTypes`, because VS Code takes file associations from the
# extension's package.json instead. Sublime has no such second source, so without this the `.kama`
# buffer never gets the `source.kama` scope — and the LSP `selector` above then never matches, which
# looks exactly like a broken server rather than an unassigned syntax.
plutil -insert fileTypes -json '["kama"]' "$ST/kama.tmLanguage"
```

(Elsewhere, any plist converter or Sublime's own PackageDev will do it.) This is the same grammar VS Code
uses — audited rule-by-rule against `kama.l`/`kama.y` with the compiler as the oracle, and guarded by
`tools/check-syntax.sh`, which runs the real TextMate engine over `tests/syntax/`. A second, hand-written
copy would drift from the language on its first change.

## Helix

*Verified against Helix 25.07.1 — `hx --health kama` resolves the server, and a real session initializes,
registers all three file watchers and publishes diagnostics.*

In `~/.config/helix/languages.toml`:

```toml
[language-server.kama]
command = "kama"
args = ["lsp"]

[[language]]
name = "kama"
scope = "source.kama"
file-types = ["kama"]
roots = ["kama.json"]
language-servers = ["kama"]
```

`hx --health kama` will show the language server as `✓` and *Highlight queries* as `✘`, and the log says
`Skipping syntax config for 'kama' because the parser's shared library does not exist`. That is expected —
see the table above; the buffer is uncoloured and every LSP feature still works.

## Kate

*Documented from Kate's LSP client reference — **not** verified against a running Kate.*

*Settings → Configure Kate → LSP Client → User Server Settings*:

```json
{
  "servers": {
    "kama": {
      "command": ["kama", "lsp"],
      "url": "https://kama-lang.org",
      "highlightingModeRegex": "^kama$",
      "rootIndicationFileNames": ["kama.json"]
    }
  }
}
```

Kate matches servers by its own highlighting mode name, so it needs a syntax-highlighting definition for
`.kama` to exist before `highlightingModeRegex` can match. **Documented but not verified** — unlike the
others on this page, this snippet has not been exercised against a running Kate.

## Zed

**Not yet possible.** A Zed extension registers a language, and Zed requires a tree-sitter grammar to do
that — there is no grammar-less "LSP only" extension. It arrives with M7 alongside Helix colouring. Until
then, use VS Code or one of the editors above.
