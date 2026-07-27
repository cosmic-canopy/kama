// kama VSCode extension — zero-config debugging + language server.
//
// Pressing F5 in a `.kama` (when no debug session is active) builds a debug binary
// and launches it under CodeLLDB, with breakpoints mapped back to the `.kama` via
// the `#line` directives kama emits. No `launch.json` / `tasks.json` needed in the
// user's project — the whole flow is supplied here.
//
// On activation it also starts the kama language server (`kama lsp` over stdio) and
// wires a LanguageClient, so `.kama` files get live, as-you-type diagnostics (LSP M1).
// Hover / go-to-definition / completion arrive in later milestones — the same server
// gains them and this client needs no change.
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const cp = require('child_process');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

// Prefer a workspace-local ./kama (a dev checkout of the compiler), else trust PATH.
function findKama() {
  for (const f of vscode.workspace.workspaceFolders || []) {
    const p = path.join(f.uri.fsPath, 'kama');
    try { fs.accessSync(p, fs.constants.X_OK); return p; } catch (_) { /* not here */ }
  }
  return 'kama';
}

function build(kama, file, out) {
  return new Promise((resolve) => {
    cp.execFile(kama, ['build', file, '-o', out], (err, stdout, stderr) => {
      resolve({ ok: !err, message: (stderr || stdout || (err && err.message) || '').trim() });
    });
  });
}

async function debugCurrentFile() {
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== 'kama') {
    vscode.window.showErrorMessage('kama: open a .kama file to debug.');
    return;
  }
  await ed.document.save();
  const file = ed.document.fileName;
  const out = file.replace(/\.kama$/i, '');
  const kama = findKama();

  const result = await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Window, title: 'kama: building debug build…' },
    () => build(kama, file, out)
  );
  if (!result.ok) {
    vscode.window.showErrorMessage('kama build failed: ' + (result.message || 'see terminal'));
    return;
  }

  const folder = vscode.workspace.getWorkspaceFolder(ed.document.uri);
  await vscode.debug.startDebugging(folder, {
    type: 'lldb',                       // CodeLLDB (shipped via this extension's pack)
    request: 'launch',
    name: 'kama: ' + path.basename(file),
    program: out,
    args: [],
    cwd: folder ? folder.uri.fsPath : path.dirname(file),
    sourceLanguages: ['c'],
  });
}

let client;   // the running LanguageClient, so deactivate() can stop it

function startLanguageServer() {
  const kama = findKama();
  // One server binary, one transport: `kama lsp` speaks JSON-RPC over stdio.
  const serverOptions = { command: kama, args: ['lsp'], transport: TransportKind.stdio };
  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'kama' }],
  };
  client = new LanguageClient('kama', 'kama Language Server', serverOptions, clientOptions);
  client.start();
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('kama.debugCurrentFile', debugCurrentFile)
  );
  startLanguageServer();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
