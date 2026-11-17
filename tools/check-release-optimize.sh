#!/bin/sh
# check-release-optimize.sh — `"release": { "optimize": …, "speed": […], "size": […] }`, the level a release build
# compiles and links at, and the packages or modules at the other one (KR-332); and the refusal of an `-O` in any flag key.
#
# Until 0.9.965 a wasm release was always -Oz, which took 2.9x as long as -O3 on std's inflate and 11x on an n-body step,
# and nothing picked speed: a `cflags` -O sat before kama's level and never applied, while a wasm `ldflags` -O rode the
# one emcc call and set the whole program's level, from any package in the graph. This reads the assembled command lines
# (`--cc "echo CC:"`, check-buildsettings.sh's instrument) for:
#   - the default, -O3 natively and on wasm;
#   - "size" from the root, and from a target entry for that target alone;
#   - a package named under `speed` compiled on its own line at -O3 inside a size build, natively and on wasm;
#   - a debug build lifting a `debug.optimize` package to the level a release gives it, -g kept;
#   - a dependency's own `release` ignored;
#   - the refusals: an entry naming nothing, an entry under both lists, a bad level, a target's `speed`, and a
#     dependency's `-O` in `cflags`;
#   - Closure (KR-289): on a wasm release's link by default, never in a debug or native build, and off with
#     `"closure": false` from the root or from the WASM target.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-release-optimize: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-release-optimize: FAIL — $1" >&2; exit 1; }

mkdir -p "$tmp/dep/src" "$tmp/app/src"
cat > "$tmp/dep/kama.json" <<'JSON'
{ "name": "dep", "version": "0.1.0", "kind": "library", "release": { "optimize": "size" },
  "modules": { ".": { "visibility": "public" } } }
JSON
printf 'export { twice };\nfn int32 twice(int32 x) { return x * 2; }\n' > "$tmp/dep/src/dep.kama"
printf 'import { dep::twice };\nfn uint8 main() { return cast<uint8>(twice(x: 21)); }\n' > "$tmp/app/src/main.kama"
# app <extra manifest keys> — the consumer, with whatever this case says about `release`, `debug` or `select`
app() {
    printf '{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", %s\n' "$1" > "$tmp/app/kama.json"
    printf '  "modules": { ".": { "visibility": "internal" } }, "dependencies": { "dep": { "path": "../dep" } } }\n' >> "$tmp/app/kama.json"
}
# lines <out> <kama build args…>: every compile and link line the build assembles, one per line
lines() {
    out=$1; shift
    "$KAMA" build "$tmp/app/kama.json" --no-cache --cc "echo CC:" -o "$tmp/out/$out" "$@" > "$tmp/$out.log" 2>&1 \
        || { sed 's/^/  /' "$tmp/$out.log" >&2; fail "the build for '$out' failed"; }
    grep '^CC:' "$tmp/$out.log" || true
}
# refused <want> <kama build args…>: the build must fail, saying <want>
refused() {
    want=$1; shift
    if "$KAMA" build "$tmp/app/kama.json" --cc "echo CC:" -o "$tmp/out/refused" "$@" > "$tmp/r.log" 2>&1; then
        fail "accepted, where it should say: $want"
    fi
    grep -qF -- "$want" "$tmp/r.log" || { sed 's/^/  /' "$tmp/r.log" >&2; fail "the refusal does not say: $want"; }
}
level_only() {   # level_only <lines file> <want> <case>: every line carries <want> and no other -O level
    grep -q -- " $2 " "$1" || { sed 's/^/  /' "$1" >&2; fail "$3: no line at $2"; }
    if grep -E -- ' -O([0-3sz]) ' "$1" | grep -v -- " $2 " >/dev/null; then sed 's/^/  /' "$1" >&2; fail "$3: a line not at $2"; fi
}

app ''
"$KAMA" pkg install "$tmp/app/kama.json" > "$tmp/i.log" 2>&1 || { cat "$tmp/i.log" >&2; fail "the project did not install"; }

# 1. The default: -O3 on every target, and a dependency's own `release` ("size") changes nothing.
lines n1 --release > "$tmp/n1"; level_only "$tmp/n1" -O3 "a native release by default"
lines w1 --release --target wasm > "$tmp/w1"; level_only "$tmp/w1" -O3 "a wasm release by default"

# 2. The root's "size", natively and on wasm, its link included.
app '"release": { "optimize": "size" },'
lines n2 --release > "$tmp/n2"; level_only "$tmp/n2" -Oz "a native release at size"
lines w2 --release --target wasm > "$tmp/w2"; level_only "$tmp/w2" -Oz "a wasm release at size"

# 3. A target's "size" holds for that target alone.
app '"select": { "TARGET": { "WASM": { "release": { "optimize": "size" } } } },'
lines w3 --release --target wasm > "$tmp/w3"; level_only "$tmp/w3" -Oz "the WASM target's own size"
lines n3 --release > "$tmp/n3"; level_only "$tmp/n3" -O3 "a native release beside a WASM target at size"

# 4. A package under `speed` inside a size build. A release build is ONE translation unit, for the inlining across
#    modules it exists for, so the unit compiles at -O3, every unit at size has its functions marked `minsize` (what
#    -Oz is to clang), and the link stays at -Oz. Read off the kept C: `dep`'s `twice` is defined outside every
#    `minsize` region, the program's `main` inside one.
app '"release": { "optimize": "size", "speed": ["dep"] },'
for leg in n4 w4; do
    if [ $leg = w4 ]; then lines $leg --release --target wasm --keep-c > "$tmp/$leg"; else lines $leg --release --keep-c > "$tmp/$leg"; fi
    # The compile is the line with `-c`: a path is quoted on Windows and bare elsewhere, so it cannot be told by its `.c"`.
    grep -- ' -c ' "$tmp/$leg" | grep -q -- ' -O3 ' || { sed 's/^/  /' "$tmp/$leg" >&2; fail "$leg: the mixed unit was not compiled at -O3"; }
    grep -- ' -c ' "$tmp/$leg" | grep -q -- ' -Oz ' && fail "$leg: the mixed unit's compile also carries -Oz"
    grep -v -- ' -c ' "$tmp/$leg" | grep -q -- ' -Oz ' || { sed 's/^/  /' "$tmp/$leg" >&2; fail "$leg: the link left the build's level, -Oz"; }
    c=$(ls "$tmp/out/$leg"*.c 2>/dev/null | head -1)
    [ -n "$c" ] || fail "$leg: --keep-c kept no C"
    where=$(awk '/#pragma clang attribute push/ {in_=1} /#pragma clang attribute pop/ {in_=0}
                 /^int32_t dep__twice\(int32_t k_x\)$/ {print "twice=" in_+0}
                 /^[a-z0-9_]+ kama_main\(/ {print "main=" in_+0}' "$c" | sort -u | tr '\n' ' ')
    case "$where" in *"twice=0"*) ;; *) fail "$leg: \`dep\`'s twice, under release.speed, is not outside the minsize regions ($where)";; esac
    case "$where" in *"main=1"*) ;; *) fail "$leg: the program's main is not inside a minsize region ($where)";; esac
done

# 5. A debug build lifts a `debug.optimize` package to the level a release would give it, `-g` kept.
app '"debug": { "optimize": ["dep"] }, "release": { "size": ["dep"] },'
lines d5 > "$tmp/d5"
grep 'dep__dep\.c' "$tmp/d5" | grep -q -- '-g -Oz ' || { sed 's/^/  /' "$tmp/d5" >&2; fail "debug.optimize did not lift \`dep\` to its release level, -Oz, with -g"; }
grep 'app__main\.c' "$tmp/d5" | grep -q -- '-g -O0 ' || fail "the program's own unit left -O0 in a debug build"

# 6. The refusals.
app '"release": { "speed": ["nope"] },'
refused 'names `nope`, which is no package or module of this build' --release
app '"release": { "speed": ["dep"], "size": ["dep"] },'
refused 'names `dep` under both `speed` and `size`' --release
app '"release": { "optimize": "fast" },'
refused '`release.optimize` is "speed" (-O3, the default) or "size" (-Oz), not "fast"' --release
app '"select": { "TARGET": { "WASM": { "release": { "speed": ["dep"] } } } },'
refused "unknown key \`speed\` in a target's \`release\`" --release
app ''
cp "$tmp/dep/kama.json" "$tmp/dep.json"
sed 's/"release": { "optimize": "size" },/"cflags": ["-O2"],/' "$tmp/dep.json" > "$tmp/dep/kama.json"
# The build reads a dependency through the view `pkg install` makes, so the edited manifest is installed first.
if "$KAMA" pkg install "$tmp/app/kama.json" > "$tmp/r.log" 2>&1; then
    refused '`cflags` entry `-O2` sets the optimization level, which no flag sets' --release
else
    grep -qF '`cflags` entry `-O2` sets the optimization level, which no flag sets' "$tmp/r.log" \
        || { sed 's/^/  /' "$tmp/r.log" >&2; fail "a dependency's -O2 was not refused"; }
fi
cp "$tmp/dep.json" "$tmp/dep/kama.json"
"$KAMA" pkg install "$tmp/app/kama.json" > "$tmp/i.log" 2>&1 || { cat "$tmp/i.log" >&2; fail "the project did not reinstall"; }

# 7. Closure (KR-289): a wasm release runs it (case 1's lines), a native release and a wasm debug build never do, and
#    `"closure": false` turns it off from the root, or from the WASM target for that target.
grep -q -- '--closure=1' "$tmp/w1" || { sed 's/^/  /' "$tmp/w1" >&2; fail "a wasm release does not run Closure"; }
grep -q -- '--closure' "$tmp/n1" && fail "a native release passes --closure"
app ''
lines w7d --target wasm > "$tmp/w7d"
grep -q -- '--closure' "$tmp/w7d" && fail "a wasm debug build runs Closure"
app '"release": { "closure": false },'
lines w7 --release --target wasm > "$tmp/w7"
grep -q -- '--closure' "$tmp/w7" && fail "the root's \"closure\": false still runs Closure"
app '"select": { "TARGET": { "WASM": { "release": { "closure": false } } } },'
lines w7t --release --target wasm > "$tmp/w7t"
grep -q -- '--closure' "$tmp/w7t" && fail "the WASM target's \"closure\": false still runs Closure"

echo "check-release-optimize: PASS (-O3 on every target by default; \"size\" from the root and from a target; a \`speed\` package at -O3 inside a size build, natively and on wasm; debug.optimize at the release level; a dependency's \`release\` ignored; five refusals; Closure on a wasm release alone, off from the root and from a target)"
