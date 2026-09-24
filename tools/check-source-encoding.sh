#!/bin/sh
# check-source-encoding.sh — kama reads UTF-8, skips a leading UTF-8 byte-order mark and refuses UTF-16 (SPEC
# "Model"; KR-90). The build path is pinned by fixtures: tests/source_bom.kama, tests/source_bom_manifest.d
# (its kama.json), tests/xfail/source_utf16.kama and tests/xfail/source_bom_column.kama. This guard does the
# two things a fixture cannot do for itself:
#
#   1. HOLDS THE BYTES DOWN. A byte-order mark is three bytes no editor shows, and one save that drops them
#      leaves the positive fixtures passing while they test nothing — a plain UTF-8 file builds either way.
#      So their first bytes are asserted here, the vacuity control check-compiler-path.sh keeps for its
#      non-ASCII directory.
#   2. DRIVES THE EDITOR PATH. `kama build`/`check`/`query` read a file through parseFile; a buffer the
#      editor sends goes through parseForQuery instead, which no fixture reaches. A BOM buffer must publish
#      no diagnostics, and a line-1 error behind one must keep its column (25, as without the mark).
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

[ "$fail" -eq 0 ] || exit 1
echo "check-source-encoding: PASS (3 BOM fixtures + 1 UTF-16 fixture hold their bytes; an LSP buffer skips the mark and keeps line-1 columns)"
