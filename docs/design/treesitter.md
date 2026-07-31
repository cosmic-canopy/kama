# The tree-sitter grammar (M7) — decisions of record

**Status: SHIPPED** (grammar, guards, Helix, Zed extension). This is the record of what was decided and
why, and of the several things that turned out not to be true. The status of the LSP campaign it grew out
of is in [lsp.md](lsp.md); its hand-off brief was [lsp-m6-d-kickoff.md](lsp-m6-d-kickoff.md).

## Why a third grammar at all

kama's front end is now described in three places: `kama.l`/`kama.y` (the source of truth),
`editor/vscode/syntaxes/kama.tmLanguage.json` (TextMate, for VS Code and Sublime), and
`tree-sitter-kama/`. A third description of the same language is a liability, and it was taken on for
exactly four things nothing else could deliver:

- **Helix colouring.** Helix colours *only* from tree-sitter. Before this it showed an uncoloured buffer
  with every LSP feature working.
- **A Zed extension at all.** A Zed extension registers a *language*, which requires a grammar. There is
  no grammar-less LSP-only Zed extension.
- **Neovim/Vim colouring beyond semantic tokens**, which only cover identifiers.
- **GitHub linguist**, which needs a grammar before `.kama` can be recognized.

## Decision: the grammar lives in this repository

`tree-sitter-kama/` is a directory here, not a separate `tree-sitter-kama` repository, even though the
ecosystem convention is a repository per grammar. Helix (`subpath`), Zed (`path`) and nvim-treesitter
(`location`) all consume a grammar from a subdirectory, so nothing is lost — and what is gained is the
whole point of the milestone: **`tools/check-treesitter.sh` can fail this repository's own test suite the
moment `kama.l` gains a keyword the grammar lacks.** A separate repository would let the two drift for a
release cycle with nothing to notice.

The generated parser under `src/` is **committed**, because every consumer compiles `parser.c` rather than
running the generator. The CLI is pinned **exact** (`"tree-sitter-cli": "0.26.11"`, no caret) because
`tree-sitter generate` output is not byte-stable across versions and the guard diffs it.

## Decision: the guard is a pair, folded into one file, split by dependency

The TextMate grammar is guarded by a pair — `check-syntax-drift.sh` greps and can only see that a rule
EXISTS; `check-syntax.sh` runs the real tokenizer and can see that a rule FIRES. Both are needed, because
two TextMate rules were once present, correct and unreachable.

`tools/check-treesitter.sh` keeps that split but along a more useful axis: **what a checkout can verify
without the tree-sitter CLI installed.**

| # | oracle | needs |
|---|---|---|
| 1 | keyword drift, both directions (below) | nothing but `kama.l` |
| 2 | `tests/syntax/*.kama` agree with `kama check` | `$KAMA` |
| 3 | `tree-sitter generate` is a no-op against the committed `src/` | the pinned CLI |
| 4 | corpus tests pass; every `.scm` compiles, in both vocabularies | the pinned CLI |
| 5 | **the whole-corpus oracle** | the pinned CLI |

1 and 2 always run, so a fresh clone still has real teeth. 3–5 skip loudly (exit 0), like `check-syntax.sh`.

### Oracle 1 runs in both directions

The forward direction is the obvious one: every keyword in `kama.l`'s table must be a terminal in the
generated grammar. The **inverse** direction is what protects this grammar's central design decision: if
`value`, `resource`, `view`, `contract` or `both` ever becomes a grammar *terminal*, `word:` promotes it to
a keyword and `int32 value = 1;` stops parsing. A drift grep can normally only ask whether something is
present; here it must also ask whether something is absent.

### Oracle 5 is the one that matters

Every `.kama` file in `tests/`, `lib/`, `examples/` and `prelude/` must parse with zero ERROR nodes if the
compiler accepts it, and must produce one if the compiler rejects it with a parse or lexical error.

**The partition is the compiler's own parse-vs-semantic split, and it is derived, never hand-curated.** The
obvious shortcut — "everything under `tests/xfail/` must fail to parse" — is wrong and would have failed on
day one: of 228 xfail fixtures, **217 are semantic failures that parse perfectly well.** Only 11 files in
the whole tree are genuine parse or lexical errors.

Running `kama check` over 884 files costs ~43 s, which is too slow for every `run_tests.sh`. So the guard
parses the corpus with tree-sitter in **one** process (~1 s), diffs against the committed 11-line
`test/parse-errors.txt`, and consults the compiler **only about files where the two disagree** — which is
what lets it distinguish, by name, a stale manifest from a wrong grammar from a too-permissive one. Steady
state: zero `kama check` calls.

It earned this on its first run, rejecting `tests/vtable_depth3.kama` and
`tests/query/coverage/spellings.kama` — a missing `base.m()` callee that no fixture covered.

## What was verified rather than assumed

**`>>` inside nested generics needs no external scanner.** `kama.l` splits it with a parser-maintained
`genericDepth` counter and `yyless(1)`; tree-sitter needs neither, because its lexer is generated per LR
state from that state's valid symbols, so where only `,` and `>` can follow a type, `>>` is not a
candidate. kama makes this *easier* than C++ or Rust for a reason worth preserving: `statement_expression`
(kama.y:499) is restricted, so `a < b;` is not a legal statement and a leading `IDENT <` can only open a
type. Ten forms in `test/corpus/generics.txt` pin it.

**`assignment` takes a unary-level left-hand side** (kama.y:620), not a full expression. This is not a
detail: with a full-expression LHS, a statement could begin with a binary expression, `a < b` becomes
reachable at statement position, and the entire `>>` argument above collapses into an unresolvable conflict
between `type_name` and `_expression`.

**The contextual kind words get a node, not a keyword.** `type_kind` and `kind_name` wrap an identifier, so
a query can colour them without any pattern-ordering assumption. This is the direct fix for the TextMate
dead-rule defect, where the equivalent rule was present, correct and unreachable.

## The two query-precedence rules, and how they were found

Both were established by rendering a buffer in a real Helix 25.07.1 through a pty and reading the SGR
colours back — `tools/helix-render.py`, kept in the tree so the next person can re-measure instead of
guess. Neither failure errors, and neither is visible to `tree-sitter query`, which reports every match
rather than the winner. **The only instrument that sees them is a running editor.**

1. **On the same node, the last matching pattern wins.** So `queries/highlights.scm` is written
   least-specific first, with the generic `(identifier) @variable` as the *first* rule. Written last — the
   ordering most grammars use — it silently clobbered all fifteen specific captures and every identifier
   rendered as plain default foreground.
2. **A capture on a deeper node beats one on its ancestor, whatever the order.** This defeats rule 1
   entirely: `(type_kind) @keyword.storage.type` never applied, because `type_kind` *contains* an
   identifier and the generic capture sits on that deeper node. Every wrapper-node capture must therefore
   target the inner identifier. The tell was that `fn T copy()` coloured correctly while `fn T area()` did
   not — `copy` is an anonymous token with no identifier beneath it to lose to.

A third rule needs no measurement: never let a bare anonymous token compete with a named node. `copy` and
`give` may name a method (kama.y:980), so `["give" "copy"] @keyword.operator` painted `fn Point copy()`
keyword-coloured wherever it sat. The handoff captures are scoped to their parent nodes instead.

**Zed needs its own `highlights.scm`,** not a copy. Its theme vocabulary is a flat ~44-entry set with no
`keyword.control.*`, no `function.method` and no `variable.other.member`. An unknown capture is not an error
in Zed — it simply produces no colour — so a Helix-vocabulary file would load cleanly and render half the
buffer grey. The guard compiles both and checks they reference the same node types.

## Zed: the parts that constrain the workflow

- **The LSP half requires Rust.** `[language_servers.…]` in `extension.toml` is metadata only;
  `language_server_command` must be a `wasm32-wasip2` component. Zed runs the build itself, so a developer
  needs only `rustup` and an end user installing from the registry needs nothing. No Rust enters the
  compiler build, the container, or the CI test legs — it is confined to `editor/zed/`.
- **Zed does not read the working tree.** It runs `git fetch --depth 1 origin <rev>` against the
  `repository` in `extension.toml`, so a grammar change is invisible until it is committed and `rev` is
  updated. Iterate with `tree-sitter test` and Helix — whose local `source = { path = … }` needs no commit —
  and touch Zed once, at the end.

## Things this campaign's brief got wrong

In the tradition of the rest of this campaign, the M7 brief was wrong in three load-bearing places:

- It said the guard could probe for the CLI with `[ -x ]`. It cannot: `node_modules/.bin/tree-sitter` is a
  symlink to a JS shim that is always present and executable, while the 20 MB binary beside it is fetched by
  an install script. Under `npm ci --ignore-scripts` the `-x` test passes and the CLI then dies with an
  unhandled `error` event. The guard probes by running `--version`.
- It measured the corpus at 886 files. Two of those are **directories** named `.kama`
  (`tests/pkg_path_dep.d/`), and feeding one to the CLI aborts the entire run with
  `Is a directory (os error 21)`. `find` must be `-type f`.
- It expected the query layer to be settled by making pattern order irrelevant. Order turned out to be only
  half the problem; node depth was the other half, and no amount of grammar-side disambiguation addresses
  it. See above.

## Forward work

- **Flip the `file://` grammar source to the public URL** the day the repository goes public:
  `https://github.com/cosmic-canopy/kama` with a tag `rev` in `editor/zed/extension.toml`, and the
  `git`+`subpath` form in the Helix snippet (both are already written out in `docs/editors.md`).
- **Verify the Zed extension in a running Zed.** `zed: install dev extension` is a GUI action; the Rust is
  compile-verified against `zed_extension_api` and the queries are checked, but the loop has not been closed.
- **Publish to the Zed extension registry**, and **register with nvim-treesitter** (`install_info` with
  `location = 'tree-sitter-kama'`) and **GitHub linguist** — all gated on the repository being public.
- **Upstream the Helix `[[language]]`/`[[grammar]]` entries** into Helix's built-in `languages.toml`, the
  same class of work as the `nvim-lspconfig`/`eglot-server-programs` registrations already on ROADMAP §10.
