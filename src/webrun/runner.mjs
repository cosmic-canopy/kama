// kama run's browser runner (KR-377): serves a wasm build, runs it in a browser, and ends as the program ends.
//
//   node runner.mjs <page.html> <browser> [--headless] [--scale=<n>] [--window=<W>x<H>] -- [<program args>]
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
import { spawn, spawnSync } from 'node:child_process';

const argv = process.argv.slice(2);
const dash = argv.indexOf('--');
const [page, browser, ...opts] = dash < 0 ? argv : argv.slice(0, dash);
const progArgs = dash < 0 ? [] : argv.slice(dash + 1);
const option = name => { const o = opts.find(a => a === name || a.startsWith(name + '=')); return o && o.slice(name.length + 1); };
const headless = option('--headless') !== undefined;
const scale = option('--scale');                                   // the page's devicePixelRatio, e.g. "1.5"
const [width, height] = (option('--window') || '').split('x');      // the window's size, the driver checked as WxH
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
let status = null, browserProc = null, requested = false;
// What the browser says is kept, and shown only if the run ends on its account: a Chromium that cannot start its sandbox
// (a downloaded build on Ubuntu 24.04 or later, which restricts the user namespaces it needs) says why there.
let browserSaid = '';
function lastWords() {
  const said = browserSaid.trim();
  return said ? '; it said:\n' + said.split('\n').slice(-12).map(l => '  ' + l).join('\n') : '';
}

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
  requested = true;
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

// The processes the browser started that are still running. Each carries the run's directory on its command line
// (Chromium's helpers their --user-data-dir, its crash handler a --database inside it; Firefox's its -profile), and the
// browser's main process can end before them: a Chrome helper outliving it on Linux wrote into the run's directory
// after it was removed and made it again (CI #481). Read from /proc on Linux and from `ps` on macOS. Windows needs
// neither: kama holds the run in a job, and the removal's retries wait out its locks.
function stragglers() {
  try {
    if (process.platform === 'linux')
      return fs.readdirSync('/proc').filter(p => /^\d+$/.test(p) && +p !== process.pid).filter(p => {
        try { return fs.readFileSync('/proc/' + p + '/cmdline', 'utf8').includes(runDir); } catch { return false; }
      }).map(Number);
    if (process.platform === 'darwin')
      return spawnSync('ps', ['-axww', '-o', 'pid=,command='], { encoding: 'utf8' }).stdout.split('\n')
        .filter(l => l.includes(runDir)).map(l => parseInt(l, 10)).filter(p => p && p !== process.pid);
  } catch { /* no listing: nothing to wait for */ }
  return [];
}

// The browser is ended with the run, and the run's directory, its profile included, goes with it, once every process
// the browser started has gone (waited for up to 5 s, then killed). On Windows the profile's files stay locked a
// moment after the browser's processes exit, so the removal retries.
let tornDown = false;
function teardown() {
  if (tornDown) return;
  tornDown = true;
  const done = (waits = 100) => {
    const left = stragglers();
    if (left.length && waits > 0) return setTimeout(() => done(waits - 1), 50);
    for (const p of left) { try { process.kill(p, 'SIGKILL'); } catch { /* gone */ } }
    server.close();
    try { fs.rmSync(runDir, { recursive: true, force: true, maxRetries: 20, retryDelay: 150 }); }
    catch (e) { process.stderr.write('kama run: the run\'s directory ' + runDir + ' was not removed (' + e.code + ')\n'); }
    // The program's last lines may still be queued: node writes to a Windows console and to a macOS pipe
    // asynchronously, and exiting drops what has not gone. An empty write's callback runs once all before it has.
    process.stdout.write('', () => process.stderr.write('', () => process.exit(status)));
  };
  if (!browserProc || browserProc.exitCode !== null || browserProc.signalCode !== null) return done();
  browserProc.once('exit', () => done());
  browserProc.kill();
  setTimeout(() => browserProc.kill('SIGKILL'), 5000).unref();
}

// Ctrl-C, or a harness ending the run: the program is ended as a native one would be, with the shell's status for it.
// One that ends the run before the browser has asked for the page says so: that is a browser that lost its navigation, or
// one slower to start than the harness's limit, and either is worth the harness's log.
for (const [sig, code] of [['SIGINT', 130], ['SIGTERM', 143], ['SIGHUP', 129]])
  process.on(sig, () => finish(code, requested || !browserProc ? undefined
                                   : 'ended before the browser (' + browser + ') asked for the page' + lastWords()));

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
    // `--scale`: Firefox takes a device scale as a pref, not on its command line.
    ...(scale ? ['user_pref("layout.css.devPixelsPerPx", "' + scale + '");'] : []),
  ].join('\n') + '\n');
  args = ['-profile', profile, '-no-remote', ...(headless ? ['-headless'] : []),
          ...(width ? ['-width', width, '-height', height] : []), url];
} else {
  // Chromium refuses to start as root without its sandbox switched off, which is how a container runs.
  const root = process.platform === 'linux' && process.getuid && process.getuid() === 0;
  args = ['--user-data-dir=' + profile, '--no-first-run', '--no-default-browser-check',
          ...(headless ? ['--headless=new'] : []), ...(root ? ['--no-sandbox'] : []),
          ...(scale ? ['--force-device-scale-factor=' + scale] : []),
          ...(width ? ['--window-size=' + width + ',' + height] : []), url];
}
// On Windows the browser leaves the console, so Ctrl-C reaches this runner alone, which ends the browser itself. A POSIX
// browser stays in the run's process group, so a signal sent to the group (a terminal's, `timeout`'s) reaches it too.
// The browser's own temporary files go in the run's directory too, and go with it: Chrome on Linux makes a
// `com.google.Chrome.XXXXXX` in $TMPDIR and leaves it there when it is ended.
const browserTmp = path.join(runDir, 'tmp');
fs.mkdirSync(browserTmp, { recursive: true });
browserProc = spawn(browser, args, {
  stdio: ['ignore', 'ignore', 'pipe'], detached: process.platform === 'win32',
  env: { ...process.env, TMPDIR: browserTmp, TMP: browserTmp, TEMP: browserTmp },
});
browserProc.stderr.setEncoding('utf8');
browserProc.stderr.on('data', d => { browserSaid = (browserSaid + d).slice(-8192); });
browserProc.on('error', e => { browserProc = null; finish(2, 'cannot start ' + browser + ': ' + e.message); });
// A browser that ends before the program did was closed, or never started: the run ends saying so, with its last words.
browserProc.on('exit', () => finish(129, 'the browser ended before the program did' + lastWords()));
// A browser that starts and never asks for the page would leave the run waiting, silent, until a harness ended it: a
// Chrome for Testing unzipped rather than installed loses its first navigation when its network service crashes at
// start. A browser that has asked for nothing in 60 s is reported by name, and the run ends as a browser that cannot
// start does. The limit is generous because a cold start can be slow: headless Firefox on CI's Ubuntu image took longer
// than 30 s once. A harness that ends the run sooner hears the same thing (the signal handlers above). The navigation is
// not retried: a browser that lost it is broken, and says so in its last words.
setTimeout(() => {
  if (!requested) finish(2, 'the browser (' + browser + ') never asked for the page ' + url + ' in 60 s' + lastWords());
}, 60000).unref();
