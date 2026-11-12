// kama run's side of a browser run (KR-377), linked with `--pre-js` into a `kama run` build only, never into a
// `kama build`. It reports the program to the runner that served the page (src/webrun/runner.mjs): each line the
// program prints, then how it ended. No automation protocol is spoken, so any browser that runs the page runs the
// program; this is emrun's shape (emscripten's src/emrun_postjs.js).
//
// Every hook ADDS to the page's own: a project's `shellFile` may define `Module.print`, `onExit` and the rest, and keeps
// them. A pre-js runs after the glue's `var Module = …` and before the glue reads `Module.arguments`, `print` and
// `printErr` (emscripten's src/shell.js), which is what lets it set the arguments and wrap the output.
(function () {
  // The page's main thread alone: under node there is no page, and a pthread's output reaches the main thread's hooks.
  if (!(globalThis.window && globalThis.document)) return;

  // `kama run -- a b`: the runner puts the arguments in the query, as emrun does (emscripten's src/emrun_prejs.js).
  if (location.search.length > 1)
    Module['arguments'] = location.search.slice(1).split('&').map(decodeURIComponent);

  // Posts are relative, so they reach the runner under the path it served the page from. An asynchronous post may
  // arrive before an earlier one, so each carries its sequence number and the runner prints them in order. The end is
  // posted synchronously: the runner has it before anything that follows can tear the page down.
  var seq = 0, ended = false;
  function post(kind, text, sync) {
    var x = new XMLHttpRequest();
    x.open('POST', '__kama_run', !sync);
    x.send(JSON.stringify({ seq: seq++, kind: kind, text: String(text) }));
  }
  function end(kind, text) {
    if (ended) return;
    ended = true;
    post(kind, text, true);
  }

  var print = Module['print'], printErr = Module['printErr'], onExit = Module['onExit'], onAbort = Module['onAbort'];
  Module['print'] = function (s) { post('out', s); if (print) print(s); };
  Module['printErr'] = function (s) { post('err', s); if (printErr) printErr(s); };
  // The runtime flushes the program's last unterminated line before it calls onExit, so the status follows every line.
  Module['onExit'] = function (c) { if (onExit) onExit(c); end('exit', c | 0); };
  // emscripten calls onAbort BEFORE it prints `Aborted(…)` and throws, so the end waits a turn for that line. The
  // error the abort throws is not a second ending.
  Module['onAbort'] = function (w) {
    if (onAbort) onAbort(w);
    if (ended) return;
    ended = true;
    setTimeout(function () { post('abort', w, true); }, 0);
  };
  // An exception the program did not catch (a trap is `RuntimeError: unreachable`): emscripten ends the program with
  // status 1 and prints it under node, and in a page it is only thrown, so the runner prints it and ends with 1.
  addEventListener('error', function (e) { end('error', e.error && e.error.stack ? e.error.stack : e.message); });
  addEventListener('unhandledrejection', function (e) {
    end('error', e.reason && e.reason.stack ? e.reason.stack : 'Uncaught (in promise) ' + e.reason);
  });
  // The page closed before the program ended (its window closed, or the browser quit). A page going away may not post
  // synchronously, and a beacon is what it may send.
  addEventListener('pagehide', function () {
    if (ended) return;
    ended = true;
    navigator.sendBeacon('__kama_run', JSON.stringify({ seq: seq++, kind: 'closed', text: '' }));
  });
})();
