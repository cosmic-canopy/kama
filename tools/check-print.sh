#!/bin/sh
# check-print.sh — floor console output (print/println/eprint/eprintln). The standard harness only asserts a
# fixture's EXIT CODE and captures its stderr into the sanitizer-report file (so a committed fixture must not
# write to stderr); this runner checks the actual printed bytes on both streams:
#   1. stdout — tests/print_demo.kama emits "Hello, world\nx = 42\n" (print + println + interpolation, fd 1).
#   2. stderr + separation — a self-contained probe writes to BOTH streams; assert eprintln lands on stderr
#      and does NOT leak onto stdout. Kept here (not a harness fixture) precisely because it writes to stderr.
#   3. embedded — transpile --target embedded to prove the print family lowers freestanding (to the weak
#      kama_log_sink, no libc).
# Native only (runs native binaries).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/print_demo.kama"

if [ ! -x "$KAMA" ]; then echo "check-print: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-print: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 1. STDOUT — build the harness fixture, run, check the stdout bytes.
"$KAMA" build "$FIXTURE" -o "$tmp/demo" >/dev/null 2>"$tmp/build.err" || {
    echo "check-print: FAIL — build failed" >&2; sed 's/^/  /' "$tmp/build.err" >&2; exit 1; }
"$tmp/demo" >"$tmp/out" 2>"$tmp/err" || { echo "check-print: FAIL — fixture exited nonzero" >&2; exit 1; }
printf 'Hello, world\nx = 42\n' > "$tmp/out.expect"
if ! cmp -s "$tmp/out" "$tmp/out.expect"; then
    echo "check-print: FAIL — stdout mismatch" >&2
    echo "  expected:" >&2; sed 's/^/    /' "$tmp/out.expect" >&2
    echo "  actual:"   >&2; sed 's/^/    /' "$tmp/out" >&2
    exit 1
fi

# 2. STDERR + STREAM SEPARATION — a probe that writes to both streams.
cat > "$tmp/streams.kama" <<'KAMA'
fn int32 main() {
    println(s: "on-stdout");
    eprintln(s: "on-stderr");
    return 0;
}
KAMA
"$KAMA" build "$tmp/streams.kama" -o "$tmp/streams" >/dev/null 2>"$tmp/s.build.err" || {
    echo "check-print: FAIL — streams probe build failed" >&2; sed 's/^/  /' "$tmp/s.build.err" >&2; exit 1; }
"$tmp/streams" >"$tmp/s.out" 2>"$tmp/s.err" || { echo "check-print: FAIL — streams probe exited nonzero" >&2; exit 1; }
if ! grep -qF "on-stderr" "$tmp/s.err"; then
    echo "check-print: FAIL — eprintln did not reach stderr" >&2; sed 's/^/  /' "$tmp/s.err" >&2; exit 1
fi
if grep -qF "on-stderr" "$tmp/s.out"; then
    echo "check-print: FAIL — the eprintln line leaked onto stdout (streams not separated)" >&2; exit 1
fi
if ! grep -qF "on-stdout" "$tmp/s.out"; then
    echo "check-print: FAIL — println did not reach stdout" >&2; exit 1
fi

# 3. EMBEDDED — the print family must lower freestanding (routes to the weak kama_log_sink, no libc).
"$KAMA" transpile --target embedded "$FIXTURE" -o "$tmp/emb.c" >/dev/null 2>"$tmp/emb.err" || {
    echo "check-print: FAIL — --target embedded transpile failed" >&2; sed 's/^/  /' "$tmp/emb.err" >&2; exit 1; }
if ! grep -q 'kama_print_write' "$tmp/emb.c"; then
    echo "check-print: FAIL — embedded C does not reference kama_print_write" >&2; exit 1
fi

echo "check-print: PASS (stdout/stderr bytes on the right streams; embedded lowers to the weak kama_log_sink)"
