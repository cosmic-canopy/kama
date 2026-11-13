#!/bin/sh
# ci-leftovers.sh — after a failed or timed-out CI test step (Linux, macOS): the processes the job started that are still
# running, with their parents and command lines. A leftover that holds the step's output open keeps the step from ending.
# Only the processes younger than the job's worker (Runner.Worker): on macOS the runner's user also owns the desktop
# session's, a hundred lines that say nothing about the job.
ps -ww -U "$(id -u)" -o pid=,ppid=,etime=,command= | awk '
  function secs(t,   d, n, a, s, i) {   # ps etime, [[dd-]hh:]mm:ss, in seconds
    d = 0; if (t ~ /-/) { d = substr(t, 1, index(t, "-") - 1); t = substr(t, index(t, "-") + 1) }
    n = split(t, a, ":"); s = 0; for (i = 1; i <= n; i++) s = s * 60 + a[i]; return d * 86400 + s
  }
  { e[NR] = secs($3); l[NR] = $0 }
  /Runner\.Worker/ { w = secs($3) }
  END { print "  PID  PPID  ELAPSED COMMAND"; for (i = 1; i <= NR; i++) if (!w || e[i] < w) print l[i] }'
