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
# Overridable so THIS HARNESS can be pointed at a chosen fixture set. `./dev fixture <name>` already runs
# one fixture, but on the host only and without the watchdog — so neither the wasm leg nor the hang
# instrumentation could be exercised on a single fixture, which is exactly what diagnosing an intermittent
# hang (or rehearsing the diagnostics for one) needs.
TESTS_DIR="${KAMA_TESTS_DIR:-tests}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# Report-only timing (never gates pass/fail): each fixture records its build+run wall-clock in ms so we can
# watch the suite's cost trend as fixtures grow. `$EPOCHREALTIME` (bash 5, on the container + brew-bash) is
# microseconds, and absent on macOS's bash 3.2 — see HAVE_MS below. Per-fixture wall-clock is NOISY under the
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

# ...and `kama build` must NOT also fan out. `-j` (KAMA_BUILD_JOBS) compiles a program's translation
# units concurrently, which is ~2x for a developer building one program on an idle machine — but this
# loop already saturates every core with one fixture per core, so per-TU splitting here only adds
# processes and costs CPU (a cold per-TU compile is ~30-40% more work than one invocation over the same
# sources). j=1 is also *today's exact command*, so the suite keeps covering the path everyone ships on.
# Overridable, deliberately: `KAMA_BUILD_JOBS=4 ./dev test` is the A/B that justifies this line.
export KAMA_BUILD_JOBS="${KAMA_BUILD_JOBS:-1}"

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

# (There used to be a `wait -n` capability probe here, for a gate that polled. The gate is a fifo token
# semaphore now — see `spawn` below — which blocks on a `read` and needs no probe. Worth remembering why
# the probe had to sit exactly here if one is ever reintroduced: `wait -n` waits for the NEXT job to
# finish, so with any live background job it BLOCKS, and the wasm leg starts helper servers below that
# never exit. Probed after them, it hung the whole leg forever — `./dev test wasm` printed its banner and
# sat there, between 69c1123 and its fix.)

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

# The concurrency gate: a TOKEN SEMAPHORE over a fifo. `spawn` takes a token before forking and the job
# hands it back as its last act, so the pool is exactly $NCPU wide.
#
# It reads as more machinery than the `while [ $(jobs -rp | wc -l) -ge CAP ]` count it replaced, and it
# is strictly less work. That loop ran in the PARENT, on the serial path that launches every fixture, and
# each turn of it cost a subshell + a `wc` — and on macOS (bash 3.2, no `wait -n`) it then slept 20 ms, so
# a phase of 597 fixtures could spend ~12 s doing nothing but waiting to notice a free slot. `read` on a
# fifo blocks until a token is actually there, waking immediately, forking nothing.
#
# History worth keeping, because both of these FAILED OPEN — the suite still passed, it just stopped
# throttling, and neither is possible in this shape:
#
#   1. `wait -n` is bash 4.3+; on macOS's bash 3.2 it exits 2 immediately, so the original
#      `wait -n || break` let every job through. Measured: 8 jobs against a cap of 2 left 8 running, i.e.
#      the host leg forked ~1800 concurrent jobs onto 10 cores and thrashed instead of compiling.
#   2. Even on bash 5, `wait -n` returns the FINISHED JOB'S status, so one failing fixture tripped
#      `|| break` and punched a hole in the cap for the rest of the phase.
#
# The long-lived helper servers (ws/wt echo, signal relay) no longer need budgeting for: they are not
# spawned through `spawn`, so they hold no token, and the fan-out gets its full $NCPU on every leg. That
# is what the old JOB_CAP fudge existed to patch up.
#
# A job killed outright would never return its token and the pool would narrow by one. Nothing kills
# these (the trap fixtures are a separate serial leg, and a crashing fixture BINARY is captured by the
# subshell, which still returns its token), and the failure mode is "slower", never "wrong".
SEM="$TMP/sem.fifo"
mkfifo "$SEM"
exec 9<>"$SEM"                                   # read-write, so it never sees EOF and never blocks on open
_i=0; while [ "$_i" -lt "$NCPU" ]; do printf '\n' >&9; _i=$((_i+1)); done

# spawn <fn> [args…] — block for a slot, run the job in the background, release the slot when it ends.
# $! is shell-global, so a caller can still collect the pid straight after calling this.
spawn() {
    read -r -u 9 _tok
    { "$@"; printf '\n' >&9; } &
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
# Run a fixture under a WATCHDOG, because a fixture that hangs used to wedge the whole suite forever.
#
# ✅ SOLVED 2026-08-23, after `tests/fs_raii.kama` hung the wasm leg four times (08-13, 08-15, 08-17,
# 08-23). Kept in full because the watchdog below is what solved it, and because the two readings this
# comment carried BEFORE the answer were both wrong in instructive ways.
#
# The cause was not kama and not this fixture. Without `-sEXIT_RUNTIME=1` an emscripten program returns
# from `main` and leaves node to wind the runtime down on its own, which calls
# node::NodePlatform::DrainTasks — and that deadlocks against V8's own background threads:
#
#     main thread   DrainTasks                -> waiting for background tasks to finish
#     background    AwaitCollectionBackground -> waiting for the main thread to run a GC
#
# A closed cycle, no kama frame in it, and it happens AFTER the fixture's work is done. Reproduced at
# ~11-13% under 16-way parallel load on a 6-CPU container; `--no-concurrent-recompilation` takes it to
# 0/200 while `--no-concurrent-marking` changes nothing, which names the background thread as a
# concurrent TurboFan compile job. fs_raii is the most exposed fixture because its 5,000-iteration loop
# is exactly what triggers optimization, immediately before it exits. The fix is in the driver: every
# wasm build now sets EXIT_RUNTIME, so the main thread calls process.exit() and never enters that
# teardown path — 0/300 against 40/300 for the same program without it.
#
# ⚠️ Two wrong turns worth keeping, because both are shapes of reasoning to distrust:
#
#   1. "0% CPU is blocked, not slow" was RIGHT, and was then talked out of. The first two catches had no
#      evidence at all — no time bound, so the log's mtime just froze — and each re-run passed, so it was
#      written off as a flake. A flake does not pick the same fixture four times.
#   2. The third catch was read as "parked in a synchronous syscall … the corpus's heaviest syscall user
#      … container I/O contention", and concluded "NOT deadlocked: 123,908 ms against a 1,909 ms normal
#      time — 65x slow, killed at the cap, not stopped." That inference does not hold: being killed AT
#      THE CAP is exactly what a deadlock looks like, so elapsed-time-at-the-timeout can never
#      distinguish slow from stopped. (It was also measured against the wrong baseline — 1,909 ms is
#      build+run; the program itself runs in 35 ms.) And when the fourth catch finally produced stacks,
#      no thread was in a filesystem syscall at all.
#
# The lesson the watchdog encodes: a hang is only diagnosable if something captures state WHILE it is
# hung. Three catches produced narrative; the one that produced an eu-stack produced the answer.
#
# So: bound it, and make the timeout produce the evidence the hang never did. On expiry the child gets
# SIGUSR2 first — for `node` that writes a diagnostic report naming every live libuv handle (`timer`,
# `fs_event`, `tcp`, …), i.e. exactly what the loop is waiting on — and only then SIGKILL.
#
# Hand-rolled rather than `timeout(1)`: macOS ships neither `timeout` nor `gtimeout`, and the native leg
# is the host leg. Two extra idle processes per fixture is the price; the suite already forks per fixture.
# $1 is a marker path: the watchdog TOUCHES it when it fires, and that — not the exit code — is how the
# caller knows. Inferring "was it killed?" from `128 + signo` does not survive the platform: SIGUSR2 is 12
# on Linux and 31 on macOS, and the native leg is the host leg, so an exit-code test would silently stop
# recognising a hang on exactly one platform. A file is unambiguous everywhere.
KAMA_FIXTURE_TIMEOUT="${KAMA_FIXTURE_TIMEOUT:-120}"   # seconds; 0 disables. Slowest real fixture: ~1.2s.
watchdog_run() {
    local mark="$1"; shift
    if [ "$KAMA_FIXTURE_TIMEOUT" = 0 ]; then "$@"; return $?; fi
    "$@" &
    local child=$!
    ( sleep "$KAMA_FIXTURE_TIMEOUT"
      kill -0 "$child" 2>/dev/null || exit 0          # finished in time — nothing to do
      : >"$mark"
      # Native-level evidence FIRST, while the process is still alive and before any signal. It is the
      # only kind that survives every hang shape. `--report-on-signal` below covers exactly one of the
      # three: node writes its report on the MAIN THREAD, so a loop that is idle-waiting on a handle
      # produces one, while a thread that is spinning or parked in a synchronous syscall produces
      # nothing at all. Probed on node v26 (2026-08-17): idle `setInterval` -> report written;
      # `while(true){}` -> no report; `fs.readFileSync(<fifo>)` -> no report. Two of the three shapes
      # were invisible, and both are what a filesystem fixture under emscripten NODEFS actually does.
      { if [ -r "/proc/$child/status" ]; then                    # Linux (the container legs)
            echo "  state:   $(awk '/^State:/{ $1=""; print }' "/proc/$child/status" 2>/dev/null)"
            echo "  wchan:   $(cat "/proc/$child/wchan" 2>/dev/null)"
            echo "  syscall: $(cut -d' ' -f1 "/proc/$child/syscall" 2>/dev/null)"
            echo "  open fds: $(ls "/proc/$child/fd" 2>/dev/null | wc -l)"
            # EVERY thread, not just the main one. A `futex_do_wait` on the main thread says a lock is
            # held; it cannot say by whom, and that is the only question worth asking about a deadlock.
            # node under emscripten NODEFS runs a libuv threadpool, so the shape that matters is whether
            # the workers are idle (main thread waiting on work that never arrives) or themselves parked
            # (a real cycle). Free to collect, readable without privileges — unlike a native backtrace,
            # which needs elfutils/gdb in the image AND ptrace, neither of which the container has.
            for t in "/proc/$child/task"/*; do
                [ -d "$t" ] || continue
                echo "  thread ${t##*/}: state=$(awk '/^State:/{ print $2 }' "$t/status" 2>/dev/null)" \
                     "wchan=$(cat "$t/wchan" 2>/dev/null)" \
                     "syscall=$(cut -d' ' -f1 "$t/syscall" 2>/dev/null)"
            done
            # ...and the frames, which is the only thing that NAMES the lock. `eu-stack` (elfutils) is in
            # the image for this; the container can already ptrace a sibling (`ptrace_scope` 0, uid 0), so
            # no capability change was needed. Rehearsed against a futex-parked node before being wired in
            # — it resolves `FutexEmulation::WaitJs32`, `uv_run`, `uv_cond_wait` and friends per thread.
            # Taken BEFORE the USR2 below because this is the shape node's own report cannot cover: a main
            # thread parked in a syscall never reaches the handler that would write one.
            if command -v eu-stack >/dev/null 2>&1; then
                echo "  --- eu-stack (first 80 frames) ---"
                timeout 20 eu-stack -p "$child" 2>&1 | head -80 | sed 's/^/  /'
            else
                echo "  (no eu-stack — install elfutils in the image to name the blocking frame)"
            fi
        else                                                     # macOS host leg
            ps -o state=,wchan=,time= -p "$child" 2>/dev/null | sed 's/^/  ps: /'
            # `sample` ships with the Xcode command line tools; absent, this is simply skipped. NOT
            # wrapped in `timeout`: macOS has neither `timeout` nor `gtimeout`, so the wrapper silently
            # swallowed the whole thing (found by rehearsing this path, not by reading it). `sample`
            # self-terminates after its duration argument, so it needs no wrapper anyway.
            if command -v sample >/dev/null 2>&1; then
                echo "  --- sample (1s) ---"
                # Just the call graph. `sample` frames it with ~25 lines of process header and then a
                # `Binary Images:` dump of every loaded dylib, which is longer than the graph and says
                # nothing about the hang — left in, it pushed the frames out of the budget entirely.
                sample "$child" 1 -mayDie 2>/dev/null \
                    | sed -n '/^Call graph:/,/^Binary Images:/p' | grep -v '^Binary Images:' \
                    | head -60 | sed 's/^/  /'
            fi
        fi
      } >"${mark%/*}/hang_native.txt" 2>/dev/null
      kill -s USR2 "$child" 2>/dev/null               # ask node to dump WHY it is still alive
      sleep 3
      kill -s KILL "$child" 2>/dev/null ) &
    local dog=$!
    wait "$child"; local rc=$?
    kill "$dog" 2>/dev/null; wait "$dog" 2>/dev/null
    return $rc
}

# Run one built fixture: $1 = output base path, $2 = stderr capture file. Sets global `actual`. A browser
# transport (BROWSER=1, e.g. WebTransport) runs the wasm in headless Chromium via Playwright; other wasm
# fixtures run under node; native runs the binary directly.
run_one() {
    # --report-on-signal costs nothing until the signal arrives, and both the report and the watchdog's
    # marker land in the fixture's own build dir, which is already per-fixture and already swept.
    local rdir; rdir="$(dirname -- "$1")"
    if [ "$WASM" = 1 ]; then
        if [ "${BROWSER:-0}" = 1 ]; then
            KAMA_WT_CERT_HASH="$WT_CERT_HASH" \
            watchdog_run "$rdir/timed_out" node "$TESTS_DIR/support/browser_run.js" "$1.js" 2>"$2"
        else
            # ⚠️ `--no-concurrent-recompilation` is not a performance knob — it is the fs_raii hang.
            # node's shutdown deadlocks against V8's own background threads:
            #
            #     main thread   node::NodePlatform::DrainTasks            -> waits for background tasks
            #     background    CollectionBarrier::AwaitCollectionBackground -> waits for main to run a GC
            #
            # A closed cycle with no kama frame and no wasm frame in it, entered after the fixture's work
            # is done and its assertion has passed. The background thread is a concurrent TurboFan compile
            # job — bisected: this flag took it to 0/200 while `--no-concurrent-marking` changed nothing.
            # fs_raii is the most exposed fixture because its 5,000-iteration loop is exactly what triggers
            # optimization, right before exit.
            #
            # `-sEXIT_RUNTIME=1` (kama.driver.cpp) was believed to close this path. It does NOT, and a live
            # stack from a 2026-08-25 matrix run proves it: emscripten's node `quit_` sets `process.exitCode`
            # and THROWS, so node still winds down gracefully and still reaches DrainTasks. EXIT_RUNTIME
            # only narrows the window, which is why the hang went from ~11-13% to roughly 1 in 12,000 and
            # then came back.
            #
            # Costs nothing here (8 vs 9 ms/run, measured): a 32 ms fixture never profits from concurrent
            # recompilation anyway. Deliberately NOT pushed into what kama EMITS — forcing `process.exit()`
            # into every user's wasm output truncates piped stdout (300,000 lines -> 309 against a slow
            # reader, measured), which is a far worse bug than a rare teardown deadlock.
            NODE_OPTIONS="--report-on-signal --report-directory=$rdir --report-filename=hang.json" \
            watchdog_run "$rdir/timed_out" node --no-concurrent-recompilation "$1.js" 2>"$2"
        fi
    else
        watchdog_run "$rdir/timed_out" "$1" 2>"$2"
    fi
    actual=$?
}

timed_out() { [ -f "$1/timed_out" ]; }
hang_evidence() {   # $1 = the fixture's build dir
    [ -f "$1/hang_native.txt" ] && cat "$1/hang_native.txt"
    local r="$1/hang.json"
    # The report's ABSENCE is evidence, not a gap in the instrumentation — say so, because reading it as
    # "the hook did not fire" is what let the first two catches be written off. On the wasm leg node is
    # always the child and `--report-on-signal` is always armed, so no report means the main thread was
    # not idle-waiting on a handle: it was spinning or parked in a synchronous syscall. Combined with the
    # `state:` line above (R = spinning, D/S = parked) that narrows it to one.
    if [ ! -f "$r" ]; then
        if [ "$WASM" = 1 ]; then
            echo "  (no node report — its ABSENCE means the main thread was spinning or blocked in a"
            echo "   syscall; an idle-but-alive event loop DOES write one. See \`state:\` above.)"
        else
            echo "  (no node report — a native fixture; the node report is a wasm-leg instrument)"
        fi
        return
    fi
    node -e '
      const r = require(process.argv[1]);
      const h = [...new Set((r.libuv || []).filter(x => x.is_active).map(x => x.type))];
      console.log("  live libuv handles:", h.length ? h.join(", ") : "(none — loop was idle)");
      const s = (r.javascriptStack && r.javascriptStack.stack) || [];
      if (s.length) console.log("  js stack:", s.slice(0, 4).join(" | "));
    ' "$r" 2>/dev/null || echo "  (report present but unreadable: $r)"
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
    name="${src##*/}"; name="${name%.kama}"      # parameter expansion, not a `basename` fork
    out="$TMP/$name.out"; res="$TMP/$name.res"
    expect_file="$TESTS_DIR/$name.expect"
    if [ ! -f "$expect_file" ]; then echo "SKIP $name (no .expect)" >"$out"; echo SKIP >"$res"; return; fi
    expected="$(<"$expect_file")"                # `$(<file)` is a bash builtin; `$(cat file)` is a fork

    # Net transports split by target (see below); skip the half that doesn't apply to the active target.
    # These are static properties of the corpus, swept ONCE into $SET_* before the fan-out (see there) —
    # they used to be five `grep`s per fixture, i.e. ~3000 processes per leg to recompute a constant.
    local uses_net_web=0 uses_net=0 uses_proc=0 BROWSER=0
    case "$SET_NET_WEB" in *"|$name|"*) uses_net_web=1;; esac
    case "$SET_NET"     in *"|$name|"*) uses_net=1;;     esac
    case "$SET_PROC"    in *"|$name|"*) uses_proc=1;;    esac
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
        case "$SET_ASM" in *"|$name|"*)
            echo "SKIP $name (inline asm: native/embedded only)" >"$out"; echo SKIP >"$res"; return;;
        esac
    else
        if [ "$uses_net_web" = 1 ]; then
            echo "SKIP $name (web net: browser-only transport)" >"$out"; echo SKIP >"$res"; return
        fi
    fi
    case "$SET_BROWSER" in *"|$name|"*) BROWSER=1;; esac
    if [ "$BROWSER" = 1 ] && [ "$BROWSER_TESTS" = 0 ]; then
        echo "SKIP $name (browser E2E — set KAMA_BROWSER=1 to run)" >"$out"; echo SKIP >"$res"; return
    fi

    # Isolate each build in its own dir: the driver writes multi-unit intermediates to dirname(-o), and
    # imported-module `.c` names key off the MODULE (e.g. dynamic_array_1.c), so two fixtures importing the
    # same stdlib module would collide in a shared dir under parallelism.
    local wd="$TMP/w_$name"; mkdir -p "$wd"; exe="$wd/$name"
    # Report-only timing, and bash-5-only (see HAVE_MS). Skipped entirely on bash 3.2 rather than forking
    # two subshells per fixture to compute 0 - 0.
    local t0=0; [ "$HAVE_MS" = 1 ] && t0=$(now_ms)
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
    [ "$HAVE_MS" = 1 ] && echo $(( $(now_ms) - t0 )) >"$TMP/$name.ms"   # report-only build+run wall-clock
    if timed_out "$wd"; then
        { echo "FAIL $name (HUNG — killed after ${KAMA_FIXTURE_TIMEOUT}s)"; hang_evidence "$wd"; } >"$out"
        echo FAIL >"$res"; return
    fi
    if [ ${#SAN_FLAGS[@]} -gt 0 ] && [ -s "$TMP/$name.san" ]; then
        { echo "FAIL $name (sanitizer)"; head -20 "$TMP/$name.san"; } >"$out"; echo FAIL >"$res"; return
    fi
    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (exit $actual)" >"$out"; echo PASS >"$res"
    else
        echo "FAIL $name (got $actual, expected $expected)" >"$out"; echo FAIL >"$res"
    fi
}

# Fan out across NCPU cores, `spawn` holding the pool to that width. Collect ONLY the fixture job PIDs and
# wait on those explicitly: a bare `wait` also blocks on the long-lived echo servers (ws_echo/wt_echo/
# sig_relay, started with `&` for the wasm net::web tests), which never exit — that hung the whole wasm leg
# after the single-file phase.
# Which fixtures use which seam — swept ONCE over the whole corpus rather than re-derived per fixture.
# These are properties of the source tree, constant for the run, and test_one used to spend five `grep`
# processes rediscovering them for each of 597 fixtures. Five processes now, ~3000 before.
#
# Membership is tested with `case "$SET" in *"|$name|"*)`, so the delimiters on both ends are load-bearing:
# without them `net_ws` would match `net_ws_smoke`.
sweep() {
    local out="|" f
    for f in $(grep -lE "$1" "$TESTS_DIR"/*.kama 2>/dev/null); do
        f="${f##*/}"; out="$out${f%.kama}|"
    done
    printf '%s' "$out"
}
# `std::net` deliberately also matches `std::net::web` — as the per-fixture greps it replaces did.
SET_NET_WEB=$(sweep 'std::net::web|kama_net_web\.h')
SET_NET=$(sweep 'std::net')
SET_PROC=$(sweep 'std::process')
SET_ASM=$(sweep 'asm\(')
SET_BROWSER=$(sweep 'kama_wt_|kama_rtc_')

phase_start "single-file fixtures"
fixture_pids=()
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    spawn test_one "$src"
    fixture_pids+=($!)
done
if [ ${#fixture_pids[@]} -gt 0 ]; then wait "${fixture_pids[@]}" 2>/dev/null; fi
phase_end
# Tally in fixture order (stable output regardless of completion order). Parameter expansion and `$(<f)`
# rather than `basename`/`cat`: this loop is serial and runs once per fixture, so each fork here is paid
# in full rather than spread across the pool.
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    name="${src##*/}"; name="${name%.kama}"
    [ -f "$TMP/$name.out" ] && cat "$TMP/$name.out"
    if [ -f "$TMP/$name.res" ]; then
        case "$(<"$TMP/$name.res")" in
            PASS) pass=$((pass+1));;
            FAIL) fail=$((fail+1));;
        esac
    fi
done

# Multi-file fixtures: tests/<name>.d/ with several .kama built together. Same shape as test_one — buffer
# each fixture's lines into $TMP/md_$name.out and its verdict into .res, then tally in directory order —
# because this leg used to be the one fan-out that was not one: 15 multi-unit programs built and run
# strictly one after another while nine cores idled.
multi_one() {
    local dir="$1" name expect_file expected exe out res
    name="${dir##*/}"; name="${name%.d}"
    out="$TMP/md_$name.out"; res="$TMP/md_$name.res"
    expect_file="$dir/expect"
    if [ ! -f "$expect_file" ]; then echo "SKIP $name (no expect)" >"$out"; echo SKIP >"$res"; return; fi
    expected="$(<"$expect_file")"

    local BROWSER=0   # multi-file fixtures never use a browser transport
    # Its own build dir, for the reason test_one has one: the driver writes multi-unit intermediates to
    # dirname(-o) and names an imported module's .c after the MODULE, so two fixtures importing the same
    # stdlib module collide. Serial, `$TMP/$name` was safe; the moment this leg fans out it is not, which
    # is exactly how 13 of these failed the first time they ran concurrently.
    local wd="$TMP/wm_$name"; mkdir -p "$wd"; exe="$wd/$name"
    # A fixture with `dependencies` needs its `.kama/deps` view MATERIALIZED, and materializing writes into
    # the project — so build a copy under $TMP instead of the worktree (tools/check-clean-tree.sh holds
    # that line, and it is the same rule the rest of this harness follows).
    #
    # The view used to be COMMITTED, as a git symlink into the fixture's own vendor/ directory. Three of
    # them. Git only creates real symlinks on Windows when core.symlinks is on, which needs Developer Mode
    # or elevation and is off on the CI runners — everywhere else git writes a PLAIN FILE whose contents
    # are the target path. The module resolver then found a 16-byte text file where a package directory
    # was meant to be and said "cannot resolve module 'geo'". Running the real `pkg install` is also the
    # more honest test: it exercises the code that builds the view rather than a hand-made stand-in.
    local src="$dir"
    if [ -f "$dir/kama.json" ] && grep -q '"dependencies"' "$dir/kama.json"; then
        src="$wd/src"
        cp -R "$dir" "$src"
        if ! "$KAMA" pkg install "$src/kama.json" >/dev/null 2>"$TMP/$name.err"; then
            { echo "FAIL $name (pkg install failed)"; cat "$TMP/$name.err"; } >"$out"; echo FAIL >"$res"; return
        fi
    fi
    # A fixture WITH a manifest is named BY ITS MANIFEST, because the operand is the mode: naming the
    # .kama files instead is a LOOSE build, which by design applies no manifest at all — no `flags`
    # universe, no dependency view, no `out` root. That is what these fixtures are testing, so they have
    # to be spelled as projects. One without a manifest is a bare pile of .kama files — but "bare pile"
    # means EVERY .kama under it, found recursively and sorted, exactly as the xfail leg has always done.
    #
    # ⚠️ This used to glob `"$src"/*.kama`, top level only, so a subfolder was SILENTLY NOT COMPILED. No
    # manifest-less fixture has one today, which is what made it invisible: the defect was latent, and the
    # first fixture to add a subfolder would have had it half-built with the suite green. Same class as the
    # four-glob reach defect `tools/check-fixture-reach.sh` exists for. `sort` for the same reason the
    # xfail leg gives: unit ORDER is observable, so a fixture must not depend on what the filesystem
    # happens to hand back.
    if [ -f "$src/kama.json" ]; then
        set -- "$src/kama.json"
    else
        # shellcheck disable=SC2046
        set -- $(find "$src" -name '*.kama' | sort)
    fi
    if ! build_one "$exe" "$@" >/dev/null 2>"$TMP/$name.err"; then
        { echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; } >"$out"; echo FAIL >"$res"; return
    fi
    run_one "$exe" "$TMP/$name.san"
    if timed_out "$wd"; then
        { echo "FAIL $name (HUNG — killed after ${KAMA_FIXTURE_TIMEOUT}s)"; hang_evidence "$wd"; } >"$out"
        echo FAIL >"$res"; return
    fi
    if [ ${#SAN_FLAGS[@]} -gt 0 ] && [ -s "$TMP/$name.san" ]; then
        { echo "FAIL $name (sanitizer)"; head -20 "$TMP/$name.san"; } >"$out"; echo FAIL >"$res"; return
    fi

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (multi-file, exit $actual)" >"$out"; echo PASS >"$res"
    else
        echo "FAIL $name (got $actual, expected $expected)" >"$out"; echo FAIL >"$res"
    fi
}

phase_start "multi-file fixtures"
multi_pids=()
for dir in "$TESTS_DIR"/*.d; do
    [ -d "$dir" ] || continue
    spawn multi_one "$dir"
    multi_pids+=($!)
done
if [ ${#multi_pids[@]} -gt 0 ]; then wait "${multi_pids[@]}" 2>/dev/null; fi
for dir in "$TESTS_DIR"/*.d; do
    [ -d "$dir" ] || continue
    name="${dir##*/}"; name="${name%.d}"
    [ -f "$TMP/md_$name.out" ] && cat "$TMP/md_$name.out"
    if [ -f "$TMP/md_$name.res" ]; then
        case "$(<"$TMP/md_$name.res")" in
            PASS) pass=$((pass+1));;
            FAIL) fail=$((fail+1));;
        esac
    fi
done

# Negative fixtures: tests/xfail/<name>.kama MUST FAIL to build (a clear compile-time
# rejection — this is how we guard "reject bad code" guarantees like const-correctness,
# access control, and use-after-move). Optional tests/xfail/<name>.msg holds a substring
# the compiler's error output must contain, so we assert the RIGHT error, not any failure.
#
# A fixture may also be a DIRECTORY, tests/xfail/<name>.d/, whose every .kama is passed to one build and
# whose message lives in `msg` beside them — the same shape tests/<name>.d/ uses for the positive leg.
# Some rejections need more than one file to be reachable at all: an import collision needs two modules
# to collide, and "does not export" needs a module that exists and declines to export the name. Those
# used to be spelled as one loose file importing sibling DIRECTORIES, which worked only because a loose
# build searched the filesystem — the behavior SPEC.md §Modules removes. Passing every source
# is the rule now, so the fixture has to be able to say what its sources are.
#
# Fanned out across NCPU like the single-file leg, and for the same reason: each fixture is an independent
# `kama build` into its own $TMP files. Serial, this leg was ~325 compiler processes run one at a time.
# Every POSITION a diagnostic names must be real: a file, and a line that file actually has. This is the
# cheap half of "nothing in the repo checks a diagnostic's line number" — it cannot tell a right line from
# a wrong-but-plausible one (DIAGNOSTIC_LINES below does that), but it is free here, because the build has
# already run and its stderr is already captured, and it applies to EVERY diagnostic of EVERY fixture
# rather than to the one each `.msg` happens to assert.
#
# It is not hypothetical. On the corpus it stood up on, it found 18 fixtures: 15 emitting a diagnostic with
# NO FILE NAME AT ALL (`:3:0: error: …` — `diagFile()` returned empty and nobody looked), 2 reporting line
# 0, which no file has, and one pointing at line 31 of a 19-line file. A reader sent to line 31 of a 19-line
# file learns that the compiler does not know where the mistake is; a wrong line inside the file merely
# misleads them quietly, which is worse but needs the golden record to catch.
#
# LC_ALL=C throughout: diagnostic text carries em-dashes and backticks, and a UTF-8 locale makes BSD sed
# and grep fail outright ("illegal byte sequence") on the very messages we are trying to read.
diag_position_faults() {   # diag_position_faults <errfile> <fixture.kama>...
    local err="$1" s sb slen; shift
    if LC_ALL=C grep -qE '^:[0-9]+:[0-9]+: ' "$err"; then echo "names NO FILE at all"; fi
    for s in "$@"; do
        sb="${s##*/}"; slen=$(wc -l < "$s" | tr -d ' ')
        LC_ALL=C awk -v sb="$sb" -v max="$slen" '
            { i = index($0, sb ":"); if (i == 0) next
              rest = substr($0, i + length(sb) + 1)
              if (rest !~ /^[0-9]+:/) next
              n = rest + 0
              if (n < 1 || n > max) seen[n] = 1 }
            END { for (n in seen) print sb ":" n " (the file has " max " lines)" }' "$err"
    done
}

xfail_one() {
    local src="$1" name out res err rc msg_file fsrcs faults
    name="${src##*/}"; name="${name%.kama}"; name="${name%.d}"
    out="$TMP/xf_$name.out"; res="$TMP/xf_$name.res"; err="$TMP/xf_$name.err"
    # One file, or every file of a .d directory. `sort` because the ORDER the operands are written in used
    # to be observable in the output, so a fixture must not depend on whatever order the filesystem hands
    # back. Less of that is true since §2e.26 — a build now sorts its units canonically and derives every
    # generated name from identity — but a diagnostic naming "the first declaration" still picks by unit
    # order, and `kama check` does not sort. Sorting here costs nothing and keeps the input stable too.
    if [ -d "$src" ]; then
        msg_file="$src/msg"
        fsrcs=$(find "$src" -name '*.kama' | sort)
        if [ -f "$src/kama.json" ]; then
            # A fixture WITH a manifest is named BY ITS MANIFEST — the same rule the POSITIVE .d leg
            # already follows, because the operand is the mode. Naming the .kama files instead is a LOOSE
            # build, which by design applies no manifest at all, so a rejection that needs one could never
            # fire: `visibility` would be read by nothing and the fixture would compile clean. This arm was
            # simply never added when 1c gave it to the positive leg.
            "$KAMA" build "$src/kama.json" -o "$TMP/xf_$name" >/dev/null 2>"$err"; rc=$?
        else
            # shellcheck disable=SC2086
            "$KAMA" build $fsrcs -o "$TMP/xf_$name" >/dev/null 2>"$err"; rc=$?
        fi
    else
        msg_file="$TESTS_DIR/xfail/$name.msg"
        fsrcs="$src"
        "$KAMA" build "$src" -o "$TMP/xf_$name" >/dev/null 2>"$err"; rc=$?
    fi
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
    if [ -f "$msg_file" ] && ! grep -qF "$(cat "$msg_file")" "$err"; then
        { echo "FAIL xfail/$name (rejected, but error missing \"$(cat "$msg_file")\")"; head -2 "$err"; } >"$out"
        echo FAIL >"$res"; return
    fi
    # A rejection is an ERROR, and says so. The emitter used to stream `kama: warning: unsupported <msg>`
    # while filing the same defect as severity Error, so a build called a hard error a "warning" and a
    # single `kama check` printed both spellings on consecutive lines. Nothing in the suite noticed,
    # because every `.msg` matches the message BODY — the prefix was free to say anything at all. It is
    # asserted here rather than in a guard so that all 557 fixtures hold it down, not one hand-written case.
    if ! grep -q 'error:' "$err"; then
        { echo "FAIL xfail/$name (rejected, but no line says \`error:\` — a rejection must name itself one)"
          head -2 "$err"; } >"$out"; echo FAIL >"$res"; return
    fi
    if grep -qi 'warning' "$err"; then
        { echo "FAIL xfail/$name (rejected, but reported as a WARNING — a compiler that calls an error a"
          echo "  warning cannot be trusted about the errors it does report)"; grep -i warning "$err" | head -2; } >"$out"
        echo FAIL >"$res"; return
    fi
    # shellcheck disable=SC2086
    faults=$(diag_position_faults "$err" $fsrcs)
    if [ -n "$faults" ]; then
        { echo "FAIL xfail/$name (rejected, but a diagnostic points nowhere real)"
          printf '%s\n' "$faults" | sed 's/^/    /'
          head -3 "$err"; } >"$out"; echo FAIL >"$res"; return
    fi
    echo "PASS xfail/$name (rejected)" >"$out"; echo PASS >"$res"
}
phase_end
phase_start "xfail fixtures"
xfail_pids=()
for src in "$TESTS_DIR"/xfail/*.kama "$TESTS_DIR"/xfail/*.d; do
    [ -e "$src" ] || continue
    spawn xfail_one "$src"
    xfail_pids+=($!)
done
if [ ${#xfail_pids[@]} -gt 0 ]; then wait "${xfail_pids[@]}" 2>/dev/null; fi
phase_end
# Tally in fixture order, so output is identical to the serial version regardless of completion order.
for src in "$TESTS_DIR"/xfail/*.kama "$TESTS_DIR"/xfail/*.d; do
    [ -e "$src" ] || continue
    name="${src##*/}"; name="${name%.kama}"; name="${name%.d}"
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
        name="${src##*/}"; name="${name%.kama}"
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
analysis_skip() {   # fixtures where `check` legitimately cannot match `build`
    # M7: a `comptime assert` over an AGGREGATE's layout lowers to a C11 `_Static_assert`, because kama
    # does not model struct layout — clang does. So the rejection comes from the C compiler, which `check`
    # never runs. This is not the hazard the leg guards against (a diagnostic kama COULD give and drops on
    # the check path); it is the documented cost of letting the target answer a target question, and the
    # docs say so at the surface. Adding an entry here is a deliberate act — read the note above first.
    case "$1" in
        comptime_assert_layout_false) return 0 ;;
    esac
    return 1
}
# This leg is ONE assertion, not one per fixture. It re-checks every fixture the suite already built, so
# counting each as a separate pass would double the headline number without doubling what is covered —
# the corpus is the same, only the entry point differs. A mismatch names the fixture; the tally is a
# single PASS/FAIL plus the sample size.
#
# BATCHED, then fanned out across NCPU. `kama check --each` runs N programs in ONE process, reusing the
# parsed prelude and the shared `std::` import closure across all of them — measured over this corpus:
# 909 programs re-parse the prelude 909 times and the ~106 distinct closure units 4,736 times. At NCPU=10
# this phase went 22 s -> 10 s, measured A/B on one binary via KAMA_NO_BATCH=1 below. (Standalone, with
# nothing else competing, the same corpus is 18.4 s -> 8.1 s wall and 157.6 s -> 55.7 s CPU.)
#
# The batch is only a fast PRE-FILTER. Anything that does not come back with the expected verdict — a
# disagreement, or no verdict at all because the process died partway — is re-run SOLO through the two
# one-file functions below, which stay the single source of truth for every message this leg prints. So:
#   * a crash still names its fixture and its signal (the batch loses the verdict, the solo re-run
#     reproduces it), which is the whole reason the xfail leg checks for signals at all;
#   * a MISMATCH message and its `head -3` diagnostics are byte-for-byte what they were unbatched, with
#     no need to demultiplex one process's stderr back into per-file slices.
# In the green case nothing is re-run, so the fidelity is free.
#
# Proof of equivalence, and the escape hatch: KAMA_NO_BATCH=1 restores the per-file path. Both were run
# over all 923 fixtures — identical exit codes, and each chunk's `--each` stderr byte-identical to the
# concatenation of its files' solo stderr:
#   ls tests/*.kama tests/xfail/*.kama > /tmp/f; split -l 32 /tmp/f /tmp/chunk.
#   for c in /tmp/chunk.*; do "$KAMA" check --each $(cat "$c"); done
check_pos_one() {
    local src="$1" name
    name="${src##*/}"; name="${name%.kama}"
    if ! "$KAMA" check "$src" >/dev/null 2>"$TMP/ck_$name.err"; then
        { echo "  MISMATCH $name: builds, but \`kama check\` rejects it (the editor would show a clean file as broken)"
          head -3 "$TMP/ck_$name.err"; } >"$TMP/ck_$name.bad"
    fi
}
check_neg_one() {
    local src="$1" name ck_rc
    name="${src##*/}"; name="${name%.kama}"
    "$KAMA" check "$src" >/dev/null 2>&1; ck_rc=$?
    if [ "$ck_rc" -eq 0 ]; then
        echo "  MISMATCH xfail/$name: \`kama build\` rejects it but \`kama check\` accepts it (the editor would show a broken file as clean)" >"$TMP/ckx_$name.bad"
    elif [ "$ck_rc" -ge 128 ]; then
        # `kama lsp` runs this analysis in-process on every keystroke, so a crash here kills the editor's
        # language server. Rejecting-by-crashing agreed with `build` and therefore used to pass silently.
        echo "  MISMATCH xfail/$name: \`kama check\` CRASHED (signal $((ck_rc-128))) — the language server would die on this input" >"$TMP/ckx_$name.bad"
    fi
}
# Split a fixture list into chunks and LAUNCH one `kama check --each` per chunk into the shared pool.
# Chunked at 32: big enough that the shared `std::` closure is parsed ~29 times instead of ~909, small
# enough that `gate` still load-balances and one crash costs one chunk.
#
# Launch and collect are separate so the POSITIVE and NEGATIVE lists occupy ONE pool with one barrier.
# Draining them in sequence leaves a straggler in each of the two waves: 11 s that way, 10 s pooled.
CKB_PIDS=()
# One chunk, as its own job — a function so it can go through `spawn`, which takes a command and not a
# redirection. Unquoted `$(cat …)` on purpose: one argument per path. No fixture path contains whitespace,
# and the collect step re-runs anything that does not come back with a verdict, so a pathological name
# degrades to the unbatched path rather than to a wrong answer.
check_batch_one() { "$KAMA" check --each $(cat "$1") >"$1.v" 2>/dev/null; }
check_batch_launch() {
    local list="$1" tag="$2" c
    split -l 32 "$list" "$TMP/ckb_${tag}." 2>/dev/null || true
    for c in "$TMP/ckb_${tag}."*; do
        [ -e "$c" ] || continue
        case "$c" in *.v) continue;; esac
        spawn check_batch_one "$c"
        CKB_PIDS+=($!)
    done
}
# Echo back the fixtures whose verdict is not $2 (the expected per-file exit code), including any the
# batch never reported. The caller re-runs exactly that set solo.
check_batch_redo() {
    local list="$1" want="$2" tag="$3"
    : >"$TMP/ckb_${tag}.verdicts"
    cat "$TMP/ckb_${tag}."*.v 2>/dev/null >>"$TMP/ckb_${tag}.verdicts"
    # One awk pass rather than a lookup per fixture: `<rc> <path>` verdicts in, the paths that did not
    # answer `want` out, in list order. Keyed on FILENAME rather than the usual NR==FNR, which silently
    # reads the LIST's first line as a verdict when the verdicts file is empty — i.e. exactly when every
    # chunk died, which is the one case this leg must not under-report.
    awk -v want="$want" -v vf="$TMP/ckb_${tag}.verdicts" \
        'FILENAME == vf { v[$2] = $1; next } { if (!($0 in v) || v[$0] != want) print }' \
        "$TMP/ckb_${tag}.verdicts" "$list"
}

ck_pos=0; ck_neg=0; ck_bad=0
phase_start "analysis agreement"
: >"$TMP/ck_pos.list"; : >"$TMP/ck_neg.list"
for src in "$TESTS_DIR"/*.kama; do
    [ -e "$src" ] || continue
    name="${src##*/}"; name="${name%.kama}"
    [ -f "$TMP/$name.res" ] && [ "$(cat "$TMP/$name.res")" = "PASS" ] || continue   # only fixtures that built
    ck_pos=$((ck_pos+1))
    echo "$src" >>"$TMP/ck_pos.list"
done
# Single-file fixtures only. A tests/xfail/<name>.d/ fixture is one build over SEVERAL files, and this
# leg checks each file independently — the rejection it asserts (a collision between two modules, a
# module declining to export) exists only in the combined build, so feeding its files in one at a time
# would assert nothing and report a confident pass. The .d fixtures are covered by the leg above.
for src in "$TESTS_DIR"/xfail/*.kama; do
    [ -e "$src" ] || continue
    name="${src##*/}"; name="${name%.kama}"
    if analysis_skip "$name"; then echo "  SKIP xfail/$name (rejected by the C compiler, not by kama)"; continue; fi
    ck_neg=$((ck_neg+1))
    echo "$src" >>"$TMP/ck_neg.list"
done

if [ -n "${KAMA_NO_BATCH:-}" ]; then      # the escape hatch: every fixture its own process, as before
    cp "$TMP/ck_pos.list" "$TMP/ck_pos.redo"; cp "$TMP/ck_neg.list" "$TMP/ck_neg.redo"
else
    check_batch_launch "$TMP/ck_pos.list" pos
    check_batch_launch "$TMP/ck_neg.list" neg
    # `"${arr[@]}"` on an EMPTY array is an unbound-variable error under `set -u` on bash 3.2 (macOS), and
    # a bare `wait` would block on the never-exiting echo helpers — so gate on the count, as line 293 does.
    if [ ${#CKB_PIDS[@]} -gt 0 ]; then wait "${CKB_PIDS[@]}" 2>/dev/null; fi
    check_batch_redo "$TMP/ck_pos.list" 0 pos >"$TMP/ck_pos.redo"   # a positive must exit 0
    check_batch_redo "$TMP/ck_neg.list" 1 neg >"$TMP/ck_neg.redo"   # a negative must exit nonzero
fi

# Solo re-runs: normally empty. These produce every message this leg prints, batched or not.
ck_pids=()
while read -r src; do [ -n "$src" ] || continue; spawn check_pos_one "$src"; ck_pids+=($!); done <"$TMP/ck_pos.redo"
while read -r src; do [ -n "$src" ] || continue; spawn check_neg_one "$src"; ck_pids+=($!); done <"$TMP/ck_neg.redo"
if [ ${#ck_pids[@]} -gt 0 ]; then wait "${ck_pids[@]}" 2>/dev/null; fi   # normally EMPTY — see above
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
