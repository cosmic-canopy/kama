#!/usr/bin/env bash
# run.sh — the measurements behind `release` (docs/targets.md; KR-332, 0.9.965): what a wasm release's optimization level
# costs in size and buys in speed. Builds sim.kama and png.kama (and examples/webgpu, for size) for wasm at -Oz, -Os,
# -O2 and -O3, and "mixed": the code at -O3 and the link at -Oz, which is what `release.speed` gives a package in a size
# build. Every level is compiled and linked by hand from the default build's `kama build --cc echo` line, since a
# manifest names only "speed" and "size"; the browsers go through `kama run`, so they time those two.
#
# Usage: bench/wasm-opt/run.sh <a large truecolour PNG> <out-dir> [<browser>...]
#   needs emcc and node on PATH (emsdk_env.sh). Each <browser> (a path) also times `release.optimize` "size" (-Oz) against
#   "speed" (-O3) through `kama run --headless`. Writes <out-dir>/sizes.tsv (brotli at quality 11; libc_optz is 1 when the
#   link pulled emscripten's size libc) and <out-dir>/times.tsv (each line is the program's own "best of N: <us> us").
#   Time on a quiet machine.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$here/../.." && pwd)
. "$ROOT/tools/kama-bin.sh"
png=${1:?usage: run.sh <png> <out-dir> [browser...]}; out=${2:?usage: run.sh <png> <out-dir> [browser...]}; shift 2
browsers=("$@")
mkdir -p "$out"; out=$(CDPATH= cd -- "$out" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
# msys2 rewrites an argument that looks like a POSIX path for a native program; `/image.png` names a file inside the page.
raw() { MSYS2_ARG_CONV_EXCL='*' "$@"; }
win() { case $(uname -s) in MINGW*|MSYS*|CYGWIN*) cygpath -m "$1";; *) printf '%s' "$1";; esac; }

# project <dir> <program> [embed] [optimize]: the program's manifest, the image embedded for a timing build
project() {
    mkdir -p "$1/src"; cp "$here/$2.kama" "$1/src/main.kama"
    embed=""; [ "${3:-}" = embed ] && { cp "$png" "$1/image.png"; embed='"embedFiles": ["image.png@/image.png"],'; }
    grow=""; [ "$2" = png ] && grow='"emSettings": { "ALLOW_MEMORY_GROWTH": 1 },'   # a 2048x2048 image outgrows the default
    opt=""; [ -n "${4:-}" ] && opt="\"release\": { \"optimize\": \"$4\" },"
    printf '{ "name": "%s", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", %s %s %s\n  "modules": { ".": { "visibility": "internal" } } }\n' \
        "$2" "$embed" "$grow" "$opt" > "$1/kama.json"
}
# A stand-in compiler: it answers kama's questions about itself (`-v`) as emcc does, so kama gives it emcc's flags
# (`--cc echo` cannot be asked, and gets none of clang's), and prints each command, shell-quoted, instead of running it.
cat > "$tmp/spy" <<'SH'
#!/usr/bin/env bash
for a in "$@"; do case "$a" in -v|--version|-dump*|-print*) exec emcc "$@";; esac; done
printf 'CC: '; printf '%q ' "$@"; echo
SH
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) SPY="bash $(kama_native_path "$tmp")/spy";; *) chmod +x "$tmp/spy"; SPY="$tmp/spy";; esac
# levels <project dir> <level>...: the default build's one emcc line, compiled and linked by hand at each level into
# <dir>-<level>/out. A link-only argument (a setting, a file to embed, a library) goes to the link, a port to both.
levels() {
    v=$1; shift
    line=$(cd "$v" && "$KAMA" build kama.json --release --target wasm --keep-c --cc "$SPY" -o "$v/c/main.js" \
           | grep '^CC: ' | grep -- '-std=c11' | sed 's/^CC: //')
    set -- "$@" "--"
    eval "set -- \"\$@\" $line"
    lv=(); while [ "$1" != "--" ]; do lv+=("$1"); shift; done; shift
    comp=(); link=(); cfile=""; next=""
    for a in "$@"; do
        if [ -n "$next" ]; then [ "$next" = o ] || link+=("$a"); next=""; continue; fi
        case "$a" in
            -o) next=o;;
            --embed-file|--preload-file|--js-library|--pre-js|--post-js|--shell-file) link+=("$a"); next=link;;
            *.c) cfile=$a;;
            --use-port=*) comp+=("$a"); link+=("$a");;
            -s*|*@/*) link+=("$a");;
            -O[0-3sz]) ;;
            *) comp+=("$a");;
        esac
    done
    for l in "${lv[@]}"; do
        cl=-$l; ll=-$l; [ "$l" = mixed ] && { cl=-O3; ll=-Oz; }
        m="$v-$l"; mkdir -p "$m/out"
        # in the project's directory: the line's `-I.` and relative files are relative to it
        (cd "$v" && emcc "${comp[@]}" $cl -c "$cfile" -o "$m/main.o" && emcc "$m/main.o" "${link[@]}" $ll -v -o "$m/out/main.js" 2> "$m/link.log") \
            || { tail -20 "$m/link.log" >&2; exit 1; }
    done
}
sizes() {   # sizes <program> <level> <dir with out/main.wasm, out/main.js and link.log>
    node -e '
        const fs = require("fs"), z = require("zlib"); const r = [];
        for (const f of process.argv.slice(1)) { const b = fs.readFileSync(f);
            r.push(b.length, z.brotliCompressSync(b, { params: { [z.constants.BROTLI_PARAM_QUALITY]: 11 } }).length); }
        console.log(r.join("\t"));' "$(win "$3/out/main.wasm")" "$(win "$3/out/main.js")" \
    | sed "s/^/$1\t$2\t$(grep -c -- '-lc_optz' "$3/link.log" || true)\t/" >> "$out/sizes.tsv"
}
printf 'program\tlevel\tlibc_optz\twasm\twasm_brotli\tjs\tjs_brotli\n' > "$out/sizes.tsv"
printf 'where\tround\tprogram\tlevel\tline\n' > "$out/times.tsv"

all="Oz Os O2 O3 mixed"
project "$tmp/sim" sim;            levels "$tmp/sim" $all
project "$tmp/png" png embed;      levels "$tmp/png" $all
project "$tmp/png-code" png;       levels "$tmp/png-code" $all    # sizes without the image
for l in $all; do sizes sim $l "$tmp/sim-$l"; done
for l in $all; do sizes png $l "$tmp/png-code-$l"; done
# examples/webgpu: size only, as a page with little code of its own (a triangle has no loop to time)
mkdir -p "$tmp/webgpu"; cp -r "$ROOT/examples/webgpu/kama.json" "$ROOT/examples/webgpu/src" "$ROOT/examples/webgpu/csrc" "$tmp/webgpu/"
levels "$tmp/webgpu" Oz Os O2 O3
for l in Oz Os O2 O3; do sizes webgpu $l "$tmp/webgpu-$l"; done

run() {     # run <where> <round> <program> <level> <dir> <how...>: one timed line per mode
    case $3 in sim) modes="nbody:1000000 libm:2000000";; png) modes="decode:/image.png inflate:/image.png";; esac
    for mo in $modes; do
        line=$(cd "$5" && raw "${@:6}" "${mo%%:*}" "${mo#*:}" 10 2>&1 | grep -E 'best of|rror' | tail -1)
        printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$line" >> "$out/times.tsv"
    done
}
order() { case $1 in 1) echo Oz Os O2 O3 mixed;; 2) echo mixed O3 O2 Os Oz;; *) echo Os mixed O3 Oz O2;; esac; }
for r in 1 2 3; do for l in $(order $r); do for p in sim png; do
    run node $r $p $l "$tmp/$p-$l/out" node main.js
done; done; done
for p in sim png; do for o in size speed; do
    project "$tmp/$p-$o" $p $([ $p = png ] && echo embed || echo none) $o
done; done
for b in "${browsers[@]}"; do for r in 1 2; do for o in $([ $r = 1 ] && echo size speed || echo speed size); do for p in sim png; do
    run "$(basename "$b")" $r $p $([ $o = size ] && echo Oz || echo O3) "$tmp/$p-$o" \
        env KAMA_BROWSER="$b" timeout 300 "$KAMA" run kama.json --target WASM --release --headless --
done; done; done; done
echo "run.sh: wrote $out/sizes.tsv and $out/times.tsv"
