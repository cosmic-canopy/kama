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
#   L4 (M3) a BODY: two `Point` locals at kama 4:17 / 4:26 (LSP 3:17 / 3:26) and a CALL to `mid` at kama
#      4:36 (LSP 3:36). Only the M3 reference index sees these — M0's signature walk never enters a body.
#      Appended, so every position above is unchanged.
QURI="file:///shapes.kama"
SHP='namespace t;\ntype value Point { public int32 x; }\nfn Point mid(Point a) { return a; }\nfn int32 use() { Point p; Point q = mid(a: p); return q.x; }\n'

# Module-loading fixture: a REAL on-disk file that imports a std module. Unlike the in-memory buffers above
# (fake paths -> single-file fallback), this exercises loadProgramUnits pulling std::collections off disk so
# the imported DynamicArray resolves (no false "does not export"), and cross-module go-to-def into the std
# source. URI must be the real path so imports resolve relative to it + the stdlib.
IURI="file://$ROOT/tests/query/imports.kama"
IMP='namespace importsprobe;\nimport std::collections::{DynamicArray};\nfn int32 useit(DynamicArray<int32> a) { return 0; }\n'

# Semantic-diagnostic fixture: an undeclared type in a body (kama line 2 -> LSP line 1).
SURI="file:///sem.kama"
SEM='fn int32 main() {\n    Nonexistent thing;\n    return 0;\n}\n'

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
# --- M3: find-references + rename over the same buffer ---
# 8:  references on the Point DECL name (LSP 1:11), includeDeclaration -> decl + both signature refs + both body refs.
# 9:  the same query with includeDeclaration:false -> the decl's own range must be absent.
# 10: references from a BODY use (LSP 3:17) -> the same set (a use and its decl resolve to one key).
# 11: references on the `mid` call site (LSP 3:36) -> the fn decl + that call (the resolveFunc hook).
# 12: prepareRename on the Point decl -> its identifier range.
# 13: prepareRename on the local `a` (LSP 2:31) -> null (locals are M3.4).
# 14: rename Point -> Pnt: a WorkspaceEdit with one TextEdit per reference, all in this file.
# 15: rename to a KEYWORD must be refused (the lexer's own table decides, via kamaIsKeyword).
frame '{"jsonrpc":"2.0","id":8,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":11},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":9,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":11},"context":{"includeDeclaration":false}}}'
frame '{"jsonrpc":"2.0","id":10,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":3,"character":17},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":11,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":3,"character":36},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":12,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":11}}}'
frame '{"jsonrpc":"2.0","id":13,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":31}}}'
frame '{"jsonrpc":"2.0","id":14,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":11},"newName":"Pnt"}}'
frame '{"jsonrpc":"2.0","id":15,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":11},"newName":"return"}}'
# --- module loading: open the import-using file; its imports resolve, so diagnostics are empty and
#     go-to-def on DynamicArray (kama 3:15 -> LSP 2:15) jumps into the std::collections source ---
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$IURI"'","languageId":"kama","version":1,"text":"'"$IMP"'"}}}'
frame '{"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":2,"character":15}}}'
# 16: the M3.3 SAFETY GUARD. DynamicArray is declared in std and used across several std files, so a rename
#     here would rewrite files the user can't see — and the loaded-unit set still isn't every user of the
#     symbol. Rename must REFUSE rather than half-rewrite. (Lifted by workspace indexing in M3.5.)
frame '{"jsonrpc":"2.0","id":16,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":2,"character":15},"newName":"Foo"}}'
# --- SEMANTIC diagnostics: an undeclared type in a BODY. Everything above asserts on a PARSE error,
#     because semantic diagnostics used to be too incomplete to test — an unknown body type resolved to
#     nothing and was emitted verbatim, so `kama check` said OK and the editor showed a clean file that
#     then failed in the C compiler. checkTypeResolves closes that; this asserts the squiggle is live.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$SURI"'","languageId":"kama","version":1,"text":"'"$SEM"'"}}}'
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
expect '"referencesProvider":true'                   "advertises referencesProvider (M3)"
expect '"renameProvider":{"prepareProvider":true}'   "advertises renameProvider with prepareProvider (M3)"
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
expect '"id":6,"result":null'                                 "hover: null on a local use-site (M3.4 scope, intentional)"

echo "check-lsp: M3 find-references"
expect '"id":8,"result":[{"uri":"file:///shapes.kama"'        "references: returns Locations in this file"
expect '"start":{"line":3,"character":17}'                    "references: BODY use-site 'Point p' (LSP 3:17) is indexed"
expect '"start":{"line":3,"character":26}'                    "references: BODY use-site 'Point q' (LSP 3:26) is indexed"
expect '"id":11,"result":[{"uri":"file:///shapes.kama","range":{"start":{"line":2,"character":9}' \
                                                              "references: a CALL resolves to the fn decl (resolveFunc hook)"
expect '"start":{"line":3,"character":36}'                    "references: the call site itself (LSP 3:36) is indexed"

echo "check-lsp: M3 rename"
expect '"id":12,"result":{"start":{"line":1,"character":11}'  "prepareRename: the Point identifier range"
expect '"id":13,"result":null'                                "prepareRename: null on a local (not renameable until M3.4)"
expect '"id":14,"result":{"changes":{"file:///shapes.kama":[' "rename: a WorkspaceEdit keyed by this file's URI"
expect '"newText":"Pnt"'                                      "rename: each edit carries the new name"
expect '"id":15,"error"'                                      "rename: a reserved keyword is refused"

echo "check-lsp: module loading (imports resolve across files)"
expect 'imports.kama","diagnostics":[]'   "import-using file analyzes clean (std::collections loaded; no false 'does not export')"
expect 'dynamic_array.kama'               "cross-module go-to-def resolves DynamicArray into the std source"
expect '"id":16,"error"'                  "rename REFUSES a symbol used in other files (no half-rewrite)"
expect 'Cross-file rename needs workspace indexing'  "...and says why"

echo "check-lsp: semantic diagnostics (not just parse errors)"
expect 'unknown type `Nonexistent`'                        "undeclared body type surfaces as a live diagnostic"
expect 'sem.kama","diagnostics":[{"range":{"start":{"line":1'  "the squiggle lands on the decl line (kama 2 -> LSP 1)"

if [ "$fail" != 0 ]; then
    echo "check-lsp: FAILED. Server stdout was:" >&2
    printf '%s\n' "$out" | sed 's/^/      /' >&2
    exit 1
fi
echo "check-lsp: OK"
