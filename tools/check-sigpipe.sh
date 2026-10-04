#!/bin/sh
# check-sigpipe.sh — a write to a closed socket or pipe is an ERROR, not the end of the program, and the
# three places that must NOT change did not.
#
# A kama `main` ignores SIGPIPE, so a write to a peer that hung up, or to the stdin of a child that exited, returns
# `IoError::BrokenPipe` (tests/net_write_closed_peer.kama, tests/proc_write_exited_child.kama prove that half). The
# fixtures cannot prove the other half, because each is about a process boundary a fixture's exit code does not see:
#
#   1. stdout keeps the Unix convention. `prog | head -n 1` must still END `prog` by SIGPIPE (exit 141): a program
#      that turned a closed stdout into an ignored error would run a whole loop into a pipe nobody reads.
#   2. A child is handed back the disposition it would have inherited. An ignored signal SURVIVES exec, so without
#      the reset in kama_proc_spawn a spawned `yes | head -n 1` gets EPIPE and prints "yes: stdout: Broken pipe"
#      where the shell expects it to be stopped silently. Measured: 25 bytes of stderr without the reset, 0 with it.
#   3. A `--shared` library never touches its host's signals. It has no `main`, so nothing ignores SIGPIPE there;
#      a C host that calls into one must see the disposition it had before the call.
#
# Each runs TWICE, under a launcher that sets the disposition the program inherits: default, and IGNORED. The second
# is not hypothetical — the GitHub Actions runner starts every step with SIGPIPE ignored, and this guard passed on
# every developer machine while all three checks failed on every CI leg at 0.9.477 (a kama `main` then left an
# inherited ignore alone). A shell cannot reset a signal it was started with ignored, so the launcher is C.
#
# POSIX only: Windows has no SIGPIPE.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-sigpipe: $KAMA not built" >&2; exit 1; fi
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) echo "SKIP check-sigpipe (Windows has no SIGPIPE)"; exit 0;; esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*" >&2; fail=1; }
ok()  { echo "  ok: $*"; }

# `disp dfl|ign PROGRAM ARGS…` — exec PROGRAM with SIGPIPE at its default, or ignored.
cat > "$tmp/disp.c" <<'C'
#include <signal.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 3) return 120;
    (void)signal(SIGPIPE, strcmp(argv[1], "ign") == 0 ? SIG_IGN : SIG_DFL);
    execv(argv[2], argv + 2);
    return 121;
}
C
cc -o "$tmp/disp" "$tmp/disp.c" > "$tmp/b0.log" 2>&1 || { echo "check-sigpipe: the launcher did not build" >&2; cat "$tmp/b0.log" >&2; exit 1; }

# 1. stdout into a closed pipe still ends the program by SIGPIPE.
cat > "$tmp/loud.kama" <<'KAMA'
import { core::println };
fn int32 main() {
    int32 i = 0;
    while (i < 1000000) { println(s: "line"); i = i + 1; }
    return 0;
}
KAMA
"$KAMA" build "$tmp/loud.kama" -o "$tmp/loud" > "$tmp/b1.log" 2>&1 || { bad "loud.kama did not build"; cat "$tmp/b1.log" >&2; }
for d in dfl ign; do
    ( set +e; "$tmp/disp" $d "$tmp/loud"; echo $? > "$tmp/st" ) | head -n 1 > /dev/null
    st=$(cat "$tmp/st")
    if [ "$st" = 141 ]; then ok "[$d] \`prog | head -n 1\` still ends prog by SIGPIPE (exit 141)"
    else bad "[$d] \`prog | head -n 1\` left prog with exit $st — stdout must keep the Unix convention (141)"; fi
done

# 2. A spawned child gets SIGPIPE back at its default.
cat > "$tmp/spawn.kama" <<'KAMA'
import { std::process::Command, std::process::Output, std::io::IoError };
fn int32 main() {
    Command c = Command.make(program: "sh");
    c.arg(a: "-c"); c.arg(a: "yes | head -n 1 >/dev/null");
    Result<Output, IoError> r = c.run();
    return match (give r) { case Ok(value: o): cast<int32>(o.stderr().length()); case Err(error: e): 99; };
}
KAMA
"$KAMA" build "$tmp/spawn.kama" -o "$tmp/spawn" > "$tmp/b2.log" 2>&1 || { bad "spawn.kama did not build"; cat "$tmp/b2.log" >&2; }
for d in dfl ign; do
    set +e; "$tmp/disp" $d "$tmp/spawn"; st=$?; set -e
    if [ "$st" = 0 ]; then ok "[$d] a spawned \`yes | head\` is stopped silently — the child started at the default"
    else bad "[$d] a spawned \`yes | head\` wrote $st bytes to stderr — the child inherited SIGPIPE ignored"; fi
done

# 3. A --shared library leaves its host's disposition alone.
cat > "$tmp/lib.kama" <<'KAMA'
import { core::print };
expose fn int32 ping(int32 x) { print(s: ""); return x + 1; }
KAMA
case "$(uname -s)" in Darwin) so="$tmp/libping.dylib";; *) so="$tmp/libping.so";; esac
"$KAMA" build "$tmp/lib.kama" --shared -o "$so" > "$tmp/b3.log" 2>&1 || { bad "lib.kama --shared did not build"; cat "$tmp/b3.log" >&2; }
cat > "$tmp/host.c" <<'C'
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
int32_t lib_ping(int32_t x);   /* an `expose fn` is `<module>_<name>` in C */
int main(void) {
    struct sigaction before, after;
    if (sigaction(SIGPIPE, NULL, &before) != 0) return 4;
    if (lib_ping(1) != 2) return 3;
    if (sigaction(SIGPIPE, NULL, &after) != 0) return 4;
    return after.sa_handler == before.sa_handler ? 0 : 5;
}
C
if cc -o "$tmp/host" "$tmp/host.c" "$so" > "$tmp/b4.log" 2>&1; then
    for d in dfl ign; do
        set +e; ( cd "$tmp" && LD_LIBRARY_PATH="$tmp" DYLD_LIBRARY_PATH="$tmp" "$tmp/disp" $d "$tmp/host" ); st=$?; set -e
        if [ "$st" = 0 ]; then ok "[$d] a host that calls into a --shared kama library keeps its SIGPIPE disposition"
        else bad "[$d] the host's SIGPIPE disposition changed, or the call failed (exit $st)"; fi
    done
else
    bad "the C host did not link against the --shared library"; cat "$tmp/b4.log" >&2
fi

if [ "$fail" != 0 ]; then echo "check-sigpipe: FAILED" >&2; exit 1; fi
echo "check-sigpipe: PASS (stdout keeps SIGPIPE, a child starts at the default, a --shared library touches nothing — each inheriting SIGPIPE at default and ignored)"
