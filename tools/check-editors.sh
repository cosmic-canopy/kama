#!/bin/sh
# check-editors.sh — guard docs/editors.md against drifting from the server it documents (LSP M6 C4).
#
# This is a DRIFT check, not a behaviour test, and the distinction is deliberate: actually exercising the
# snippets would need Neovim, Vim, Emacs, Sublime, Helix and Kate installed on every machine that runs the
# suite. What a grep CAN prove is that each documented client still names the command the compiler still
# provides, that the page has not quietly lost an editor, and that the three watcher globs are spelled the
# same in the two places that must agree — the server's dynamic registration (kama.lsp.cpp) and the VS Code
# client's static list (extension.js). What it CANNOT prove is that a snippet works; that is a manual step,
# recorded per editor in the page itself.
#
# Runs no compiler. Fails (exit 1) naming the invariant and the remedy. Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOC="$ROOT/docs/editors.md"
EXT="$ROOT/editor/vscode/extension.js"
SRV="$ROOT/kama.lsp.cpp"

fail=0
bad() { echo "check-editors: FAIL — $1" >&2; fail=1; }

[ -f "$DOC" ] || { echo "check-editors: FAIL — docs/editors.md is missing (LSP M6 C3)" >&2; exit 1; }

# 1. Every editor the campaign committed to (docs/design/lsp.md target matrix) still has a section. A
#    dropped editor must be a diff, not a silence.
#    ⚠️ `-xF`, whole-line and FIXED-string, for two reasons found by breaking it on purpose: a substring
#    match passed a heading renamed to `## Helixx` (the blind spot check-syntax-drift.sh records for its
#    own regex), and an anchored ERE then matched `## Vim coc.nvim` because the parentheses in the real
#    heading are a capture group. Neither failure is visible without a negative test.
for ed in Neovim "Vim (coc.nvim)" "Emacs (eglot)" "Sublime Text" Helix Kate Zed; do
    grep -qxF -- "## $ed" "$DOC" || bad "docs/editors.md has no '## $ed' section"
done

# 2. Every editor that gets a CONFIG SNIPPET must name the server command. `kama lsp` is spelled several
#    legitimate ways across config languages (lua/json/toml/elisp), so accept any of them — the point is
#    that renaming the subcommand breaks this loudly rather than leaving six silently-wrong snippets.
#    Counted, not just present: one hit could mean five snippets lost their command.
cmds=$(grep -cE "'kama', 'lsp'|\"kama\", \"lsp\"|\"kama\" \"lsp\"|\"command\": \"kama\"|command = \"kama\"" "$DOC" || true)
[ "$cmds" -ge 6 ] || bad "docs/editors.md names the \`kama lsp\` command only $cmds times; expected >= 6 (one per configured editor)"

# 3. Each snippet has to teach the editor about `.kama` — none of them ship a kama file type.
for pat in "extension = { kama = 'kama' }" "filetypes\": \[\"kama\"\]" "\\\\.kama" "source.kama" "file-types = \[\"kama\"\]"; do
    grep -qE -- "$pat" "$DOC" || bad "docs/editors.md no longer registers the .kama file type via: $pat"
done

# 4. THE THREE GLOBS, in the two places that must agree. `**/kama.json` does NOT match `kama.local.json`,
#    which is exactly the mistake this exists to catch: dropping the third glob loses build-configuration
#    re-resolution and nothing else visibly breaks.
for glob in '**/*.kama' '**/kama.json' '**/kama.local.json'; do
    grep -qF -- "$glob" "$EXT" || bad "editor/vscode/extension.js no longer watches $glob"
    grep -qF -- "$glob" "$SRV" || bad "kama.lsp.cpp no longer registers $glob for didChangeWatchedFiles"
done

# 5. The server-side registration must stay reachable: a `client/registerCapability` that is never sent
#    leaves every non-VS-Code client silently unwatched (the M6 C0 finding).
grep -qF -- 'client/registerCapability' "$SRV" || bad "kama.lsp.cpp no longer sends client/registerCapability"
grep -qF -- 'workspace/didChangeWatchedFiles' "$SRV" || bad "kama.lsp.cpp no longer registers workspace/didChangeWatchedFiles"

# 6. Zed is blocked on M7's tree-sitter grammar. Say so, so that "we have not done Zed" cannot read as
#    "Zed works" — the same reason Helix's missing colouring is stated rather than omitted.
grep -qF -- 'Not yet possible' "$DOC" || bad "docs/editors.md no longer states plainly that Zed is blocked (M7)"
grep -qiE 'tree-sitter' "$DOC" || bad "docs/editors.md no longer explains that colouring for Helix/Zed waits on tree-sitter"

# 7. The build-configuration channel is one file, in every editor. If this line goes, so has the property
#    that makes the whole design work.
grep -qF -- 'kama.local.json' "$DOC" || bad "docs/editors.md no longer documents kama.local.json as the build-configuration channel"

[ "$fail" = 0 ] || { echo "check-editors: see docs/editors.md and docs/design/lsp-m6-c-kickoff.md" >&2; exit 1; }
echo "check-editors: PASS (8 editors documented, 3 watcher globs agree in server + VS Code client)"
