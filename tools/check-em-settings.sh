#!/bin/sh
# check-em-settings.sh — kama's list of the emscripten settings emcc reads at COMPILE time stays emscripten's own.
#
# A `csources` file compiles as a job of its own from the compile flags, so an `emSettings` entry that emcc reads at
# compile time has to be in them (`emCompileTimeSetting` in src/kama.driver.cpp). Up to `0.9.941` none was, and
# `"emSettings": {"WASM_WORKERS": 1}` left a C source's atomic unlinkable. The list is emscripten's, copied: its
# `COMPILE_TIME_SETTINGS` and `PORTS_SETTINGS` in tools/settings.py. A setting a later emsdk adds would otherwise miss
# every `csources` compile in silence, the same defect again, so this reads the installed emscripten's two sets and
# holds kama's to them, both ways.
#
# check-legs: wasm
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail() { echo "check-em-settings: FAIL — $*" >&2; exit 1; }
emcc=$(command -v "${EMCC:-emcc}") || fail "no emcc: this guard belongs to the wasm leg"
settings="$(dirname -- "$emcc")/tools/settings.py"
[ -f "$settings" ] || { [ -n "${EMSDK:-}" ] && settings="$EMSDK/upstream/emscripten/tools/settings.py"; }
[ -f "$settings" ] || fail "no tools/settings.py beside $emcc or under \$EMSDK: cannot read emscripten's own list"

python3 -I - "$settings" "$ROOT/src/kama.driver.cpp" <<'EOF' || exit 1
import ast, re, sys
em = set()
for n in ast.parse(open(sys.argv[1]).read()).body:
    if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
        v, name = n.value, n.targets[0].id
        if name == "PORTS_SETTINGS" and isinstance(v, ast.Set):
            em |= {e.value for e in v.elts}
        if name == "COMPILE_TIME_SETTINGS":     # `{...}.union(PORTS_SETTINGS)`
            s = v.func.value if isinstance(v, ast.Call) else v
            if isinstance(s, ast.Set): em |= {e.value for e in s.elts}
if len(em) < 10:
    sys.exit("check-em-settings: FAIL — read %d settings from %s; its shape changed, so this guard reads nothing" % (len(em), sys.argv[1]))
src = open(sys.argv[2]).read()
m = re.search(r"static bool emCompileTimeSetting\(.*?\n\}", src, re.S)
if not m:
    sys.exit("check-em-settings: FAIL — no `emCompileTimeSetting` in src/kama.driver.cpp")
kama = set(re.findall(r'"([A-Z0-9_]+)"', m.group(0)))
missing, extra = sorted(em - kama), sorted(kama - em)
if missing or extra:
    msg = "check-em-settings: FAIL — kama's compile-time settings differ from %s:" % sys.argv[1]
    if missing: msg += "\n  emscripten reads at compile time, kama passes only to the link: " + ", ".join(missing)
    if extra:   msg += "\n  kama passes to the compile, emscripten does not read there: " + ", ".join(extra)
    sys.exit(msg)
print("check-em-settings: OK (%d settings, the same as %s)" % (len(em), sys.argv[1]))
EOF
