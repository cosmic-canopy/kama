#!/bin/sh
# check-expose-shared.sh — `kama build --shared` + `expose` + `@linkName`, proved through `dlopen`/`dlsym`.
#
# tests/support/expose_shared_check.sh is the proof: build a native shared library from a module with no
# `main`, load it from a C host, resolve the exported symbols by NAME and call them. run_tests.sh is
# exit-code-only and never loads a library, so that script "lives apart" — and apart meant unrun: it
# rotted twice without anyone noticing. `int` became a reserved word (0.9.80) and its kama stopped
# lexing; then the runtime slots moved next to the synthesized `main` (0.9.162) and a `--shared` module,
# which has none, stopped LINKING — `kama_panic_hook` undefined. Both were found in one session, months
# later, the first time someone ran it by hand. A guard is the difference between a proof and a script.
#
# Native only: a wasm build exports `expose`d functions its own way and has no `dlopen`.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"   # sets $KAMA, which the harness reads
export KAMA
if ! command -v cc >/dev/null 2>&1 || ! command -v bash >/dev/null 2>&1; then
    echo "check-expose-shared: SKIP (needs cc and bash on PATH)"
    exit 0
fi
out=$(cd "$ROOT" && bash tests/support/expose_shared_check.sh 2>&1) || {
    printf '%s\n' "$out" | tail -20 >&2
    echo "check-expose-shared: FAIL — the --shared/dlsym proof did not pass (output above)" >&2
    exit 1
}
echo "check-expose-shared: PASS ($(printf '%s\n' "$out" | tail -1))"
