// Playwright browser runner for wasm fixtures that need a real browser (WebTransport / WebRTC / WebSocket-
// in-browser) — node can't run those transports. Serves <base>.js + <base>.wasm behind a minimal HTML that
// hooks Module.onExit to capture the wasm program's exit code, launches headless Chromium, and exits with
// that code (so run_tests.sh compares it like a native/node run). Usage: node browser_run.js <app.js>.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const appJs = process.argv[2];
if (!appJs) { console.error('usage: browser_run.js <app.js>'); process.exit(2); }
const dir = path.dirname(path.resolve(appJs));
const base = path.basename(appJs);

// Predefine Module so the emscripten glue adopts our hooks (it does `Module = typeof Module ... : {}`).
const HTML = '<!doctype html><meta charset=utf-8><body><script>' +
  'window.__kamaExit = undefined;' +
  'var Module = {' +
  '  onExit:  function(c){ if (window.__kamaExit === undefined) window.__kamaExit = c|0; },' +
  '  onAbort: function(w){ window.__kamaExit = 134; },' +
  '  print:   function(s){ console.log("[out] " + s); },' +
  '  printErr:function(s){ console.log("[err] " + s); }' +
  '};' +
  '</script><script src="' + base + '"></script></body>';

const server = http.createServer((req, res) => {
  const p = req.url.split('?')[0];
  if (p === '/' || p === '/index.html') { res.setHeader('content-type', 'text/html'); return res.end(HTML); }
  const f = path.join(dir, path.basename(p));
  fs.readFile(f, (e, buf) => {
    if (e) { res.statusCode = 404; return res.end('not found'); }
    if (f.endsWith('.wasm')) res.setHeader('content-type', 'application/wasm');
    else if (f.endsWith('.js')) res.setHeader('content-type', 'text/javascript');
    res.end(buf);
  });
});

(async () => {
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  const browser = await chromium.launch({ args: ['--no-sandbox', '--disable-dev-shm-usage'] });
  let code = 124;
  try {
    const page = await browser.newPage();
    page.on('console', (m) => process.stderr.write('[console] ' + m.text() + '\n'));
    page.on('pageerror', (e) => process.stderr.write('[pageerror] ' + e.message + '\n'));
    await page.goto('http://127.0.0.1:' + port + '/', { waitUntil: 'load' });
    await page.waitForFunction('window.__kamaExit !== undefined', { timeout: 20000 });
    code = await page.evaluate('window.__kamaExit');
  } catch (e) {
    process.stderr.write('[runner] ' + e.message + '\n');
  } finally {
    await browser.close();
    server.close();
  }
  process.exit(code);
})();
