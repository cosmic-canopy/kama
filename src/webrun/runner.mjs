// kama run's browser runner (KR-377): serves a wasm build, runs it in a browser, and ends as the program ends.
//
//   node runner.mjs <page.html> <browser> [--headless] [--scale=<n>] [--window=<W>x<H>] -- [<program args>]
//
// and, when the project or the command line tells the browser anything or the program can ask for input,
// `browser.json` beside this file.
//
// The driver writes this file into the run's directory (it is embedded in the compiler), finds node and the browser,
// and passes the status this process exits with through as `kama run`'s. Only node's own modules are used, so
// nothing is installed and it runs on the node Emscripten already requires.
//
// The page reports over HTTP (src/webrun/pre.js), as emrun's does, and any browser that runs the page runs the program.
// The browser is a process started on the page's URL in a profile of its own, and ended after. An automation protocol
// is spoken only to a program that asks for input (`std::app::runner`, KR-378; `"input": true` in browser.json): the
// DevTools protocol over a pipe on Chromium, WebDriver BiDi on Firefox. Attaching either makes the page's
// `navigator.webdriver` true, so a program that cannot ask runs with neither.
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
// What the program's project or command line tells the browser beyond the flags below (`browser` in kama.json,
// `--chromium-arg`, `--firefox-arg`, `--firefox-pref`): the driver writes it here when there is any. Each list goes to
// the browser it names and no other, since which one runs is $KAMA_BROWSER's or kama's pick, not the caller's. Each comes
// after the runner's own, so where a browser keeps the last of a repeated setting, the caller's wins.
// `input` is the driver's: the program reaches `std::app::runner`, so the browser is attached to make its input.
const settingsFile = path.join(runDir, 'browser.json');
const settings = { chromiumArgs: [], firefoxArgs: [], firefoxPrefs: [], input: false,
                   ...(fs.existsSync(settingsFile) ? JSON.parse(fs.readFileSync(settingsFile, 'utf8')) : {}) };

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
  if (req.method === 'POST' && rel === '__kama_input' && settings.input) {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', d => body += d);
    req.on('end', () => {
      inputs = inputs.then(() => input(body)).then(answer => {
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify(answer));
      });
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

// ---- Input the program asks for (KR-378) ----
// A request is a list of steps the page's glue (lib/std/app/runner/kama_app_runner.js) sends as
// {"steps": [{k, ...}]}, and is answered {"ok": true} once the protocol has answered for its last step, or
// {"refused": "<why>"}. Requests are made one at a time, in the order they arrive. Positions are the viewport's CSS
// pixels. The steps:
//   move {p, id, x, y}, down {p, id, b}, up {p, id, b}   a pointer: p is mouse, pen or touch, id names a touch, b is
//                                                        the W3C button (0 left, 1 middle, 2 right, 3 back, 4 forward)
//   keydown {code}, keyup {code}                         a key, by its W3C KeyboardEvent.code on a US layout
//   text {text}                                          characters with no physical key, one grapheme at a time
//   wheel {x, y, dx, dy}                                 pixels
//   pause {ms}
// A key held or a button pressed stays so across requests until a step lets it go; the run's end lets everything go.

// Every code a step may name: what the DevTools protocol needs of each (key, its shifted key, the Windows virtual-key
// code, whether it types, where it sits, the modifier it is), and WebDriver's value for it, which BiDi takes. A
// printable key's value is its character, from which Firefox derives the code; the rest are WebDriver's key points.
// Puppeteer's US layout is the reference. Keys WebDriver has no value for (CapsLock, NumLock, PrintScreen,
// ContextMenu) are left out, so a request naming one is refused the same way on every browser.
const KEYS = {};
for (const c of 'abcdefghijklmnopqrstuvwxyz')
  KEYS['Key' + c.toUpperCase()] = { key: c, shift: c.toUpperCase(), vk: c.toUpperCase().charCodeAt(0), text: true, bidi: c };
'0123456789'.split('').forEach((d, i) =>
  KEYS['Digit' + d] = { key: d, shift: ')!@#$%^&*('[i], vk: 48 + i, text: true, bidi: d });
for (const [code, key, shift, vk] of [
  ['Backquote', '`', '~', 192], ['Minus', '-', '_', 189], ['Equal', '=', '+', 187], ['BracketLeft', '[', '{', 219],
  ['BracketRight', ']', '}', 221], ['Backslash', '\\', '|', 220], ['Semicolon', ';', ':', 186],
  ['Quote', '\'', '"', 222], ['Comma', ',', '<', 188], ['Period', '.', '>', 190], ['Slash', '/', '?', 191]])
  KEYS[code] = { key, shift, vk, text: true, bidi: key };
KEYS.Space = { key: ' ', vk: 32, text: true, bidi: '\uE00D' };
for (const [code, key, vk, bidi, more] of [
  ['Backspace', 'Backspace', 8, '\uE003'], ['Tab', 'Tab', 9, '\uE004'], ['Enter', 'Enter', 13, '\uE006', { text: '\r' }],
  ['Escape', 'Escape', 27, '\uE00C'], ['PageUp', 'PageUp', 33, '\uE00E'], ['PageDown', 'PageDown', 34, '\uE00F'],
  ['End', 'End', 35, '\uE010'], ['Home', 'Home', 36, '\uE011'], ['ArrowLeft', 'ArrowLeft', 37, '\uE012'],
  ['ArrowUp', 'ArrowUp', 38, '\uE013'], ['ArrowRight', 'ArrowRight', 39, '\uE014'],
  ['ArrowDown', 'ArrowDown', 40, '\uE015'], ['Insert', 'Insert', 45, '\uE016'], ['Delete', 'Delete', 46, '\uE017'],
  ['ShiftLeft', 'Shift', 16, '\uE008', { loc: 1, mod: 8 }], ['ShiftRight', 'Shift', 16, '\uE050', { loc: 2, mod: 8 }],
  ['ControlLeft', 'Control', 17, '\uE009', { loc: 1, mod: 2 }],
  ['ControlRight', 'Control', 17, '\uE051', { loc: 2, mod: 2 }],
  ['AltLeft', 'Alt', 18, '\uE00A', { loc: 1, mod: 1 }], ['AltRight', 'Alt', 18, '\uE052', { loc: 2, mod: 1 }],
  ['MetaLeft', 'Meta', 91, '\uE03D', { loc: 1, mod: 4 }], ['MetaRight', 'Meta', 92, '\uE053', { loc: 2, mod: 4 }],
  ['NumpadEnter', 'Enter', 13, '\uE007', { loc: 3, text: '\r' }], ['NumpadMultiply', '*', 106, '\uE024', { loc: 3, text: true }],
  ['NumpadAdd', '+', 107, '\uE025', { loc: 3, text: true }], ['NumpadSubtract', '-', 109, '\uE027', { loc: 3, text: true }],
  ['NumpadDecimal', '.', 110, '\uE028', { loc: 3, text: true }], ['NumpadDivide', '/', 111, '\uE029', { loc: 3, text: true }],
  ...Array.from({ length: 10 }, (_, i) => ['Numpad' + i, String(i), 96 + i, String.fromCharCode(0xE01A + i), { loc: 3, text: true }]),
  ...Array.from({ length: 12 }, (_, i) => ['F' + (i + 1), 'F' + (i + 1), 112 + i, String.fromCharCode(0xE031 + i)])])
  KEYS[code] = { key, vk, bidi, ...more };

const KINDS = new Set(['move', 'down', 'up', 'keydown', 'keyup', 'text', 'wheel', 'pause']);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const graphemes = t => Array.from(new Intl.Segmenter().segment(t), g => g.segment);

// Every step is checked before the first is made, so a request that names what no browser can make leaves no half of
// itself behind. A step the browser itself cannot make (Firefox's pen) is refused when it is made, with its reason.
function check(steps) {
  if (!Array.isArray(steps)) return 'the request holds no list of steps';
  for (const s of steps) {
    if (!s || !KINDS.has(s.k)) return 'no step is ' + JSON.stringify(s && s.k);
    if ((s.k === 'keydown' || s.k === 'keyup') && !Object.hasOwn(KEYS, s.code))
      return 'no key has the code ' + JSON.stringify(s.code) + ' (a W3C KeyboardEvent.code, on a US layout)';
    if ((s.k === 'down' || s.k === 'up') && s.p === 'touch' && s.b !== 0) return 'a touch presses only its contact, Left';
  }
}

// The protocol client, attached once the browser is up, as a function that makes one step. A request that arrives
// before the attach waits for it: the page loads before the protocol is attached, and answering such a request at once
// failed every early step in the probe's first Firefox run.
let inputs = Promise.resolve();
let attached = null;
async function input(body) {
  let steps;
  try { steps = JSON.parse(body).steps; } catch { return { refused: 'the request is not JSON' }; }
  const bad = check(steps);
  if (bad) return { refused: bad };
  const make = await attached;
  try { await make(steps); return { ok: true }; }
  catch (e) { return { refused: String(e.message || e) }; }
}

// Chromium: the DevTools protocol over --remote-debugging-pipe, NUL-framed JSON on the browser's fds 3 (to it) and
// 4 (from it), so no port is opened. The protocol keeps no input state, so the runner keeps it: each pointer's place,
// the buttons it holds and its click count, the touches down, and the keys held, whose modifiers every event carries.
async function attachChromium(origin, deadline) {
  const to = browserProc.stdio[3], from = browserProc.stdio[4];
  to.on('error', () => {});    // the browser ended: the run ends on its 'exit'
  from.on('error', () => {});
  const waiting = new Map();
  let id = 0, buf = '';
  from.setEncoding('utf8');
  from.on('data', d => {
    buf += d;
    for (let i = buf.indexOf('\0'); i >= 0; i = buf.indexOf('\0')) {
      const m = JSON.parse(buf.slice(0, i));
      buf = buf.slice(i + 1);
      const w = m.id && waiting.get(m.id);
      if (w) { waiting.delete(m.id); m.error ? w.no(new Error(m.error.message)) : w.yes(m.result); }
    }
  });
  const send = (method, params, sessionId) => new Promise((yes, no) => {
    waiting.set(++id, { yes, no });
    to.write(JSON.stringify({ id, method, params, sessionId }) + '\0');
  });
  let target;
  while (!(target = (await send('Target.getTargets', {})).targetInfos.find(t => t.type === 'page' && t.url.startsWith(origin)))) {
    if (Date.now() > deadline) throw new Error('its page never appeared to the DevTools protocol');
    await sleep(25);
  }
  const { sessionId } = await send('Target.attachToTarget', { targetId: target.targetId, flatten: true });
  const S = (method, params) => send('Input.' + method, params, sessionId);

  const held = new Set();
  const mods = () => { let m = 0; for (const c of held) m |= KEYS[c].mod || 0; return m; };
  const NAME = ['left', 'middle', 'right', 'back', 'forward'], BIT = [1, 4, 2, 8, 16];
  const pointers = { mouse: { x: 0, y: 0, buttons: 0, count: 0, last: null }, pen: { x: 0, y: 0, buttons: 0, count: 0, last: null } };
  const touches = new Map();   // id -> { x, y, down }
  const touchPoints = () => [...touches].filter(([, t]) => t.down).map(([id, t]) => ({ id, x: t.x, y: t.y }));
  return async steps => { for (const s of steps) await (s.k === 'pause' ? sleep(s.ms) : step(s)); };
  async function step(s) {
    if (s.p === 'touch' && (s.k === 'move' || s.k === 'down' || s.k === 'up')) {
      // The protocol takes every touch down, and makes a press, move or release of each that changed since the last.
      const t = touches.get(s.id) || { x: 0, y: 0, down: false };
      touches.set(s.id, t);
      if (s.k === 'move') { t.x = s.x; t.y = s.y; if (!t.down) return; }
      else t.down = s.k === 'down';
      const points = touchPoints();
      return S('dispatchTouchEvent', { type: !points.length ? 'touchEnd' : s.k === 'down' ? 'touchStart' : 'touchMove',
                                       touchPoints: points, modifiers: mods() });
    }
    if (s.k === 'move' || s.k === 'down' || s.k === 'up') {
      const p = pointers[s.p];
      let type = 'mouseMoved', button = NAME[BIT.findIndex(b => p.buttons & b)] || 'none';
      if (s.k === 'move') { p.x = s.x; p.y = s.y; }
      else {
        type = s.k === 'down' ? 'mousePressed' : 'mouseReleased';
        button = NAME[s.b];
        if (s.k === 'down') {
          // A press soon after another of the same button, in about the same place, counts on, as a desktop counts a
          // double click; BiDi counts its own.
          const now = Date.now(), l = p.last;
          p.count = l && l.b === s.b && now - l.at < 500 && Math.abs(l.x - p.x) <= 4 && Math.abs(l.y - p.y) <= 4 ? p.count + 1 : 1;
          p.last = { b: s.b, at: now, x: p.x, y: p.y };
          p.buttons |= BIT[s.b];
        } else p.buttons &= ~BIT[s.b];
      }
      return S('dispatchMouseEvent', { type, x: p.x, y: p.y, button, buttons: p.buttons, pointerType: s.p,
                                       modifiers: mods(), ...(type === 'mouseMoved' ? {} : { clickCount: p.count }) });
    }
    if (s.k === 'keydown' || s.k === 'keyup') {
      // An event carries the modifiers as they are once it has happened: Shift's own keydown says Shift, its keyup not.
      const K = KEYS[s.code];
      if (s.k === 'keydown') held.add(s.code); else held.delete(s.code);
      const m = mods(), key = m & 8 && K.shift ? K.shift : K.key;
      const p = { code: s.code, key, windowsVirtualKeyCode: K.vk, nativeVirtualKeyCode: K.vk, modifiers: m,
                  location: K.loc || 0, isKeypad: K.loc === 3 };
      if (s.k === 'keyup') return S('dispatchKeyEvent', { type: 'keyUp', ...p });
      // A key types its character unless Control, Alt or Meta is held, as on a desktop.
      const text = K.text && !(m & 7) ? (K.text === true ? key : K.text) : '';
      return S('dispatchKeyEvent', text ? { type: 'keyDown', ...p, text, unmodifiedText: K.text === true ? K.key : K.text }
                                        : { type: 'rawKeyDown', ...p });
    }
    if (s.k === 'text') {
      // A key event's text holds at most 3 UTF-16 units in Chromium (blink's kTextLengthCap less its terminator), so a
      // longer character (an emoji with a modifier) is typed a code point at a time; BiDi types it whole.
      for (const g of graphemes(s.text)) for (const u of g.length > 3 ? [...g] : [g]) {
        await S('dispatchKeyEvent', { type: 'keyDown', key: u, code: '', text: u, unmodifiedText: u, modifiers: mods() });
        await S('dispatchKeyEvent', { type: 'keyUp', key: u, code: '', modifiers: mods() });
      }
      return;
    }
    return S('dispatchMouseEvent', { type: 'mouseWheel', x: s.x, y: s.y, deltaX: s.dx, deltaY: s.dy, modifiers: mods() });
  }
}

// Firefox: WebDriver BiDi, on the port --remote-debugging-port=0 chose, which Firefox prints on its stderr. The runner
// holds the browser's one session, so no other process can drive it. BiDi keeps each input source's state itself,
// keyed by the source's id, and takes WebDriver's actions as they are.
async function attachFirefox(origin, deadline) {
  if (typeof WebSocket === 'undefined')
    throw new Error('Firefox is driven over WebDriver BiDi, whose socket needs node 22 or later (this is ' + process.version + ')');
  let line;
  while (!(line = /WebDriver BiDi listening on (ws:\/\/\S+)/.exec(browserSaid))) {
    if (Date.now() > deadline) throw new Error('it never said where WebDriver BiDi listens');
    await sleep(25);
  }
  const sock = new WebSocket(line[1].replace(/\/?$/, '/session'));
  await new Promise((yes, no) => { sock.onopen = yes; sock.onerror = () => no(new Error('its BiDi socket refused the runner')); });
  sock.onerror = () => {};     // the browser ended: the run ends on its 'exit'
  const waiting = new Map();
  let id = 0;
  sock.onmessage = ev => {
    const m = JSON.parse(ev.data), w = m.id && waiting.get(m.id);
    if (w) { waiting.delete(m.id); m.type === 'error' ? w.no(new Error(m.error + ': ' + m.message)) : w.yes(m.result); }
  };
  const send = (method, params) => new Promise((yes, no) => {
    waiting.set(++id, { yes, no });
    sock.send(JSON.stringify({ id, method, params }));
  });
  await send('session.new', { capabilities: {} });
  let context;
  while (!(context = (await send('browsingContext.getTree', {})).contexts.find(c => c.url.startsWith(origin)))) {
    if (Date.now() > deadline) throw new Error('its page never appeared to WebDriver BiDi');
    await sleep(25);
  }
  // A request is one action chain, as WebDriver's actions model has it: each step a tick (a text step one per key event)
  // on its source, every other source pausing, so Firefox counts a double click across steps as a desktop does.
  return steps => {
    const sources = new Map(), ticks = [];
    const tick = (id, type, parameters, action) => {
      if (!sources.has(id)) sources.set(id, { type, id, ...(parameters && { parameters }), actions: [] });
      ticks.push([id, action]);
    };
    for (const s of steps) {
      const pointer = s.p === 'touch' ? 'touch' + s.id : s.p;
      if (s.k === 'move') tick(pointer, 'pointer', { pointerType: s.p }, { type: 'pointerMove', x: s.x, y: s.y });
      else if (s.k === 'down' || s.k === 'up')
        tick(pointer, 'pointer', { pointerType: s.p }, { type: s.k === 'down' ? 'pointerDown' : 'pointerUp', button: s.b });
      else if (s.k === 'keydown' || s.k === 'keyup')
        tick('keyboard', 'key', null, { type: s.k === 'keydown' ? 'keyDown' : 'keyUp', value: KEYS[s.code].bidi });
      else if (s.k === 'text')
        for (const g of graphemes(s.text)) { tick('keyboard', 'key', null, { type: 'keyDown', value: g });
                                             tick('keyboard', 'key', null, { type: 'keyUp', value: g }); }
      else if (s.k === 'wheel') tick('wheel', 'wheel', null, { type: 'scroll', x: s.x, y: s.y, deltaX: s.dx, deltaY: s.dy });
      else tick('wait', 'none', null, { type: 'pause', duration: s.ms });
    }
    for (const [id, action] of ticks)
      for (const [other, source] of sources) source.actions.push(other === id ? action : { type: 'pause' });
    return send('input.performActions', { context: context.context, actions: [...sources.values()] });
  };
}

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
    remove();
  };
  // Windows refuses a file for a moment after the process that held it has ended (EPERM), as it did after every Firefox
  // run, for about 200 ms. The retries are this runner's own: emsdk's node (24.19) ignores rmSync's `maxRetries`, and
  // gave up on the first refusal.
  const remove = (tries = 50) => {
    try { fs.rmSync(runDir, { recursive: true, force: true }); }
    catch (e) {
      if (tries > 0) return setTimeout(() => remove(tries - 1), 100);
      process.stderr.write('kama run: the run\'s directory ' + runDir + ' was not removed (' + e.code + ')\n');
    }
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
    // Headless, Firefox starts no GPU process unless told to, and a page without one gets no WebGPU adapter where a
    // visible Firefox's page gets the GPU's. `--headless` is a page with no window, not a page with less.
    'user_pref("layers.gpu-process.allow-headless", true);',
    // `--scale`: Firefox takes a device scale as a pref, not on its command line.
    ...(scale ? ['user_pref("layout.css.devPixelsPerPx", "' + scale + '");'] : []),
    // Headless, the page's sound may start with no gesture: a browser holds an AudioContext suspended until the user
    // makes one, and a headless run has no user, so nothing could ever start it (KR-387). A run with a window keeps
    // the browser's policy, which is what the program's users get. Chromium's half is its `--autoplay-policy`, below.
    ...(headless ? ['user_pref("media.autoplay.default", 0);', 'user_pref("media.autoplay.block-webaudio", false);'] : []),
    // The caller's, last: user.js keeps the last value a pref is given. Each value is its literal, checked by the driver.
    ...settings.firefoxPrefs.map(([name, value]) => 'user_pref(' + JSON.stringify(name) + ', ' + JSON.stringify(value) + ');'),
  ].join('\n') + '\n');
  args = ['-profile', profile, '-no-remote', ...(headless ? ['-headless'] : []),
          ...(settings.input ? ['--remote-debugging-port=0'] : []),
          ...(width ? ['-width', width, '-height', height] : []), ...settings.firefoxArgs, url];
} else {
  // Chromium refuses to start as root without its sandbox switched off, which is how a container runs.
  const root = process.platform === 'linux' && process.getuid && process.getuid() === 0;
  args = ['--user-data-dir=' + profile, '--no-first-run', '--no-default-browser-check',
          ...(headless ? ['--headless=new', '--autoplay-policy=no-user-gesture-required'] : []),
          ...(root ? ['--no-sandbox'] : []),
          ...(settings.input ? ['--remote-debugging-pipe'] : []),
          ...(scale ? ['--force-device-scale-factor=' + scale] : []),
          ...(width ? ['--window-size=' + width + ',' + height] : []), ...settings.chromiumArgs, url];
}
// On Windows the browser leaves the console, so Ctrl-C reaches this runner alone, which ends the browser itself. A POSIX
// browser stays in the run's process group, so a signal sent to the group (a terminal's, `timeout`'s) reaches it too.
// The browser's own temporary files go in the run's directory too, and go with it: Chrome on Linux makes a
// `com.google.Chrome.XXXXXX` in $TMPDIR and leaves it there when it is ended.
const browserTmp = path.join(runDir, 'tmp');
fs.mkdirSync(browserTmp, { recursive: true });
browserProc = spawn(browser, args, {
  stdio: ['ignore', 'ignore', 'pipe', ...(settings.input && !firefox ? ['pipe', 'pipe'] : [])],
  detached: process.platform === 'win32',
  env: { ...process.env, TMPDIR: browserTmp, TMP: browserTmp, TEMP: browserTmp },
});
browserProc.stderr.setEncoding('utf8');
browserProc.stderr.on('data', d => { browserSaid = (browserSaid + d).slice(-8192); });
browserProc.on('error', e => { browserProc = null; finish(2, 'cannot start ' + browser + ': ' + e.message); });
// A browser that cannot be attached ends the run as one that cannot start does: the program's requests would wait for
// ever. The limit is the page's own (below), since a cold start can be as slow.
if (settings.input)
  attached = (firefox ? attachFirefox : attachChromium)('http://' + host + '/' + token + '/', Date.now() + 60000).catch(e => {
    finish(2, 'cannot attach to the browser (' + browser + ') to make the program\'s input: ' + (e.message || e) + lastWords());
    return new Promise(() => {});
  });
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
