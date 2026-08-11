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

// This platform spelled the way the Makefile's `uname -s`-`uname -m` spells it, or null if we
// can't map it (then we just fall back to the root ./kama below).
function platformDir() {
  const os = { darwin: 'Darwin', linux: 'Linux' }[process.platform];
  const arch = { arm64: 'arm64', x64: 'x86_64' }[process.arch];
  return os && arch ? os + '-' + arch : null;
}

// The executable suffix this OS requires. Without it the search below found the root `kama` — which on
// Windows is a real 43 MB copy, because msys2's `ln -s` degrades to one — and then handed a path with no
// extension to spawn, which Windows will not start.
const EXE = process.platform === 'win32' ? '.exe' : '';

// Windows has no platformDir(): the Makefile names that directory from `uname -s`, which under msys2 is
// something like `MINGW64_NT-10.0-26200-ARM64` — a string carrying the OS BUILD NUMBER, so nothing
// outside that shell can reconstruct it. Look at what is actually on disk instead and take the most
// recently built, which is the same "whichever platform built last" rule the root symlink encodes.
function outDirsWindows(root) {
  const out = path.join(root, 'out');
  let names = [];
  try { names = fs.readdirSync(out); } catch (_) { return []; }
  return names
    .map((n) => path.join(out, n))
    .filter((d) => { try { return fs.statSync(path.join(d, 'kama' + EXE)).isFile(); } catch (_) { return false; } })
    .sort((a, b) => fs.statSync(path.join(b, 'kama' + EXE)).mtimeMs -
                    fs.statSync(path.join(a, 'kama' + EXE)).mtimeMs);
}

// Prefer a workspace-local build of the compiler (a dev checkout), else trust PATH. In a dev tree the
// Makefile builds per platform into out/<os>-<arch>/kama and leaves the root ./kama a symlink to
// whichever platform built last — so check the NATIVE path first, or a `tools/cdev make` would hand
// this extension a Linux binary it can't launch.
function findKama() {
  // An explicit setting wins outright — the search below cannot know about an install this extension
  // has no reason to expect (a versioned store, a sandbox, a second checkout).
  const configured = vscode.workspace.getConfiguration('kama').get('path');
  if (configured) return configured;
  const plat = platformDir();
  for (const f of vscode.workspace.workspaceFolders || []) {
    const roots = plat ? [path.join(f.uri.fsPath, 'out', plat), f.uri.fsPath]
                       : [...outDirsWindows(f.uri.fsPath), f.uri.fsPath];
    for (const r of roots) {
      const p = path.join(r, 'kama' + EXE);
      try { fs.accessSync(p, fs.constants.X_OK); return p; } catch (_) { /* not here */ }
    }
  }
  return 'kama' + EXE;
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

let client;       // the running LanguageClient, so deactivate() can stop it
let statusItem;   // the build-configuration indicator + picker entry point
let lastConfig;   // the most recent `kama/buildConfig` payload, or undefined before the server answers

// ---- build configuration (LSP M6 C1) ---------------------------------------------------------------
// The state shown here comes from the SERVER, never from re-reading kama.json: the server announces the
// configuration it actually resolved (`kama/buildConfig`), including which values each single-select group
// could take. Deriving it client-side is how a status bar comes to disagree with the analyzer.

function refreshStatus() {
  if (!statusItem) return;
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== 'kama' || !lastConfig) { statusItem.hide(); return; }
  const g = lastConfig.groups || {};
  const target = (g.TARGET && g.TARGET.selected) || '?';
  const build = (g.BUILD_TYPE && g.BUILD_TYPE.selected) || '?';
  statusItem.text = '$(gear) ' + target + ' · ' + build;

  const lines = [];
  lines.push(lastConfig.manifest ? lastConfig.manifest : 'no kama.json — permissive defaults');
  if (lastConfig.localManifest) lines.push('+ ' + lastConfig.localManifest);
  if (lastConfig.triple) lines.push('triple: ' + lastConfig.triple);
  if (lastConfig.flags && lastConfig.flags.length) lines.push('flags: ' + lastConfig.flags.join(' '));
  // One configuration per server process, pinned by the first document that resolved a manifest. A second
  // project opened in the same window is analyzed under the FIRST one's flags — say so where the user is
  // already looking, rather than letting it present as a phantom editor/compiler disagreement.
  if (lastConfig.manifest) {
    const root = path.dirname(lastConfig.manifest);
    const here = ed.document.uri.fsPath;
    if (here !== root && !here.startsWith(root + path.sep))
      lines.push('⚠ this file is outside the pinned project — run "kama: Restart Language Server"');
  }
  lines.push('Click to change the build configuration (writes kama.local.json).');
  statusItem.tooltip = lines.join('\n');
  statusItem.show();
}

// Pick a single-select group, then a value, then record it in `kama.local.json` — the SAME file `kama build`
// merges, so the editor and a plain build cannot end up disagreeing, and no editor-private setting exists
// to get out of step. Saving it trips the file watcher, and the server re-resolves and republishes every
// open buffer on its own; there is nothing to restart.
async function selectBuildConfig() {
  if (!lastConfig) {
    vscode.window.showInformationMessage('kama: the language server has not reported a configuration yet.');
    return;
  }
  // `kama.local.json` is only ever read as a SIBLING of a discovered `kama.json` — writing one into a
  // folder with no manifest produces a file the compiler will never look at.
  if (!lastConfig.manifest) {
    vscode.window.showWarningMessage(
      'kama: no kama.json for this file, so there is no build configuration to change. ' +
      'A kama.local.json is only read next to a kama.json.');
    return;
  }
  const groups = lastConfig.groups || {};
  const names = Object.keys(groups);
  if (!names.length) {
    vscode.window.showInformationMessage('kama: this project declares no build-configuration groups.');
    return;
  }
  const groupPick = await vscode.window.showQuickPick(
    names.map((n) => ({ label: n, description: groups[n].selected || '(none selected)' })),
    { title: 'kama: which build-configuration group?' });
  if (!groupPick) return;
  const group = groups[groupPick.label];
  const valuePick = await vscode.window.showQuickPick(
    (group.values || []).map((v) => ({ label: v, description: v === group.selected ? 'current' : '' })),
    { title: 'kama: ' + groupPick.label + ' =' });
  if (!valuePick) return;

  const localPath = path.join(path.dirname(lastConfig.manifest), 'kama.local.json');
  let doc = {};
  try {
    const raw = fs.readFileSync(localPath, 'utf8').trim();
    if (raw) doc = JSON.parse(raw);
  } catch (e) {
    if (e instanceof SyntaxError) {
      vscode.window.showErrorMessage('kama: ' + localPath + ' is not valid JSON — fix it first.');
      return;
    }
    /* no such file: start from {} */
  }
  if (!doc.select) doc.select = {};
  if (!doc.select[groupPick.label]) doc.select[groupPick.label] = {};
  const g = doc.select[groupPick.label];
  // Exactly one value per group is the point of a single-select axis, so clear any default we set before
  // rather than accumulating them. Other fields on a sibling value (a TARGET's triple/toolchain) stay.
  for (const v of Object.keys(g)) if (g[v] && typeof g[v] === 'object') delete g[v].default;
  if (!g[valuePick.label] || typeof g[valuePick.label] !== 'object') g[valuePick.label] = {};
  g[valuePick.label].default = true;
  try {
    fs.writeFileSync(localPath, JSON.stringify(doc, null, 2) + '\n');
  } catch (e) {
    vscode.window.showErrorMessage('kama: could not write ' + localPath + ': ' + e.message);
  }
}


function startLanguageServer() {
  const kama = findKama();
  // One server binary, one transport: `kama lsp` speaks JSON-RPC over stdio.
  const serverOptions = { command: kama, args: ['lsp'], transport: TransportKind.stdio };
  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'kama' }],
    // Watch every .kama in the workspace so the server hears about files created, deleted, or edited
    // outside the editor, and can drop its workspace index (LSP M3.5 — that index is what makes
    // find-references and cross-file rename see files the open one doesn't import). Registering the
    // watcher HERE, client-side, is deliberate: the alternative is server-driven registration via
    // `client/registerCapability`, a server->client request the server has no machinery for. Every
    // editor's LSP client offers the same client-side hook, so M6's other clients wire it the same way.
    //
    // The two manifests are watched for a DIFFERENT reason (M6 A1): they carry the build configuration the
    // server analyzes under, so editing one changes which `@compileFor` declarations exist — not just a
    // file in the program, but the program. The server re-resolves and republishes every open buffer.
    // Spelled out separately because `**/kama.json` does not match `kama.local.json`.
    synchronize: {
      fileEvents: [
        vscode.workspace.createFileSystemWatcher('**/*.kama'),
        vscode.workspace.createFileSystemWatcher('**/kama.json'),
        vscode.workspace.createFileSystemWatcher('**/kama.local.json'),
      ],
    },
  };
  client = new LanguageClient('kama', 'kama Language Server', serverOptions, clientOptions);
  // The server announces the build configuration it resolved, and re-announces it whenever a manifest
  // changes. Reading it here is what keeps the status bar honest — see refreshStatus.
  //
  // Registered BEFORE start(): the server sends its first `kama/buildConfig` immediately after the
  // initialize result, which can land before start()'s promise resolves. A handler added beforehand is
  // held in `_pendingNotificationHandlers` and installed with the connection, so nothing is missed.
  client.onNotification('kama/buildConfig', (cfg) => { lastConfig = cfg; refreshStatus(); });
  client.start();
}

function activate(context) {
  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  statusItem.command = 'kama.selectBuildConfig';
  context.subscriptions.push(
    statusItem,
    vscode.commands.registerCommand('kama.debugCurrentFile', debugCurrentFile),
    vscode.commands.registerCommand('kama.selectBuildConfig', selectBuildConfig),
    // The one honest answer to the one-configuration-per-process limit: re-pinning on tab switch would
    // evict the parse cache and re-analyze every open closure on every switch, so the user gets a button.
    vscode.commands.registerCommand('kama.restartServer', () => client && client.restart()),
    vscode.window.onDidChangeActiveTextEditor(refreshStatus)
  );
  startLanguageServer();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
