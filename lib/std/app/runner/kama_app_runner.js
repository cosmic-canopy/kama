// std::app::runner's JS glue (KR-378): the bodies of kama_app_runner.h, linked into a wasm program by emcc
// (--js-library) when it reaches std::app::runner. A request is built a step at a time, sent to the runner that served
// the page (src/webrun/runner.mjs, `POST <token>/__kama_input`), and polled by handle: 0 Pending, 1 Done, 2 Refused.
//
// Only a `kama run` build has a runner: its pre-js (src/webrun/pre.js) sets Module['kamaRunner']. Without it (a
// deployed page, node, a plain `kama build`) `begin` answers -1, which the program hears as NoRunner.
//
// Done means the runner has answered for the last step and the page has drawn one frame since: Chromium answers a wheel
// under a passive listener before its event lands, within the next frame (measured; docs/ROADMAP_DETAIL.md
// `s10-trusted-input`). Each change of state wakes std::app's loop, so an app on demand hears it.
//
// Every key that crosses into JSON is quoted: a release build's Closure renames an unquoted one.

mergeInto(LibraryManager.library, {
  $KamaRunner: {
    reqs: [],
    sent: null,   // the last request sent: each is sent once the one before it is answered, so they run in order
    // std::app's wake, as KamaNet's (kama_net_web.js): never after the runtime has ended.
    wake: function() {
      if (ABORT || (typeof runtimeExited != 'undefined' && runtimeExited)) return;
      if (Module['_kama_app_wake']) Module['_kama_app_wake']();
    },
    settle: function(r, state, reason) {
      if (r.dropped) return;   // the program stopped hearing it; the steps were still made
      r.state = state; r.reason = reason;
      KamaRunner.wake();
    }
  },

  kama_app_runner_begin__deps: ['$KamaRunner'],
  kama_app_runner_begin: function() {
    if (!Module['kamaRunner']) return -1;
    KamaRunner.reqs.push({ steps: [], state: 0, reason: '', dropped: false });
    return KamaRunner.reqs.length - 1;
  },

  // kind: 0 move, 1 press, 2 release, 3 key down, 4 key up, 5 text, 6 wheel, 7 pause (x is its milliseconds).
  // pointer: 0 mouse, 1 pen, 2 touch (id names the touch). text is a key's code, or the text a text step types.
  kama_app_runner_step__deps: ['$KamaRunner', '$UTF8ToString'],
  kama_app_runner_step: function(h, kind, pointer, id, button, x, y, dx, dy, text) {
    var r = KamaRunner.reqs[h]; if (!r || !r.steps) return;
    var p = ['mouse', 'pen', 'touch'][pointer];
    var s;
    if (kind === 0)      s = { 'k': 'move', 'p': p, 'id': id, 'x': x, 'y': y };
    else if (kind === 1) s = { 'k': 'down', 'p': p, 'id': id, 'b': button };
    else if (kind === 2) s = { 'k': 'up', 'p': p, 'id': id, 'b': button };
    else if (kind === 3) s = { 'k': 'keydown', 'code': UTF8ToString(text) };
    else if (kind === 4) s = { 'k': 'keyup', 'code': UTF8ToString(text) };
    else if (kind === 5) s = { 'k': 'text', 'text': UTF8ToString(text) };
    else if (kind === 6) s = { 'k': 'wheel', 'x': x, 'y': y, 'dx': dx, 'dy': dy };
    else                 s = { 'k': 'pause', 'ms': x };
    r.steps.push(s);
  },

  kama_app_runner_send__deps: ['$KamaRunner'],
  kama_app_runner_send: function(h) {
    var r = KamaRunner.reqs[h]; if (!r || !r.steps) return;
    var body = JSON.stringify({ 'steps': r.steps });
    r.steps = null;
    KamaRunner.sent = (KamaRunner.sent || Promise.resolve()).then(function() {
      return fetch('__kama_input', { method: 'POST', body: body }).then(function(res) { return res.json(); });
    }).then(function(a) {
      if (a['ok']) requestAnimationFrame(function() { KamaRunner.settle(r, 1, ''); });
      else KamaRunner.settle(r, 2, String(a['refused']));
    }, function(e) { KamaRunner.settle(r, 2, 'the runner did not answer: ' + e); });
  },

  kama_app_runner_state__deps: ['$KamaRunner'],
  kama_app_runner_state: function(h) {
    var r = KamaRunner.reqs[h];
    return r ? r.state : 2;
  },

  // A refusal's reason: its FULL byte length, with up to n bytes of it copied into buf; the caller asks again with
  // room when it is longer (the grow-and-retry protocol kama_id_name uses).
  kama_app_runner_reason__deps: ['$KamaRunner'],
  kama_app_runner_reason: function(h, buf, n) {
    var r = KamaRunner.reqs[h]; if (!r) return 0;
    var bytes = new TextEncoder().encode(r.reason);
    HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, n)), buf);
    return bytes.length;
  },

  kama_app_runner_drop__deps: ['$KamaRunner'],
  kama_app_runner_drop: function(h) {
    var r = KamaRunner.reqs[h];
    if (r) { r.dropped = true; KamaRunner.reqs[h] = null; }
  }
});
