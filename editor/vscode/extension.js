// cstar VSCode extension — zero-config debugging.
//
// Pressing F5 in a `.cstar` (when no debug session is active) builds a debug binary
// and launches it under CodeLLDB, with breakpoints mapped back to the `.cstar` via
// the `#line` directives cstar emits. No `launch.json` / `tasks.json` needed in the
// user's project — the whole flow is supplied here.
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const cp = require('child_process');

// Prefer a workspace-local ./cstar (a dev checkout of the compiler), else trust PATH.
function findCstar() {
  for (const f of vscode.workspace.workspaceFolders || []) {
    const p = path.join(f.uri.fsPath, 'cstar');
    try { fs.accessSync(p, fs.constants.X_OK); return p; } catch (_) { /* not here */ }
  }
  return 'cstar';
}

function build(cstar, file, out) {
  return new Promise((resolve) => {
    cp.execFile(cstar, ['build', file, '-o', out], (err, stdout, stderr) => {
      resolve({ ok: !err, message: (stderr || stdout || (err && err.message) || '').trim() });
    });
  });
}

async function debugCurrentFile() {
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== 'cstar') {
    vscode.window.showErrorMessage('cstar: open a .cstar file to debug.');
    return;
  }
  await ed.document.save();
  const file = ed.document.fileName;
  const out = file.replace(/\.cstar$/i, '');
  const cstar = findCstar();

  const result = await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Window, title: 'cstar: building debug build…' },
    () => build(cstar, file, out)
  );
  if (!result.ok) {
    vscode.window.showErrorMessage('cstar build failed: ' + (result.message || 'see terminal'));
    return;
  }

  const folder = vscode.workspace.getWorkspaceFolder(ed.document.uri);
  await vscode.debug.startDebugging(folder, {
    type: 'lldb',                       // CodeLLDB (shipped via this extension's pack)
    request: 'launch',
    name: 'cstar: ' + path.basename(file),
    program: out,
    args: [],
    cwd: folder ? folder.uri.fsPath : path.dirname(file),
    sourceLanguages: ['c'],
  });
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('cstar.debugCurrentFile', debugCurrentFile)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
