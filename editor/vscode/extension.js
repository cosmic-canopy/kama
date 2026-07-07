// kama VSCode extension — zero-config debugging.
//
// Pressing F5 in a `.kama` (when no debug session is active) builds a debug binary
// and launches it under CodeLLDB, with breakpoints mapped back to the `.kama` via
// the `#line` directives kama emits. No `launch.json` / `tasks.json` needed in the
// user's project — the whole flow is supplied here.
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const cp = require('child_process');

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

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('kama.debugCurrentFile', debugCurrentFile)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
