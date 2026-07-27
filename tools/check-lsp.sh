#!/bin/sh
# check-lsp.sh — LSP server guard (M1 diagnostics + M2 interactive features). Drives the `kama lsp` server
# over stdio with a scripted JSON-RPC 2.0 session and asserts, end-to-end:
#   1. initialize        -> capabilities advertising full-document sync + hover/definition/documentSymbol.
#   2. didOpen (bad buf) -> a publishDiagnostics carrying the syntax error at the right (0-based) range.
#   3. didChange (fixed) -> a publishDiagnostics with an EMPTY array (squiggles cleared).
#   4. documentSymbol    -> the outline (Point value + mid function) with accurate name ranges + kinds.
#   5. definition        -> go-to-def on a type reference lands on the decl (a file:// Location).
#   6. hover             -> "<kind> <name>" on a type reference; null on a body use-site (M3 scope).
# The buffers are inlined (tiny; avoids fragile file-content JSON-escaping in POSIX sh). Mirrors
# check-query.sh style; run standalone or from run_tests.sh. Fails (exit 1) with a diagnostic dump.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
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

# M2 fixture: a decl-rich buffer (its own URI) driving hover / go-to-definition / document symbols. The
# layout is fixed — the query positions below are tied to it (kama line 1-based/col 0-based -> LSP 0/0):
#   L2 `type value Point { ... }`   -> "Point" name at kama 2:11  (LSP 1:11, the def target)
#   L3 `fn Point mid(Point a) ...`  -> return-type "Point" at kama 3:3 (LSP 2:3); param-type at kama 3:13 (LSP 2:13)
QURI="file:///shapes.kama"
SHP='namespace t;\ntype value Point { public int32 x; }\nfn Point mid(Point a) { return a; }\n'

# Module-loading fixture: a REAL on-disk file that imports a std module. Unlike the in-memory buffers above
# (fake paths -> single-file fallback), this exercises loadProgramUnits pulling std::collections off disk so
# the imported DynamicArray resolves (no false "does not export"), and cross-module go-to-def into the std
# source. URI must be the real path so imports resolve relative to it + the stdlib.
IURI="file://$ROOT/tests/query/imports.kama"
IMP='namespace importsprobe;\nimport std::collections::{DynamicArray};\nfn int32 useit(DynamicArray<int32> a) { return 0; }\n'

frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$URI"'","languageId":"kama","version":1,"text":"'"$BAD"'"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$URI"'","version":2},"contentChanges":[{"text":"'"$GOOD"'"}]}}'
# --- M2: open the decl-rich buffer, then query it ---
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$QURI"'","languageId":"kama","version":1,"text":"'"$SHP"'"}}}'
frame '{"jsonrpc":"2.0","id":3,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$QURI"'"}}}'
frame '{"jsonrpc":"2.0","id":4,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":3}}}'
frame '{"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":13}}}'
# A body use-site ("a" in `return a`, kama 3:31 -> LSP 2:31) resolves to null BY DESIGN (M0 indexes decls +
# signature type refs only; body use-sites are the M3 find-references walk).
frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":31}}}'
# --- module loading: open the import-using file; its imports resolve, so diagnostics are empty and
#     go-to-def on DynamicArray (kama 3:15 -> LSP 2:15) jumps into the std::collections source ---
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$IURI"'","languageId":"kama","version":1,"text":"'"$IMP"'"}}}'
frame '{"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":2,"character":15}}}'
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
expect '"hoverProvider":true'                        "advertises hoverProvider (M2)"
expect '"definitionProvider":true'                   "advertises definitionProvider (M2)"
expect '"documentSymbolProvider":true'               "advertises documentSymbolProvider (M2)"
expect '"method":"textDocument/publishDiagnostics"'  "server publishes diagnostics"
expect '"message":"syntax error'                     "syntax error surfaced on the bad buffer"
expect '"start":{"line":2,"character":0}'            "error range mapped to LSP 0-based (kama 3:0 -> 2:0)"
expect '"diagnostics":[]'                            "didChange to a valid buffer clears the squiggles"

echo "check-lsp: M2 interactive features (hover / go-to-definition / document symbols)"
expect '"id":3,"result":[{"name":"Point","kind":5'            "documentSymbol: Point value -> SymbolKind.Class(5)"
expect '"selectionRange":{"start":{"line":1,"character":11}'  "documentSymbol: Point name range at LSP 1:11"
expect '"name":"mid","kind":12'                               "documentSymbol: mid -> SymbolKind.Function(12)"
expect '"id":4,"result":{"uri":"file:///shapes.kama"'         "definition: type ref -> file:// Location"
expect '"range":{"start":{"line":1,"character":11}'           "definition: lands on the Point decl name (LSP 1:11)"
expect '"id":5,"result":{"contents":{"kind":"plaintext","value":"value Point"}}'  "hover: 'value Point' on a type ref"
expect '"id":6,"result":null'                                 "hover: null on a body use-site (M3 scope, intentional)"

echo "check-lsp: module loading (imports resolve across files)"
expect 'imports.kama","diagnostics":[]'   "import-using file analyzes clean (std::collections loaded; no false 'does not export')"
expect 'dynamic_array.kama'               "cross-module go-to-def resolves DynamicArray into the std source"

if [ "$fail" != 0 ]; then
    echo "check-lsp: FAILED. Server stdout was:" >&2
    printf '%s\n' "$out" | sed 's/^/      /' >&2
    exit 1
fi
echo "check-lsp: OK"
