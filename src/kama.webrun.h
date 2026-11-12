#ifndef KAMA_WEBRUN_H
#define KAMA_WEBRUN_H

// The three files `kama run --target WASM` writes into its run's directory (KR-377), embedded into the compiler binary at
// build time so a browser run works from any install, including `--no-std`, which ships only bin/kama. Same mechanism
// as the seed templates (kama.seed.h). The definitions live in build/kama.webrun.gen.cpp, generated from src/webrun/ by
// tools/embed_webrun.sh.

extern const char* KAMA_WEBRUN_RUNNER;   // src/webrun/runner.mjs — node: serves the page, runs the browser, relays it
extern const char* KAMA_WEBRUN_PRE;      // src/webrun/pre.js     — linked with --pre-js: the page's reports to the runner
extern const char* KAMA_WEBRUN_PAGE;     // src/webrun/page.html  — the page, when the project names no `shellFile`

#endif // KAMA_WEBRUN_H
