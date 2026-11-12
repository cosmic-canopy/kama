// kama run's browser runner (KR-377): serves a wasm build, runs it in a browser, and ends as the program ends.
//
//   node runner.mjs <page.html> <browser> <headless: 0|1> [-- <program args>]
//
// The driver writes this file into the run's directory (it is embedded in the compiler), finds node and the browser,
// and passes the status this process exits with through as `kama run`'s. Only node's own modules are used, so
// nothing is installed and it runs on the node Emscripten already requires.
//
// The page reports over HTTP (src/webrun/pre.js), as emrun's does, so no automation protocol is spoken and any
// browser that runs the page runs the program. The browser is a process started on the page's URL in a profile of its
// own, and ended after.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';

const argv = process.argv.slice(2);
const dash = argv.indexOf('--');
const [page, browser, headless] = dash < 0 ? argv : argv.slice(0, dash);
const progArgs = dash < 0 ? [] : argv.slice(dash + 1);
// The run's directory, which this runner removes as it ends: the page under `page/`, this file, the pre-js, and the
// browser's profile.
const pageDir = path.dirname(path.resolve(page));
const runDir = path.dirname(pageDir);
const profile = path.join(runDir, 'profile');

// A request must name the run's token, and only 127.0.0.1 is listened on. The Host check refuses a page of another
// origin that rebinds its own name to 127.0.0.1, which could otherwise read the build.
const token = crypto.randomBytes(16).toString('hex');
let host = '';

// How the program ended, as the status this process exits with.
let status = null, browserProc = null;

function finish(code, why) {
  if (status !== null) return;
  status = code;
  if (why) process.stderr.write('kama run: ' + why + '\n');
  teardown();
}

// The page's reports, a batch of [kind, text] items per post, printed in the order they were sent: an asynchronous post
// can overtake an earlier one.
const pending = new Map();
let next = 0;
function report(m) {
  pending.set(m.seq, m.items);
  while (pending.has(next) && status === null) {
    const items = pending.get(next);
    pending.delete(next++);
    for (const [kind, text] of items) {
      if (status !== null) break;
      if (kind === 'out') process.stdout.write(text + '\n');
      else if (kind === 'err') process.stderr.write(text + '\n');
      else if (kind === 'exit') finish(Number(text) & 255);
      else if (kind === 'abort') finish(134);   // a native abort's status (128 + SIGABRT); emscripten printed why
      else if (kind === 'error') { process.stderr.write(text + '\n'); finish(1); }   // emscripten's own status for it
      else if (kind === 'closed') finish(129, 'the page was closed before the program ended');
    }
  }
}

// A build that shares its memory (threads, wasm workers) needs SharedArrayBuffer, which a browser gives only to a
// cross-origin isolated page; any other page is served without COOP and COEP, as its deployment would be, since
// `require-corp` refuses a cross-origin font or script the page loads. The answer is read from the module itself: the
// `shared` bit of the memory it imports or defines (the wasm binary format's limits flags). A SINGLE_FILE build has no
// .wasm beside its page, and is served unisolated.
function sharesMemory(file) {
  let b;
  try { b = fs.readFileSync(file); } catch { return false; }
  let p = 8;
  const leb = () => { let r = 0, s = 0, x; do { x = b[p++]; r += (x & 0x7f) * 2 ** s; s += 7; } while (x & 0x80); return r; };
  const name = () => { const n = leb(); p += n; };
  const refType = () => { const t = b[p++]; if (t === 0x63 || t === 0x64) leb(); };   // (ref null? ht) takes a heap type
  const limits = () => { const f = b[p++]; leb(); if (f & 1) leb(); return (f & 2) !== 0; };
  while (p < b.length) {
    const id = b[p++], size = leb(), end = p + size;
    if (id === 2) {                                    // imports
      for (let n = leb(); n > 0; n--) {
        name(); name();
        const kind = b[p++];
        if (kind === 0) leb();                         // a function: its type
        else if (kind === 1) { refType(); limits(); }  // a table
        else if (kind === 2) { if (limits()) return true; }
        else if (kind === 3) { refType(); p++; }       // a global: its type and mutability
        else if (kind === 4) { p++; leb(); }           // a tag
        else return false;
      }
    } else if (id === 5) {                             // memories
      for (let n = leb(); n > 0; n--) if (limits()) return true;
    }
    p = end;
  }
  return false;
}
const isolate = sharesMemory(page.replace(/\.html$/, '.wasm'));

const types = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.json': 'application/json', '.css': 'text/css', '.txt': 'text/plain', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp',
  '.wav': 'audio/wav', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg', '.ttf': 'font/ttf', '.woff2': 'font/woff2',
};

const server = http.createServer((req, res) => {
  const prefix = '/' + token + '/';
  if (req.headers.host !== host || !req.url.startsWith(prefix)) { res.statusCode = 403; return res.end(); }
  let rel;
  try { rel = decodeURIComponent(req.url.slice(prefix.length).split('?')[0]); }
  catch { res.statusCode = 400; return res.end(); }
  if (req.method === 'POST' && rel === '__kama_run') {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', d => body += d);
    req.on('end', () => {
      res.end();
      try { report(JSON.parse(body)); } catch { /* not the page's report */ }
    });
    return;
  }
  const file = path.resolve(pageDir, rel);
  if (!file.startsWith(pageDir + path.sep)) { res.statusCode = 404; return res.end(); }
  fs.readFile(file, (e, buf) => {
    if (e) { res.statusCode = 404; return res.end(); }
    res.setHeader('content-type', types[path.extname(file).toLowerCase()] || 'application/octet-stream');
    res.setHeader('cache-control', 'no-store');
    if (isolate) {
      res.setHeader('cross-origin-opener-policy', 'same-origin');
      res.setHeader('cross-origin-embedder-policy', 'require-corp');
    }
    res.end(buf);
  });
});

// The browser is ended with the run, and the run's directory, its profile included, goes with it. On Windows the
// profile's files stay locked a moment after the browser's processes exit, so the removal retries.
let tornDown = false;
function teardown() {
  if (tornDown) return;
  tornDown = true;
  const done = () => {
    server.close();
    try { fs.rmSync(runDir, { recursive: true, force: true, maxRetries: 20, retryDelay: 150 }); }
    catch (e) { process.stderr.write('kama run: the run\'s directory ' + runDir + ' was not removed (' + e.code + ')\n'); }
    // The program's last lines may still be queued: node writes to a Windows console and to a macOS pipe
    // asynchronously, and exiting drops what has not gone. An empty write's callback runs once all before it has.
    process.stdout.write('', () => process.stderr.write('', () => process.exit(status)));
  };
  if (!browserProc || browserProc.exitCode !== null || browserProc.signalCode !== null) return done();
  browserProc.once('exit', done);
  browserProc.kill();
  setTimeout(() => browserProc.kill('SIGKILL'), 5000).unref();
}

// Ctrl-C, or a harness ending the run: the program is ended as a native one would be, with the shell's status for it.
for (const [sig, code] of [['SIGINT', 130], ['SIGTERM', 143], ['SIGHUP', 129]]) process.on(sig, () => finish(code));

await new Promise(r => server.listen(0, '127.0.0.1', r));
host = '127.0.0.1:' + server.address().port;
const query = progArgs.length ? '?' + progArgs.map(encodeURIComponent).join('&') : '';
const url = 'http://' + host + '/' + token + '/' + encodeURIComponent(path.basename(page)) + query;

fs.mkdirSync(profile, { recursive: true });
const firefox = /firefox/i.test(path.basename(browser));
let args;
if (firefox) {
  // A fresh Firefox profile opens its own first-run pages beside the program's unless told not to.
  fs.writeFileSync(path.join(profile, 'user.js'), [
    'user_pref("browser.shell.checkDefaultBrowser", false);',
    'user_pref("browser.startup.homepage_override.mstone", "ignore");',
    'user_pref("browser.aboutwelcome.enabled", false);',
    'user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);',
    'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);',
  ].join('\n') + '\n');
  args = ['-profile', profile, '-no-remote', ...(headless === '1' ? ['-headless'] : []), url];
} else {
  // Chromium refuses to start as root without its sandbox switched off, which is how a container runs.
  const root = process.platform === 'linux' && process.getuid && process.getuid() === 0;
  args = ['--user-data-dir=' + profile, '--no-first-run', '--no-default-browser-check',
          ...(headless === '1' ? ['--headless=new'] : []), ...(root ? ['--no-sandbox'] : []), url];
}
// On Windows the browser leaves the console, so Ctrl-C reaches this runner alone, which ends the browser itself. A POSIX
// browser stays in the run's process group, so a signal sent to the group (a terminal's, `timeout`'s) reaches it too.
// The browser's own temporary files go in the run's directory too, and go with it: Chrome on Linux makes a
// `com.google.Chrome.XXXXXX` in $TMPDIR and leaves it there when it is ended.
const browserTmp = path.join(runDir, 'tmp');
fs.mkdirSync(browserTmp, { recursive: true });
browserProc = spawn(browser, args, {
  stdio: 'ignore', detached: process.platform === 'win32',
  env: { ...process.env, TMPDIR: browserTmp, TMP: browserTmp, TEMP: browserTmp },
});
browserProc.on('error', e => { browserProc = null; finish(2, 'cannot start ' + browser + ': ' + e.message); });
// A browser that ends before the program did was closed (or crashed): the run ends saying so.
browserProc.on('exit', () => finish(129, 'the browser ended before the program did'));
