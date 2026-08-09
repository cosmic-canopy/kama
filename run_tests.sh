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

ROOT="."                        # run_tests.sh already assumes cwd == repo root
. tools/kama-bin.sh             # sets $KAMA — this platform's build, else the root ./kama symlink
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

# `$EPOCHREALTIME` is bash 5. macOS ships bash 3.2, where now_ms() returns 0 for everything — so
# `suite_ms` was 0, the whole timing summary was skipped, and the host leg reported no timings at all
# while the container leg reported them fine. That is the wrong way round: the host is where a developer
# actually watches the clock.
#
# Phase timing needs only whole seconds, and `date +%s` has those everywhere, so phases are always
# reported. Per-fixture slowest-N still needs sub-second resolution and stays bash-5-only, with a note
# saying so rather than silently printing nothing.
HAVE_MS=1; [ -z "${EPOCHREALTIME:-}" ] && HAVE_MS=0
phase_start() { PHASE_T0=$(date +%s); PHASE_NAME="$1"; }
phase_end() { printf 'phase: %-22s %4ds\n' "$PHASE_NAME" "$(( $(date +%s) - PHASE_T0 ))"; }
suite_start_s=$(date +%s)

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

# Does this bash have `wait -n` (4.3+)? The gate below uses it to wake on the first completion instead of
# polling. bash 3.2 (macOS) does not, and errors with status 2 — which is what the `-ne 2` distinguishes.
#
# ⚠️ PROBE HERE, and nowhere later. `wait -n` waits for the next job to finish, so with ANY live background
# job it BLOCKS — and the wasm leg starts helper servers below that never exit. Probed after them, this line
# hangs the whole leg forever, which is exactly what it did between 69c1123 and this fix: `./dev test wasm`
# printed its banner and then sat there. With no jobs yet, bash 5 returns 127 immediately and bash 3.2
# returns 2, so both answers are correct and neither waits.
if wait -n >/dev/null 2>&1 || [ "$?" -ne 2 ]; then HAVE_WAIT_N=1; else HAVE_WAIT_N=0; fi

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

# The concurrency gate below counts `jobs -rp`, which includes the long-lived helper servers started just
# above — they are background jobs that never exit. Without budgeting for them the wasm leg (the SLOWEST
# one) silently runs at NCPU-1, and at NCPU-3 under KAMA_BROWSER=1. Cap on NCPU *plus* the helpers, so the
# fan-out always gets NCPU fixture slots whichever leg is active.
JOB_CAP="$NCPU"
for _p in "$WS_ECHO_PID" "$WT_ECHO_PID" "$SIG_RELAY_PID"; do [ -n "$_p" ] && JOB_CAP=$((JOB_CAP+1)); done

# The gate itself. Every fan-out below calls `gate` before spawning the next job.
#
# This used to be written inline as `while [ jobs -ge CAP ]; do wait -n || break; done`, which had two
# defects that both FAILED OPEN — the suite still passed, it just stopped throttling:
#
#   1. `wait -n` is bash 4.3+. macOS ships bash 3.2, where it exits 2 immediately, `|| break` fires, and
#      the gate lets the job through. Measured: spawning 8 jobs against a cap of 2 left 8 running. So the
#      host leg forked ~1800 concurrent jobs onto 10 cores and spent its time thrashing rather than
#      compiling.
#   2. Even on bash 5, `wait -n` returns the FINISHED JOB'S exit status, so one failing fixture also
#      tripped `|| break` and punched a hole in the cap for the rest of that phase.
#
# `wait -n` is still used where it exists, because it wakes on the first completion instead of polling;
# the `|| :` swallows a job's exit status (results are collected from files, never from `wait`). Elsewhere
# a short poll is correct, portable, and costs nothing next to a fixture that takes tens of milliseconds.
# (HAVE_WAIT_N is probed far above, before the helper servers start — see the warning there.)
gate() {
    while [ "$(jobs -rp | wc -l)" -ge "$JOB_CAP" ]; do
        if [ "$HAVE_WAIT_N" = 1 ]; then wait -n 2>/dev/null || :; else sleep 0.02; fi
    done
}

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

# The tools/check-*.sh guards. ONE runner (tools/run-checks.sh) drives them here AND in `./dev check`,
# glob-enrolled and in parallel, so the two can never drift again — which they already had: this file
# listed 22 guards by hand while `./dev check` globbed 24, and nobody noticed check-agents.sh was in one
# and not the other. Per-guard leg eligibility now lives in each guard's own `# check-legs:` header
# (default: native), so a new tools/check-*.sh joins with nothing to edit here.
#
# They used to run SERIALLY, right here, before the fan-out — 61s on a 10-core host with nine cores idle.
# In the pool the block is bounded by its slowest member instead of their sum.
#
# KAMA_TSAN is deliberately not a leg of its own: the old predicate was (SAN=0 && WASM=0), so a TSan run
# has always executed the native guard set. Keep that.
#
# `./dev matrix` sets KAMA_SKIP_CHECKS=1 for this leg, because `./dev check` is about to run the same set
# on the same host. A bare ./run_tests.sh — CI, the container — never sets it and stays complete.
if [ "${KAMA_SKIP_CHECKS:-0}" = 0 ]; then
    leg=native
    [ "${KAMA_SAN:-0}"  != 0 ] && leg=san
    [ "${KAMA_WASM:-0}" != 0 ] && leg=wasm
    phase_start "check-*.sh guards"
    # $KAMA is NOT passed through: ROOT is "." here, so it is relative, and guards `cd`. The runner
    # resolves an absolute one itself.
    sh tools/run-checks.sh --leg "$leg" --jobs "$NCPU" --tally "$TMP/checks.tally"
    phase_end
    if [ -f "$TMP/checks.tally" ]; then
        read -r cpass cfail <"$TMP/checks.tally"
        pass=$((pass+cpass)); fail=$((fail+cfail))
    else
        echo "FAIL check-*.sh guards (the runner produced no tally)"; fail=$((fail+1))
    fi
else
    echo "(guards skipped: KAMA_SKIP_CHECKS=1 — ./dev check runs them)"
fi

# std::process cross-platform child helper: ONE native binary the proc_* fixtures drive instead of POSIX-only
# utilities (sh/echo/cat/printenv/sleep/…), so they run identically on POSIX + Windows. Built once, plain
# native — it's a child process, so the parent fixture's sanitizers still cover std::process; skipped on the
# wasm leg (proc_* are skipped there). Fixtures locate it (and, for the cwd test, its dir + basename) via
# these exported vars. The dir is passed OS-native (CreateProcess's lpCurrentDirectory wants a Windows path).
if [ "$WASM" = 0 ] && [ -f "$TESTS_DIR/support/procutil.kama" ]; then
    procutil_bin="$TMP/procutil"
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) procutil_bin="$TMP/procutil.exe" ;; esac
    if "$KAMA" build "$TESTS_DIR/support/procutil.kama" -o "$procutil_bin" >/dev/null 2>"$TMP/procutil.err"; then
        procutil_dir="$(dirname "$procutil_bin")"
        export KAMA_PROCUTIL_BASE="$(basename "$procutil_bin")"
        case "$(uname -s)" in
            MINGW*|MSYS*|CYGWIN*)
                export KAMA_PROCUTIL="$(cygpath -w "$procutil_bin")"
                export KAMA_PROCUTIL_DIR="$(cygpath -w "$procutil_dir")" ;;
            *)
                export KAMA_PROCUTIL="$procutil_bin"
                export KAMA_PROCUTIL_DIR="$procutil_dir" ;;
        esac
    else
        echo "FAIL procutil (helper build failed)"; cat "$TMP/procutil.err"; fail=$((fail+1))
    fi
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
    local uses_net_web=0 uses_net=0 uses_proc=0 BROWSER=0
    { grep -q 'std::net::web' "$src" || grep -q 'kama_net_web.h' "$src"; } && uses_net_web=1
    grep -q 'std::net' "$src" && uses_net=1
    grep -q 'std::process' "$src" && uses_proc=1
    if [ "$WASM" = 1 ]; then
        if [ "$uses_net" = 1 ] && [ "$uses_net_web" = 0 ]; then
            echo "SKIP $name (native net: no raw sockets on wasm)" >"$out"; echo SKIP >"$res"; return
        fi
        # std::process has no meaning under wasm/emscripten (no fork/exec/waitpid in the sandbox) — same
        # class as native raw sockets above. Skip any fixture that imports it on the wasm leg.
        if [ "$uses_proc" = 1 ]; then
            echo "SKIP $name (std::process: no fork/exec on wasm)" >"$out"; echo SKIP >"$res"; return
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
    # A clean build means NO WARNINGS, from kama or from the C compiler. An exit-code suite is blind to
    # them, which is how a `static inline` that was declared in the shared header but defined only in one
    # translation unit, a `void**` passed where `uint8_t**` was declared, and 72 spurious shift-count
    # warnings all sat in the corpus unnoticed. Warnings are the C compiler telling us the emitter is
    # generating something it does not believe; treat that as a failure while the tree is clean.
    if grep -qi 'warning' "$TMP/$name.err"; then
        { echo "FAIL $name (built, but with warnings)"; grep -i 'warning' "$TMP/$name.err" | head -5; } >"$out"
        echo FAIL >"$res"; return
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
phase_start "single-file fixtures"
fixture_pids=()
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    test_one "$src" &
    fixture_pids+=($!)
    gate
done
wait "${fixture_pids[@]}" 2>/dev/null
phase_end
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
phase_start "multi-file fixtures"
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
#
# Fanned out across NCPU like the single-file leg, and for the same reason: each fixture is an independent
# `kama build` into its own $TMP files. Serial, this leg was ~325 compiler processes run one at a time.
xfail_one() {
    local src="$1" name out res err rc msg_file
    name="$(basename "$src" .kama)"
    out="$TMP/xf_$name.out"; res="$TMP/xf_$name.res"; err="$TMP/xf_$name.err"
    "$KAMA" build "$src" -o "$TMP/xf_$name" >/dev/null 2>"$err"; rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL xfail/$name (compiled, but must be REJECTED)" >"$out"; echo FAIL >"$res"; return
    fi
    # A rejection must be CLEAN. Death by signal (>=128) is a compiler crash, and without this arm it
    # scored a PASS here — "did not build" was indistinguishable from "segfaulted", which is exactly how
    # three diagnose-then-dereference bugs sat green in this suite.
    if [ "$rc" -ge 128 ]; then
        { echo "FAIL xfail/$name (compiler CRASHED, signal $((rc-128)) — a rejection must be clean, not a crash)"
          head -2 "$err"; } >"$out"; echo FAIL >"$res"; return
    fi
    msg_file="$TESTS_DIR/xfail/$name.msg"
    if [ -f "$msg_file" ] && ! grep -qF "$(cat "$msg_file")" "$err"; then
        { echo "FAIL xfail/$name (rejected, but error missing \"$(cat "$msg_file")\")"; head -2 "$err"; } >"$out"
        echo FAIL >"$res"; return
    fi
    echo "PASS xfail/$name (rejected)" >"$out"; echo PASS >"$res"
}
phase_end
phase_start "xfail fixtures"
xfail_pids=()
for src in "$TESTS_DIR"/xfail/*.kama; do
    [ -e "$src" ] || continue
    xfail_one "$src" &
    xfail_pids+=($!)
    gate
done
wait "${xfail_pids[@]}" 2>/dev/null
phase_end
# Tally in fixture order, so output is identical to the serial version regardless of completion order.
for src in "$TESTS_DIR"/xfail/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    [ -f "$TMP/xf_$name.out" ] && cat "$TMP/xf_$name.out"
    if [ "$(cat "$TMP/xf_$name.res" 2>/dev/null)" = "PASS" ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
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

# Analysis-path agreement: `kama check` must reach the SAME accept/reject verdict as `kama build`.
#
# Why this leg exists. Everything above drives `kama build`; `kama check` runs a different entry point
# (`analyze()`) that the LANGUAGE SERVER also runs, and until this leg was added NOTHING in the suite
# executed it. Two whole classes of defect lived in that blind spot:
#   - a use-after-free in the reference index crashed `kama check` (and so the editor) on any file that
#     reached an emitter-synthesized type node — 45 fixtures, while the build leg stayed green; and
#   - a diagnostic recorded on only ONE of the two paths makes the editor call a file clean that the
#     compiler rejects (comptime evaluation errors did exactly that), which is the single worst thing a
#     language server can do.
# So: every positive fixture must pass `check`, and every negative fixture must fail it. `check` does no C
# compilation, so this is cheap and target-independent — it runs on every leg.
analysis_skip() {   # fixtures where `check` legitimately cannot match `build` — currently NONE
    return 1
}
# This leg is ONE assertion, not one per fixture. It re-checks every fixture the suite already built, so
# counting each as a separate pass would double the headline number without doubling what is covered —
# the corpus is the same, only the entry point differs. A mismatch names the fixture; the tally is a
# single PASS/FAIL plus the sample size.
#
# Fanned out across NCPU, like both legs above. This is ~915 more compiler processes; run one at a time it
# was the single most expensive phase in the suite, and every one of them is independent.
check_pos_one() {
    local src="$1" name
    name="$(basename "$src" .kama)"
    if ! "$KAMA" check "$src" >/dev/null 2>"$TMP/ck_$name.err"; then
        { echo "  MISMATCH $name: builds, but \`kama check\` rejects it (the editor would show a clean file as broken)"
          head -3 "$TMP/ck_$name.err"; } >"$TMP/ck_$name.bad"
    fi
}
check_neg_one() {
    local src="$1" name ck_rc
    name="$(basename "$src" .kama)"
    "$KAMA" check "$src" >/dev/null 2>&1; ck_rc=$?
    if [ "$ck_rc" -eq 0 ]; then
        echo "  MISMATCH xfail/$name: \`kama build\` rejects it but \`kama check\` accepts it (the editor would show a broken file as clean)" >"$TMP/ckx_$name.bad"
    elif [ "$ck_rc" -ge 128 ]; then
        # `kama lsp` runs this analysis in-process on every keystroke, so a crash here kills the editor's
        # language server. Rejecting-by-crashing agreed with `build` and therefore used to pass silently.
        echo "  MISMATCH xfail/$name: \`kama check\` CRASHED (signal $((ck_rc-128))) — the language server would die on this input" >"$TMP/ckx_$name.bad"
    fi
}
ck_pos=0; ck_neg=0; ck_bad=0
phase_start "analysis agreement"
ck_pids=()
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    [ -f "$TMP/$name.res" ] && [ "$(cat "$TMP/$name.res")" = "PASS" ] || continue   # only fixtures that built
    ck_pos=$((ck_pos+1))
    check_pos_one "$src" &
    ck_pids+=($!)
    gate
done
for src in "$TESTS_DIR"/xfail/*.kama; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .kama)"
    if analysis_skip "$name"; then echo "  SKIP xfail/$name (rejected by the C compiler, not by kama)"; continue; fi
    ck_neg=$((ck_neg+1))
    check_neg_one "$src" &
    ck_pids+=($!)
    gate
done
wait "${ck_pids[@]}" 2>/dev/null
phase_end
# Report mismatches in fixture order — a MISMATCH names the fixture, so stable ordering keeps a diff of two
# runs meaningful.
for f in "$TMP"/ck_*.bad "$TMP"/ckx_*.bad; do
    [ -e "$f" ] || continue
    cat "$f"; ck_bad=$((ck_bad+1))
done
if [ "$ck_bad" -eq 0 ]; then
    echo "PASS analysis agreement ($ck_pos accepted + $ck_neg rejected, kama check matches kama build)"
    pass=$((pass+1))
else
    echo "FAIL analysis agreement ($ck_bad mismatch(es) across $((ck_pos+ck_neg)) fixtures)"
    fail=$((fail+1))
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
