# LSP M6 Stage C — editor clients + `docs/editors.md` (cold-start brief)

**Status: NOT STARTED. Everything before it is done** — M0–M5, M6 Stage A, B1, B2 and B3 are all shipped
and green (`baee217`, dev, 2026-07-30). Stage C is the last build stage; Stage D is the campaign exit.

Read [lsp.md](lsp.md) for campaign context and [lsp-m6-kickoff.md](lsp-m6-kickoff.md) for the parent M6
brief (its § "Stage C/D — start here" is the seam map this expands). Server behaviour is settled; **Stage C
adds no server code.** One server, many thin clients — that is the whole point of the shape the campaign
chose, and it is now cashable.

⚠️ **Every prior milestone's brief was wrong somewhere load-bearing** — M4's in three places, M5's in three,
and B3's in one (it pointed the rename group at the conformance loops; the tables were the right source).
Assume this one is too. Where it says "verified", it was run; where it says "expect", it was not.

---

## What the server already provides

Advertised in `initialize` today, i.e. what any client gets for free by connecting:

```
textDocumentSync (full) · publishDiagnostics · hover · definition · documentSymbol
references · rename (+ prepareRename) · workspace/symbol · completion (trigger: . and ::)
signatureHelp (trigger: ( and ,) · semanticTokens/full · workspaceFolders
```

Plus `workspace/didChangeWatchedFiles`, which the client must feed (see the watcher note below).
`workspace/didChangeConfiguration` is **deliberately unhandled** — the override channel is `kama.local.json`
(Stage A, user decision), so configuration reaches the server as a watched FILE, not as a settings push.

Transport is `kama lsp` over stdio, one process. Nothing here needs per-client negotiation.

---

## The four pieces of Stage C

### C1 — VS Code: finish the client

The client works. What it lacks (verified against `editor/vscode/package.json` and `extension.js` at
`baee217`):

- **No `activationEvents`.** There is no such key at all. Add `["onLanguage:kama"]`. Today activation
  relies on the implicit `contributes.languages` activation, which is enough in practice but is not
  declared — and a status-bar item needs a defined activation point.
- **No `contributes.configuration`.** Nothing for a user to set. Keep it minimal; the build configuration
  lives in `kama.local.json` by design, so resist adding a settings mirror of it.
- **No `kama.selectBuildConfig` command, and no status-bar item.** The picker **writes `kama.local.json`**,
  which is what makes the editor and `kama build` agree by construction and needs no F5 change. Put the
  status-bar item in `activate()` and refresh it from `onDidChangeActiveTextEditor`.
  ⚠️ The server already ANNOUNCES the configuration it resolved, via `window/logMessage` — the line reads
  `config: … | target HOST (aarch64-macos-none) | BUILD_TYPE DEBUG | flags: …`. Read the state from there
  rather than re-deriving it client-side, or the status bar will eventually disagree with the analyzer.
- ⚠️ **Documented limit worth surfacing in the UI copy: ONE configuration per server process**, pinned from
  the first document that resolved a manifest (the parse cache holds units pruned in place). A long-running
  session that opens a second project analyzes it under the first one's flags. Stage A lost time to this
  presenting as a phantom editor/compiler disagreement. If the picker cannot restart the client, say so.

### C2 — the other clients

The parent brief names 7 more editors. The watcher is the only non-obvious part, and it is the SAME hook
everywhere:

⚠️ **The file watcher is registered CLIENT-side, deliberately.** The alternative is server-driven
registration via `client/registerCapability`, a server→client request this server has no machinery for.
VS Code does it through `synchronize.fileEvents`; every other LSP client offers the equivalent. It must
cover **three** globs, and the two manifests are separate for a different reason than the sources:

```
**/*.kama          — the program (drops the workspace index: M3.5 find-references / cross-file rename)
**/kama.json       — the build CONFIGURATION (changes which @compileFor decls exist: not a file in the
**/kama.local.json   program but the program itself; the server re-resolves and republishes)
```

⚠️ `**/kama.json` does **not** match `kama.local.json`. Spell both.

### C3 — `docs/editors.md` (does not exist yet)

The one thing it must get right is **honesty about where colour comes from**, because it differs per editor
and users will otherwise file bugs:

| editor | LSP features | syntax colour |
|---|---|---|
| VS Code | all | TextMate grammar (`editor/vscode/syntaxes/kama.tmLanguage.json`) + semantic tokens |
| Sublime | all | the SAME `.tmLanguage` — reuse it, do not fork it |
| Neovim, Emacs (eglot) | all | semantic tokens only |
| Helix, Zed | all | **none until M7's tree-sitter grammar** — say so plainly |

Register the new page in `README.md`'s layout bullet, `llms.txt` § Toolchain & runtime (which mentions
neither the LSP nor any editor today), `GETTING_STARTED.md` §4 (VS-Code-only today), and ROADMAP.

### C4 — a guard for the clients

`tools/check-lsp.sh` guards the SERVER and is not the right tool for client config. The cheap, honest guard
is a static one: assert that each client's watcher list covers all three globs and that each snippet names
`kama lsp` as the command. A drift check, not a behaviour test — a real client test needs each editor
installed, which is not worth it. Whatever you add, `run_tests.sh` must call it.

---

## Gates (campaign non-negotiables, unchanged)

Stage C should touch no compiler source, so most of these should be trivially green — run them anyway, and
if `lspref` ever moves, something touched the emitter that should not have.

```sh
make && ./run_tests.sh                                  # 803/803 before you start, and after
tools/lspref.sh > build/lspref-<tag>.txt
diff build/lspref-before.txt build/lspref-<tag>.txt     # byte-identical, 537 fixtures
sh tools/check-lsp.sh && sh tools/check-query.sh        # counts must GROW, never shrink: 136 / 227 today
tools/lsp-bench.sh --lsp                                # ~91 ms; the budget is 100 ms
```

⚠️ `build/` is gitignored — **regenerate `build/lspref-before.txt`, never assume it is there.**

⚠️ The sanitized-compiler run of both harnesses is a MANUAL step and the `KAMA_SAN=1` leg does NOT do it
(that leg sanitizes generated programs and gates check-lsp/check-query OFF). Run it on macOS **and in the
container** — macOS ASan has no LeakSanitizer, and this is the step that caught M4's out-of-bounds read:

```sh
make clean && make EXTRA_CXXFLAGS="-fsanitize=address,undefined"
sh tools/check-lsp.sh && sh tools/check-query.sh
tools/cdev exec make clean && tools/cdev exec make EXTRA_CXXFLAGS="-fsanitize=address,undefined"
tools/cdev exec sh tools/check-lsp.sh && tools/cdev exec sh tools/check-query.sh
make clean && make            # leave a normal build behind, on BOTH platforms
```

⚠️ **`tools/check-lsp.sh` is ONE FLAT SHELL SCOPE.** Take fixture variable names and request ids from the
inventory in [lsp-m6-kickoff.md](lsp-m6-kickoff.md) (free ids: 31, 50–52, 68+) and refresh it afterwards, or
a new variable will silently retarget a fixture 80 lines away with its assertions still "passing".

⚠️ **A fixture's coordinates come from the BUFFER the harness opens, not from the file on disk.** Several
`$…URI` documents are opened with a compact inlined buffer that does not match the on-disk layout. B3f lost
a debug cycle to this.

---

## After Stage C

- **Stage D — campaign exit.** Reconcile `docs/design/lsp.md` § Acceptance, ROADMAP §10, and the memory
  pointer; confirm the sanitized runs on both platforms; the campaign ends.
- **M7 — tree-sitter grammar + Zed extension.** Split out of M6; unlocks Neovim/Helix/Zed colouring and
  GitHub linguist. A second grammar to keep in sync with `kama.l`/`kama.y`, so it gets its own campaign.
- **Carried, not blocking:** the ~10 ms fixed prelude-ANALYSIS floor (a pre-baked/forkable CEmitter) is on
  ROADMAP, not in M6.
- **A standing obligation, not a task:** `tests/query/coverage/*.kama` must gain a case whenever the
  LANGUAGE gains a naming construct. The reference index is built by instrumenting the emitter, so it is
  only ever as complete as the set of sites someone remembered — the coverage oracle is what turns a gap
  into a diff instead of a discovery. It found nine gaps where B3's brief had named two, then a tenth
  (B3g), then an eleventh (B3h).
