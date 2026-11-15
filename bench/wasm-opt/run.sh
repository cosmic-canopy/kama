#!/usr/bin/env bash
# run.sh — KR-332's measurements (ROADMAP_DETAIL `s5-wasm-opt`): what a wasm release's optimization level costs in size and buys
# in speed. Builds sim.kama and png.kama (and examples/webgpu, for size) for wasm at -Oz, -Os, -O2 and -O3, and "mixed":
# the code compiled at -O3 and linked at -Oz, a fast package in a size-first page, assembled by hand from
# `kama build --cc echo` since no manifest can ask for it.
# The level is set the one way a manifest can today: a gated `ldflags` entry, which lands after kama's -Oz on the single emcc
# call that compiles kama's C and links it.
#
# Usage: bench/wasm-opt/run.sh <a large truecolour PNG> <out-dir> [<browser>...]
#   needs emcc and node on PATH (emsdk_env.sh). Each <browser> (a path) also times every level through `kama run --headless`.
#   Writes <out-dir>/sizes.tsv (brotli at quality 11; libc_optz is 1 when the link pulled emscripten's size libc) and
#   <out-dir>/times.tsv (each line is the program's own "best of N: <us> us"). Time on a quiet machine.
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

# project <dir> <program> <level> [embed]: a manifest at <level>, the image embedded for a timing build
project() {
    mkdir -p "$1/src"; cp "$here/$2.kama" "$1/src/main.kama"
    flags=""; [ "$3" != Oz ] && flags="\"ldflags\": [{ \"flag\": \"-$3\", \"compileFor\": [\"ARCH_WASM32\"] }],"
    embed=""; [ "${4:-}" = embed ] && { cp "$png" "$1/image.png"; embed='"embedFiles": ["image.png@/image.png"],'; }
    grow=""; [ "$2" = png ] && grow='"emSettings": { "ALLOW_MEMORY_GROWTH": 1 },'   # a 2048x2048 image outgrows the default
    printf '{ "name": "%s", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", %s %s %s\n  "modules": { ".": { "visibility": "internal" } } }\n' \
        "$2" "$flags" "$embed" "$grow" > "$1/kama.json"
}
sizes() {   # sizes <program> <level> <dir with out/main.wasm, out/main.js> <link log>
    node -e '
        const fs = require("fs"), z = require("zlib"); const r = [];
        for (const f of process.argv.slice(1)) { const b = fs.readFileSync(f);
            r.push(b.length, z.brotliCompressSync(b, { params: { [z.constants.BROTLI_PARAM_QUALITY]: 11 } }).length); }
        console.log(r.join("\t"));' "$(win "$3/out/main.wasm")" "$(win "$3/out/main.js")" \
    | sed "s/^/$1\t$2\t$(grep -c -- '-lc_optz' "$4" || true)\t/" >> "$out/sizes.tsv"
}
printf 'program\tlevel\tlibc_optz\twasm\twasm_brotli\tjs\tjs_brotli\n' > "$out/sizes.tsv"
printf 'where\tround\tprogram\tlevel\tline\n' > "$out/times.tsv"

levels="Oz Os O2 O3"
for p in sim png; do for l in $levels; do
    d="$tmp/$p-$l"; project "$d" $p $l $([ $p = png ] && echo embed)
    (cd "$d" && EMCC_CFLAGS=-v "$KAMA" build kama.json --release --target wasm -o "$d/out/main.js" > "$d/build.log" 2>&1) \
        || { tail -20 "$d/build.log" >&2; exit 1; }
    c="$tmp/$p-$l-code"                      # sizes without the image
    if [ $p = png ]; then project "$c" $p $l
        (cd "$c" && EMCC_CFLAGS=-v "$KAMA" build kama.json --release --target wasm -o "$c/out/main.js" > "$c/build.log" 2>&1)
    else c=$d; fi
    sizes $p $l "$c" "$c/build.log"
done
    # mixed: kama's one emcc line, split into a compile at -O3 and a link at -Oz
    for v in "$tmp/$p-Oz" "$tmp/$p-Oz-code"; do [ -d "$v" ] || continue
        m="$v-mixed"; mkdir -p "$m/out"
        line=$(cd "$v" && "$KAMA" build kama.json --release --target wasm --keep-c --cc echo -o "$m/main.js" | grep -- '-std=c11')
        eval "set -- $line"; comp=(); link=(); cfile=""; skip=0
        for a in "$@"; do
            if [ $skip = 1 ]; then skip=0; continue; fi
            case "$a" in -o) skip=1;; *.c) cfile=$a;; -s*|--embed-file|*@/*) link+=("$a");; -Oz) comp+=(-O3);; *) comp+=("$a");; esac
        done
        # in the project's directory: the line's `-I.` and `--embed-file` are relative to it
        (cd "$v" && emcc "${comp[@]}" -c "$cfile" -o "$m/main.o" && emcc "$m/main.o" "${link[@]}" -Oz -v -o "$m/out/main.js" 2> "$m/link.log")
    done
    sizes $p mixed "$tmp/$p-Oz$([ $p = png ] && echo -code)-mixed" "$tmp/$p-Oz$([ $p = png ] && echo -code)-mixed/link.log"
done

# examples/webgpu: size only, as a page with little code of its own (a triangle has no loop to time)
for l in $levels; do
    w="$tmp/webgpu-$l"; mkdir -p "$w"; cp -r "$ROOT/examples/webgpu/kama.json" "$ROOT/examples/webgpu/src" "$ROOT/examples/webgpu/csrc" "$w/"
    [ $l != Oz ] && node -e '
        const fs = require("fs"), f = process.argv[1], j = JSON.parse(fs.readFileSync(f, "utf8"));
        (j.ldflags = j.ldflags || []).push({ flag: process.argv[2], compileFor: ["ARCH_WASM32"] });
        fs.writeFileSync(f, JSON.stringify(j));' "$(win "$w/kama.json")" "-$l"
    (cd "$w" && EMCC_CFLAGS=-v "$KAMA" build kama.json --release --target wasm -o "$w/out/main.js" > "$w/build.log" 2>&1) \
        || { tail -20 "$w/build.log" >&2; exit 1; }
    sizes webgpu $l "$w" "$w/build.log"
done

run() {     # run <where> <round> <program> <level> <dir> <how...>: one timed line per mode
    case $3 in sim) modes="nbody:1000000 libm:2000000";; png) modes="decode:/image.png inflate:/image.png";; esac
    for mo in $modes; do
        line=$(cd "$5" && raw "${@:6}" "${mo%%:*}" "${mo#*:}" 10 2>&1 | grep -E 'best of|rror' | tail -1)
        printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$line" >> "$out/times.tsv"
    done
}
order() { case $1 in 1) echo Oz Os O2 O3 mixed;; 2) echo mixed O3 O2 Os Oz;; *) echo Os mixed O3 Oz O2;; esac; }
for r in 1 2 3; do for l in $(order $r); do for p in sim png; do
    d="$tmp/$p-$l"; [ $l = mixed ] && d="$tmp/$p-Oz-mixed"
    run node $r $p $l "$d/out" node main.js
done; done; done
for b in "${browsers[@]}"; do for r in 1 2; do for l in $([ $r = 1 ] && echo $levels || echo O3 O2 Os Oz); do for p in sim png; do
    run "$(basename "$b")" $r $p $l "$tmp/$p-$l" env KAMA_BROWSER="$b" timeout 300 "$KAMA" run kama.json --target WASM --release --headless --
done; done; done; done
echo "run.sh: wrote $out/sizes.tsv and $out/times.tsv"
