#!/bin/sh
# run.sh — build bench/watch's probe for this host and run it into <out.tsv>: the measurements behind KR-330's brief
# (ROADMAP_DETAIL s5-file-watch). On Windows (msys2 UCRT64) it is rdcw.c, ReadDirectoryChangesExW; on Linux, inotify.c,
# which under WSL can also be pointed at a directory on drvfs (/mnt/c) to see what inotify reports there.
#
# Usage: bench/watch/run.sh <out.tsv> [<dir-on-drvfs> <its-windows-path>]
#
# Each probe's header names its columns. The scenarios run in a temporary directory, never the worktree.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: run.sh <out.tsv> [<dir-on-drvfs> <its-windows-path>]}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*)
        cc -O2 -std=c11 -Wall -Wextra -Werror -o "$tmp/rdcw.exe" "$here/rdcw.c"
        "$tmp/rdcw.exe" "$(cygpath -w "$tmp/run")" > "$out" ;;
    Linux)
        cc -O2 -std=c11 -Wall -Wextra -Werror -o "$tmp/inotify" "$here/inotify.c"
        shift
        "$tmp/inotify" "$tmp/run" "$@" > "$out" ;;
    *) echo "run.sh: Windows or Linux only (FSEvents and kqueue are unprobed)" >&2; exit 2 ;;
esac
