#!/bin/bash
# kama end-to-end test harness.
#
# Each fixture is a .kama file under tests/ with a matching .expect file whose
# single line is the expected process exit code. We transpile, compile, run, and
# compare the exit code.
#
# A multi-file fixture is a directory tests/<name>.d/ containing several .kama
# files plus one .expect; all its .kama are built together (the module system).
set -u

KAMA="./kama"
TESTS_DIR="tests"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# Opt-in memory-safety pass: KAMA_SAN=1 builds every positive (and multi-file) fixture with
# ASan + UBSan and runs it, so a use-after-free / overflow / leak / UB fails the suite. Native
# only; xfail fixtures never link so they're unaffected. Requires the compiler-rt runtime in the
# image (Dockerfile: libclang-rt-*-dev). A halted sanitizer run exits nonzero -> reported as FAIL.
SAN_FLAGS=()
if [ "${KAMA_SAN:-0}" != "0" ]; then
    # -fno-sanitize=function: vtable / interface / BindableFunctionPtr dispatch stores each slot as
    # `Ret (*)(void* self, ...)` and calls the concrete `Ret C__m(C* self, ...)` through it. That
    # type-erased self is ABI-identical (how essentially all C OO dispatch works), but UBSan's
    # `function` sub-check enforces exact function-pointer type identity and would flag it. All other
    # UBSan checks (integer overflow, null, bounds, alignment, …) and ASan stay on.
    SAN_FLAGS=(--cc "clang -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer -g")
    export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
    echo "(sanitizer mode: ASan + UBSan on native positive fixtures)"
fi

# Opt-in wasm pass: KAMA_WASM=1 builds every positive (and multi-file) fixture to wasm via emcc and runs
# it under node, comparing the SAME .expect exit code — so a codegen/runtime divergence on the wasm target
# (or an emcc-integration regression like the source-map load break) fails the suite, not just the single
# CI smoke fixture. Mutually exclusive with KAMA_SAN. xfail fixtures never build, so they're unaffected.
WASM=0
if [ "${KAMA_WASM:-0}" != "0" ]; then
    WASM=1
    echo "(wasm mode: build every positive fixture to wasm + run under node)"
fi

# Servers for the wasm net::web E2E fixtures, started once for the wasm leg and torn down on exit.
# net_ws_loopback -> a Node WebSocket echo server (Node built-ins only). net_wt_loopback -> an aioquic
# HTTP/3 WebTransport echo server; capture the self-signed cert's hash so the browser harness can trust it.
# Browser E2E tests (WebTransport / WebRTC) launch headless Chromium per fixture — slow. They (and their
# aioquic / signaling-relay servers) run only when opted in with KAMA_BROWSER=1. The Node-based web tests
# (WebSocket) are cheap and stay in the default wasm leg.
BROWSER_TESTS="${KAMA_BROWSER:-0}"
WS_ECHO_PID=""; WT_ECHO_PID=""; SIG_RELAY_PID=""; WT_CERT_HASH=""
if [ "$WASM" = 1 ] && [ -f "$TESTS_DIR/support/ws_echo.js" ]; then
    node "$TESTS_DIR/support/ws_echo.js" 47670 >/dev/null 2>&1 &
    WS_ECHO_PID=$!
fi
if [ "$WASM" = 1 ] && [ "$BROWSER_TESTS" != 0 ] && [ -f "$TESTS_DIR/support/sig_relay.js" ]; then
    node "$TESTS_DIR/support/sig_relay.js" 47690 >/dev/null 2>&1 &   # WebRTC signaling relay (net_rtc_signaling)
    SIG_RELAY_PID=$!
fi
if [ "$WASM" = 1 ] && [ "$BROWSER_TESTS" != 0 ] && [ -f "$TESTS_DIR/support/wt_echo.py" ]; then
    python3 "$TESTS_DIR/support/wt_echo.py" 47680 >"$TMP/wt_echo.out" 2>/dev/null &
    WT_ECHO_PID=$!
    for _ in $(seq 1 50); do
        WT_CERT_HASH="$(awk '/CERTHASH/{print $2; exit}' "$TMP/wt_echo.out" 2>/dev/null)"
        [ -n "$WT_CERT_HASH" ] && break
        sleep 0.1
    done
fi
[ "$WASM" = 1 ] && sleep 0.3
trap '[ -n "$WS_ECHO_PID" ] && kill "$WS_ECHO_PID" 2>/dev/null; [ -n "$WT_ECHO_PID" ] && kill "$WT_ECHO_PID" 2>/dev/null; [ -n "$SIG_RELAY_PID" ] && kill "$SIG_RELAY_PID" 2>/dev/null; rm -rf "$TMP"' EXIT

# Build one fixture: $1 = output base path, $2… = source .kama file(s). Honors the active mode.
build_one() {
    local out="$1"; shift
    if [ "$WASM" = 1 ]; then
        "$KAMA" build "$@" --target wasm --cc "${EMCC:-emcc}" -o "$out.js"
    else
        "$KAMA" build "$@" ${SAN_FLAGS[@]+"${SAN_FLAGS[@]}"} -o "$out"
    fi
}
# Run one built fixture: $1 = output base path, $2 = stderr capture file. Sets global `actual`. A browser
# transport (BROWSER=1, e.g. WebTransport) runs the wasm in headless Chromium via Playwright; other wasm
# fixtures run under node; native runs the binary directly.
run_one() {
    if [ "$WASM" = 1 ]; then
        if [ "${BROWSER:-0}" = 1 ]; then
            KAMA_WT_CERT_HASH="$WT_CERT_HASH" node "$TESTS_DIR/support/browser_run.js" "$1.js" 2>"$2"
        else
            node "$1.js" 2>"$2"
        fi
    else
        "$1" 2>"$2"
    fi
    actual=$?
}

for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    expect_file="$TESTS_DIR/$name.expect"
    if [ ! -f "$expect_file" ]; then
        echo "SKIP $name (no .expect)"
        continue
    fi
    expected="$(cat "$expect_file")"

    # Net transports split by target. Native (TCP/UDP/Poller via raw sockets) can't run under the
    # browser/emscripten sandbox; the web transports (std::net::web — WebSocket/WebTransport over the JS
    # glue) can't run natively. Skip the half that doesn't apply to the active target.
    uses_net_web=0; { grep -q 'std::net::web' "$src" || grep -q 'kama_net_web.h' "$src"; } && uses_net_web=1
    uses_net=0;     grep -q 'std::net'      "$src" && uses_net=1
    if [ "$WASM" = 1 ]; then
        if [ "$uses_net" = 1 ] && [ "$uses_net_web" = 0 ]; then
            echo "SKIP $name (native net: no raw sockets on wasm)"; continue
        fi
    else
        if [ "$uses_net_web" = 1 ]; then
            echo "SKIP $name (web net: browser-only transport)"; continue
        fi
    fi

    # WebTransport + WebRTC are browser-only (no node) — run their wasm in headless Chromium via Playwright.
    # Opt-in (slow): skipped unless KAMA_BROWSER=1.
    BROWSER=0; grep -qE 'kama_wt_|kama_rtc_' "$src" && BROWSER=1
    if [ "$BROWSER" = 1 ] && [ "$BROWSER_TESTS" = 0 ]; then
        echo "SKIP $name (browser E2E — set KAMA_BROWSER=1 to run)"; continue
    fi

    exe="$TMP/$name"
    if ! build_one "$exe" "$src" >/dev/null 2>"$TMP/$name.err"; then
        echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; fail=$((fail+1)); continue
    fi
    run_one "$exe" "$TMP/$name.san"
    if [ ${#SAN_FLAGS[@]} -gt 0 ] && [ -s "$TMP/$name.san" ]; then
        echo "FAIL $name (sanitizer)"; head -20 "$TMP/$name.san"; fail=$((fail+1)); continue
    fi

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (exit $actual)"; pass=$((pass+1))
    else
        echo "FAIL $name (got $actual, expected $expected)"; fail=$((fail+1))
    fi
done

# Multi-file fixtures: tests/<name>.d/ with several .kama built together.
for dir in "$TESTS_DIR"/*.d; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir" .d)"
    expect_file="$dir/expect"
    if [ ! -f "$expect_file" ]; then
        echo "SKIP $name (no expect)"
        continue
    fi
    expected="$(cat "$expect_file")"

    BROWSER=0   # multi-file fixtures never use a browser transport
    exe="$TMP/$name"
    if ! build_one "$exe" "$dir"/*.kama >/dev/null 2>"$TMP/$name.err"; then
        echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; fail=$((fail+1)); continue
    fi
    run_one "$exe" "$TMP/$name.san"
    if [ ${#SAN_FLAGS[@]} -gt 0 ] && [ -s "$TMP/$name.san" ]; then
        echo "FAIL $name (sanitizer)"; head -20 "$TMP/$name.san"; fail=$((fail+1)); continue
    fi

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (multi-file, exit $actual)"; pass=$((pass+1))
    else
        echo "FAIL $name (got $actual, expected $expected)"; fail=$((fail+1))
    fi
done

# Negative fixtures: tests/xfail/<name>.kama MUST FAIL to build (a clear compile-time
# rejection — this is how we guard "reject bad code" guarantees like const-correctness,
# access control, and use-after-move). Optional tests/xfail/<name>.msg holds a substring
# the compiler's error output must contain, so we assert the RIGHT error, not any failure.
for src in "$TESTS_DIR"/xfail/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    err="$TMP/xf_$name.err"
    if "$KAMA" build "$src" -o "$TMP/xf_$name" >/dev/null 2>"$err"; then
        echo "FAIL xfail/$name (compiled, but must be REJECTED)"; fail=$((fail+1)); continue
    fi
    msg_file="$TESTS_DIR/xfail/$name.msg"
    if [ -f "$msg_file" ] && ! grep -qF "$(cat "$msg_file")" "$err"; then
        echo "FAIL xfail/$name (rejected, but error missing \"$(cat "$msg_file")\")"; head -2 "$err"; fail=$((fail+1)); continue
    fi
    echo "PASS xfail/$name (rejected)"; pass=$((pass+1))
done

# Trap fixtures: tests/trap/<name>.kama MUST build, then ABORT at runtime — a clean trap that guards the
# "no undefined behavior" guarantee (integer divide-by-zero, INT_MIN/-1, shift-past-width, float->int
# overflow, signed overflow in a debug build, out-of-bounds index, panic/assert). We assert the process
# was killed by a signal (exit >= 128; __builtin_trap -> SIGTRAP/SIGILL, abort -> SIGABRT). Optional
# tests/trap/<name>.msg is a substring the stderr must contain (bounds/panic print "… out of bounds" /
# "kama: panic: …"; a bare __builtin_trap prints nothing). Skipped under KAMA_SAN (UBSan would intercept
# the trap) and KAMA_WASM (node/wasm abort exit codes differ) — native-default leg only, like xfail is
# SAN-skipped. `ulimit -c 0` is best-effort core suppression (a pipe core_pattern ignores it, but those
# cores go unwritten to systemd-coredump anyway).
if [ "$WASM" = 0 ] && [ ${#SAN_FLAGS[@]} -eq 0 ]; then
    ulimit -c 0
    for src in "$TESTS_DIR"/trap/*.kama; do
        [ -e "$src" ] || continue
        name="$(basename "$src" .kama)"
        if ! "$KAMA" build "$src" -o "$TMP/trap_$name" >/dev/null 2>"$TMP/trap_$name.builderr"; then
            echo "FAIL trap/$name (build failed)"; head -5 "$TMP/trap_$name.builderr"; fail=$((fail+1)); continue
        fi
        "$TMP/trap_$name" 2>"$TMP/trap_$name.err"; actual=$?
        if [ "$actual" -lt 128 ]; then
            echo "FAIL trap/$name (exited $actual, expected a runtime trap)"; fail=$((fail+1)); continue
        fi
        msg_file="$TESTS_DIR/trap/$name.msg"
        if [ -f "$msg_file" ] && ! grep -qF "$(cat "$msg_file")" "$TMP/trap_$name.err"; then
            echo "FAIL trap/$name (trapped, but stderr missing \"$(cat "$msg_file")\")"; head -2 "$TMP/trap_$name.err"; fail=$((fail+1)); continue
        fi
        echo "PASS trap/$name (trapped, exit $actual)"; pass=$((pass+1))
    done
fi

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
