#!/bin/sh
# check-compiler-asan.sh — build the COMPILER under AddressSanitizer and drive the whole fixture corpus
# through it, so a memory bug in kama's own C++ fails the gate.
#
# WHY THIS EXISTS. `KAMA_SAN=1 ./run_tests.sh` sanitizes every positive fixture's EMITTED PROGRAM
# (run_tests.sh's SAN_FLAGS is a `--cc` override). That answers "does kama produce memory-safe programs".
# It says nothing about "is the compiler memory-safe" — and until this guard there was no `-fsanitize`
# anywhere in the Makefile, so the compiler binary had no sanitized build at all. The consequence was
# specific: an xfail fixture never links, so it never reached the emitted-program sanitizer either, and all
# 582 tests/xfail/ fixtures drove the compiler's REJECTION paths with no instrumentation whatsoever. That
# is exactly where three known diagnose-then-dereference bugs lived.
#
# WHAT IT ASSERTS: memory safety, and ONLY that. A fixture's verdict is not this guard's business — an
# xfail is SUPPOSED to exit nonzero, and run_tests.sh already owns every verdict in the corpus. Duplicating
# that here would mean maintaining a second copy of its `analysis_skip` table for no added coverage. So the
# only failures are an ASan report and a crash.
#
# THREE PASSES, because three different halves of the compiler are reachable only by three entry points:
#   1. `check --each`  parse + resolve + type-check + ownership/serde over ALL fixtures, positive and
#                      negative — every rejection path. Batched 32 per process (`--each` reuses the parsed
#                      prelude and the std:: closure across a chunk; see run_tests.sh's "analysis agreement"
#                      leg, which batches the same way and for the same reason).
#   2. `transpile`     the C-EMISSION walk over the positives. `check` runs CEmitter::analyze() with no
#                      output stream, so it never enters the emit walk — 24k lines of kama.cemit.cpp that
#                      no leg sanitized. No clang is invoked, so this costs the front end only.
#   3. `check-lsp.sh`  the LSP session, re-run with $KAMA pointed at the sanitized binary. This is the one
#                      pass that covers kama.lsp.cpp + kama.query.cpp, and the one that matters most:
#                      `kama lsp` is the only LONG-LIVED kama process, re-running analysis per keystroke
#                      over a mutating buffer, so a use-after-free compounds there instead of being
#                      reclaimed at exit. tools/kama-bin.sh honors an externally set $KAMA, so this reuses
#                      that guard's scripted JSON-RPC session verbatim rather than inventing a second one.
#
# LEAKS. Measured 2026-08-29 over 80 fixtures on Linux with LSan armed (verified armed by leaking 1234
# bytes from a probe and watching it get reported): the compiler leaks NOTHING. The AST is std::shared_ptr
# end to end (SharedAST/SharedStatement/… in kama.forward.h) with no parent back-pointers, so RAII frees it
# and the classic shared_ptr-cycle leak is not structurally present. That makes `detect_leaks=1` a REAL
# assertion rather than noise to be suppressed, so it is on wherever the platform supports it. macOS has no
# LeakSanitizer at all ("detect_leaks is not supported on this platform"), so there it drops to the
# use-after-free / overflow assertion and the leak half is asserted on Linux hosts and container runs.
#
# check-heavy: yes
#
# The marker means what it means for check-no-inheritance.sh: a full second compiler build, too slow for
# run_tests.sh's set and unwilling to share a machine with 50 other guards. Under `--all` (i.e. `./dev
# check`) it runs ALONE, after the pool drains. Unlike that guard this one never repoints the root ./kama
# symlink — it names the target it wants (`make KAMA_ASAN=1 out/<platform>-asan/kama`) rather than letting
# the default `all: kama` rule run, so there is nothing to put back afterwards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

JOBS="${KAMA_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
PLATFORM=$(sh "$ROOT/tools/platform.sh")   # the shared derivation, not a copy — see that file
ASAN="out/$PLATFORM-asan/kama"        # the Makefile picks this directory itself when KAMA_ASAN=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT             # a private dir, NOT a fixed /tmp path: two worktrees would collide

# LeakSanitizer exists on Linux and not on macOS. `exitcode=86` is a distinctive value so an ASan report
# is unambiguous; ASan on Darwin defaults to abort_on_error=1 and aborts (rc 134) BEFORE consulting it,
# which is why the detector below tests rc>=128 as well and greps for the report as a third, independent
# signal. Any one of the three is enough; relying on only one of them missed the abort case.
LEAKS=1
[ "$(uname -s)" = Darwin ] && LEAKS=0
ASAN_OPTIONS="detect_leaks=$LEAKS:exitcode=86"
export ASAN_OPTIONS

# --- 0. IS THERE AN ASan RUNTIME AT ALL? ------------------------------------------------------------
# mingw-w64 has no AddressSanitizer. Measured with `pacman -Fl` across all three environments — asan
# files: 0 everywhere. What `compiler-rt` actually ships is builtins, profile, and the three fuzzer
# archives (5 files on ucrt64/mingw64, 2 on clangarm64); NO sanitizer runtime of any kind. So this is
# not a missing package that a `pacman -S` fixes, and the CI install list is not the place to look —
# clang is not even linked against it (`mingw-w64-ucrt-x86_64-clang` does not depend on compiler-rt).
# clang accepts `-fsanitize=address` regardless and fails at the LINK, which is why the whole compiler
# compiled before anything went wrong:
#
#   ld: cannot find …/libclang_rt.asan_dynamic.dll.a: No such file or directory
#
# That made the Windows leg RED for a guard about memory safety in portable C++ — a fact no Windows
# machine is needed to check and which the Linux and container legs already assert. So: probe, and skip.
#
# ⚠️ The probe LINKS, it does not just compile. `clang++ -fsanitize=address -c` succeeds here; only the
# link reaches the missing runtime, so a compile-only probe would report ASan as available and hand the
# failure straight back to `make`. Costs one trivial TU (~0.3 s) on platforms that do have it.
#
# `clang++` is spelled out rather than taken from $CXX ON PURPOSE — the Makefile sets `CXX = clang++`
# with a plain `=`, which OVERRIDES an environment CXX, so honoring $CXX here would probe a compiler
# the build is not going to use and answer for the wrong toolchain.
echo 'int main(void){return 0;}' >"$tmp/probe.cpp"
if ! clang++ -fsanitize=address "$tmp/probe.cpp" -o "$tmp/probe" >"$tmp/probe.log" 2>&1; then
    echo "SKIP check-compiler-asan (no AddressSanitizer runtime for this toolchain on $(uname -s) — \
mingw-w64 ships no sanitizer runtime at all; the Linux and container legs assert this)"
    # Say WHY, always. A skip is indistinguishable from a pass in the tally, so on a platform that is
    # SUPPOSED to have ASan this line is the only thing standing between a missing libclang-rt-dev and a
    # guard that quietly stopped running. Do not remove it to tidy the output.
    sed -n '1p' "$tmp/probe.log" | sed 's/^/  probe: /'
    exit 0
fi

# --- 1. BUILD ---------------------------------------------------------------------------------------
echo "check-compiler-asan: building the compiler with -fsanitize=address ..."
if ! make -j"$JOBS" KAMA_ASAN=1 "$ASAN" >"$tmp/build.log" 2>&1; then
    echo "check-compiler-asan: FAIL — the KAMA_ASAN=1 build does not compile:" >&2
    tail -30 "$tmp/build.log" >&2
    exit 1
fi
if grep -q ' error:\| warning:' "$tmp/build.log"; then
    echo "check-compiler-asan: FAIL — the KAMA_ASAN=1 build is not warning-clean:" >&2
    grep ' error:\| warning:' "$tmp/build.log" | head -20 >&2; exit 1
fi

# A run is BAD if ASan said so (86), if it died on a signal (>=128, which is how Darwin's abort arrives),
# or if the report text is present at all. Callers pass the captured stderr.
bad_run() {   # bad_run <rc> <errfile>
    [ "$1" -eq 86 ] || [ "$1" -ge 128 ] || grep -q 'AddressSanitizer' "$2"
}
report() {    # report <label> <errfile>
    echo "check-compiler-asan: FAIL — a memory error in the compiler itself:" >&2
    echo "  reached by: $1" >&2
    grep -m1 -E 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' "$2" | sed 's/^/  /' >&2
    sed -n '1,25p' "$2" | sed 's/^/    /' >&2
}

# --- 2. ANALYSIS + REJECTION ------------------------------------------------------------------------
# Every single-file fixture, positive and negative. A tests/<name>.d/ fixture is ONE build over several
# files and its rejection exists only in the combined build, so feeding its files in one at a time would
# assert nothing — run_tests.sh's analysis leg skips them for the same reason, and they are covered there.
ls tests/*.kama tests/xfail/*.kama >"$tmp/all.list" 2>/dev/null || true
total=$(wc -l <"$tmp/all.list" | tr -d ' ')
split -l 32 "$tmp/all.list" "$tmp/chunk."
echo "check-compiler-asan: pass 1/3 — analysis + rejection over $total fixtures ..."

# The batch is a PRE-FILTER only. ASan aborts the whole process, so a chunk that dies takes its 32
# verdicts with it and cannot name the culprit; on a bad chunk, re-run its files SOLO so the failure
# names one fixture. The green path re-runs nothing, so the fidelity costs nothing.
for c in "$tmp"/chunk.*; do
    [ -e "$c" ] || continue
    rc=0; "$ASAN" check --each $(cat "$c") >/dev/null 2>"$c.err" || rc=$?
    if bad_run "$rc" "$c.err"; then
        while read -r f; do
            [ -n "$f" ] || continue
            src=0; "$ASAN" check "$f" >/dev/null 2>"$tmp/solo.err" || src=$?
            if bad_run "$src" "$tmp/solo.err"; then report "kama check $f" "$tmp/solo.err"; exit 1; fi
        done <"$c"
        # The chunk died but no single file reproduces it: still a real failure, just not isolatable.
        report "kama check --each (chunk $(basename "$c"), not reproducible solo)" "$c.err"; exit 1
    fi
done

# --- 3. EMISSION ------------------------------------------------------------------------------------
# The positives only — a rejected program never reaches the emit walk. Fanned out; the per-file body is
# written into $tmp rather than inlined into an xargs template, which blows the argument limit at 670.
echo "check-compiler-asan: pass 2/3 — C emission over $(ls tests/*.kama | wc -l | tr -d ' ') positives ..."
cat >"$tmp/emit_one.sh" <<'EOF'
n=$(basename "$1" .kama)
rc=0; "$ASAN" transpile "$1" -o "$OUT/$n.c" >"$OUT/$n.err" 2>&1 || rc=$?
# A plain nonzero is NOT a failure here: this guard asserts memory safety, and the suite owns verdicts.
if [ "$rc" -eq 86 ] || [ "$rc" -ge 128 ] || grep -q AddressSanitizer "$OUT/$n.err"; then
    echo "$n" >>"$OUT/BAD"
fi
exit 0
EOF
OUT="$tmp/emit"; mkdir -p "$OUT"
export ASAN OUT
ls tests/*.kama | xargs -P "$JOBS" -n 1 sh "$tmp/emit_one.sh"
if [ -f "$OUT/BAD" ]; then
    n=$(head -1 "$OUT/BAD")
    report "kama transpile tests/$n.kama" "$OUT/$n.err"; exit 1
fi

# --- 4. THE LSP SESSION -----------------------------------------------------------------------------
# Reuses check-lsp.sh's scripted JSON-RPC session against the sanitized binary. If it fails here but
# passes in its own right, the difference IS the finding — so the stderr is shown either way.
echo "check-compiler-asan: pass 3/3 — LSP session ..."
rc=0
KAMA="$ROOT/$ASAN" sh tools/check-lsp.sh >"$tmp/lsp.err" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
    if bad_run "$rc" "$tmp/lsp.err"; then
        report "kama lsp (tools/check-lsp.sh session)" "$tmp/lsp.err"
    else
        echo "check-compiler-asan: FAIL — the LSP session failed under the sanitized compiler (rc=$rc)." >&2
        echo "  If tools/check-lsp.sh passes on its own, the difference is the finding." >&2
        tail -20 "$tmp/lsp.err" >&2
    fi
    exit 1
fi

leakmsg="leak detection ON, no leaks"
[ "$LEAKS" = 0 ] && leakmsg="leak detection unavailable on $(uname -s)"
echo "PASS compiler-asan ($total fixtures analyzed, $(ls tests/*.kama | wc -l | tr -d ' ') emitted, LSP session; $leakmsg)"
