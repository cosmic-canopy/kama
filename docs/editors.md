# Editors

kama ships a language server in the compiler itself. `kama lsp` speaks JSON-RPC over stdio and takes no
arguments, so wiring up an editor is a few lines pointing at it — **one server, thin clients**. Every
editor below gets the same features, because they all read the same server.

VS Code additionally has a packaged extension ([`editor/vscode/`](../editor/vscode/README.md)) with syntax
highlighting, F5 debugging and a build-configuration picker. Everything else on this page is configuration
you paste into your own dotfiles.

The extension also gives `.kama` its own **file icon** in the Explorer, in VS Code and VSCodium alike. It is
contributed as a *language* icon (`contributes.languages[].icon`), not as a file icon *theme*, and that
distinction is the whole design: a theme would replace whichever icon set you already run, whereas a
language icon is a fallback the active theme uses only where it has no entry of its own for `.kama` — which
Seti, the default in both editors, does not have. A theme can decline them with
`"showLanguageModeIcons": false`; Seti does not set it, so the icon appears out of the box. One asset serves
light and dark themes because the artwork is red ink and the pick is negative space, so it takes on the
Explorer's own background rather than carrying a field of its own.

**The supported set is these eight editors** — VS Code, Neovim, Vim (coc.nvim), Emacs (eglot), Sublime Text,
Helix, Kate and Zed. That list is the commitment, and `tools/check-editors.sh` fails the build if any of them
loses its section here: dropping an editor has to be a deliberate diff, never a silence.

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
| auto-import quick fix on an unimported name | `textDocument/codeAction` (`quickfix`) |

Rename refuses a symbol the project does not own (a `std` or dependency declaration), and renaming a
parameter also rewrites its argument labels at every call site.

**Auto-import.** Every name a file uses that it does not declare needs an `import`, including a sibling
in the same module, so the editor writes it for you: put the cursor on the error and the lightbulb offers
one fix per module that exports the name — the bare spelling for a sibling (`import { Widget };`), the
qualified one otherwise (`import { std::collections::DynamicArray };`). A file has exactly one
`import { … };` block, so the fix either starts it or adds one entry to it.

**The diagnostic is still the answer, and that is on purpose.** The message itself names the exact line
to paste, so nothing above is required to find out what to write — it works over a pipe, in CI, and in an
editor with no kama support at all. The quick fix saves the typing; it is not the only way to learn the
fix.

## Syntax colouring differs per editor — read this before filing a bug

LSP features are identical everywhere. **Colouring is not**, because it comes from three different places
and not every editor has all three:

| editor | LSP features | syntax colour |
|---|---|---|
| **VS Code** | all | TextMate grammar (shipped in the extension) **+ semantic tokens** |
| **Sublime Text** | all | the same TextMate grammar — see below |
| **Neovim** | all | semantic tokens **+ tree-sitter** (optional, via nvim-treesitter) |
| **Vim** (coc.nvim) | all | semantic tokens **+ tree-sitter** (optional, via nvim-treesitter) |
| **Emacs** (eglot) | all | semantic tokens only |
| **Kate** | all | semantic tokens only |
| **Helix** | all | tree-sitter grammar **+ semantic tokens** |
| **Zed** | all | tree-sitter grammar (via the extension in `editor/zed/`) |

Each section below says whether its snippet was **verified against a running editor** or is documented from
the editor's own configuration reference. We would rather tell you which is which than imply we tested
everything.

Semantic tokens colour what the *resolver* concluded — types, fields, locals, parameters, enum members —
which is strictly more accurate than a regex grammar, but it only covers identifiers. Keywords, strings,
numbers and comments come from the editor's own grammar, so in a semantic-tokens-only editor those stay
uncoloured.

**Where the grammar lives.** The tree-sitter grammar is `tree-sitter-kama/` in this repository, beside the
compiler it has to agree with rather than in a repository of its own — Helix, Zed and nvim-treesitter can
all consume a grammar from a subdirectory, and keeping it here is what lets `tools/check-treesitter.sh` fail
the build the moment `kama.l` gains a keyword the grammar lacks. `queries/` holds the canonical highlight
queries in the Helix/nvim vocabulary; Zed's flatter vocabulary needs its own copies, which live in
`editor/zed/languages/kama/`. The generated parser under `src/` is committed, because every consumer
compiles `parser.c` rather than running the grammar generator.

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

Helix colours **only** from tree-sitter, so add the grammar too — this is what the `[[grammar]]` block is
for. The grammar lives in a subdirectory of the compiler repo, which is what `subpath` is for:

```toml
[[grammar]]
name = "kama"
source = { git = "https://github.com/cosmic-canopy/kama", rev = "29c90fa9c450e4d0d978fbcd3fbae5b89411dead", subpath = "tree-sitter-kama" }
```

If you are working on the grammar itself, point it at your checkout instead — a local path needs no git, no
commit and no network, which makes it the fast iteration loop:

```toml
[[grammar]]
name = "kama"
source = { path = "/absolute/path/to/kama/tree-sitter-kama" }
```

Then build the parser and install the queries:

```sh
hx --grammar build
mkdir -p ~/.config/helix/runtime/queries/kama
cp /path/to/kama/tree-sitter-kama/queries/*.scm ~/.config/helix/runtime/queries/kama/
```

`hx --health kama` should now show **five** ✓ — the language server, the tree-sitter parser, and the
highlight, textobject and indent queries. `hx --grammar build` reports failures for every *other* grammar
it has not fetched; that is normal and unrelated.

The exact block above is kept as a real file at
[`editor/helix/languages.toml`](../editor/helix/languages.toml) so it cannot rot silently.

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

*Documented from Zed's extension reference — **not** verified against a running Zed.* The extension's Rust
component is compile-verified against `zed_extension_api` (it builds clean for `wasm32-wasip2`), the grammar
and its queries are checked by `tools/check-treesitter.sh`, and `tools/check-editors.sh` asserts that this
page and `editor/zed/src/kama.rs` agree about how the server is launched. What has *not* been done is
loading it into a running Zed, because `zed: install dev extension` is a GUI action. If you run it, please
report back so this line can change.

Zed is the only editor here that cannot be configured from a settings file. A Zed extension registers a
*language*, which requires a tree-sitter grammar, and pointing that language at a language server requires a
small WebAssembly component — `[language_servers.…]` in `extension.toml` is metadata only. Both live in
[`editor/zed/`](../editor/zed/).

**Prerequisite:** [rustup](https://rustup.rs). Zed runs the build itself and downloads the wasi-sdk it needs;
you never invoke cargo. End users installing from Zed's extension registry get a prebuilt `.wasm` and need
nothing at all.

1. Open the command palette and run **`zed: install dev extension`**.
2. Choose the `editor/zed/` directory.

That gives you syntax colouring, the outline view, bracket matching and indentation from the grammar, plus
diagnostics, hover, go-to-definition, find-references, rename, completion, signature help, semantic
tokens and the auto-import quick fix from `kama lsp`.

The server is launched by `editor/zed/src/kama.rs`, which resolves the binary through the worktree's `PATH`:

```rust
Ok(Command {
    command: worktree.which("kama")?,
    args: vec!["lsp".to_string()],
    env: worktree.shell_env(),
})
```

Resolving through `PATH` is deliberate rather than lazy: `~/.kama/bin/kama` is a *selector* that reads the
project's pinned toolchain and re-execs, so hardcoding a versioned binary would defeat the per-project pin.
To override it anyway, set `lsp.kama.binary.path` in your Zed settings.

> ⚠️ **Zed fetches the grammar with git, and does not read your working tree.** It runs
> `git fetch --depth 1 origin <rev>` against the `repository` in `extension.toml`, so a grammar change is
> invisible to Zed until it is **committed** and `rev` is updated to a commit the remote will serve. Iterate
> on the grammar with `tree-sitter test` and Helix — whose local `source = { path = … }` needs no commit —
> and come back to Zed once it is settled.
