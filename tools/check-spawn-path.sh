#!/bin/sh
# check-spawn-path.sh — std::process starts a program named by a path relative to the current directory (KR-380).
#
# A program named with a directory part is a path, never a name to search for. On Windows, CreateProcessW was given no
# application name and found the program from the command line, and its search takes a name holding only `/` for a bare
# one: with NoDefaultCurrentDirectoryInExePath set (a hardened machine's choice, which takes the current directory out of
# that search), `sub/tool.exe` was "not found" while `sub\tool.exe` started. Every spelling here runs with the variable
# set, which is inert off Windows. There `sub/tool`, with no `.exe`, starts too, as Rust's std::process starts it.
# The starter lives in `bin/`, not beside `sub/`: Windows' search also tries the directory the parent was loaded from,
# which would find `sub/tool.exe` there and hide the defect.
#
# check-legs: native
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
fail() { echo "check-spawn-path: FAIL — $*" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/sub" "$tmp/bin"

cat > "$tmp/tool.kama" <<'EOF'
fn uint8 main() { return 0; }
EOF
cat > "$tmp/starter.kama" <<'EOF'
import { core::println, core::env::args, std::process::Command, std::process::Output, std::io::IoError };
// Starts the program its argument names, and says whether it started and succeeded.
fn uint8 main() {
    string p = match (args().get(at: 0)) { case Some(value: v): give v; case None: ""; };
    Result<Output, IoError> r = Command.make(program: copy p).run();
    match (r) {
        case Ok(value: o): { if (o.status().success()) { return 0; } return 2; }
        case Err(error: e): { string why = e.message(); println(s: "could not start ${p}: ${why}"); return 1; }
    };
}
EOF
"$KAMA" build "$tmp/tool.kama" -o "$tmp/sub/tool" >/dev/null 2>"$tmp/b.err" || { sed 's/^/  /' "$tmp/b.err" >&2; fail "the tool did not build"; }
"$KAMA" build "$tmp/starter.kama" -o "$tmp/bin/starter" >/dev/null 2>"$tmp/b.err" || { sed 's/^/  /' "$tmp/b.err" >&2; fail "the starter did not build"; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) spellings='sub/tool.exe ./sub/tool.exe sub\tool.exe sub/tool' ;;
    *)                    spellings='sub/tool ./sub/tool' ;;
esac
for p in $spellings; do
    st=0
    ( cd "$tmp" && env NoDefaultCurrentDirectoryInExePath=1 ./bin/starter "$p" ) >"$tmp/out" 2>&1 || st=$?
    [ "$st" = 0 ] || { sed 's/^/  /' "$tmp/out" >&2; fail "\`Command.make(program: \"$p\")\` did not start the program (status $st)"; }
done
echo "check-spawn-path: OK (a relative program path started, spelled $(echo $spellings | sed 's/ /, /g'))"
