#!/usr/bin/env bash
# count.sh — what one request costs kama's fiber server in system calls and io_uring events, on the ring arm against
# the epoll arm (KR-253, docs/benchmarks/ring-counts-*.tsv). Counts, not timings: load on the machine moves a timing
# and not these, so the two machines' counts can be set side by side and a count that differs names the suspect.
#
# Usage: bench/server/count.sh <out.tsv> [runs]      Linux, with perf's tracepoints open to it: as root on a host
#        (`sudo bench/server/count.sh …`), or `bench/run count <out.tsv>` in the bench container.
#   CONFIGS  `echo:<conns>` pairs (default: echo:1000 echo:10000)     CORES  the server's cores (default 3)
#   WARMUP, SECONDS_  the client's warm-up and measured window (default 1 and 5)
#
# For each run, config and arm (`kama-fibers`, the ring; `kama-fibers-epoll`, the same binary under KAMA_IO_URING=0) it
# starts the server and the client as run.sh does, and once the client's warm-up is over, counts the server's process
# for the client's measured window with `perf stat`. Each count is divided by the requests the client completed in that
# window. Recorded per request:
#   - system calls by name: recvfrom, sendto, io_uring_enter, the epoll waits (epoll_wait, epoll_pwait, epoll_pwait2
#     together), read (the waker's eventfd, which is all the server reads with read()), and every other call summed;
#   - the tracepoints io_uring_submit_req, io_uring_complete, io_uring_poll_arm, io_uring_local_work_run and
#     io_uring_task_work_run, and the server's context switches.
# Also per call: completions per io_uring_enter (the ring arm), and events per epoll wait (the epoll arm, from the
# waits' return values, which `perf record` keeps). A tracepoint the kernel lacks is written as `-`. The output goes
# where the caller says, never into the worktree unless asked; run as root through sudo, it is handed back to the user.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=${1:?usage: count.sh <out.tsv> [runs]}
runs=${2:-3}
[ "$(uname)" = Linux ] || { echo "count.sh: Linux only (io_uring, perf's tracepoints)" >&2; exit 2; }
command -v perf >/dev/null || { echo "count.sh: needs perf" >&2; exit 2; }
if ! perf stat -e raw_syscalls:sys_enter -- true >/dev/null 2>&1; then
    echo "count.sh: perf cannot count tracepoints here: run it as root (sudo), or lower kernel.perf_event_paranoid" >&2
    echo "  and open /sys/kernel/tracing; in a container, \`bench/run count\` passes what it needs" >&2
    echo "  (on a rootless podman machine, the VM needs two settings first: see the comment above \`count)\` in bench/run)" >&2
    exit 2
fi
mkdir -p "$(dirname "$out")"
tmp=$(mktemp -d); spid=""
kill_server() { [ -n "$spid" ] || return 0; kill -TERM "$spid" 2>/dev/null || true; wait "$spid" 2>/dev/null || true; spid=""; }
trap 'kill_server; rm -rf "$tmp"' EXIT

ncpu=$(getconf _NPROCESSORS_ONLN)
cores=${CORES:-3}
warm=${WARMUP:-1}
secs=${SECONDS_:-5}
configs=${CONFIGS:-"echo:1000 echo:10000"}
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -o "$tmp/client" "$here/client.c"
ROOT=$root; . "$root/tools/kama-bin.sh"
# A compiler older than the tree would count the old runtime: the stale-binary trap (AGENTS.md). bench/run builds one in
# the container; on a host, `./dev build` first, as yourself rather than through sudo.
built=$("$KAMA" --version | awk '{print $2}'); built=${built%%+*}
[ "$built" = "$(cat "$root/VERSION")" ] || { echo "count.sh: $KAMA is $built and this tree is $(cat "$root/VERSION"): build it first" >&2; exit 2; }
"$KAMA" build "$here/kama/fibers.kama" -o "$tmp/kama-fibers" --release >/dev/null

# The events this kernel has: a tracepoint it lacks is left out of the count and written as `-`.
tracing=/sys/kernel/tracing; [ -d "$tracing/events" ] || tracing=/sys/kernel/debug/tracing
have() { [ -d "$tracing/events/$1" ]; }
events="raw_syscalls:sys_enter context-switches"
for sc in recvfrom sendto io_uring_enter epoll_wait epoll_pwait epoll_pwait2 read; do
    have "syscalls/sys_enter_$sc" && events="$events syscalls:sys_enter_$sc"
done
for tp in io_uring_submit_req io_uring_complete io_uring_poll_arm io_uring_local_work_run io_uring_task_work_run; do
    have "io_uring/$tp" && events="$events io_uring:$tp"
done
exits=""
for sc in epoll_wait epoll_pwait epoll_pwait2; do have "syscalls/sys_exit_$sc" && exits="$exits,syscalls:sys_exit_$sc"; done
exits=${exits#,}

printf 'run\tarm\tmode\tconns\tkernel\trps\twindow_s\trecvfrom\tsendto\tio_uring_enter\tepoll_wait\tread\tother_syscalls\tsubmit_req\tcomplete\tpoll_arm\tlocal_work_run\ttask_work_run\tctx_switches\tcompletions_per_enter\tevents_per_epoll_wait\n' > "$out"
cthreads=$(( ncpu - cores > 0 ? ncpu - cores : 1 ))
port=22000
for r in $(seq 1 "$runs"); do
    for cfg in $configs; do
        mode=${cfg%%:*}; conns=${cfg#*:}
        for arm in kama-fibers kama-fibers-epoll; do
            port=$((port + 1))
            off=1; [ "$arm" = kama-fibers-epoll ] && off=0
            KAMA_IO_URING=$off BENCH_MODE=$mode BENCH_PORT=$port BENCH_CORES=$cores \
                taskset -c "0-$((cores - 1))" "$tmp/kama-fibers" & spid=$!
            sleep 0.3
            taskset -c "$cores-$((ncpu - 1))" "$tmp/client" "$mode" "$port" "$conns" "$cthreads" "$warm" "$secs" \
                > "$tmp/line" 2> "$tmp/err" & cl=$!
            sleep "$warm"
            rec=""
            if [ "$arm" = kama-fibers-epoll ] && [ -n "$exits" ]; then
                perf record -q -e "$exits" -p "$spid" -o "$tmp/waits.data" -- sleep "$secs" >/dev/null 2>&1 & rec=$!
            fi
            perf stat -x, -o "$tmp/stat" -e "${events// /,}" -p "$spid" -- sleep "$secs" >/dev/null 2>&1 || true
            [ -n "$rec" ] && { wait "$rec" || true; }
            wait "$cl" || true
            : > "$tmp/waits"
            [ -n "$rec" ] && perf script -i "$tmp/waits.data" -F trace > "$tmp/waits" 2>/dev/null || true
            python3 - "$tmp/stat" "$tmp/line" "$tmp/waits" "$r" "$arm" "$mode" "$conns" "$(uname -r)" "$secs" >> "$out" <<'PY'
import re, sys
stat, line, waits, run, arm, mode, conns, kernel, secs = sys.argv[1:]
counts = {}
for l in open(stat):
    f = l.strip().split(",")
    if len(f) < 3 or not f[2] or l.startswith("#"):
        continue
    counts[f[2]] = None if f[0].startswith("<") else float(f[0])
cols = open(line).read().split("\t")
rps = float(cols[5]) if len(cols) > 5 and cols[5] else 0.0
reqs = rps * float(secs)
def get(*names):                           # the sum of those counted; None when none was
    vals = [counts[n] for n in names if counts.get(n) is not None]
    return sum(vals) if vals else None
def per(v):
    return "-" if v is None or reqs <= 0 else f"{v / reqs:.4f}"
named = ["recvfrom", "sendto", "io_uring_enter", "epoll_wait", "epoll_pwait", "epoll_pwait2", "read"]
total = get("raw_syscalls:sys_enter")
known = sum(counts.get(f"syscalls:sys_enter_{n}") or 0 for n in named)
enter = get("syscalls:sys_enter_io_uring_enter")
complete = get("io_uring:io_uring_complete")
cpe = "-" if not enter or complete is None else f"{complete / enter:.2f}"
rets = []                                  # each wait's return, the last hex number on its line (perf prints 0x%lx)
for l in open(waits):
    h = re.findall(r"0x[0-9a-f]+", l)
    if h:
        rets.append(int(h[-1], 16))
epw = f"{sum(r for r in rets if r < 1 << 63) / len(rets):.2f}" if rets else "-"
row = [run, arm, mode, conns, kernel, f"{rps:.0f}", secs,
       per(get("syscalls:sys_enter_recvfrom")), per(get("syscalls:sys_enter_sendto")),
       per(enter), per(get(*[f"syscalls:sys_enter_{n}" for n in ("epoll_wait", "epoll_pwait", "epoll_pwait2")])),
       per(get("syscalls:sys_enter_read")), per(None if total is None else total - known),
       per(get("io_uring:io_uring_submit_req")), per(complete), per(get("io_uring:io_uring_poll_arm")),
       per(get("io_uring:io_uring_local_work_run")), per(get("io_uring:io_uring_task_work_run")),
       per(get("context-switches")), cpe, epw]
print("\t".join(row))
PY
            kill_server
            echo "count.sh: run $r $cfg $arm done" >&2
        done
    done
done
[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$out" || true   # handed back to the user who ran sudo
echo "count.sh: done — $out" >&2
