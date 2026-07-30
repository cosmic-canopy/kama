# LSP Stage D (campaign exit) + M7 (tree-sitter) — cold-start brief

**Status: Stage C SHIPPED 2026-07-30** (`69b6db4`, `d5c8f96`, dev, not yet pushed). M0–M5 and all of M6
(A, B1, B2, B3, C) are done. **Stage D is a reconciliation pass, not a build stage** — it should be one
short session. M7 is a real campaign and gets its own.

⚠️ **Every brief in this campaign has been wrong somewhere load-bearing — including Stage C's, in two
places, and this one probably is too.** Where it says "verified", it was run; where it says "expect", it
was not.

---

## Where things stand (measured at `d5c8f96`, macOS host)

```
./run_tests.sh                 804 / 804        (803 + tools/check-editors.sh)
tools/lspref.sh                537 fixtures, byte-identical, 0 TRANSPILE_FAILED
sh tools/check-lsp.sh          153  ok: lines   (136 before Stage C)
sh tools/check-query.sh        227  ok: lines   (unchanged — Stage C touched no query path)
tools/lsp-bench.sh --lsp       ~93 ms, exactly 12 timing lines   (budget 100)
sanitized compiler             both harnesses clean on macOS AND in the container
```

Regenerate the lspref baseline before touching anything — `build/` is gitignored:

```sh
make && tools/lspref.sh > build/lspref-before.txt
```

---

## Stage D — the campaign exit

The work is reconciliation. Four things to check, in order of how likely they are to be stale:

1. **`docs/design/lsp.md` § Acceptance (v1)** — written at M0 and never revisited. It says *"In VSCode over
   the real extension: live diagnostics, hover, go-to-definition, outline"*, which the campaign has
   massively overshot and which no longer names the deliverable. Rewrite it against what actually shipped,
   and say which editors it was verified in (Neovim 0.12.4, Emacs 30.2, Helix 25.07.1 — see the Stage C
   sub-bullet in that file).
2. **ROADMAP §10** — the LSP entry is now ~100 lines and reads as a changelog. Decide whether it collapses
   to a shipped-summary plus pointers now that the campaign is closing. **Ask the user before compressing
   it** — the per-milestone detail has been deliberate throughout.
3. **The memory pointer** (`next-lsp.md`) — must end the campaign and hand off to M7.
4. **`docs/design/lsp-m6-c-kickoff.md`** — leave it; its value now is as the record of a brief that was
   wrong twice. Do not delete a brief to make the tree tidy.

**Carried, explicitly not blocking the exit:**

- The **~10 ms fixed prelude-ANALYSIS floor** per keystroke (a pre-baked or forkable `CEmitter`). On
  ROADMAP, not in M6.
- **Per-project build configuration.** One configuration per server process is pinned by the first
  document that resolves a manifest (`kama.lsp.cpp:520-537`, enforced at `:738`). The real fix needs
  per-configuration parse caches, because `pruneInactiveDecls` rewrites cached units in place. Stage C
  softened it with a status-bar warning and `kama.restartServer`; it did not fix it.
- **Upstream editor registration** — a `kama` entry in `nvim-lspconfig`, Helix's built-in
  `languages.toml`, and `eglot-server-programs`. These turn six pasted lines into zero for users, but they
  are PRs to *other* projects and gate on a public release. Record on ROADMAP §10; do not attempt now.
- **Vim/coc.nvim, Sublime and Kate snippets are documented, not verified** (`docs/editors.md` says so per
  editor, and `tools/check-editors.sh` asserts a status line per editor). Verifying them needs a human at
  a GUI; Sublime additionally needs the LSP package installed through Package Control, since a bare git
  clone did not bring it up.

**A standing obligation, not a task:** `tests/query/coverage/*.kama` must gain a case whenever the
LANGUAGE gains a naming construct. The reference index is built by instrumenting the emitter, so it is
only ever as complete as the set of sites someone remembered; the coverage oracle is what turns a gap into
a diff. It found nine gaps where B3's brief named two, then a tenth, then an eleventh.

---

## M7 — tree-sitter grammar + Zed extension

**This is a real campaign, not a stage.** It is a **third** grammar to keep in sync with `kama.l`/`kama.y`
(after the compiler's own front end and the TextMate grammar), so it needs its own drift guard from day
one — that is why the user split it out of M6 on 2026-07-28.

What it unlocks, and nothing else does:

- **Helix syntax colouring.** Helix colours *only* from tree-sitter. Today it logs
  `Skipping syntax config for 'kama' because the parser's shared library does not exist` and shows an
  uncoloured buffer with every LSP feature working. Verified, 25.07.1.
- **A Zed extension at all.** Zed extensions register a *language*, and that requires a grammar; there is
  no grammar-less LSP-only Zed extension. This is the only reason Zed is absent from `docs/editors.md`.
- **Full Neovim/Vim colouring** (they have semantic tokens today, which cover identifiers only —
  keywords, strings, numbers and comments come from the editor's own grammar).
- **GitHub linguist** (`.kama` recognized and coloured on GitHub).

Three things to settle before writing any grammar:

1. **The oracle.** B1 established the method and it should be reused verbatim: *write the literal, run
   `kama check`, compare* — the compiler decides, not eyeballing. That pass rejected six invented
   spellings that would each have shipped a confidently-wrong grammar rule (among them `fn name() -> T`;
   kama has no `->`). ⚠️ It is also the trap that cost time in Stage C: an invalid fixture (`twice(21)` —
   kama has **no positional arguments**) made Neovim's hover and definition look broken when the server
   was fine.
2. **The drift guard's shape.** `tools/check-syntax-drift.sh` greps and can only see that a rule EXISTS;
   `tools/check-syntax.sh` runs the real TextMate engine and can see that a rule FIRES. B1 needed both,
   because two rules were present, correct and *unreachable*. A tree-sitter guard wants the equivalent
   pair — a corpus test (tree-sitter's own `test` harness over `test/corpus/`) plus agreement with
   `kama check` over `tests/syntax/`, which already exists and can be reused.
3. **Where the grammar lives and how it is built.** A tree-sitter grammar is a JS `grammar.js` plus
   generated C, and consumers (Helix, Zed, nvim-treesitter) each fetch it by git URL. Decide early whether
   it is a directory in this repo or a separate repository — Zed and Helix both want a URL, and the
   toolchain is containerized (`tools/cdev`), so a node build step needs a decision.

---

## Gates (unchanged, non-negotiable)

```sh
make && ./run_tests.sh                                  # 804/804
tools/lspref.sh > build/lspref-after.txt
diff build/lspref-before.txt build/lspref-after.txt     # byte-identical, 537 fixtures
sh tools/check-lsp.sh && sh tools/check-query.sh        # counts GROW, never shrink: 153 / 227 today
sh tools/check-editors.sh
tools/lsp-bench.sh --lsp                                # ~93 ms, budget 100
```

⚠️ The sanitized-compiler run of both harnesses is a **MANUAL** step; the `KAMA_SAN=1` leg does NOT do it
(that leg sanitizes generated programs and gates check-lsp/check-query OFF). Run it on macOS **and** in the
container — macOS ASan has no LeakSanitizer, and this is the step that caught M4's out-of-bounds read:

```sh
make clean && make EXTRA_CXXFLAGS="-fsanitize=address,undefined"
sh tools/check-lsp.sh && sh tools/check-query.sh
tools/cdev exec make clean && tools/cdev exec make EXTRA_CXXFLAGS="-fsanitize=address,undefined"
tools/cdev exec sh tools/check-lsp.sh && tools/cdev exec sh tools/check-query.sh
make clean && make            # leave a normal build behind, on BOTH platforms
```

⚠️ **`tools/check-lsp.sh` is ONE FLAT SHELL SCOPE.** Take fixture variable names and request ids from the
inventory in [lsp-m6-kickoff.md](lsp-m6-kickoff.md) (**free ids: 31, 50–52, 69+**) and refresh it
afterwards, or a new variable will silently retarget a fixture 80 lines away with its assertions still
"passing".

⚠️ **A fixture's coordinates come from the BUFFER the harness opens, not the file on disk.** Several
`$…URI` documents are opened with a compact inlined buffer that does not match the on-disk layout.

⚠️ **Negative-test any new guard.** Stage C's `check-editors.sh` had two false passes on first write — a
substring match let `## Helixx` through, and the anchored ERE that replaced it matched `## Vim coc.nvim`
because the heading's parentheses are a capture group. `grep -xF` fixed both. Neither was visible until the
guard was deliberately broken.

---

## Local state left behind by Stage C's verification (macOS host, not in the repo)

Installed via brew for the real-editor pass, and safe to remove: **neovim**, **helix**, **emacs**.
The user installed **Sublime Text**; its `Packages/User/` now holds `kama.tmLanguage` (converted, with
`fileTypes` inserted) and an `LSP.sublime-settings` containing exactly the documented snippet, and
`Packages/LSP` is a git clone that did **not** bootstrap — reinstall through Package Control if picking
Sublime verification back up.
