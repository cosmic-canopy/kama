#!/bin/sh
# lsp-bench.sh — the LSP perf oracle (M5.0). Splits one analysis into its phases and prints a
# comparable table, so the incremental/perf work is sized from data instead of guesses.
#
# It exists because the M5 cold-start brief sized the whole caching plan on a split nobody had
# measured — it assumed a parse cache would take a 240 ms analysis to ~20 ms, when the per-unit cost
# is parse AND analyze and only the parse share is cacheable. Its table also wasn't reproducible from
# a checkout. This is that table, reproducible.
#
# Requires KAMA_TIMING support in the compiler (kama.driver.cpp, `timingDump`).
#
#   tools/lsp-bench.sh                     # the default fixture set, `kama query` mode
#   tools/lsp-bench.sh a.kama b.kama       # your own files
#   tools/lsp-bench.sh --lsp               # a real `kama lsp` stdio session
#   REPS=11 tools/lsp-bench.sh             # more samples (default 7; the first is discarded)
#
# Numbers only mean something next to a baseline, so record one BEFORE touching anything — build/ is
# gitignored (same contract as tools/lspref.sh), so regenerate it, never assume it is present:
#
#   tools/lsp-bench.sh > build/lsp-bench-before.txt
#   ...change...
#   tools/lsp-bench.sh > build/lsp-bench-after.txt && diff -u build/lsp-bench-before.txt build/lsp-bench-after.txt
#
# Default mode drives `kama query <f> --symbols`, a faithful proxy for one lspAnalyze (same
# loadProgramUnits + prelude + fresh-CEmitter setup), with no JSON-RPC session — that is what makes it
# runnable from a plain checkout. It CANNOT see anything that only pays off in a process which
# analyzes more than once (the prelude cache, the closure cache, M4.6's repair); use --lsp for those.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "lsp-bench: $KAMA not built" >&2; exit 1; fi

REPS=${REPS:-7}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# median <file-of-numbers> — the median, not the mean: process-startup noise on macOS is one-sided, so
# a mean drifts upward with any single hiccup while the median does not.
median() {
    n=$(wc -l < "$1" | tr -d ' ')
    [ "$n" -gt 0 ] || { echo 0; return; }
    sort -n "$1" | sed -n "$(( (n + 1) / 2 ))p"
}

# field <line> <key> — pull `key=<number>` out of a kama-timing line.
field() {
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}

# ---- default mode: one analysis per process, via `kama query` ---------------------------------------
bench_query() {
    printf '%-42s %8s %8s %8s %8s %9s\n' file units buf-parse clo-parse prelude analyze
    printf '%-42s %8s %8s %8s %8s %9s\n' ------------------------------------------ ----- --------- --------- ------- -------
    for f in "$@"; do
        [ -f "$f" ] || { echo "lsp-bench: no such file: $f" >&2; continue; }
        : > "$tmp/clo"; : > "$tmp/pre"; : > "$tmp/ana"; : > "$tmp/tot"
        units=0
        i=0
        while [ "$i" -lt "$REPS" ]; do
            i=$((i + 1))
            line=$(KAMA_TIMING=1 "$KAMA" query "$f" --symbols 2>&1 >/dev/null | grep '^kama-timing:' | tail -1 || true)
            [ -n "$line" ] || { echo "lsp-bench: no timing line for $f (is KAMA_TIMING wired up?)" >&2; break; }
            [ "$i" -eq 1 ] && continue          # discard the warm-up run (cold page cache, cold dyld)
            field "$line" closure-parse >> "$tmp/clo"
            field "$line" prelude-parse >> "$tmp/pre"
            field "$line" analyze       >> "$tmp/ana"
            field "$line" total         >> "$tmp/tot"
            units=$(field "$line" closure-units)
        done
        printf '%-42s %8s %8s %8s %8s %9s\n' \
            "$(basename "$f")" "${units:-?}" 0.00 "$(median "$tmp/clo")" \
            "$(median "$tmp/pre")" "$(median "$tmp/ana")"
        printf 'key %s total=%s closure-parse=%s prelude-parse=%s analyze=%s units=%s\n' \
            "$f" "$(median "$tmp/tot")" "$(median "$tmp/clo")" \
            "$(median "$tmp/pre")" "$(median "$tmp/ana")" "${units:-?}" >> "$tmp/keys"
    done
    echo
    echo "# machine-diffable (ms, median of $((REPS - 1)) runs after one discarded warm-up)"
    cat "$tmp/keys" 2>/dev/null || true
}

# ---- --lsp mode: a real server session, many analyses in ONE process --------------------------------
# The only way to observe anything that pays off across requests. Reuses check-lsp.sh's framing: %s
# never interprets the body's backslashes, so JSON `\n` escapes stay two literal bytes — exactly what
# Content-Length counts.
bench_lsp() {
    session="$tmp/session"; : > "$session"
    frame() {
        body="$1"
        len=$(printf '%s' "$body" | wc -c | tr -d ' ')
        printf 'Content-Length: %s\r\n\r\n%s' "$len" "$body" >> "$session"
    }

    # A real file, so module resolution actually runs and the import closure is non-trivial.
    src="$ROOT/lib/std/process/process.kama"
    uri="file://$src"
    text=$(sed 's/\\/\\\\/g; s/"/\\"/g' "$src" | awk '{printf "%s\\n", $0}')

    frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$uri"'","languageId":"kama","version":1,"text":"'"$text"'"}}}'
    # Ten keystrokes. Each appends a comment line, so the buffer differs every time (no client-side
    # dedupe) while staying parseable — the common editing case.
    i=0
    while [ "$i" -lt 10 ]; do
        i=$((i + 1))
        frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'","version":'"$((i + 1))"'},"contentChanges":[{"text":"'"$text"'// keystroke '"$i"'\n"}]}}'
    done
    # One completion on a buffer that does NOT parse, with the LINE COUNT MOVED since the last good
    # parse — that pair is what defeats indexForRequest's fast path and fires M4.6's repair, which is
    # the 235 ms number M5 is here to fix. Appending a broken line at the SAME line count would take
    # the fast path instead and silently measure nothing, which is the whole reason the blank line
    # below is deliberate rather than cosmetic.
    frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$uri"'","version":99},"contentChanges":[{"text":"'"$text"'\nfn int32 zz() { Command c; c.\n"}]}}'
    # The file ends in a newline, so with one blank line inserted the broken line is 1-based kama line
    # wc+2 == 0-based LSP line wc+1. Off by one here puts the cursor past EOF, indexForRequest bails
    # before the repair, and the run silently measures nothing. Column 29 is just past the `.`.
    frame '{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":'"$(( $(wc -l < "$src" | tr -d ' ') + 1 ))"',"character":29}}}'
    frame '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit","params":null}'

    echo "# one 'kama lsp' process: didOpen, 10x didChange, then a completion on an unparseable buffer."
    echo "# Watch closure-parse/prelude-parse collapse after the first line once caching lands, and"
    echo "# watch for TWO lines against the completion (the second is M4.6's repair)."
    echo
    KAMA_TIMING=1 "$KAMA" lsp < "$session" 2>&1 >/dev/null | grep '^kama-timing:' | cat -n
}

if [ "${1:-}" = "--lsp" ]; then
    bench_lsp
else
    if [ "$#" -gt 0 ]; then
        bench_query "$@"
    else
        # Spans the closure sizes that matter: the fixed-cost floor (a 3-line file), a mid-size
        # single-unit file, and the two std-importing files that are over budget today.
        bench_query "$ROOT/tests/noheap_ok.kama" \
                    "$ROOT/tests/query/shapes.kama" \
                    "$ROOT/lib/std/collections/sorted_map.kama" \
                    "$ROOT/tests/query/complete.kama" \
                    "$ROOT/lib/std/process/process.kama"
    fi
fi
