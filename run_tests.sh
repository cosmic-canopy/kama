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

# Report-only timing (never gates pass/fail): each fixture records its build+run wall-clock in ms so we can
# watch the suite's cost trend as fixtures grow. `$EPOCHREALTIME` (bash 5, on the container + brew-bash) is
# microseconds; the harness already needs bash >=4.3 for `wait -n`. Per-fixture wall-clock is NOISY under the
# parallel fan-out (cores are saturated), so treat the slowest-N as a trend signal, not a per-fixture budget.
now_ms() { local e="${EPOCHREALTIME:-0.0}"; echo $(( ${e%.*} * 1000 + 10#${e#*.} / 1000 )); }
suite_start=$(now_ms)

# Parallelism: each single-file fixture builds+runs independently into its own $TMP/$name.* files, so the
# main loop fans out across cores (the dominant cost is one clang invocation per fixture). Override with
# KAMA_JOBS. Results are collected per fixture then tallied in fixture order for stable output.
NCPU="${KAMA_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

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

# Opt-in data-race pass: KAMA_TSAN=1 builds every positive (and multi-file) fixture with ThreadSanitizer
# and runs it, so a data race across `isolate`s fails the suite — the shared-nothing PROOF for the M2
# concurrency seam (a multi-isolate fixture must stay TSan-clean). Native only; mutually exclusive with
# KAMA_SAN (one -fsanitize set at a time) and KAMA_WASM (wasm has no pthreads). xfail fixtures never link.
if [ "${KAMA_TSAN:-0}" != "0" ]; then
    if [ "${KAMA_SAN:-0}" != "0" ] || [ "${KAMA_WASM:-0}" != "0" ]; then
        echo "error: KAMA_TSAN is mutually exclusive with KAMA_SAN and KAMA_WASM" >&2; exit 2
    fi
    SAN_FLAGS=(--cc "clang -fsanitize=thread -fno-omit-frame-pointer -g")
    export TSAN_OPTIONS="halt_on_error=1"
    echo "(thread-sanitizer mode: TSan on native positive fixtures)"
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

# Editor-syntax drift guard: the hand-maintained VSCode grammar must cover every kama.l keyword. Run once
# (the plain native pass), not under the SAN/WASM re-runs. Keeps the highlighter honest as the language grows.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-syntax-drift.sh ]; then
    if sh tools/check-syntax-drift.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# MCU step 3: `--target embedded` freestanding-build guard (emitted entry shape + libc-free object). Like
# the drift guard, run once on the plain native pass (the SAN/WASM re-runs build the fixture hosted anyway).
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-embedded.sh ]; then
    if sh tools/check-embedded.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# MCU end-to-end: build a kama program into Cortex-M firmware and RUN it on emulated silicon (QEMU). SKIPs
# (still counts as a pass) when the cross toolchain is absent, so this only truly exercises under the opt-in
# `kama-mcu` image; on the base image it's a no-op. Native pass only (like the guards above).
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-mcu.sh ]; then
    if sh tools/check-mcu.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# Soft-float end-to-end: build a float-math kama program into no-FPU Cortex-M0 firmware and RUN it on QEMU
# (the emitted float ops become soft-float libcalls). SKIPs (still a pass) without the cross toolchain, like
# check-mcu.sh above. Native pass only.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-softfloat.sh ]; then
    if sh tools/check-softfloat.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# Task #2: command-line argv + environment access in the prelude floor. The standard harness runs fixtures
# with no args/env (only the empty paths, via tests/args_env_empty.kama), so this dedicated guard drives the
# WITH-args / SET-env paths + the `kama run --` passthrough. Runs on the native AND ASan passes (under
# KAMA_SAN the script builds the probe with sanitizers, covering the owned-string copies with real args);
# skipped on WASM (runs a native binary + needs a controllable process environment).
if [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-argv-env.sh ]; then
    if sh tools/check-argv-env.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# const-eval 6b-3: `comptime fn` table-baking guard (baked static-const aggregate + comptime fn not emitted).
# Transpile-only + host-checkable, so run once on the plain native pass like the drift/embedded guards.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-comptime.sh ]; then
    if sh tools/check-comptime.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# MCU step 5: `--no-heap` flag guard (flag-driven rejection can't ride the no-flag xfail loop). Run once on
# the plain native pass, like the guards above.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-noheap.sh ]; then
    if sh tools/check-noheap.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# Conditional compilation: `@compileFor(FLAG)` decl-gate guard — builds the same fixture DEBUG vs
# RELEASE and asserts (via transpile-grep) the gated body reaches the emitted C in exactly one build
# (Kama-level selection, no #ifdef). Transpile+build, so run once on the plain native pass.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-compilefor.sh ]; then
    if sh tools/check-compilefor.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# `debugAssert` strip: proves on the transpiled C that `--release` drops the dev-only debugAssert while the
# always-on assert survives (Kama-level strip, not a C #ifdef). Transpile-grep, so plain native pass only.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-debug-assert.sh ]; then
    if sh tools/check-debug-assert.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# M2.1 package store: `kama install` fetches git/url deps into the content-addressed store, verifies
# sha256 integrity, and writes a reproducible lock. Network-free (file:// git repo + local tarball). The
# `.d/` harness only runs `kama build`, so install/store/integrity ride here — plain native pass only.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-packages.sh ]; then
    if sh tools/check-packages.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# M1 toolchain selector: a versioned store + a PATH selector that resolves which toolchain to run per
# directory (project pin > KAMA_VERSION > global default). Network-free (stub versioned binaries) — proves
# resolution/precedence, not the download; plain native pass only.
if [ "${KAMA_SAN:-0}" = 0 ] && [ "${KAMA_WASM:-0}" = 0 ] && [ -f tools/check-toolchain.sh ]; then
    if sh tools/check-toolchain.sh; then pass=$((pass+1)); else fail=$((fail+1)); fi
fi

# One fixture's build+run+compare, run in a background subshell. Buffers its status line(s) into
# $TMP/$name.out and records PASS/FAIL/SKIP into $TMP/$name.res (tallied in fixture order afterward).
test_one() {
    local src="$1" name expect_file expected exe out res
    name="$(basename "$src" .kama)"
    out="$TMP/$name.out"; res="$TMP/$name.res"
    expect_file="$TESTS_DIR/$name.expect"
    if [ ! -f "$expect_file" ]; then echo "SKIP $name (no .expect)" >"$out"; echo SKIP >"$res"; return; fi
    expected="$(cat "$expect_file")"

    # Net transports split by target (see below); skip the half that doesn't apply to the active target.
    local uses_net_web=0 uses_net=0 BROWSER=0
    { grep -q 'std::net::web' "$src" || grep -q 'kama_net_web.h' "$src"; } && uses_net_web=1
    grep -q 'std::net' "$src" && uses_net=1
    if [ "$WASM" = 1 ]; then
        if [ "$uses_net" = 1 ] && [ "$uses_net_web" = 0 ]; then
            echo "SKIP $name (native net: no raw sockets on wasm)" >"$out"; echo SKIP >"$res"; return
        fi
        # Inline asm (MCU 6a) is native/embedded-only — target-specific machine instructions have no wasm
        # form. Skip any fixture that uses `asm(` on the wasm leg (mnemonics like `nop`/`wfi` aren't wasm).
        if grep -q 'asm(' "$src"; then
            echo "SKIP $name (inline asm: native/embedded only)" >"$out"; echo SKIP >"$res"; return
        fi
    else
        if [ "$uses_net_web" = 1 ]; then
            echo "SKIP $name (web net: browser-only transport)" >"$out"; echo SKIP >"$res"; return
        fi
    fi
    grep -qE 'kama_wt_|kama_rtc_' "$src" && BROWSER=1
    if [ "$BROWSER" = 1 ] && [ "$BROWSER_TESTS" = 0 ]; then
        echo "SKIP $name (browser E2E — set KAMA_BROWSER=1 to run)" >"$out"; echo SKIP >"$res"; return
    fi

    # Isolate each build in its own dir: the driver writes multi-unit intermediates to dirname(-o), and
    # imported-module `.c` names key off the MODULE (e.g. dynamic_array_1.c), so two fixtures importing the
    # same stdlib module would collide in a shared dir under parallelism.
    local wd="$TMP/w_$name"; mkdir -p "$wd"; exe="$wd/$name"
    local t0; t0=$(now_ms)
    if ! build_one "$exe" "$src" >/dev/null 2>"$TMP/$name.err"; then
        { echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; } >"$out"; echo FAIL >"$res"; return
    fi
    run_one "$exe" "$TMP/$name.san"
    echo $(( $(now_ms) - t0 )) >"$TMP/$name.ms"   # report-only build+run wall-clock (ms)
    if [ ${#SAN_FLAGS[@]} -gt 0 ] && [ -s "$TMP/$name.san" ]; then
        { echo "FAIL $name (sanitizer)"; head -20 "$TMP/$name.san"; } >"$out"; echo FAIL >"$res"; return
    fi
    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (exit $actual)" >"$out"; echo PASS >"$res"
    else
        echo "FAIL $name (got $actual, expected $expected)" >"$out"; echo FAIL >"$res"
    fi
}

# Fan out across NCPU cores, gating the number of concurrent jobs. Collect ONLY the fixture job PIDs and wait
# on those explicitly: a bare `wait` also blocks on the long-lived echo servers (ws_echo/wt_echo/sig_relay,
# started with `&` for the wasm net::web tests), which never exit — that hung the whole wasm leg after the
# single-file phase. `wait -n` in the gate is fine (it returns as soon as ANY fixture finishes).
fixture_pids=()
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    test_one "$src" &
    fixture_pids+=($!)
    while [ "$(jobs -rp | wc -l)" -ge "$NCPU" ]; do wait -n 2>/dev/null || break; done
done
wait "${fixture_pids[@]}" 2>/dev/null
# Tally in fixture order (stable output regardless of completion order).
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    [ -f "$TMP/$name.out" ] && cat "$TMP/$name.out"
    if [ -f "$TMP/$name.res" ]; then
        case "$(cat "$TMP/$name.res")" in
            PASS) pass=$((pass+1));;
            FAIL) fail=$((fail+1));;
        esac
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
# the trap) and KAMA_WASM (node/wasm abort exit codes differ), and on Windows/MSYS2 — there __builtin_trap
# surfaces as a 128+SIGILL exit but abort() does NOT (it exits 127), so the "killed by a signal" assertion
# is POSIX-only. Native POSIX (Linux/macOS) leg, like xfail is SAN-skipped. `ulimit -c 0` is best-effort
# core suppression (a pipe core_pattern ignores it, but those cores go unwritten to systemd-coredump anyway).
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) TRAP_OK=0 ;; *) TRAP_OK=1 ;; esac
if [ "$WASM" = 0 ] && [ ${#SAN_FLAGS[@]} -eq 0 ] && [ "$TRAP_OK" = 1 ]; then
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
elif [ "$WASM" = 0 ] && [ ${#SAN_FLAGS[@]} -eq 0 ] && [ "$TRAP_OK" = 0 ]; then
    for src in "$TESTS_DIR"/trap/*.kama; do
        [ -e "$src" ] || continue
        echo "SKIP $(basename "$src" .kama) (trap: POSIX signal-exit convention only)"
    done
fi

# Report-only timing summary (never affects pass/fail). Total suite wall-clock + the slowest fixtures, so a
# creeping compile cost is visible as the suite grows. Skipped if EPOCHREALTIME was unavailable (all 0 ms).
suite_ms=$(( $(now_ms) - suite_start ))
timed=$(ls "$TMP"/*.ms 2>/dev/null | wc -l | tr -d ' ')
if [ "$timed" -gt 0 ] && [ "$suite_ms" -gt 0 ]; then
    echo "----"
    printf 'timing: suite %d.%03ds wall, %d fixtures timed (build+run). slowest:\n' \
        $((suite_ms/1000)) $((suite_ms%1000)) "$timed"
    for f in "$TMP"/*.ms; do printf '%s %s\n' "$(cat "$f")" "$(basename "$f" .ms)"; done \
        | sort -rn | head -15 | while read -r ms nm; do printf '  %6d ms  %s\n' "$ms" "$nm"; done
fi

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
