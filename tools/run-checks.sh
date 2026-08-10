#!/bin/sh
# run-checks.sh — run the tools/check-*.sh guards. ONE runner, used by BOTH `./dev check` and
# run_tests.sh.
#
# It used to be two implementations of the same list: a glob in `./dev`, and 22 hand-written
# `if sh tools/check-X.sh; then pass=…` blocks in run_tests.sh. They had already drifted —
# check-agents.sh was in one and not the other, with no comment saying why — which is exactly the
# failure the repo's glob-enrolment rule exists to prevent. Now there is one list, and it is the glob.
#
# It also runs them in PARALLEL. They are independent scripts; run serially they were ~73s of a 187s
# native leg with nine cores idle, and the block sits BEFORE the fixture fan-out, so nothing else was
# using them either.
#
# POSIX sh on purpose. run_tests.sh is bash but must stay bash-3.2-compatible (macOS ships 3.2), and
# that is where the last concurrency bug came from: `wait -n` is bash 4.3+ and `$EPOCHREALTIME` is
# bash 5, so a gate written with them FAILED OPEN on the host and throttled nothing (fixed in
# 69c1123). Writing the pool in sh, with neither, makes that class of bug unrepresentable.
#
# WHICH GUARDS RUN WHERE. Nearly every guard is native-only, because it asserts something
# target-agnostic — the emitted C, a transpile, the grammar, a doc, the packaging surface — and
# re-running it under the SAN and WASM legs would re-prove the same claim about the same host binary.
# So `native` is the DEFAULT, and a guard that needs otherwise says so in its own header:
#
#   # check-legs: native san    which suite legs this guard belongs to. Default: native.
#   # check-heavy: yes          builds a second compiler, or writes to the worktree / $ROOT/.git.
#                               Excluded from run_tests.sh's set; under --all it runs ALONE, after
#                               the pool has drained.
#
# Markers live in the guard, not in a list here, so a new tools/check-*.sh still joins the gate with
# nothing to update. Only two guards carry one today: check-argv-env.sh (also runs under SAN) and
# check-no-inheritance.sh (heavy).
#
# THE CONCURRENCY CONTRACT for a guard, since they now share a machine: work in a private
# `mktemp -d`, do not write into the worktree, do not `cd` outside a subshell, and resolve the
# compiler through $KAMA (this runner exports an absolute one) rather than the ./kama symlink —
# which check-no-inheritance.sh repoints while it runs. A guard that cannot honor that needs
# `# check-heavy: yes`.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

# Guards `cd` — into their own tmp dir, into tree-sitter-kama/, into an installed test project — so
# the binary they inherit MUST be absolute. run_tests.sh sets ROOT="." and its $KAMA is therefore
# "./build/…"; it does not export it, and it must not start.
case "$KAMA" in
    /*) ;;
    *)  KAMA=$(CDPATH= cd -- "$(dirname -- "$KAMA")" && pwd)/$(basename "$KAMA") ;;
esac
export KAMA

LEG=native; ALL=0; LIST=0; TALLY=; JOBS=
while [ $# -gt 0 ]; do
    case "$1" in
        --leg)   LEG="$2";   shift 2 ;;
        --all)   ALL=1;      shift ;;   # include `# check-heavy:` guards — the pre-commit gate does
        --list)  LIST=1;     shift ;;   # print the enrolled set and exit: makes the leg rules testable
        --tally) TALLY="$2"; shift 2 ;; # write "<pass> <fail>" here, for run_tests.sh's counters
        --jobs)  JOBS="$2";  shift 2 ;;
        *) echo "run-checks.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$JOBS" ] || JOBS="${KAMA_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# This pool is already $JOBS wide and several guards call `kama build`, so `kama build`'s own `-j` must
# not fan out underneath it — same reasoning as run_tests.sh. Overridable so check-build-jobs.sh (which
# passes -j on the command line, where it wins) still measures what it means to.
export KAMA_BUILD_JOBS="${KAMA_BUILD_JOBS:-1}"

# Read a marker from the guard's OWN header. Bounded to the first 40 lines so a guard that merely
# discusses the markers in prose cannot accidentally set one.
marker() { sed -n "1,40{s/^# *$2: *//p;}" "$1" | head -1; }

# --- enrolment: the glob IS the list ---------------------------------------------------------------
sel=          # every enrolled guard, in glob order — the PRINT order
par=          # runs in the pool
exc=          # runs alone, after the pool
for g in "$ROOT"/tools/check-*.sh; do
    legs=$(marker "$g" check-legs); [ -n "$legs" ] || legs=native
    case " $legs " in *" $LEG "*) ;; *) continue ;; esac
    heavy=$(marker "$g" check-heavy)
    if [ -n "$heavy" ] && [ "$heavy" != no ]; then
        [ "$ALL" = 1 ] || continue
        exc="$exc $g"
    else
        par="$par $g"
    fi
    sel="$sel $g"
done

if [ "$LIST" = 1 ]; then
    for g in $sel; do echo "tools/$(basename "$g")"; done
    exit 0
fi

WORK=$(mktemp -d) || exit 2
running=                # live pool PIDs; the interrupt trap needs them, so declare before the trap
trap 'rm -rf "$WORK"' EXIT
# A bare Ctrl-C would otherwise leave the pool running, orphaned and writing into a directory that is
# about to be deleted. Take the whole pool down with us.
trap 'trap - EXIT; for _p in $running; do kill "$_p" 2>/dev/null; done; rm -rf "$WORK"; exit 130' INT TERM

# --- one guard --------------------------------------------------------------------------------------
# Writes <name>.log / .rc / .sec and NOTHING to stdout. Results are printed later, in glob order, so a
# parallel run's output is diffable against a serial one.
HAVE_TIME=0; [ -x /usr/bin/time ] && HAVE_TIME=1
run_one() {
    _g=$1; _n=$(basename "$_g" .sh)
    # A private package store per guard. Nothing in the tree needs the developer's real ~/.kama/store
    # (check-query/check-lsp install only `path` deps, which never reach it), but making it structural
    # means a future git/url dep in any guard cannot race another guard — or the user's own store.
    # HOME is deliberately NOT overridden: npm and the tree-sitter CLI cache under it, and
    # check-toolchain.sh already isolates its own.
    KAMA_STORE="$WORK/store/$_n"; export KAMA_STORE
    if [ "$HAVE_TIME" = 1 ]; then
        # `-p` prints "real <sec>" on ITS stderr; the inner sh redirects the guard's own stdout+stderr
        # into the log before exec, so the two never mix. Exit status passes through (verified: macOS
        # and GNU coreutils both).
        /usr/bin/time -p sh -c 'exec sh "$0" >"$1" 2>&1' "$_g" "$WORK/$_n.log" 2>"$WORK/$_n.time"
        echo $? >"$WORK/$_n.rc"
        awk '/^real/{print $2; exit}' "$WORK/$_n.time" >"$WORK/$_n.sec"
    else
        _t0=$(date +%s)
        sh "$_g" >"$WORK/$_n.log" 2>&1
        echo $? >"$WORK/$_n.rc"
        echo $(( $(date +%s) - _t0 )) >"$WORK/$_n.sec"
    fi
}

# --- the pool -----------------------------------------------------------------------------------------
# No `jobs -rp` (job control is off in a non-interactive sh) and no `wait -n` (bash 4.3+, and it returns
# the finished job's exit status, which is how the old gate punched a hole in its own cap). Keep live PIDs
# in a string and reap them with `kill -0`. ~23 guards, so a 50ms poll costs nothing.
gate() {
    while :; do
        alive=; n=0
        for p in $running; do
            if kill -0 "$p" 2>/dev/null; then alive="$alive $p"; n=$((n+1)); fi
        done
        running=$alive
        [ "$n" -lt "$JOBS" ] && return 0
        sleep 0.05
    done
}

wall0=$(date +%s)
for g in $par; do gate; run_one "$g" & running="$running $!"; done
for p in $running; do wait "$p" 2>/dev/null; done     # explicit PIDs, never a bare `wait`
for g in $exc; do ( run_one "$g" ); done              # alone, last: it repoints ./kama while it builds
                                                      # (subshell: run_one exports KAMA_STORE)
wall=$(( $(date +%s) - wall0 ))

# --- results, in glob order ----------------------------------------------------------------------------
p=0; f=0; rc=0
for g in $sel; do
    n=$(basename "$g" .sh)
    printf '%s: ' "tools/$n.sh"
    grc=$(cat "$WORK/$n.rc" 2>/dev/null || echo 137)   # no .rc == killed (OOM/signal) == a failure
    if [ "$grc" = 0 ]; then
        last=$(tail -1 "$WORK/$n.log" 2>/dev/null); [ -n "$last" ] || last=OK
        echo "$last"; p=$((p+1))
    else
        echo "FAILED (exit $grc)"; cat "$WORK/$n.log" 2>/dev/null
        f=$((f+1)); rc=1
    fi
done

# --- timing: the data the rest of the build-perf campaign is steered by -----------------------------------
# `cpu` is the summed guard wall-clock, so cpu/wall is the parallel speedup actually achieved. The slowest
# guard is the floor: the pool can never finish sooner than its longest member.
cpu=$(for g in $sel; do n=$(basename "$g" .sh); cat "$WORK/$n.sec" 2>/dev/null; done \
      | awk '{s+=$1} END{printf "%.0f", s}')
echo "----"
printf 'checks: %d passed, %d failed  (%ss wall, %ss summed). slowest:\n' "$p" "$f" "$wall" "$cpu"
for g in $sel; do n=$(basename "$g" .sh); printf '%s %s\n' "$(cat "$WORK/$n.sec" 2>/dev/null || echo 0)" "$n"; done \
  | sort -rn | head -8 | while read -r s nm; do printf '  %7ss  %s\n' "$s" "$nm"; done

[ -n "$TALLY" ] && echo "$p $f" >"$TALLY"
exit $rc
