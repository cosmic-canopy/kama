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

  // Lines go to the runner in batches: a post per line is a request per line, and a program printing 20,000 of them
  // queued 20,000 requests on one host and delivered ~20 a second. A program printing without yielding (a loop in
  // `main`) posts at most every 50 ms, so its output keeps arriving; one that yields has the rest posted on its next
  // turn. An asynchronous post may arrive before an earlier one, so each carries its sequence number and the runner
  // prints them in order. The end goes with whatever is queued, synchronously: the runner has every line before anything
  // that follows can tear the page down. Posts are relative, so they reach the runner under the path that served the page.
  var seq = 0, queue = [], last = 0, scheduled = false, ended = false;
  function flush(sync) {
    scheduled = false;
    if (!queue.length) return;
    var x = new XMLHttpRequest();
    x.open('POST', '__kama_run', !sync);
    x.send(JSON.stringify({ seq: seq++, items: queue }));
    queue = [];
    last = performance.now();
  }
  function line(kind, text) {
    queue.push([kind, String(text)]);
    if (performance.now() - last > 50) flush(false);
    else if (!scheduled) { scheduled = true; setTimeout(function () { flush(false); }, 0); }
  }
  function end(kind, text) {
    if (ended) return;
    ended = true;
    queue.push([kind, String(text)]);
    flush(true);
  }

  var print = Module['print'], printErr = Module['printErr'], onExit = Module['onExit'], onAbort = Module['onAbort'];
  Module['print'] = function (s) { line('out', s); if (print) print(s); };
  Module['printErr'] = function (s) { line('err', s); if (printErr) printErr(s); };
  // The runtime flushes the program's last unterminated line before it calls onExit, so the status follows every line.
  Module['onExit'] = function (c) { if (onExit) onExit(c); end('exit', c | 0); };
  // emscripten calls onAbort BEFORE it prints `Aborted(…)` and throws, so the end waits a turn for that line. The
  // error the abort throws is not a second ending.
  Module['onAbort'] = function (w) {
    if (onAbort) onAbort(w);
    if (ended) return;
    ended = true;
    setTimeout(function () { queue.push(['abort', String(w)]); flush(true); }, 0);
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
    queue.push(['closed', '']);
    navigator.sendBeacon('__kama_run', JSON.stringify({ seq: seq++, items: queue }));
  });
})();
