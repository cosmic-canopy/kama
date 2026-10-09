#!/bin/sh
# check-debug-optimize.sh — `"debug": { "optimize": [...] }` compiles the named packages at the release level in a
# debug build, and changes nothing else (KR-128).
#
# A debug build compiles at -O0, and a dependency's `cflags` reach the whole program, so before 0.9.564 nothing
# could optimize one package alone: a DSP ran ~95x slower in debug. This builds a project whose manifest names its
# dependency `voice`, through a --cc shim that records every invocation and then runs the real compiler. It checks
# that `voice`'s unit compiles at -O3 while the program's own stays at -O0; that no unit gets -DNDEBUG; that an
# overflow inside the optimized package still TRAPS, since kama's checks are emitted C, not -O; and that an entry
# naming nothing is refused.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-debug-optimize: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-debug-optimize: FAIL — $1" >&2; exit 1; }

mkdir -p "$tmp/voice/src" "$tmp/app/src"
cat > "$tmp/voice/kama.json" <<'JSON'
{ "name": "voice", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$tmp/voice/src/voice.kama" <<'K'
export { mix, wrap };
fn int64 mix(int64 n) { int64 s = 0i64; int64 i = 0i64; while (i < n) { s = s + i * 3i64; i = i + 1i64; } return s; }
fn int32 wrap(int32 x) { return x + 2147483647; }
K
cat > "$tmp/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } },
  "dependencies": { "voice": { "path": "../voice" } },
  "debug": { "optimize": ["voice"] } }
JSON
printf 'import { voice::mix, voice::wrap };\nfn int32 main() { if (mix(n: 10i64) != 135i64) { return 1; } return wrap(x: 1); }\n' \
    > "$tmp/app/src/main.kama"

# The shim answers the driver's `-v` family question the way the real compiler does, records a compile, and runs it.
cat > "$tmp/spy" <<'SH'
#!/bin/sh
case " $* " in *" -v "*) exec ${REALCC:-cc} "$@" ;; esac
echo "$*" >> "$SPYLOG"
exec ${REALCC:-cc} "$@"
SH
chmod +x "$tmp/spy"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) SPY="sh $(kama_native_path "$tmp")/spy" ;;
    *)                    SPY="$tmp/spy" ;;
esac
SPYLOG="$(kama_native_path "$tmp")/spy.log"; export SPYLOG

"$KAMA" pkg install "$tmp/app/kama.json" >"$tmp/i.log" 2>&1 || { cat "$tmp/i.log" >&2; fail "the project did not install"; }
"$KAMA" build "$tmp/app/kama.json" -o "$tmp/out/app" --cc "$SPY" >"$tmp/b.log" 2>&1 \
    || { sed 's/^/  /' "$tmp/b.log" >&2; fail "the debug build failed"; }
grep -q 'voice__voice\.c' "$tmp/spy.log" || fail "no compile of the \`voice\` unit was recorded"
grep 'voice__voice\.c' "$tmp/spy.log" | grep -q -- '-O3' || fail "\`voice\`, named by debug.optimize, was not compiled at -O3"
grep 'voice__voice\.c' "$tmp/spy.log" | grep -q -- ' -g ' || fail "the optimized unit lost -g"
grep 'app__main\.c' "$tmp/spy.log" | grep -q -- '-O0' || fail "the program's own unit was not left at -O0"
grep -q -- '-DNDEBUG' "$tmp/spy.log" && fail "a debug build passed -DNDEBUG — kama's checks would go"
# Under a shell that cannot exec it away (the trailing `:`), so the "Abort trap" job notice is that shell's.
rc=0; sh -c '"$1" >/dev/null 2>"$2"; echo $? > "$3"' _ "$tmp/out/app" "$tmp/run.err" "$tmp/rc" 2>/dev/null; rc=$(cat "$tmp/rc")
[ "$rc" != 0 ] && grep -q 'overflow' "$tmp/run.err" \
    || fail "an overflow inside the optimized package did not trap (rc $rc) — kama's own checks must stay"

sed 's/"optimize": \["voice"\]/"optimize": ["voic"]/' "$tmp/app/kama.json" > "$tmp/app/k2" && mv "$tmp/app/k2" "$tmp/app/kama.json"
if "$KAMA" build "$tmp/app/kama.json" -o "$tmp/out/app2" >"$tmp/e.log" 2>&1; then fail "an entry naming nothing was accepted"; fi
grep -q "names \`voic\`, which is no package or module" "$tmp/e.log" || { sed 's/^/  /' "$tmp/e.log" >&2; fail "the refusal does not name the entry"; }
echo "check-debug-optimize: PASS (a named package compiles at -O3 with -g in a debug build, the rest at -O0, no -DNDEBUG, an overflow still traps, an unknown entry is refused)"
