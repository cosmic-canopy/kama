#!/bin/sh
# check-lsp.sh — LSP walking-skeleton guard (M1). Drives the `kama lsp` server over stdio with a scripted
# JSON-RPC 2.0 session and asserts the live-diagnostics loop end-to-end:
#   1. initialize        -> a capabilities response advertising full-document sync.
#   2. didOpen (bad buf) -> a publishDiagnostics carrying the syntax error at the right (0-based) range.
#   3. didChange (fixed) -> a publishDiagnostics with an EMPTY array (squiggles cleared).
# The buffers are inlined (tiny; avoids fragile file-content JSON-escaping in POSIX sh). Mirrors
# check-query.sh style; run standalone or from run_tests.sh. Fails (exit 1) with a diagnostic dump.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA="$ROOT/kama"
if [ ! -x "$KAMA" ]; then echo "check-lsp: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
session="$tmp/session"; : > "$session"

# frame <json-body>: append one LSP-framed message (Content-Length header + CRLFCRLF + body) to the
# session. %s never interprets the body's backslashes, so the JSON `\n` escapes stay two literal bytes —
# exactly what Content-Length must count and what the server's JSON parser decodes back to newlines.
frame() {
    body="$1"
    len=$(printf '%s' "$body" | wc -c | tr -d ' ')
    printf 'Content-Length: %s\r\n\r\n%s' "$len" "$body" >> "$session"
}

URI="file:///t.kama"
BAD='fn int32 main() {\n    return 0\n}\n'     # missing semicolon -> syntax error on line 3 (LSP line 2)
GOOD='fn int32 main() {\n    return 0;\n}\n'   # fixed

frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$URI"'","languageId":"kama","version":1,"text":"'"$BAD"'"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$URI"'","version":2},"contentChanges":[{"text":"'"$GOOD"'"}]}}'
frame '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
frame '{"jsonrpc":"2.0","method":"exit"}'

out=$("$KAMA" lsp < "$session" 2>/dev/null || true)

fail=0
# expect <substring> <description>: assert the server's framed stdout contains <substring>.
expect() {
    if printf '%s' "$out" | grep -qF -- "$1"; then
        echo "  ok: $2"
    else
        echo "  FAIL: $2 — expected substring: $1" >&2
        fail=1
    fi
}

echo "check-lsp: lifecycle + live diagnostics over stdio"
expect '"capabilities"'                              "initialize -> capabilities"
expect '"textDocumentSync":1'                        "advertises full-document sync"
expect '"method":"textDocument/publishDiagnostics"'  "server publishes diagnostics"
expect '"message":"syntax error'                     "syntax error surfaced on the bad buffer"
expect '"start":{"line":2,"character":0}'            "error range mapped to LSP 0-based (kama 3:0 -> 2:0)"
expect '"diagnostics":[]'                            "didChange to a valid buffer clears the squiggles"

if [ "$fail" != 0 ]; then
    echo "check-lsp: FAILED. Server stdout was:" >&2
    printf '%s\n' "$out" | sed 's/^/      /' >&2
    exit 1
fi
echo "check-lsp: OK"
