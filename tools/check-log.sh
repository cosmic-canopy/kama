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
. "$ROOT/tools/kama-bin.sh"
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

# --- 6. baked kama.json `log` default: compiled into the binary, env/flag still win (M5) ---
# Build the same program inside a project dir whose kama.json bakes a default filter (global info, audio=debug).
mkdir -p "$tmp/proj/src"
cp "$tmp/log.kama" "$tmp/proj/src/log.kama"
cat > "$tmp/proj/kama.json" <<'JSON'
{ "name": "logtest", "version": "0.1.0", "kind": "executable", "entry": "src/log.kama",
  "log": { "level": "info", "tags": { "audio": "debug" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
# Named by its MANIFEST: the baked default is the whole point here, and naming the .kama file instead is
# a loose build, which reads no manifest and would bake nothing.
"$KAMA" build "$tmp/proj/kama.json" -o "$tmp/proj/log" >/dev/null 2>"$tmp/proj.build.err" || {
    echo "check-log: FAIL — baked-default build failed" >&2; sed 's/^/  /' "$tmp/proj.build.err" >&2; exit 1; }
# 6a. bare run: the baked per-tag audio=debug shows an audio debug line; net stays at the baked global info.
"$tmp/proj/log" >/dev/null 2>"$tmp/e6" || { echo "check-log: FAIL — baked-default run exited nonzero" >&2; exit 1; }
grep -qF "audio-debug" "$tmp/e6" || { echo "check-log: FAIL — baked audio=debug did not enable the audio debug line" >&2; sed 's/^/  /' "$tmp/e6" >&2; exit 1; }
if grep -qF "net-debug" "$tmp/e6"; then
    echo "check-log: FAIL — net debug printed though the baked default only raised audio" >&2; sed 's/^/  /' "$tmp/e6" >&2; exit 1
fi
# 6b. KAMA_LOG overrides the baked default (overwrite=0 means a set env wins): warn suppresses the audio debug.
KAMA_LOG=warn "$tmp/proj/log" >/dev/null 2>"$tmp/e6b" || { echo "check-log: FAIL — baked+env run exited nonzero" >&2; exit 1; }
if grep -qF "[DEBUG]" "$tmp/e6b" || grep -qF "[INFO]" "$tmp/e6b"; then
    echo "check-log: FAIL — KAMA_LOG=warn did not override the baked default" >&2; sed 's/^/  /' "$tmp/e6b" >&2; exit 1
fi
# 6c. --log overrides both: debug enables the net debug line the baked default withheld.
"$tmp/proj/log" --log=debug >/dev/null 2>"$tmp/e6c" || { echo "check-log: FAIL — baked+flag run exited nonzero" >&2; exit 1; }
grep -qF "net-debug" "$tmp/e6c" || { echo "check-log: FAIL — --log=debug did not override the baked default" >&2; sed 's/^/  /' "$tmp/e6c" >&2; exit 1; }

# --- 7. kama.local.json deep-merges over kama.json (M5.2): per-tag merge, local wins -----------
# base: global info + net=trace; local: adds audio=debug and raises the global to warn. The merged filter must
# (a) keep the base net=trace tag, (b) apply the local audio=debug tag, (c) apply the local global warn — so an
# untagged (`db`) info line is dropped. A dedicated program makes each of the three observable.
# ⚠️ Its OWN project, and its source under `source`. A manifest operand names every file under the
# project's source root, so sharing 6's directory would put two `main`s in one program — and a file
# sitting BESIDE src/ belongs to no project at all.
mkdir -p "$tmp/proj2/src"
cat > "$tmp/proj2/src/merge.kama" <<'KAMA'
import std::log::{logInfo, logDebug};
fn int32 main() {
    logDebug(tag: "net", msg: "net-debug");     // base net=trace  -> shown
    logDebug(tag: "audio", msg: "audio-debug"); // local audio=debug -> shown
    logInfo(tag: "db", msg: "db-info");         // global warn      -> dropped
    return 0;
}
KAMA
cat > "$tmp/proj2/kama.json" <<'JSON'
{ "name": "logmerge", "version": "0.1.0", "kind": "executable", "entry": "src/merge.kama",
  "log": { "level": "info", "tags": { "net": "trace" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/proj2/kama.local.json" <<'JSON'
{ "log": { "level": "warn", "tags": { "audio": "debug" } } }
JSON
"$KAMA" build "$tmp/proj2/kama.json" -o "$tmp/proj/merge" >/dev/null 2>"$tmp/proj2.build.err" || {
    echo "check-log: FAIL — local-override build failed" >&2; sed 's/^/  /' "$tmp/proj2.build.err" >&2; exit 1; }
"$tmp/proj/merge" >/dev/null 2>"$tmp/e7" || { echo "check-log: FAIL — local-override run exited nonzero" >&2; exit 1; }
grep -qF "net-debug" "$tmp/e7" || { echo "check-log: FAIL — base net=trace not preserved through the local merge" >&2; sed 's/^/  /' "$tmp/e7" >&2; exit 1; }
grep -qF "audio-debug" "$tmp/e7" || { echo "check-log: FAIL — local audio=debug tag did not take effect" >&2; sed 's/^/  /' "$tmp/e7" >&2; exit 1; }
if grep -qF "db-info" "$tmp/e7"; then
    echo "check-log: FAIL — local level=warn did not raise the global threshold (an untagged info leaked)" >&2; sed 's/^/  /' "$tmp/e7" >&2; exit 1
fi
rm -f "$tmp/proj/kama.local.json"

# --- 8. std::log v2 (M7): recognized-facade lowering — message built only when the record passes ----------
# 8a/8b: the message is a function call with an OBSERVABLE side effect (prints a marker to stdout). The v2
# lowering builds the message INSIDE the runtime guard, so a filtered-out call never runs it.
cat > "$tmp/v2.kama" <<'KAMA'
import std::log::{logInfo, logDebug};
fn string expensive() { println(s: "BUILT"); return "payload"; }
fn int32 main() {
    logInfo(tag: "x", msg: "info-line");
    logDebug(tag: "net", msg: expensive());
    return 0;
}
KAMA
"$KAMA" build "$tmp/v2.kama" -o "$tmp/v2" >/dev/null 2>"$tmp/v2.build.err" || {
    echo "check-log: FAIL — v2 build failed" >&2; sed 's/^/  /' "$tmp/v2.build.err" >&2; exit 1; }
# 8a. default Info: the debug is filtered → expensive() must NOT run (no BUILT on stdout), no debug line.
"$tmp/v2" >"$tmp/v8.out" 2>"$tmp/v8.err" || { echo "check-log: FAIL — v2 default run exited nonzero" >&2; exit 1; }
if grep -qF "BUILT" "$tmp/v8.out"; then
    echo "check-log: FAIL — v2 built the message though the debug record was filtered out (not zero-cost)" >&2; exit 1
fi
grep -qF "[INFO] x: info-line" "$tmp/v8.err" || { echo "check-log: FAIL — v2 dropped a passing info line" >&2; sed 's/^/  /' "$tmp/v8.err" >&2; exit 1; }
if grep -qF "[DEBUG]" "$tmp/v8.err"; then echo "check-log: FAIL — v2 emitted a filtered debug line" >&2; exit 1; fi
# 8b. --log=debug: the guard passes → expensive() runs and the record is logged.
"$tmp/v2" --log=debug >"$tmp/v8b.out" 2>"$tmp/v8b.err" || { echo "check-log: FAIL — v2 --log=debug run exited nonzero" >&2; exit 1; }
grep -qF "BUILT" "$tmp/v8b.out" || { echo "check-log: FAIL — v2 did not build the message when the debug record passed" >&2; exit 1; }
grep -qF "[DEBUG] net: payload" "$tmp/v8b.err" || { echo "check-log: FAIL — v2 did not log the passing debug record" >&2; sed 's/^/  /' "$tmp/v8b.err" >&2; exit 1; }

# 8c: --release physically strips Debug/Trace call sites (keep Error/Warn/Info). Distinctive message literals
# appear ONLY in each level's interpolation, so a transpile-grep proves presence/absence unambiguously.
cat > "$tmp/strip.kama" <<'KAMA'
import std::log::{logInfo, logDebug, logTrace};
fn int32 main() {
    int32 n = 1;
    logInfo(tag: "x", msg: "KEEPME ${n}");
    logDebug(tag: "y", msg: "DROPDEBUG ${n}");
    logTrace(tag: "z", msg: "DROPTRACE ${n}");
    return 0;
}
KAMA
"$KAMA" transpile --release "$tmp/strip.kama" -o "$tmp/strip.rel.c" >/dev/null 2>"$tmp/strip.err" || {
    echo "check-log: FAIL — --release transpile failed" >&2; sed 's/^/  /' "$tmp/strip.err" >&2; exit 1; }
grep -qF "KEEPME" "$tmp/strip.rel.c" || { echo "check-log: FAIL — --release stripped an info call (only Debug/Trace should go)" >&2; exit 1; }
if grep -qF "DROPDEBUG" "$tmp/strip.rel.c" || grep -qF "DROPTRACE" "$tmp/strip.rel.c"; then
    echo "check-log: FAIL — --release did not strip a Debug/Trace call site" >&2; exit 1
fi
# Without --release all three survive (runtime-configurable).
"$KAMA" transpile "$tmp/strip.kama" -o "$tmp/strip.dbg.c" >/dev/null 2>"$tmp/strip.dbg.err" || {
    echo "check-log: FAIL — debug transpile failed" >&2; sed 's/^/  /' "$tmp/strip.dbg.err" >&2; exit 1; }
for want in "KEEPME" "DROPDEBUG" "DROPTRACE"; do
    grep -qF "$want" "$tmp/strip.dbg.c" || { echo "check-log: FAIL — a non-release build dropped $want (should be runtime-gated, not stripped)" >&2; exit 1; }
done

# --- 5. embedded lowers freestanding ------------------------------------------
"$KAMA" transpile --target embedded "$tmp/log.kama" -o "$tmp/emb.c" >/dev/null 2>"$tmp/emb.err" || {
    echo "check-log: FAIL — --target embedded transpile failed" >&2; sed 's/^/  /' "$tmp/emb.err" >&2; exit 1; }
grep -q 'kama_log_dispatch' "$tmp/emb.c" || { echo "check-log: FAIL — embedded C does not reference kama_log_dispatch" >&2; exit 1; }
if grep -q 'stdio\.h' "$tmp/emb.c"; then echo "check-log: FAIL — embedded C leaked <stdio.h>" >&2; exit 1; fi

echo "check-log: PASS (level+tag filter, --log/KAMA_LOG config, baked kama.json default, kama.local.json deep-merge, swappable sink, v2 msg-inside-guard + --release strip, freestanding lowering)"
