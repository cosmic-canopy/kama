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

# 1b. Every configured editor states whether its snippet was VERIFIED against a running editor or merely
#     documented from that editor's reference. Three were driven for real (Neovim, Emacs, Helix); three
#     were not. Which is which is the kind of thing that silently becomes a lie, so it is asserted: the
#     count of status lines must match the count of configured editors.
statuses=$(grep -cE '^\*(Verified against|Documented from)' "$DOC" || true)
[ "$statuses" -eq 7 ] || bad "docs/editors.md has $statuses verification-status lines; expected 7 (one per configured editor). A new editor must declare whether its snippet was actually run."

# 2. Every editor that gets a CONFIG SNIPPET must name the server command. `kama lsp` is spelled several
#    legitimate ways across config languages (lua/json/toml/elisp), so accept any of them — the point is
#    that renaming the subcommand breaks this loudly rather than leaving six silently-wrong snippets.
#    Counted, not just present: one hit could mean five snippets lost their command.
#    Zed's command is Rust, not a config language, so its spelling (`worktree.which("kama")`) is listed
#    too — bumping the count without adding a spelling the Zed section actually contains would give a
#    guard that passes for the wrong reason, which is the failure mode this file already has two of.
cmds=$(grep -cE "'kama', 'lsp'|\"kama\", \"lsp\"|\"kama\" \"lsp\"|\"command\": \"kama\"|command = \"kama\"|worktree\.which\(\"kama\"\)" "$DOC" || true)
[ "$cmds" -ge 7 ] || bad "docs/editors.md names the \`kama lsp\` command only $cmds times; expected >= 7 (one per configured editor)"

#    ⚠️ The count ALONE cannot carry Zed. It has slack — the Emacs section names the command twice (once
#    for eglot, once as an lsp-mode aside) — so breaking Zed's snippet still leaves seven matches and the
#    threshold passes for the wrong reason. That is the third instance of this bug in this file, and it was
#    caught by negative-testing the bump rather than by reading it. Assert Zed's spelling by name.
grep -qF -- 'worktree.which("kama")' "$DOC" || bad "docs/editors.md no longer shows how the Zed extension launches the server (worktree.which(\"kama\"))"

# 2b. CROSS-FILE, and the part a doc-only grep can never see: the page can be perfectly consistent while
#     the code it documents has drifted. Assert the Zed extension really does launch `kama lsp`.
ZED_SRC="$ROOT/editor/zed/src/kama.rs"
ZED_TOML="$ROOT/editor/zed/extension.toml"
[ -f "$ZED_SRC" ] || bad "editor/zed/src/kama.rs is missing — docs/editors.md documents a Zed extension that is not there"
[ -f "$ZED_TOML" ] || bad "editor/zed/extension.toml is missing"
if [ -f "$ZED_SRC" ]; then
    grep -qF -- '"lsp"' "$ZED_SRC" || bad "editor/zed/src/kama.rs no longer passes \`lsp\` to the compiler — the Zed extension would launch the wrong subcommand"
    grep -qF -- 'worktree.which("kama")' "$ZED_SRC" || bad "editor/zed/src/kama.rs no longer resolves \`kama\` through the worktree PATH (which is what honours the per-project toolchain pin)"
fi
if [ -f "$ZED_TOML" ]; then
    grep -qF -- 'grammars.kama' "$ZED_TOML" || bad "editor/zed/extension.toml no longer declares the kama grammar"
    grep -qF -- 'tree-sitter-kama' "$ZED_TOML" || bad "editor/zed/extension.toml no longer points at the tree-sitter-kama subdirectory"
    # No local dev path may ship: Zed and Helix fetch by URL, and a `file://` or an absolute home directory
    # works only on the machine it was written on.
    grep -qE 'file://|/Users/|/home/' "$ZED_TOML" && bad "editor/zed/extension.toml points at a LOCAL path; it must fetch from the public repository" || true
    grep -qE 'file://|/Users/|/home/' "$ROOT/editor/helix/languages.toml" && bad "editor/helix/languages.toml's [[grammar]] points at a LOCAL path; the shipped block must fetch by git (a local path belongs in the commented contributor note)" || true

    # 2c. THE REV MUST CONTAIN THE GRAMMAR. This is the one way the Zed extension can be perfectly
    #     well-formed and still be broken for every user: a rev that predates tree-sitter-kama/ (no tag
    #     does yet — the grammar landed after v0.1.76) fetches a tree with no grammar in it, and the
    #     failure surfaces in the user's editor, not here. Checked against the local object database, so
    #     it needs no network and works while the repo is private.
    rev=$(sed -n 's/^rev = "\([^"]*\)".*/\1/p' "$ZED_TOML" | head -1)
    if [ -n "$rev" ] && command -v git >/dev/null 2>&1 && [ -d "$ROOT/.git" ]; then
        if ! git -C "$ROOT" cat-file -e "$rev^{commit}" 2>/dev/null; then
            bad "editor/zed/extension.toml pins rev=$rev, which is not a commit in this repository"
        elif [ -z "$(git -C "$ROOT" ls-tree --name-only "$rev" -- tree-sitter-kama 2>/dev/null)" ]; then
            bad "editor/zed/extension.toml pins rev=$rev, which does NOT contain tree-sitter-kama/ — Zed would fetch a tree with no grammar and the extension would fail to load"
        fi
    fi
fi

# 3. Each snippet has to teach the editor about `.kama` — none of them ship a kama file type.
for pat in "extension = { kama = 'kama' }" "filetypes\": \[\"kama\"\]" "\\\\.kama" "source.kama" "file-types = \[\"kama\"\]" "subpath = \"tree-sitter-kama\""; do
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

# 6. INVERTED at M7. This assertion used to REQUIRE the page to say Zed was impossible; now it requires the
#    opposite, because the grammar shipped. Leaving the old direction in place would have quietly forced the
#    documentation to keep lying.
grep -qF -- 'Not yet possible' "$DOC" && bad "docs/editors.md still says Zed is 'Not yet possible', but M7 shipped the tree-sitter grammar and the extension" || true
grep -qiE 'tree-sitter' "$DOC" || bad "docs/editors.md no longer explains where the tree-sitter grammar fits"
#    Both editors need a real setup step that a config snippet alone does not convey.
grep -qF -- 'install dev extension' "$DOC" || bad "docs/editors.md no longer tells Zed users how to load the extension (zed: install dev extension)"
grep -qF -- 'hx --grammar build' "$DOC" || bad "docs/editors.md no longer tells Helix users to build the grammar, without which the buffer stays uncoloured"

# 7. The build-configuration channel is one file, in every editor. If this line goes, so has the property
#    that makes the whole design work.
grep -qF -- 'kama.local.json' "$DOC" || bad "docs/editors.md no longer documents kama.local.json as the build-configuration channel"

[ "$fail" = 0 ] || { echo "check-editors: see docs/editors.md and docs/design/lsp-m6-c-kickoff.md" >&2; exit 1; }
echo "check-editors: PASS (8 editors documented, 7 configured, 3 watcher globs agree in server + VS Code client, Zed extension agrees with its docs)"
