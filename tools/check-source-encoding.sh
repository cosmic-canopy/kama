#!/bin/sh
# check-source-encoding.sh — kama reads UTF-8, skips a leading UTF-8 byte-order mark and refuses UTF-16 (SPEC
# "Model"; KR-90), and refuses any byte that is not UTF-8 text anywhere in a file, comments included (KR-91).
# The build path is pinned by fixtures: tests/source_bom.kama, tests/source_bom_manifest.d (its kama.json),
# tests/xfail/source_utf16.kama, tests/xfail/source_bom_column.kama and tests/xfail/source_utf8_*.kama. This
# guard does the three things a fixture cannot do for itself:
#
#   1. HOLDS THE BYTES DOWN. A byte-order mark is three bytes no editor shows, and one save that drops them
#      leaves the positive fixtures passing while they test nothing — a plain UTF-8 file builds either way.
#      So their first bytes are asserted here, the vacuity control check-compiler-path.sh keeps for its
#      non-ASCII directory.
#   2. DRIVES THE EDITOR PATH. `kama build`/`check`/`query` read a file through parseFile; a buffer the
#      editor sends goes through parseForQuery instead, which no fixture reaches. A BOM buffer must publish
#      no diagnostics, and a line-1 error behind one must keep its column (25, as without the mark). A buffer
#      holding a byte that is not UTF-8 text must be refused where the byte is — raw 0xFF, a lone `\ud800`
#      (the JSON decoder writes it as the bytes ED A0 80), and a `\u0000`, which a C-string view of the
#      buffer used to stop at, so everything after it went unread.
#   3. READS A MANIFEST. A manifest refusal prints no `error:`, so no xfail fixture can hold one; kama.json
#      is UTF-8 by the same SPEC sentence, and a Latin-1 byte in it must be refused by line and offset.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-source-encoding: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-source-encoding: FAIL — $1" >&2; fail=1; }

# ---- 1. the fixtures still carry their marks ----------------------------------------------------------
lead() { od -An -tx1 -N"$2" "$ROOT/$1" | tr -d ' \n'; }
for f in tests/source_bom.kama tests/source_bom_manifest.d/kama.json tests/xfail/source_bom_column.kama; do
    [ "$(lead "$f" 3)" = efbbbf ] || note "$f no longer starts with a UTF-8 byte-order mark (EF BB BF) — it
  now tests nothing. Restore it: printf '\\357\\273\\277' | cat - <file-without-it> > $f"
done
[ "$(lead tests/xfail/source_utf16.kama 2)" = fffe ] \
    || note "tests/xfail/source_utf16.kama no longer starts with FF FE — it is not the UTF-16 file it claims to be"

# ---- 2. the editor path: a BOM buffer through `kama lsp` ----------------------------------------------
session="$tmp/session"; : > "$session"
frame() {   # byte length, not character length — see check-lsp.sh's frame() for why LC_ALL=C
    _lc=${LC_ALL-__lc_unset__}; LC_ALL=C; len=${#1}
    if [ "$_lc" = __lc_unset__ ]; then unset LC_ALL; else LC_ALL=$_lc; fi
    printf 'Content-Length: %s\r\n\r\n%s' "$len" "$1" >> "$session"
}
# The mark is built at run time so this script stays ASCII: an invisible mark in the SOURCE of a guard is the
# very vacuity it exists to catch (and one editing tool already decoded a JSON escape of it into raw bytes).
BOM=$(printf '\357\273\277')
frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///bomclean.kama","languageId":"kama","version":1,"text":"'"$BOM"'fn int32 main() { return 0; }\n"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///bomcol.kama","languageId":"kama","version":1,"text":"'"$BOM"'fn int32 main() { return 1 $ 2; }\n"}}}'
FF=$(printf '\377')
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///rawff.kama","languageId":"kama","version":1,"text":"fn int32 main() {\n    // x'"$FF"'\n    return 0;\n}\n"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///surrogate.kama","languageId":"kama","version":1,"text":"fn int32 main() {\n    string s = \"\ud800\";\n    return 0;\n}\n"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///nul.kama","languageId":"kama","version":1,"text":"fn int32 main() {\n    return 0; \u0000\n}\n"}}}'
frame '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
frame '{"jsonrpc":"2.0","method":"exit"}'
out=$("$KAMA" lsp < "$session" 2>/dev/null | tr '\r' '\n' || true)

clean=$(printf '%s\n' "$out" | LC_ALL=C grep -ao 'bomclean.kama","diagnostics":\[[^]]*\]' || true)
[ "$clean" = 'bomclean.kama","diagnostics":[]' ] \
    || note "a BOM buffer through the LSP published diagnostics (want none): ${clean:-<nothing published>}"
col=$(printf '%s\n' "$out" | LC_ALL=C grep -ao 'bomcol.kama","diagnostics":\[{"range":{"start":{"line":0,"character":[0-9]*}' || true)
case "$col" in
    *'"character":25}') ;;
    *) note "a line-1 error behind a BOM, through the LSP, is not at character 25: ${col:-<nothing published>}" ;;
esac

# A refusal is published at the byte: line 1 (0-based), the byte's column, and an Encoding diagnostic.
refused() {   # refused <uri-name> <character> <what>
    got=$(printf '%s\n' "$out" | LC_ALL=C grep -ao "$1.kama\",\"diagnostics\":\[{\"range\":{\"start\":{\"line\":1,\"character\":[0-9]*}[^]]*\"code\":\"Encoding\"" || true)
    case "$got" in
        *"\"character\":$2}"*) ;;
        *) note "an LSP buffer holding $3 was not refused at line 1, character $2: ${got:-<no Encoding diagnostic>}" ;;
    esac
}
refused rawff 8 "a raw 0xFF"
refused surrogate 16 "an encoded surrogate (a JSON \\ud800)"
refused nul 14 "a NUL (a JSON \\u0000)"

# ---- 3. a manifest ------------------------------------------------------------------------------------
mkdir -p "$tmp/man/src"
printf 'fn int32 main() { return 0; }\n' > "$tmp/man/src/main.kama"
printf '{ "name": "man", "version": "0.1.0", "kind": "executable",\n  "license": "caf\351, in Latin-1",\n  "entry": "src/main.kama", "modules": { ".": { "visibility": "internal" } } }\n' > "$tmp/man/kama.json"
man=$("$KAMA" build "$tmp/man/kama.json" -o "$tmp/man/out" 2>&1 || true)
case "$man" in
    *"not UTF-8 text at line 2 (byte offset 76): 0xE9 begins a 3-byte character that is cut short"*) ;;
    *) note "a kama.json holding a Latin-1 byte was not refused by line and offset: $(printf '%s' "$man" | head -2)" ;;
esac

[ "$fail" -eq 0 ] || exit 1
echo "check-source-encoding: PASS (3 BOM fixtures + 1 UTF-16 fixture hold their bytes; an LSP buffer skips the mark and keeps line-1 columns, and is refused at a byte that is not UTF-8 text; so is a manifest)"
