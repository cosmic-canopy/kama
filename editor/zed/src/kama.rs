//! The Zed extension for kama.
//!
//! Zed is the one editor of the eight that cannot be configured into supporting a language from a config
//! file: registering a language requires a tree-sitter grammar, and pointing that language at a language
//! server requires a WebAssembly component, because `[language_servers.…]` in extension.toml is metadata
//! only. Hence this file. It is a launcher and nothing more — all of the intelligence is in the compiler's
//! own `kama lsp` subcommand (kama.lsp.cpp), exactly as it is for the other seven editors.
//!
//! Zed compiles this itself when the extension is installed, and ships a prebuilt `.wasm` to end users, so
//! nobody installing kama needs Rust.

use zed_extension_api::{self as zed, settings::LspSettings, Command, LanguageServerId, Result, Worktree};

struct KamaExtension;

const SERVER_ID: &str = "kama";

impl zed::Extension for KamaExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Command> {
        // A user-configured binary wins, so a project pinned to a specific toolchain build behaves the
        // same here as it does under the toolchain selector.
        let configured = LspSettings::for_worktree(SERVER_ID, worktree)
            .ok()
            .and_then(|settings| settings.binary)
            .and_then(|binary| binary.path);

        let command = match configured {
            Some(path) => path,
            // `~/.kama/bin/kama` is a SELECTOR: it resolves the project's pinned toolchain and re-execs.
            // Resolving it through the worktree's PATH is therefore the correct thing to do, not a
            // fallback — hardcoding a versioned binary would defeat the per-project pin.
            None => worktree.which("kama").ok_or_else(|| {
                "kama was not found on $PATH. Install the toolchain (which puts ~/.kama/bin/kama on \
                 PATH), or set lsp.kama.binary.path in your Zed settings. Note the binary must be \
                 native to this OS — a container-built Linux binary will not launch."
                    .to_string()
            })?,
        };

        Ok(Command {
            command,
            args: vec!["lsp".to_string()],
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(KamaExtension);
