#!/bin/sh
# check-sigpipe.sh — a write to a closed socket or pipe is an ERROR, not the end of the program (KPG-1), and the
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
#      a C host that calls into one must still see SIG_DFL afterwards.
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
( set +e; "$tmp/loud"; echo $? > "$tmp/st" ) | head -n 1 > /dev/null
st=$(cat "$tmp/st")
if [ "$st" = 141 ]; then ok "\`prog | head -n 1\` still ends prog by SIGPIPE (exit 141)"
else bad "\`prog | head -n 1\` left prog with exit $st — stdout must keep the Unix convention (141)"; fi

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
set +e; "$tmp/spawn"; st=$?; set -e
if [ "$st" = 0 ]; then ok "a spawned \`yes | head\` is stopped silently — the child inherited SIGPIPE at its default"
else bad "a spawned \`yes | head\` wrote $st bytes to stderr — the child inherited SIGPIPE ignored"; fi

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
    struct sigaction sa;
    if (lib_ping(1) != 2) return 3;
    if (sigaction(SIGPIPE, NULL, &sa) != 0) return 4;
    return sa.sa_handler == SIG_DFL ? 0 : 5;
}
C
if cc -o "$tmp/host" "$tmp/host.c" "$so" > "$tmp/b4.log" 2>&1; then
    set +e; ( cd "$tmp" && LD_LIBRARY_PATH="$tmp" DYLD_LIBRARY_PATH="$tmp" ./host ); st=$?; set -e
    if [ "$st" = 0 ]; then ok "a host that calls into a --shared kama library still has SIGPIPE at its default"
    else bad "the host's SIGPIPE disposition changed, or the call failed (exit $st)"; fi
else
    bad "the C host did not link against the --shared library"; cat "$tmp/b4.log" >&2
fi

if [ "$fail" != 0 ]; then echo "check-sigpipe: FAILED" >&2; exit 1; fi
echo "check-sigpipe: PASS (stdout keeps SIGPIPE, a child inherits the default, a --shared library touches nothing)"
