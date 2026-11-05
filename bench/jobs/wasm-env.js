// Hands node's BENCH_ and POOL_ settings to emscripten's `getenv`, for pool.c under wasm.sh.
Module.preRun = (Module.preRun || []).concat([function () { for (var k in process.env) if (k.indexOf('BENCH_') === 0 || k.indexOf('POOL_') === 0) ENV[k] = process.env[k]; }]);
