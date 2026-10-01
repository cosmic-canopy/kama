#!/bin/sh
# check-lexer-buffer.sh — a source file lexes the same at any LENGTH.
#
# `give`, `copy` and `truncate` are keywords only where their grep anchor stands (KB-35), so the lexer reads the
# text AFTER the word to decide — contextualWord, in src/kama.l — and its comment states the condition that makes
# that safe: the whole source is one flex buffer. The editor's door scanned from memory and kept it; the file door
# (`kama check`/`kama build`) read through a FILE*, which flex loads 16 KB at a time. So at a refill the look past
# the word met the buffer's end instead of the text, `give s` straddling byte 16384 lexed as two names, and a
# correct file failed to parse because of where an unrelated edit had moved a line ("unexpected IDENTIFIER").
# Found at 0.9.501, when migrating std::io pushed one `give s` in streams.kama onto that byte.
#
# A fixture cannot hold this down: its offsets move with every edit to it. So this generates the file, placing the
# word at every offset across the boundary, and asks the file door to check each one.
#
# Deterministic, no network; ~60 `kama check` runs of a one-function program.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-lexer-buffer: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

BOUNDARY=16384
fails=0
runs=0
for word in give copy; do
    # The program up to the padding, and after it: the word sits at `prefixLen + padLen + suffixHead`.
    head="fn int32 take(string s) { return cast<int32>(s.length()); }
fn int32 main() {
    string s = \"ab\";
    // "
    tail="
    return take(s: $word s);
}
"
    headLen=$(printf '%s' "$head" | wc -c | tr -d ' ')
    lead=$(printf '%s' "
    return take(s: " | wc -c | tr -d ' ')
    off=$((BOUNDARY - 12))
    while [ "$off" -le $((BOUNDARY + 16)) ]; do
        pad=$((off - headLen - lead))
        f="$tmp/w_${word}_$off.kama"
        { printf '%s' "$head"; awk -v n="$pad" 'BEGIN { for (i = 0; i < n; i++) printf "x" }'; printf '%s' "$tail"; } > "$f"
        # the word must really be where it is meant to be, or the sweep proves nothing
        at=$(dd if="$f" bs=1 skip="$off" count=4 2>/dev/null)
        if [ "$at" != "$word" ]; then
            echo "check-lexer-buffer: FAIL — the generator put \`$at\` at byte $off, not \`$word\`; fix the generator" >&2
            exit 1
        fi
        runs=$((runs + 1))
        if ! "$KAMA" check "$f" > "$tmp/out" 2>&1; then
            fails=$((fails + 1))
            if [ "$fails" -le 3 ]; then
                echo "check-lexer-buffer: FAIL — \`$word s\` at byte $off does not parse:" >&2
                sed 's/^/    /' "$tmp/out" | head -3 >&2
            fi
        fi
        off=$((off + 1))
    done
done
if [ "$fails" -ne 0 ]; then
    echo "check-lexer-buffer: FAIL — $fails of $runs placements failed. The lexer's lookahead needs the whole source" >&2
    echo "  in one buffer (contextualWord); the file door must scan from memory, as parseSource does." >&2
    exit 1
fi
echo "check-lexer-buffer: PASS ($runs placements of \`give\`/\`copy\` across byte $BOUNDARY all parse)"
