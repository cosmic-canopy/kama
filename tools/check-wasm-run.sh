#!/bin/sh
# check-wasm-run.sh — `kama run --target WASM` runs the page in a browser (KR-377).
#
# The run builds into a temp directory of its own with a pre-js linked in, serves it on 127.0.0.1, opens it in a
# browser with a fresh profile, prints what the program prints and exits with its status. None of that is a fixture:
# the suite's browser fixtures run THROUGH it, so they would all fail together without saying which promise broke.
# Checked here, each on its own:
#   - the program's lines, in order, on the stream it wrote them to; its `-- args`; its status; 20,000 lines in time;
#   - a project's `shellFile` page keeps its own hooks beside the runner's;
#   - an abort ends the run with 134, after emscripten's `Aborted(…)` line;
#   - a build whose memory is shared (a thread form, or a setting the manifest forces) is served cross-origin isolated,
#     and any other build is not, as its deployment would be;
#   - a run leaves nothing: no browser process, no profile, no run directory, no `out/` in the project;
#   - a run ended by a signal ends its browser, and on POSIX exits 143 and removes its directory;
#   - the refusals: a `$KAMA_BROWSER` that names nothing, and `--headless` on a run that is not a browser's.
# It runs every browser it can name on PATH as well as the one kama picks, so a leg with Firefox checks Firefox's flags.
#
# check-legs: wasm
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
fail() { echo "check-wasm-run: FAIL — $*" >&2; exit 1; }
command -v "${EMCC:-emcc}" >/dev/null 2>&1 || fail "no emcc: this guard belongs to the wasm leg"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Every run's directory lands in this TMPDIR, so "the run left nothing" is "it is empty", and the browser processes a
# run started are the ones whose command line names it.
mkdir "$tmp/runs"
TMPDIR=$(kama_native_path "$tmp/runs"); export TMPDIR
tag=$(basename "$tmp")
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; *) windows=0 ;; esac
TO=; command -v timeout >/dev/null 2>&1 && TO="timeout 300"

# proj NAME [EXTRA-MANIFEST-KEYS]: a project around $tmp/NAME.kama.
proj() {
    mkdir -p "$tmp/$1/src"
    mv "$tmp/$1.kama" "$tmp/$1/src/main.kama"
    if [ -f "$tmp/shell.html" ]; then cp "$tmp/shell.html" "$tmp/$1/"; fi   # a shellFile lives inside its project
    printf '{ "name": "%s", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "modules": { ".": { "visibility": "internal" } }%s }\n' "$1" "${2:-}" > "$tmp/$1/kama.json"
}
# run NAME [ARGS…]: `kama run` the project headless; sets $st, leaves its streams in $tmp/NAME.out / .err.
run() {
    n=$1; shift
    st=0
    $TO "$KAMA" run "$tmp/$n/kama.json" --target WASM --headless -- "$@" >"$tmp/$n.out" 2>"$tmp/$n.err" || st=$?
}
# Processes still running that a run started: their command lines name its TMPDIR.
survivors() {
    if [ "$windows" = 1 ]; then
        powershell.exe -NoProfile -Command "@(Get-CimInstance Win32_Process | Where-Object { \$_.CommandLine -like '*$tag*' -and \$_.Name -ne 'powershell.exe' }).Count" | tr -d '\r'
    else
        ps -eo args | grep -F "$tag" | grep -cv grep || true
    fi
}
left() {
    [ -z "$(ls -A "$tmp/runs")" ] || fail "$1 left its run directory behind: $(ls -A "$tmp/runs")"
    [ ! -e "$tmp/$2/out" ] || fail "$1 wrote $2/out — a run builds into a directory of its own"
    s=$(survivors); [ "$s" = 0 ] || fail "$1 left $s browser process(es) running"
}

cat > "$tmp/lines.kama" <<'EOF'
import { core::println, core::eprintln, core::env::args };
fn uint8 main() {
    println(s: "one");
    eprintln(s: "two");
    println(s: "three");
    foreach (string a in args()) { println(s: "arg=" + a); }
    return 7;
}
EOF
proj lines

# Lines, args and status, through every browser that can be named. With no browser here the leg cannot check what it
# does not have; kama's own search decides that.
run lines "x y" "a&b=c"
if [ "$st" = 2 ] && grep -q "none was found" "$tmp/lines.err"; then
    echo "check-wasm-run: OK (skipped: no browser — $(head -1 "$tmp/lines.err"))"; exit 0
fi
expect_lines() {
    [ "$st" = 7 ] || { sed 's/^/  /' "$tmp/lines.err" >&2; fail "$1: \`return 7\` ran to status $st"; }
    printf 'one\nthree\narg=x y\narg=a&b=c\n' | cmp -s - "$tmp/lines.out" \
        || { sed 's/^/  | /' "$tmp/lines.out" >&2; fail "$1: stdout is not the program's lines and args, in order"; }
    grep -qx "two" "$tmp/lines.err" || fail "$1: the line written to stderr did not arrive there"
    left "$1" lines
}
expect_lines "the browser kama picks"
browsers="kama's pick"
for b in firefox chromium chromium-browser google-chrome microsoft-edge; do
    command -v "$b" >/dev/null 2>&1 || continue
    KAMA_BROWSER=$b run lines "x y" "a&b=c"
    expect_lines "$b"
    browsers="$browsers, $b"
done

# Many lines, all of them in order, in time. Each line costs the page a little: emscripten's default page wrote every one
# into a textarea, copying all the text before it, and 20,000 took minutes (node: 570 ms). The time limit holds that down.
cat > "$tmp/many.kama" <<'EOF'
import { core::println, std::fmt::i32Str };
fn uint8 main() {
    int32 i = 0;
    while (i < 20000) { println(s: "line " + i32Str(v: i)); i = i + 1; }
    return 3;
}
EOF
proj many
run many
[ "$st" = 3 ] || fail "a program printing 20,000 lines ran to status $st (124: the time limit)"
seq 0 19999 | sed 's/^/line /' | cmp -s - "$tmp/many.out" || fail "a program's 20,000 lines did not all arrive, in order"
left "the 20,000-line run" many

# A shell page of the project's own keeps its hooks: its print still sees each line, and its onExit still runs, which
# reports what it saw and whether the page was cross-origin isolated.
cat > "$tmp/shell.html" <<'EOF'
<!doctype html><meta charset=utf-8><body><script>
var seen = 0;
var Module = { print: function (s) { seen++; },
               onExit: function (c) { Module.printErr('shell: ' + seen + ' line(s), status ' + c + ', isolated=' + self.crossOriginIsolated); } };
</script>{{{ SCRIPT }}}</body>
EOF
cat > "$tmp/plain.kama" <<'EOF'
import { core::println };
fn uint8 main() { println(s: "plain"); return 5; }
EOF
proj plain ', "shellFile": "shell.html"'
run plain
grep -qx "shell: 1 line(s), status 5, isolated=false" "$tmp/plain.err" \
    || { sed 's/^/  /' "$tmp/plain.err" >&2; fail "a shellFile page's own print and onExit did not both run, or an unthreaded page was isolated"; }
[ "$st" = 5 ] || fail "a shellFile project ran to status $st, not 5"
left "the shellFile run" plain

# Shared memory: a thread form makes the build threaded, and a manifest can share memory without one.
cat > "$tmp/threads.kama" <<'EOF'
import { core::println, std::collections::DynamicArray };
fn uint8 main() {
    DynamicArray<int32> p = DynamicArray.empty();
    p.add(item: 1); p.add(item: 2); p.add(item: 3);
    parallel_for (ref int32 e in p, chunks: 2) { e = e * 2; }
    println(s: "threads");
    return cast<uint8>(p[2]);
}
EOF
proj threads ', "shellFile": "shell.html"'
run threads
grep -qx "shell: 1 line(s), status 6, isolated=true" "$tmp/threads.err" \
    || { sed 's/^/  /' "$tmp/threads.err" >&2; fail "a threaded build was not served cross-origin isolated (status $st)"; }
left "the threaded run" threads
cat > "$tmp/shared.kama" <<'EOF'
import { core::println };
fn uint8 main() { println(s: "shared"); return 4; }
EOF
proj shared ', "shellFile": "shell.html", "emSettings": { "SHARED_MEMORY": true }'
run shared
grep -qx "shell: 1 line(s), status 4, isolated=true" "$tmp/shared.err" \
    || { sed 's/^/  /' "$tmp/shared.err" >&2; fail "a build sharing its memory through emSettings was not served isolated (status $st)"; }
left "the emSettings-shared run" shared

# An abort: emscripten's line, then a native abort's status.
cat > "$tmp/trap.kama" <<'EOF'
fn uint8 main() {
    InlineArray<int32>#(2) a = [0; 2];
    int32 i = 2;
    return cast<uint8>(a[i]);
}
EOF
proj trap
run trap
[ "$st" = 134 ] || { sed 's/^/  /' "$tmp/trap.err" >&2; fail "a trapping program ran to status $st, not 134"; }
grep -q "^Aborted(" "$tmp/trap.err" || fail "an abort's status arrived without emscripten's Aborted(…) line before it"
left "the aborting run" trap

# A run ended by a signal ends its browser. On POSIX kama has become the runner, which exits 143 and removes its
# directory; on Windows a TerminateProcess leaves the runner no turn, and the job kama holds them in ends them all.
cat > "$tmp/forever.kama" <<'EOF'
import { core::println, std::app::App, std::app::Lifecycle, std::app::run };
type resource Forever implements App {
    public ctor make() { }
    public fn bool tick() { return true; }
    public fn void lifecycle(Lifecycle event) { }
}
fn uint8 main() { println(s: "forever"); Owned<App> app = new Forever.make(); run(app: give app); return 0; }
EOF
proj forever
"$KAMA" run "$tmp/forever/kama.json" --target WASM --headless >"$tmp/forever.out" 2>"$tmp/forever.err" &
pid=$!
i=0
until grep -q forever "$tmp/forever.out" 2>/dev/null; do
    i=$((i + 1)); [ "$i" -lt 600 ] || { kill -9 "$pid" 2>/dev/null; fail "the never-ending program printed nothing in 60 s"; }
    sleep 0.1
done
kill -TERM "$pid"
st=0; wait "$pid" || st=$?
sleep 2
s=$(survivors); [ "$s" = 0 ] || fail "a run ended by SIGTERM left $s process(es) running"
if [ "$windows" = 0 ]; then
    [ "$st" = 143 ] || fail "a run ended by SIGTERM exited $st, not 143"
    [ -z "$(ls -A "$tmp/runs")" ] || fail "a run ended by SIGTERM left its run directory behind"
fi
rm -rf "$tmp/runs"/*

# The refusals, before anything is built.
st=0; KAMA_BROWSER="$tmp/no-such-browser" "$KAMA" run "$tmp/lines/kama.json" --target WASM 2>"$tmp/r1.err" >/dev/null || st=$?
[ "$st" = 2 ] && grep -q 'KAMA_BROWSER' "$tmp/r1.err" || fail "a \$KAMA_BROWSER naming nothing was not refused by name (status $st)"
printf 'fn uint8 main() { return 0; }\n' > "$tmp/native.kama"
st=0; "$KAMA" run "$tmp/native.kama" --headless 2>"$tmp/r2.err" >/dev/null || st=$?
[ "$st" = 2 ] && grep -q -- '--headless' "$tmp/r2.err" || fail "--headless on a native run was not refused (status $st)"

echo "check-wasm-run: OK (lines, args and status through $browsers; a shellFile keeps its hooks; isolation only for shared memory; abort 134; nothing left; SIGTERM; refusals)"
