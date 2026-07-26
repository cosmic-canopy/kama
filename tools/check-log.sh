#!/bin/sh
# check-log.sh — std::log v1 (leveled/tagged diagnostics). std::log writes to stderr, and the standard harness
# captures a fixture's stderr into the sanitizer-report file (so a committed tests/*.kama fixture must not log)
# — hence this dedicated runner with INLINE fixtures. It checks:
#   1. default level (Info) — Error/Warn/Info reach stderr, Debug/Trace are filtered out.
#   2. KAMA_LOG env — raises the threshold (all five levels appear); the process-global config source.
#   3. --log flag + per-tag — `--log=warn,audio=debug` prints an audio debug but not an untagged one, and the
#      flag overrides KAMA_LOG (it is primary). Proves the argv->env bridge works in a debug multi-file build.
#   4. custom sink — setLogSink reroutes records (here to stdout), the default console sink goes silent.
#   5. embedded — transpile --target embedded lowers freestanding (kama_log_dispatch, no <stdio.h>).
# Native only.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KAMA="$ROOT/kama"
if [ ! -x "$KAMA" ]; then echo "check-log: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A program that logs one line at every level, with tags, so the filter's effect is observable on stderr.
cat > "$tmp/log.kama" <<'KAMA'
import std::log::{logError, logWarn, logInfo, logDebug, logTrace};
fn int32 main() {
    logError(tag: "net", msg: "err-line");
    logWarn(tag: "", msg: "warn-line");
    logInfo(tag: "audio", msg: "info-line");
    logDebug(tag: "audio", msg: "audio-debug");
    logDebug(tag: "net", msg: "net-debug");
    logTrace(tag: "net", msg: "trace-line");
    return 0;
}
KAMA
"$KAMA" build "$tmp/log.kama" -o "$tmp/log" >/dev/null 2>"$tmp/build.err" || {
    echo "check-log: FAIL — build failed" >&2; sed 's/^/  /' "$tmp/build.err" >&2; exit 1; }

# --- 1. default level = Info: Error/Warn/Info shown, Debug/Trace filtered -----
"$tmp/log" >/dev/null 2>"$tmp/e1" || { echo "check-log: FAIL — default-level run exited nonzero" >&2; exit 1; }
for want in "[ERROR] net: err-line" "[WARN] warn-line" "[INFO] audio: info-line"; do
    grep -qF "$want" "$tmp/e1" || { echo "check-log: FAIL — default level missing: $want" >&2; sed 's/^/  /' "$tmp/e1" >&2; exit 1; }
done
if grep -qF "[DEBUG]" "$tmp/e1" || grep -qF "[TRACE]" "$tmp/e1"; then
    echo "check-log: FAIL — Debug/Trace leaked at the default Info level" >&2; sed 's/^/  /' "$tmp/e1" >&2; exit 1
fi

# --- 2. KAMA_LOG env raises the threshold: all five levels appear --------------
KAMA_LOG=trace "$tmp/log" >/dev/null 2>"$tmp/e2" || { echo "check-log: FAIL — KAMA_LOG run exited nonzero" >&2; exit 1; }
for want in "[ERROR]" "[WARN]" "[INFO]" "[DEBUG]" "[TRACE]"; do
    grep -qF "$want" "$tmp/e2" || { echo "check-log: FAIL — KAMA_LOG=trace missing: $want" >&2; sed 's/^/  /' "$tmp/e2" >&2; exit 1; }
done

# --- 3a. --log per-tag: raise only `audio` to debug; `net` stays at the global warn ---
"$tmp/log" --log=warn,audio=debug >/dev/null 2>"$tmp/e3" || { echo "check-log: FAIL — --log run exited nonzero" >&2; exit 1; }
grep -qF "audio-debug" "$tmp/e3" || { echo "check-log: FAIL — per-tag audio=debug did not enable the audio debug line" >&2; sed 's/^/  /' "$tmp/e3" >&2; exit 1; }
if grep -qF "net-debug" "$tmp/e3"; then
    echo "check-log: FAIL — net debug printed though only audio was raised to debug" >&2; sed 's/^/  /' "$tmp/e3" >&2; exit 1
fi
# --- 3b. --log is primary: it overrides KAMA_LOG (flag error-global beats env trace) ---
KAMA_LOG=trace "$tmp/log" --log=error >/dev/null 2>"$tmp/e3b" || { echo "check-log: FAIL — override run exited nonzero" >&2; exit 1; }
grep -qF "[ERROR]" "$tmp/e3b" || { echo "check-log: FAIL — --log=error suppressed even Error" >&2; sed 's/^/  /' "$tmp/e3b" >&2; exit 1; }
if grep -qF "[WARN]" "$tmp/e3b" || grep -qF "[TRACE]" "$tmp/e3b"; then
    echo "check-log: FAIL — --log=error did not override KAMA_LOG=trace (flag must be primary)" >&2; sed 's/^/  /' "$tmp/e3b" >&2; exit 1
fi

# --- 4. custom sink reroutes records; the default console sink goes silent -----
cat > "$tmp/sink.kama" <<'KAMA'
import std::log::{setLogSink, logInfo};
fn void mySink(int32 level, string tag, string msg) { println(s: "SINK:${level}:${msg}"); }
fn int32 main() {
    setLogSink(s: mySink);
    logInfo(tag: "x", msg: "routed");
    return 0;
}
KAMA
"$KAMA" build "$tmp/sink.kama" -o "$tmp/sink" >/dev/null 2>"$tmp/sink.build.err" || {
    echo "check-log: FAIL — sink build failed" >&2; sed 's/^/  /' "$tmp/sink.build.err" >&2; exit 1; }
"$tmp/sink" >"$tmp/s.out" 2>"$tmp/s.err" || { echo "check-log: FAIL — sink run exited nonzero" >&2; exit 1; }
grep -qF "SINK:2:routed" "$tmp/s.out" || { echo "check-log: FAIL — custom sink did not receive the record on stdout" >&2; sed 's/^/  /' "$tmp/s.out" >&2; exit 1; }
if grep -qF "[INFO]" "$tmp/s.err"; then
    echo "check-log: FAIL — the default console sink still fired after setLogSink" >&2; exit 1
fi

# --- 5. embedded lowers freestanding ------------------------------------------
"$KAMA" transpile --target embedded "$tmp/log.kama" -o "$tmp/emb.c" >/dev/null 2>"$tmp/emb.err" || {
    echo "check-log: FAIL — --target embedded transpile failed" >&2; sed 's/^/  /' "$tmp/emb.err" >&2; exit 1; }
grep -q 'kama_log_dispatch' "$tmp/emb.c" || { echo "check-log: FAIL — embedded C does not reference kama_log_dispatch" >&2; exit 1; }
if grep -q 'stdio\.h' "$tmp/emb.c"; then echo "check-log: FAIL — embedded C leaked <stdio.h>" >&2; exit 1; fi

echo "check-log: PASS (level+tag filter, --log/KAMA_LOG config, swappable sink, freestanding lowering)"
