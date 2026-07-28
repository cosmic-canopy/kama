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

# M3.5 workspace fixture: the DECLARING half of the tests/query/ws package. app.kama (on disk, never
# opened here) imports it and uses `Widget` three times; widget.kama imports nothing, so its own closure
# is just itself. Opening it and renaming `Widget` is exactly the case M3.3 had to refuse.
WWURI="file://$ROOT/tests/query/ws/widget.kama"
WW='namespace widget;\nexport { Widget, defaultSize };\ntype value Widget {\n    public int32 size;\n    public ctor of(int32 size) { Widget r; r.size = size; return give r; }\n}\nfn int32 defaultSize() { return 7; }\n'

# Semantic-diagnostic fixture: an undeclared type in a body (kama line 2 -> LSP line 1).
SURI="file:///sem.kama"
SEM='fn int32 main() {\n    Nonexistent thing;\n    return 0;\n}\n'

# M3.4 fixture: one of each binding kind, each WITH the trailing syntax whose span used to be swallowed.
# The prepareRename ranges below are the DATA-LOSS GUARD — rename replaces the range it is given, so a
# range that ran past the name would rewrite `seeded = 7` (or `Code::Ok`, or `Bad = 2`) as the new name.
# LSP 0-based lines/chars:
#   L1 `enum Code { Ok, Bad = 2 }`                     -> `Bad` 16..19  (NOT 16..23)
#   L3 `    public int32 scale = 3;`                   -> `scale` 17..22 (NOT 17..26)
#   L4 `    public fn int32 twice(int32 bias) { … }`   -> `bias` 32..36 (NOT 26..36, which starts at the type)
#      ... its body `this.scale` -> `scale` at 52..57 (NOT 47.., which starts at the receiver)
#   L7 `    int32 seeded = 7;`                         -> `seeded` 10..16 (NOT 10..20)
#   L8 `    Code c = Code::Ok;`                        -> `Ok` 19..21 (NOT 13..21, which eats `Code::`)
MURI="file:///bindings.kama"
M34='namespace m34;\nenum Code { Ok, Bad = 2 }\ntype value Cfg {\n    public int32 scale = 3;\n    public fn int32 twice(int32 bias) { return this.scale * bias; }\n}\nfn int32 run() {\n    int32 seeded = 7;\n    Code c = Code::Ok;\n    return seeded + cast<int32>(c);\n}\n'

# M3.5 dependency fixture: a real installed path dependency, so `.kama/deps` is populated. Built here
# rather than committed — `.kama/deps` is install output, and a path dep needs no network. The guard under
# test is that rename REFUSES a symbol whose definition lives in a dependency: `DefSite.unit == nullptr`
# filters only the built-in prelude, so a dep's units look like ordinary user code and would otherwise be
# rewritten. Previously this guard was only ever exercised against std.
dep="$tmp/depproj"
mkdir -p "$dep/geo" "$dep/app"
cat > "$dep/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "sources": ["."] }
JSON
cat > "$dep/geo/geo.kama" <<'KAMA'
namespace geo;
export { Point };
type value Point {
    public int32 x;
    public ctor of(int32 x) { Point r; r.x = x; return give r; }
}
KAMA
cat > "$dep/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "main": "app.kama", "sources": ["."],
  "dependencies": { "geo": { "path": "../geo" } } }
JSON
cat > "$dep/app/app.kama" <<'KAMA'
import geo::{Point};
fn int32 main() {
    Point p = Point.of(x: 7);
    return p.x;
}
KAMA
depok=0
(cd "$dep/app" && "$KAMA" pkg install >/dev/null 2>&1) && depok=1

# M3.5 ownership fixture: a project whose `sources` reach OUTSIDE its own directory. Ownership is decided
# by the project's FILE SET, not by a path prefix — so this file IS renameable (the project declared it),
# while the dependency above is NOT (inside the root, but not ours). A prefix test gets both backwards.
mkdir -p "$tmp/own/proj" "$tmp/own/shared"
cat > "$tmp/own/proj/kama.json" <<'JSON'
{ "name": "own", "version": "0.1.0", "sources": [".", "../shared"] }
JSON
cat > "$tmp/own/shared/shared.kama" <<'KAMA'
namespace shared;
type value Leak { public int32 v; }
KAMA
OURI="file://$tmp/own/proj/app.kama"
OSRC='namespace shared;\nfn int32 main() { Leak l; l.v = 1; return l.v; }\n'
printf 'namespace shared;\nfn int32 main() { Leak l; l.v = 1; return l.v; }\n' > "$tmp/own/proj/app.kama"
DURI="file://$dep/app/app.kama"
DSRC='import geo::{Point};\nfn int32 main() {\n    Point p = Point.of(x: 7);\n    return p.x;\n}\n'

frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file://'"$ROOT"'/tests/query","capabilities":{}}}'
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
# --- M3.4: locals / params / fields / enum members ---
# 20-24: prepareRename on each kind, asserting the EXACT range (the data-loss guard described above).
# 25:    references on the field `scale` -> its `this.scale` use, proving the member-access span is the
#        name and not the receiver. 26: a full rename of a local. 27: hover reports the binding kind.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$MURI"'","languageId":"kama","version":1,"text":"'"$M34"'"}}}'
frame '{"jsonrpc":"2.0","id":20,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":7,"character":12}}}'
frame '{"jsonrpc":"2.0","id":21,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":4,"character":33}}}'
frame '{"jsonrpc":"2.0","id":22,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":3,"character":18}}}'
frame '{"jsonrpc":"2.0","id":23,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":1,"character":17}}}'
frame '{"jsonrpc":"2.0","id":24,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":8,"character":20}}}'
frame '{"jsonrpc":"2.0","id":25,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":3,"character":18},"context":{"includeDeclaration":false}}}'
frame '{"jsonrpc":"2.0","id":26,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":7,"character":12},"newName":"total"}}'
frame '{"jsonrpc":"2.0","id":27,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":9,"character":13}}}'
# --- M3.5: workspace indexing. Open widget.kama (a REAL on-disk file inside tests/query/ws, which holds
#     a kama.json) and rename `Widget`. rootUri is tests/query and the nearest manifest below it is ws/,
#     so the project is exactly those two files — and the rewrite must reach app.kama, which widget.kama
#     does NOT import. 28: references span both files. 29: the multi-file WorkspaceEdit. 30: a project-wide
#     symbol search. 31: didChangeWatchedFiles is accepted (a notification, so silence == success; the
#     assertion is that it does not come back as "method not found").
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$WWURI"'","languageId":"kama","version":1,"text":"'"$WW"'"}}}'
frame '{"jsonrpc":"2.0","id":28,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$WWURI"'"},"position":{"line":2,"character":11},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":29,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$WWURI"'"},"position":{"line":2,"character":11},"newName":"Gadget"}}'
frame '{"jsonrpc":"2.0","id":30,"method":"workspace/symbol","params":{"query":"efaultSi"}}'
frame '{"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":"'"$WWURI"'","type":2}]}}'
# --- M3.5: an installed DEPENDENCY. 32: rename on a type declared in .kama/deps must REFUSE (it is not
#     ours to rewrite). 33: the dep's symbols must not show up in the project symbol picker either.
if [ "$depok" = 1 ]; then
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$DURI"'","languageId":"kama","version":1,"text":"'"$DSRC"'"}}}'
frame '{"jsonrpc":"2.0","id":32,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$DURI"'"},"position":{"line":2,"character":4},"newName":"Pt"}}'
frame '{"jsonrpc":"2.0","id":33,"method":"workspace/symbol","params":{"query":"Point"}}'
fi
# 34: a type declared in a source the project reaches OUTSIDE its own directory is still ours to rename.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$OURI"'","languageId":"kama","version":1,"text":"'"$OSRC"'"}}}'
frame '{"jsonrpc":"2.0","id":34,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$OURI"'"},"position":{"line":1,"character":19},"newName":"Seep"}}' 
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
expect '"id":6,"result":{"contents":{"kind":"plaintext","value":"param a"}}'      "hover: 'param a' on a parameter use-site (M3.4)"

echo "check-lsp: M3 find-references"
expect '"id":8,"result":[{"uri":"file:///shapes.kama"'        "references: returns Locations in this file"
expect '"start":{"line":3,"character":17}'                    "references: BODY use-site 'Point p' (LSP 3:17) is indexed"
expect '"start":{"line":3,"character":26}'                    "references: BODY use-site 'Point q' (LSP 3:26) is indexed"
expect '"id":11,"result":[{"uri":"file:///shapes.kama","range":{"start":{"line":2,"character":9}' \
                                                              "references: a CALL resolves to the fn decl (resolveFunc hook)"
expect '"start":{"line":3,"character":36}'                    "references: the call site itself (LSP 3:36) is indexed"

echo "check-lsp: M3 rename"
expect '"id":12,"result":{"start":{"line":1,"character":11}'  "prepareRename: the Point identifier range"
expect '"id":13,"result":{"start":{"line":2,"character":31},"end":{"line":2,"character":32}}' \
                                                              "prepareRename: a parameter use is renameable and spans just the name (M3.4)"
expect '"id":14,"result":{"changes":{"file:///shapes.kama":[' "rename: a WorkspaceEdit keyed by this file's URI"
expect '"newText":"Pnt"'                                      "rename: each edit carries the new name"
expect '"id":15,"error"'                                      "rename: a reserved keyword is refused"

echo "check-lsp: module loading (imports resolve across files)"
expect 'imports.kama","diagnostics":[]'   "import-using file analyzes clean (std::collections loaded; no false 'does not export')"
expect 'dynamic_array.kama'               "cross-module go-to-def resolves DynamicArray into the std source"
expect '"id":16,"error"'                             "rename still REFUSES a symbol we do not own"
expect 'this symbol is defined outside the project'   "...and now says WHY: DynamicArray is declared in std"

echo "check-lsp: M3.4 bindings (locals / params / fields / enum members)"
# Each of these asserts the FULL range. An `end` past the name is the data-loss bug the M3.4 grammar
# pass fixed — rename replaces this range verbatim, so a wrong end silently eats the initializer/value.
expect '"id":20,"result":{"start":{"line":7,"character":10},"end":{"line":7,"character":16}}' \
                                          "prepareRename: local 'seeded' spans the NAME, not 'seeded = 7'"
expect '"id":21,"result":{"start":{"line":4,"character":32},"end":{"line":4,"character":36}}' \
                                          "prepareRename: param 'bias' starts at the name, not at its type"
expect '"id":22,"result":{"start":{"line":3,"character":17},"end":{"line":3,"character":22}}' \
                                          "prepareRename: field 'scale' spans the NAME, not 'scale = 3'"
expect '"id":23,"result":{"start":{"line":1,"character":16},"end":{"line":1,"character":19}}' \
                                          "prepareRename: enum member 'Bad' spans the NAME, not 'Bad = 2'"
expect '"id":24,"result":{"start":{"line":8,"character":19},"end":{"line":8,"character":21}}' \
                                          "prepareRename: a 'Code::Ok' use spans 'Ok', not the qualifier too"
expect '"id":25,"result":[{"uri":"file:///bindings.kama","range":{"start":{"line":4,"character":52}' \
                                          "references: 'this.scale' is a field use at the NAME, not the receiver"
expect '"id":26,"result":{"changes":{"file:///bindings.kama":['  "rename: a local produces a WorkspaceEdit"
expect '"newText":"total"'                                       "rename: the local's edits carry the new name"
expect '"id":27,"result":{"contents":{"kind":"plaintext","value":"local seeded"}}' \
                                          "hover: 'local seeded' on a local use-site"

echo "check-lsp: semantic diagnostics (not just parse errors)"
expect 'unknown type `Nonexistent`'                        "undeclared body type surfaces as a live diagnostic"
expect 'sem.kama","diagnostics":[{"range":{"start":{"line":1'  "the squiggle lands on the decl line (kama 2 -> LSP 1)"

echo "check-lsp: M3.5 workspace indexing (cross-file rename)"
expect '"workspaceSymbolProvider":true'                 "advertises workspaceSymbolProvider (M3.5)"
expect '"workspace":{"workspaceFolders":{"supported":true}}' \
                                                        "advertises workspaceFolders support (M3.5)"
# THE MILESTONE: app.kama is not in widget.kama's import closure, so every one of these would be missing
# without the project-wide unit set.
expect '"id":28,"result":[{"uri":"file://'"$ROOT"'/tests/query/ws/widget.kama"' \
                                                        "references: starts with the declaring file"
expect '/tests/query/ws/app.kama","range":{"start":{"line":4,"character":3}' \
                                                        "references REACH app.kama, which widget.kama does not import"
expect '"id":29,"result":{"changes":{"file://'"$ROOT"'/tests/query/ws/app.kama":[' \
                                                        "rename: the WorkspaceEdit rewrites the OTHER file too"
expect '/tests/query/ws/widget.kama":[{"range":{"start":{"line":2,"character":11}' \
                                                        "rename: ... and the declaring file, keyed separately"
expect '"newText":"Gadget"'                             "rename: the cross-file edits carry the new name"
expect '"id":30,"result":[{"name":"defaultSize","kind":12' \
                                                        "workspace/symbol: substring search finds a project symbol"
expect '"id":30,'                                       "workspace/symbol: answered (not method-not-found)"
# didChangeWatchedFiles is a notification: the proof it is handled is that no error came back for it.
if printf '%s' "$out" | grep -qF 'method not found: workspace/'; then
    echo "  FAIL: a workspace/* method was rejected as unknown" >&2
    fail=1
else
    echo "  ok: workspace/didChangeWatchedFiles is accepted (index invalidation, not an error)"
fi

if [ "$depok" = 1 ]; then
    echo "check-lsp: M3.5 installed dependency"
    expect '"id":32,"error"'                          "rename REFUSES a type declared in a dependency"
    expect 'defined outside the project'              "...naming the dependency source it lives in"
    expect '/.kama/deps/geo/geo.kama'                 "...which is under the project's package store"
    expect '"id":33,"result":[]'                      "workspace/symbol does not offer a dependency's symbols"
else
    echo "  SKIP: installed-dependency checks (kama pkg install failed)"
fi

echo "check-lsp: M3.5 ownership is the file SET, not a path prefix"
expect '"id":34,"result":{"changes":{'          "rename succeeds on a declared source outside the project dir"
expect '/own/shared/shared.kama":[{"range"'     "...and rewrites that outside file, because the project declared it"

if [ "$fail" != 0 ]; then
    echo "check-lsp: FAILED. Server stdout was:" >&2
    printf '%s\n' "$out" | sed 's/^/      /' >&2
    exit 1
fi
echo "check-lsp: OK"
