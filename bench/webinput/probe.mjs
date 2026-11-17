// KR-378 probe (docs/ROADMAP_DETAIL.md `s10-trusted-input`): the page asks for trusted input over fetch; this runner
// dispatches it over the browser's own automation protocol (CDP on Chromium, WebDriver BiDi on Firefox) and answers
// when the protocol says it is done. No npm: node 22+ for its built-in WebSocket.
// Usage: node probe.mjs <browser> [--headless] [--port] [--scale=1.5] [--out=file.json] [-- <chromium args>]
//   then node summarize.mjs file.json. Serves page.html, which asks for each step in turn and reports what arrived.
//   Chromium: --remote-debugging-pipe by default (fds 3/4, NUL-framed JSON); --port uses --remote-debugging-port=0.
//   Firefox:  BiDi over --remote-debugging-port=0 (the port read from its stderr), always.
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const exe = process.argv[2];
const headless = process.argv.includes('--headless');
const usePort = process.argv.includes('--port');
const scale = (process.argv.find(a => a.startsWith('--scale=')) || '').slice(8);
const outFile = (process.argv.find(a => a.startsWith('--out=')) || '').slice(6);
const firefox = /firefox/i.test(path.basename(exe));
const extra = process.argv.includes('--') ? process.argv.slice(process.argv.indexOf('--') + 1) : [];
const t0 = Date.now();
const say = s => process.stderr.write(`[${String(Date.now() - t0).padStart(5)} ms] ${s}\n`);
const PAGE = fs.readFileSync(path.join(here, 'page.html'));

// ---- the steps, in an abstract form: the shape a std request would carry ----
const KEYS = {   // code -> what CDP needs, and BiDi's value
  KeyA: { key: 'a', shiftKey: 'A', vk: 65, text: true, bidi: 'a' },
  ShiftLeft: { key: 'Shift', vk: 16, loc: 1, bidi: '' },
  ShiftRight: { key: 'Shift', vk: 16, loc: 2, bidi: '' },
  Tab: { key: 'Tab', vk: 9, bidi: '' },
  Escape: { key: 'Escape', vk: 27, bidi: '' },
};
const click = (x, y) => [{ k: 'move', x, y }, { k: 'down' }, { k: 'up' }];
const tap = code => [{ k: 'keydown', code }, { k: 'keyup', code }];
const STEPS = {
  'click': click(200, 150),
  'key-a': tap('KeyA'),
  'key-shift-a': [{ k: 'keydown', code: 'ShiftLeft' }, ...tap('KeyA'), { k: 'keyup', code: 'ShiftLeft' }],
  'key-shiftright': tap('ShiftRight'),
  'key-tab': tap('Tab'),
  'wheel': [{ k: 'wheel', x: 200, y: 150, dx: 0, dy: 120 }],
  'wheel-blocking': [{ k: 'wheel', x: 200, y: 150, dx: 0, dy: 120 }],
  'touch': [{ k: 'tdown', x: 100, y: 100 }, { k: 'tmove', x: 125, y: 100 }, { k: 'tmove', x: 150, y: 100 },
            { k: 'tmove', x: 175, y: 100 }, { k: 'tup' }],
  get 'touch-none'() { return this.touch; },
  'pen': [{ k: 'pdown', x: 50, y: 50 }, { k: 'pup' }],
  'drag-out': [{ k: 'move', x: 300, y: 150 }, { k: 'down' }, { k: 'move', x: 380, y: 150 }, { k: 'move', x: 600, y: 150 },
               { k: 'up' }],
  'click-fs': click(200, 150),
  'click-audio': click(200, 150),
  'click-lock': click(200, 150),
  'move-locked': [{ k: 'move', x: 250, y: 150 }],
  'escape': tap('Escape'),
  'escape-late': tap('Escape'),
  'type-eacute': [{ k: 'text', ch: 'é' }],
  'click-lock-again': click(200, 150),
  'wait1600': [{ k: 'pause', ms: 1600 }],
  'click-lock-late': click(200, 150),
};

// ---- the browser ----
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kr378-'));
let proto;   // { send(method, params) -> Promise<result> }
let child;
let stderrTail = '';

function chromiumArgs(url) {
  return [`--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
          usePort ? '--remote-debugging-port=0' : '--remote-debugging-pipe',
          ...(headless ? ['--headless=new'] : []), ...(scale ? [`--force-device-scale-factor=${scale}`] : []),
          '--window-size=800,600', ...extra, url];
}

async function startChromium(url) {
  child = spawn(exe, chromiumArgs(url), { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'], detached: process.platform === 'win32' });
  child.stderr.on('data', d => { stderrTail = (stderrTail + d).slice(-4000); });
  let id = 0; const pending = new Map();
  const onMessage = m => {
    if (m.id && pending.has(m.id)) { const [res, rej] = pending.get(m.id); pending.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); }
  };
  let raw;
  if (usePort) {
    let wsUrl;
    for (let i = 0; i < 400 && !wsUrl; i++) {
      try { const [port, p] = fs.readFileSync(path.join(profile, 'DevToolsActivePort'), 'utf8').split('\n'); if (p) wsUrl = `ws://127.0.0.1:${port}${p}`; }
      catch { await new Promise(r => setTimeout(r, 25)); }
    }
    const ws = new WebSocket(wsUrl);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    ws.onmessage = ev => onMessage(JSON.parse(ev.data));
    raw = s => ws.send(s);
  } else {
    const w = child.stdio[3], r = child.stdio[4];
    let buf = '';
    r.setEncoding('utf8');
    r.on('data', d => { buf += d; let i; while ((i = buf.indexOf('\0')) >= 0) { const s = buf.slice(0, i); buf = buf.slice(i + 1); onMessage(JSON.parse(s)); } });
    raw = s => w.write(s + '\0');
  }
  const send = (method, params = {}, sessionId) => new Promise((res, rej) => { const i = ++id; pending.set(i, [res, rej]); raw(JSON.stringify({ id: i, method, params, sessionId })); });
  say(`protocol up (${usePort ? 'port' : 'pipe'})`);
  let target;
  for (let i = 0; i < 200 && !target; i++) {
    const { targetInfos } = await send('Target.getTargets');
    target = targetInfos.find(t => t.type === 'page' && t.url.startsWith('http://127.0.0.1'));
    if (!target) await new Promise(r => setTimeout(r, 25));
  }
  const { sessionId } = await send('Target.attachToTarget', { targetId: target.targetId, flatten: true });
  say(`attached to ${target.url}`);
  const S = (m, p) => send(m, p, sessionId);
  // State the protocol does not keep for us: the mouse's place and buttons, the held modifiers.
  let mx = 0, my = 0, buttons = 0, mods = 0;
  const MOD = { ShiftLeft: 8, ShiftRight: 8 };
  return {
    async run(acts) {
      for (const a of acts) {
        if (a.k === 'move') { mx = a.x; my = a.y; await S('Input.dispatchMouseEvent', { type: 'mouseMoved', x: mx, y: my, buttons, modifiers: mods, button: buttons ? 'left' : 'none' }); }
        else if (a.k === 'down') { buttons = 1; await S('Input.dispatchMouseEvent', { type: 'mousePressed', x: mx, y: my, button: 'left', buttons, clickCount: 1, modifiers: mods }); }
        else if (a.k === 'up') { buttons = 0; await S('Input.dispatchMouseEvent', { type: 'mouseReleased', x: mx, y: my, button: 'left', buttons, clickCount: 1, modifiers: mods }); }
        else if (a.k === 'pdown') await S('Input.dispatchMouseEvent', { type: 'mousePressed', x: a.x, y: a.y, button: 'left', buttons: 1, clickCount: 1, pointerType: 'pen' });
        else if (a.k === 'pup') await S('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 50, y: 50, button: 'left', buttons: 0, clickCount: 1, pointerType: 'pen' });
        else if (a.k === 'wheel') await S('Input.dispatchMouseEvent', { type: 'mouseWheel', x: a.x, y: a.y, deltaX: a.dx, deltaY: a.dy, modifiers: mods });
        else if (a.k === 'tdown') await S('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: a.x, y: a.y, id: 1 }] });
        else if (a.k === 'tmove') await S('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: a.x, y: a.y, id: 1 }] });
        else if (a.k === 'tup') await S('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        else if (a.k === 'pause') await new Promise(r => setTimeout(r, a.ms));
        else if (a.k === 'text') { await S('Input.dispatchKeyEvent', { type: 'keyDown', key: a.ch, text: a.ch, unmodifiedText: a.ch, code: '' }); await S('Input.dispatchKeyEvent', { type: 'keyUp', key: a.ch, code: '' }); }
        else if (a.k === 'keydown' || a.k === 'keyup') {
          const K = KEYS[a.code];
          if (a.k === 'keydown' && MOD[a.code]) mods |= MOD[a.code];
          const key = (mods & 8) && K.shiftKey ? K.shiftKey : K.key;
          const p = { code: a.code, key, windowsVirtualKeyCode: K.vk, nativeVirtualKeyCode: K.vk, modifiers: mods, location: K.loc || 0 };
          if (a.k === 'keydown') await S('Input.dispatchKeyEvent', K.text ? { type: 'keyDown', ...p, text: key, unmodifiedText: K.key } : { type: 'rawKeyDown', ...p });
          else await S('Input.dispatchKeyEvent', { type: 'keyUp', ...p });
          if (a.k === 'keyup' && MOD[a.code]) mods &= ~MOD[a.code];
        }
      }
    },
    async close() { try { await send('Browser.close'); } catch {} },
  };
}

async function startFirefox(url) {
  const prefs = {
    'browser.shell.checkDefaultBrowser': false, 'browser.startup.homepage_override.mstone': 'ignore',
    'browser.aboutwelcome.enabled': false, 'datareporting.policy.dataSubmissionPolicyBypassNotification': true,
    'toolkit.telemetry.reportingpolicy.firstRun': false, 'layers.gpu-process.allow-headless': true,
    ...(scale ? { 'layout.css.devPixelsPerPx': scale } : {}),
  };
  fs.writeFileSync(path.join(profile, 'user.js'), Object.entries(prefs).map(([k, v]) => `user_pref(${JSON.stringify(k)}, ${JSON.stringify(v)});\n`).join(''));
  child = spawn(exe, ['-profile', profile, '-no-remote', '--remote-debugging-port=0', ...(headless ? ['-headless'] : []),
                      '-width', '800', '-height', '600', url],
                { stdio: ['ignore', 'ignore', 'pipe'], detached: process.platform === 'win32' });
  let wsBase;
  const listening = new Promise(r => child.stderr.on('data', d => {
    stderrTail = (stderrTail + d).slice(-4000);
    const m = /WebDriver BiDi listening on (ws:\/\/\S+)/.exec(stderrTail);
    if (m && !wsBase) { wsBase = m[1]; r(); }
  }));
  await Promise.race([listening, new Promise(r => setTimeout(r, 20000))]);
  if (!wsBase) throw new Error('no BiDi line on stderr: ' + stderrTail);
  say(`BiDi at ${wsBase}`);
  const ws = new WebSocket(wsBase.replace(/\/?$/, '/session'));
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = e => j(new Error('ws error ' + (e.message || ''))); });
  let id = 0; const pending = new Map();
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const [res, rej] = pending.get(m.id); pending.delete(m.id); m.type === 'error' ? rej(new Error(m.error + ': ' + m.message)) : res(m.result); }
  };
  const send = (method, params = {}) => new Promise((res, rej) => { const i = ++id; pending.set(i, [res, rej]); ws.send(JSON.stringify({ id: i, method, params })); });
  const sess = await send('session.new', { capabilities: {} });
  say(`session ${sess.sessionId} ${sess.capabilities.browserName} ${sess.capabilities.browserVersion}`);
  // A second session, to see whether another client could take the browser while the runner holds it.
  try { const ws2 = new WebSocket(wsBase.replace(/\/?$/, '/session')); await new Promise((r, j) => { ws2.onopen = r; ws2.onerror = j; setTimeout(j, 3000); });
        ws2.onmessage = ev => say('second session.new: ' + String(ev.data).slice(0, 200));
        ws2.send(JSON.stringify({ id: 1, method: 'session.new', params: { capabilities: {} } })); }
  catch (e) { say('second connection refused: ' + (e.message || e.type || e)); }
  let context;
  for (let i = 0; i < 200 && !context; i++) {
    const { contexts } = await send('browsingContext.getTree', {});
    const c = contexts.find(c => c.url.startsWith('http://127.0.0.1'));
    if (c) context = c.context; else await new Promise(r => setTimeout(r, 25));
  }
  say(`context ${context}`);
  const BTN = 0;
  const one = (type, id, parameters, actions) => send('input.performActions', { context, actions: [{ type, id, ...(parameters ? { parameters } : {}), actions }] });
  return {
    async run(acts) {
      for (const a of acts) {
        if (a.k === 'move') await one('pointer', 'mouse', { pointerType: 'mouse' }, [{ type: 'pointerMove', x: a.x, y: a.y }]);
        else if (a.k === 'down') await one('pointer', 'mouse', { pointerType: 'mouse' }, [{ type: 'pointerDown', button: BTN }]);
        else if (a.k === 'up') await one('pointer', 'mouse', { pointerType: 'mouse' }, [{ type: 'pointerUp', button: BTN }]);
        else if (a.k === 'pdown') await one('pointer', 'pen', { pointerType: 'pen' }, [{ type: 'pointerMove', x: a.x, y: a.y }, { type: 'pointerDown', button: 0 }]);
        else if (a.k === 'pup') await one('pointer', 'pen', { pointerType: 'pen' }, [{ type: 'pointerUp', button: 0 }]);
        else if (a.k === 'wheel') await one('wheel', 'wheel', null, [{ type: 'scroll', x: a.x, y: a.y, deltaX: a.dx, deltaY: a.dy }]);
        else if (a.k === 'tdown') await one('pointer', 'finger', { pointerType: 'touch' }, [{ type: 'pointerMove', x: a.x, y: a.y }, { type: 'pointerDown', button: 0 }]);
        else if (a.k === 'tmove') await one('pointer', 'finger', { pointerType: 'touch' }, [{ type: 'pointerMove', x: a.x, y: a.y }]);
        else if (a.k === 'tup') await one('pointer', 'finger', { pointerType: 'touch' }, [{ type: 'pointerUp', button: 0 }]);
        else if (a.k === 'pause') await new Promise(r => setTimeout(r, a.ms));
        else if (a.k === 'text') await one('key', 'kb', null, [{ type: 'keyDown', value: a.ch }, { type: 'keyUp', value: a.ch }]);
        else if (a.k === 'keydown') await one('key', 'kb', null, [{ type: 'keyDown', value: KEYS[a.code].bidi }]);
        else if (a.k === 'keyup') await one('key', 'kb', null, [{ type: 'keyUp', value: KEYS[a.code].bidi }]);
      }
    },
    async close() { try { await send('browser.close', {}); } catch {} },
  };
}

// ---- the server the page asks ----
let browser, finish, isReady;
const ready = new Promise(r => isReady = r);   // a request that arrives before the protocol is attached waits
const finished = new Promise(r => finish = r);
const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  if (req.method === 'GET') { res.setHeader('content-type', 'text/html'); return res.end(PAGE); }
  if (u.pathname === '/req') {
    await ready;
    const step = u.searchParams.get('step'), t = Date.now();
    let body;
    try { await browser.run(STEPS[step]); body = { ok: true, ms: Date.now() - t }; }
    catch (e) { body = { ok: false, ms: Date.now() - t, err: String(e.message || e).slice(0, 300) }; }
    say(`${step}: ${JSON.stringify(body)}`);
    res.setHeader('content-type', 'application/json');
    return res.end(JSON.stringify(body));
  }
  if (u.pathname === '/done') {
    let s = ''; req.on('data', d => s += d); req.on('end', () => { res.end(); finish(JSON.parse(s)); });
    return;
  }
  res.statusCode = 404; res.end();
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/`;
try {
  browser = firefox ? await startFirefox(url) : await startChromium(url);
} catch (e) { say('START FAILED: ' + (e.stack || e)); say('stderr: ' + stderrTail); child && child.kill(); process.exit(2); }
isReady();
const timer = setTimeout(() => finish(null), 60000);
const result = await finished;
clearTimeout(timer);
say(result ? 'page done' : 'TIME LIMIT');
const record = { browser: exe, firefox, headless, transport: firefox ? 'bidi-ws' : (usePort ? 'cdp-ws' : 'cdp-pipe'), scale: scale || null, startedToDone: Date.now() - t0, ...(result || {}), stderrTail: result ? undefined : stderrTail };
if (outFile) fs.writeFileSync(outFile, JSON.stringify(record, null, 1));
else process.stdout.write(JSON.stringify(record, null, 1) + '\n');
await browser.close();
server.close();
setTimeout(() => { try { child.kill(); } catch {} try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 }); } catch {} process.exit(0); }, 1000);
