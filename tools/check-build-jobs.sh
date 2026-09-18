#!/bin/sh
# check-build-jobs.sh — `kama build -j N` must produce exactly what `-j 1` produces, only faster.
#
# A C compiler handed N sources in ONE invocation compiles them serially, so a program that imports
# `std` (16-32 translation units, because a directory-module import pulls in every file in the
# directory) uses one core for most of `kama build`'s wall time. `-j` compiles each TU as its own `-c`
# job and links the objects — measured ~2x on a 32-TU debug build.
#
# `-j` is the one build flag that provably cannot change what is produced, and that invariant is what
# this guard exists to hold. It asserts:
#
#   1. OBJECTS ARE BYTE-IDENTICAL between -j 1 and -j 10. Built twice into the SAME directory, because
#      `-g` embeds the source path in debug info, so two builds in different directories would differ
#      for a reason that has nothing to do with -j. Compare the `.o` — NOT the final binary (the macOS
#      linker makes that hash vary between two *identical* serial builds; found while measuring `make
#      -j`) and NOT the `.a` (macOS `ar` is not deterministic by default).
#   2. BEHAVIORAL EQUIVALENCE of an executable: same exit code, stdout and stderr from the program.
#   3. `-j 1` IS TODAY'S PATH, counted rather than asserted in prose. A fake `--cc` tallies invocations:
#      one invocation at -j 1, and N+1 (N compiles + one link) above it. This is what catches a future
#      refactor quietly routing -j 1 through the pool, which would cost the suite ~40% more CPU.
#   4. `--release` NATIVE IS STILL ONE TU. Release folds every unit into a unity TU for cross-module
#      inlining; if -j ever split that, the inlining (~6x on the math bench) would silently vanish.
#   5. DIAGNOSTICS ARE REPLAYED WHOLE AND IN SOURCE ORDER, and identically at every width — asserted as
#      a byte-equality between two pool widths rather than by eyeballing, and without depending on any
#      real compiler's message text.
#   6. A BAD -j IS REJECTED BY NAME rather than rounded to something.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-build-jobs: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# Absolute paths throughout rather than a `cd $ROOT`: the guards run in parallel from one runner, so
# none of them may change directory outside a subshell. The runner also exports KAMA_BUILD_JOBS=1 for
# the whole pool, which is exactly why every case below passes -j on the COMMAND LINE, where it wins.
# The fixture is GENERATED, not borrowed from tests/. It must stay comfortably wider than the -j 10 pool
# below or the width-dependence cases compare two runs that both compiled everything and prove nothing —
# and a borrowed fixture's width is not this guard's to control. Closure pruning made that concrete: it
# took tests/parse_radix.kama from 24 units to 3, and the replacement from 19 to 10 in the same afternoon,
# each time surfacing as a confusing "block order is width-dependent" rather than "your fixture shrank".
# Sixteen single-symbol modules, each genuinely referenced by main, so no resolution-level pruning can
# drop one.
#
# ⚠️ ONE MODULE PER DIRECTORY, and every file passed on the command line. Both are §2i: a module is a
# FOLDER, and a loose build resolves nothing off disk — the operands ARE the compilation. These used to be
# sixteen flat `src/wN.kama` files reached by the file-module rule (`<dir>/w1.kama` for `import w1`),
# which 2d deletes along with the search that found them.
NTU=16
mkdir -p "$tmp/src"
for i in $(seq 1 $NTU); do
    mkdir -p "$tmp/src/w$i"
    printf 'export { v%d };\n\nfn int32 v%d() { return %d; }\n' "$i" "$i" "$i" \
        > "$tmp/src/w$i/w$i.kama"
done
{
    # ONE import block — the generator emitted one `import` line per module until the scope moved inside
    # the braces, and a second `import` is now a parse error rather than a second directive.
    printf 'import {\n'
    for i in $(seq 1 $NTU); do printf '    w%d::v%d,\n' "$i" "$i"; done
    printf '};\n'
    printf '\nfn int32 main() {\n    int32 t = 0;\n'
    for i in $(seq 1 $NTU); do printf '    t = t + v%d();\n' "$i"; done
    printf '    return t - %d;\n}\n' "$(( NTU * (NTU + 1) / 2 ))"
} > "$tmp/src/multi.kama"
# The whole operand set, deliberately UNQUOTED at every use below: one argument per file (mktemp paths
# carry no spaces). $MULTI was one file back when the other fifteen were found on disk.
MULTI="$tmp/src/multi.kama"
for i in $(seq 1 $NTU); do MULTI="$MULTI $tmp/src/w$i/w$i.kama"; done
units=$("$KAMA" check $MULTI 2>&1 | sed -n 's/.*OK (\([0-9]*\) unit.*/\1/p')
if [ -z "$units" ] || [ "$units" -lt 12 ]; then
    echo "check-build-jobs: generated fixture is ${units:-?} units, expected $((NTU + 1)) — too narrow" >&2
    "$KAMA" check $MULTI >&2 2>&1 || true
    exit 1
fi

# --- 1. objects byte-identical, built at the same paths ---------------------------------------------
# STATIC is the output kind that produces objects at BOTH widths, which is what makes them comparable.
mkdir -p "$tmp/w" "$tmp/ref"
"$KAMA" build $MULTI --select OUTPUT=STATIC -o "$tmp/w/libpr.a" -j 1 --keep-c >/dev/null 2>&1 || true
if ! ls "$tmp/w"/*.o >/dev/null 2>&1; then
    bad "OUTPUT=STATIC -j 1 produced no objects at all"
else
    cp "$tmp/w"/*.o "$tmp/ref/"
    n1=$(ls "$tmp/ref"/*.o | wc -l | tr -d ' ')
    rm -f "$tmp/w"/*.o "$tmp/w"/*.a
    "$KAMA" build $MULTI --select OUTPUT=STATIC -o "$tmp/w/libpr.a" -j 10 --keep-c >/dev/null 2>&1 || true
    n2=$(ls "$tmp/w"/*.o 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n1" != "$n2" ]; then
        bad "STATIC produced $n1 objects at -j 1 but $n2 at -j 10"
    else
        diffs=0
        for o in "$tmp/ref"/*.o; do
            cmp -s "$o" "$tmp/w/$(basename "$o")" || diffs=$((diffs+1))
        done
        [ "$diffs" = 0 ] && ok "all $n1 objects byte-identical at -j 1 and -j 10" \
                         || bad "$diffs of $n1 objects differ between -j 1 and -j 10"
    fi
fi

# --- 2. an executable behaves identically ------------------------------------------------------------
# `rc=$?` after a bare command would trip `set -e` — and a fixture exiting nonzero is NORMAL here
# (parse_radix's own program exits 42). Capture through `if`, which is exempt.
run() { if "$@"; then return 0; else return $?; fi; }
if run "$KAMA" build $MULTI -o "$tmp/a" -j 1  >/dev/null 2>"$tmp/a.builderr"; then ra=0; else ra=$?; fi
if run "$KAMA" build $MULTI -o "$tmp/b" -j 10 >/dev/null 2>"$tmp/b.builderr"; then rb=0; else rb=$?; fi
if [ "$ra" != 0 ] || [ "$rb" != 0 ]; then
    bad "building the multi-unit fixture failed (rc $ra at -j 1, $rb at -j 10)"
else
    if run "$tmp/a" >"$tmp/a.out" 2>"$tmp/a.err"; then xa=0; else xa=$?; fi
    if run "$tmp/b" >"$tmp/b.out" 2>"$tmp/b.err"; then xb=0; else xb=$?; fi
    [ "$xa" = "$xb" ]              || bad "program exit code differs: $xa at -j 1, $xb at -j 10"
    cmp -s "$tmp/a.out" "$tmp/b.out" || bad "program stdout differs between -j 1 and -j 10"
    cmp -s "$tmp/a.err" "$tmp/b.err" || bad "program stderr differs between -j 1 and -j 10"
    [ "$xa" = "$xb" ] && cmp -s "$tmp/a.out" "$tmp/b.out" && ok "executable behaves identically at -j 1 and -j 10"
fi

# --- 3/4. count compiler invocations ------------------------------------------------------------------
# A --cc shim that tallies one line per invocation. It deliberately does NOT run a real compiler: only
# the invocation COUNT is under test here, and this guard runs inside a core-wide parallel pool, so four
# unnecessary 24-TU builds are four everyone else waits behind. Compiles that produce no object make the
# link fail, which is fine — the link still counts, and every case below ignores the exit status.
#
# kama runs the compiler through system(), which on Windows is `cmd /c`, and cmd cannot execute a
# `#!/bin/sh` file — it is not a program to it, so the shim never ran and every case here counted 0
# invocations. Both shims below are therefore invoked as `sh <script>` there, which is legal because kama
# treats a compiler as a SHELL STRING rather than a program path.
#
# ⚠️ Each invocation drops its OWN uniquely-named file into $COUNTDIR, and the tally is a file count.
# Appending a line to one shared file is NOT safe here: the whole point of this guard is that `-j N` runs
# N compilers AT ONCE, and on Windows concurrent `cmd` processes appending to a single file do not get
# atomic-append semantics, so writes clobber each other. It counted 18 of 18 on an idle machine and 14 of
# 18 inside the parallel guard pool — a flake that reads as "the pool lost jobs" when the pool was fine
# and the TALLY was lossy. A distinct file per invocation cannot race.
cat >"$tmp/ccount" <<'EOF'
#!/bin/sh
# `--version` is not a compile: the driver asks each distinct `--cc` string which family it belongs to,
# once per build, because the warning flags it emits are spelled differently by clang and gcc and are a
# hard error on the wrong one (KR-72). Answering it the way a real compiler does keeps this guard counting
# COMPILES — the thing it is about — instead of drifting by one every time the driver asks a question.
case " $* " in
    *" --version "*) echo "cc (kama check-build-jobs shim) 0.0.0"; exit 0 ;;
esac
# $$ is this shim process's pid — one per invocation, so no two concurrent writers pick the same name.
: > "$COUNTDIR/$$.tick"
exit 0
EOF
chmod +x "$tmp/ccount"

# ONE shim, run through `sh` on Windows rather than rewritten as a .cmd. kama treats a "compiler" as a
# SHELL STRING, not a program path (`"…/zig" cc` is the precedent), so `sh <script>` is a legal --cc and
# cmd.exe can start `sh` — which is how the POSIX shim gets to run unchanged.
#
# The .cmd version this replaces could not tally correctly: cmd has no PID to key a filename on, and
# %RANDOM% is seeded from the SYSTEM CLOCK, so `cmd` processes launched in the same tick — which is
# exactly what `-j 4` does — draw identical sequences, write the same filename, and overwrite each other.
# It counted 3, 2 and 1 of 18 across three runs. `$$` is unique by construction.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CCOUNT="sh $(kama_native_path "$tmp")/ccount" ;;
    *)                    CCOUNT="$tmp/ccount" ;;
esac

invocations() {   # invocations <label> <expected> -- <extra kama args...>
    lab="$1"; want="$2"; shift 3
    # Native-spelled: COUNTDIR reaches the shim through the ENVIRONMENT, which msys2 does not convert
    # (see kama_native_path). The shell would otherwise count files in a directory cmd never wrote to.
    rm -rf "$tmp/count.$lab"; mkdir -p "$tmp/count.$lab"
    COUNTDIR="$(kama_native_path "$tmp")/count.$lab"; export COUNTDIR
    "$KAMA" build $MULTI -o "$tmp/c_$lab" --cc "$CCOUNT" "$@" >/dev/null 2>&1 || true
    got=$(ls "$tmp/count.$lab" | wc -l | tr -d ' ')
    unset COUNTDIR
    if [ "$got" = "$want" ]; then ok "$lab: $got compiler invocation(s)"
    else bad "$lab: $got compiler invocations, expected $want"; fi
}

# -j 1 must be ONE invocation over all sources — today's path, unchanged.
invocations "j1"      1  -- -j 1
# -j 4 must be 24 compiles + 1 link for this 24-TU fixture.

nTU=$(ls "$tmp/ref"/*.o 2>/dev/null | wc -l | tr -d ' ')
if [ -n "$nTU" ] && [ "$nTU" -gt 1 ]; then
    invocations "j4" "$((nTU + 1))" -- -j 4
else
    bad "could not determine the TU count from step 1, so -j 4's invocation count is unchecked"
fi
# --release native folds to ONE unity TU, so -j cannot split it however wide it is asked to go.
invocations "release-j10" 1 -- --release -j 10

# --- 5. diagnostics: whole, in source order, identical at every width --------------------------------
# A --cc shim that emits a unique multi-line block naming its own -o, and fails for one chosen TU. This
# tests the replay, not any compiler's wording.
cat >"$tmp/ccnoise" <<'EOF'
#!/bin/sh
out=""
prev=""
for a in "$@"; do [ "$prev" = "-o" ] && out="$a"; prev="$a"; done
b=$(basename "$out")
i=1
while [ $i -le 5 ]; do echo "NOISE $b line $i" >&2; i=$((i+1)); done
# One chosen TU fails, unless the caller asked for an all-succeed run (case 5a). It names a module this
# guard GENERATES, so the trigger cannot go missing the way a stdlib TU can.
[ -n "${KAMA_JOBS_NOFAIL:-}" ] || case "$b" in w7_*) exit 1 ;; esac
exit 0
EOF
chmod +x "$tmp/ccnoise"
# Through `sh` on Windows, for the reason the tally shim is — cmd.exe cannot start a `#!/bin/sh` file.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CCNOISE="sh $(kama_native_path "$tmp")/ccnoise" ;;
    *)                    CCNOISE="$tmp/ccnoise" ;;
esac

# 5a. When every job SUCCEEDS, all N run and replay, so stderr must be byte-identical at any POOLED
#     width. This is the strongest form of the ordering claim and it needs no message text.
#     -j 1 is excluded on purpose: it is one invocation over all sources, so it has no per-TU
#     diagnostics to order. That it stays one invocation is check 3's assertion, not this one's.
for j in 2 3 10; do
    KAMA_JOBS_NOFAIL=1 "$KAMA" build $MULTI -o "$tmp/s_$j" --cc "$CCNOISE" -j "$j" \
        >"$tmp/s_$j.out" 2>"$tmp/s_$j.err" || true
    # The final link runs the shim too and names the output binary, which differs per width; drop it.
    grep '^NOISE ' "$tmp/s_$j.err" | grep -v "NOISE s_$j " >"$tmp/s_$j.blocks"
done
if cmp -s "$tmp/s_2.blocks" "$tmp/s_3.blocks" && cmp -s "$tmp/s_2.blocks" "$tmp/s_10.blocks"; then
    ok "all-succeed: per-TU diagnostics byte-identical at -j 2, 3 and 10"
else
    bad "all-succeed: per-TU diagnostics differ by pool width — the replay is not deterministic"
    diff "$tmp/s_2.blocks" "$tmp/s_10.blocks" | head -10 >&2
fi

# 5b. When a job FAILS, the wave stops LAUNCHING but drains what is in flight, so *how many* blocks
#     appear is width-dependent — exactly as `make -j` behaves, and not a defect. What must still hold:
#     every block that appears is whole, and the blocks are in SOURCE order at every width (so the
#     narrower run's block list is a prefix of the wider run's).
for j in 2 10; do
    "$KAMA" build $MULTI -o "$tmp/n_$j" --cc "$CCNOISE" -j "$j" >"$tmp/n_$j.out" 2>"$tmp/n_$j.err" || true
done
torn=0
for b in $(awk '/^NOISE /{print $2}' "$tmp/n_10.err" | sort -u); do
    c=$(grep -c "^NOISE $b line " "$tmp/n_10.err" || true)
    [ "$c" = 5 ] || { torn=$((torn+1)); echo "    $b: $c of 5 lines" >&2; }
done
[ "$torn" = 0 ] && ok "on failure: every captured block replayed whole (5/5 lines)" \
                || bad "on failure: $torn diagnostic block(s) were torn"

awk '/ line 1$/{print $2}' "$tmp/n_2.err"  >"$tmp/order2"
awk '/ line 1$/{print $2}' "$tmp/n_10.err" >"$tmp/order10"
n2=$(wc -l <"$tmp/order2" | tr -d ' ')
if head -n "$n2" "$tmp/order10" | cmp -s - "$tmp/order2"; then
    ok "on failure: block order is source order at both widths (-j 2's list prefixes -j 10's)"
else
    bad "on failure: block order is width-dependent"
    diff "$tmp/order2" "$tmp/order10" | head -10 >&2
fi

# --- 6. a bad -j is rejected by name ------------------------------------------------------------------
for badj in 0 -1 x 99999; do
    if "$KAMA" build $MULTI -o "$tmp/z" -j "$badj" >/dev/null 2>"$tmp/j.err"; then
        bad "-j $badj was accepted"
    elif grep -q 'positive job count' "$tmp/j.err"; then
        ok "-j $badj rejected by name"
    else
        bad "-j $badj failed, but not with the job-count message"
    fi
done
# ...and the long form is the same option. Counted through the shim rather than really built, so that
# "accepted" is checked without a fifth 24-TU compile.
rm -rf "$tmp/count.longform"; mkdir -p "$tmp/count.longform"
COUNTDIR="$(kama_native_path "$tmp")/count.longform"; export COUNTDIR
if "$KAMA" build $MULTI -o "$tmp/z2" --cc "$CCOUNT" --jobs 4 >/dev/null 2>&1 \
   || [ "$(ls "$tmp/count.longform" | wc -l | tr -d ' ')" -gt 0 ]; then ok "--jobs is accepted as -j's long form"
else bad "--jobs 4 was rejected"; fi
unset COUNTDIR

[ "$fail" = 0 ] && echo "check-build-jobs: PASS" || echo "check-build-jobs: FAIL" >&2
exit "$fail"
