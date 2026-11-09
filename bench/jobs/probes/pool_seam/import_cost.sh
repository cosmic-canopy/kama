#!/bin/sh
# import_cost.sh — what a pool file in `std::concurrent` costs a program that imports only `Atomic` (KR-3 decision 14 asked to gate
# the file if that cost is real). Two copies of the stdlib beside two copies of the compiler, one with a stand-in the pool's size
# (bench/jobs/pool_kama.kama's pool, unreferenced); a type error planted in it shows whether the import compiles it at all; then
# `kama check` and `kama build` timed interleaved. From the repo root: sh bench/jobs/probes/pool_seam/import_cost.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$here/../../../.." && pwd)
. "$ROOT/tools/kama-bin.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
for v in a b; do
    mkdir -p "$tmp/$v/out/x"; cp "$KAMA" "$tmp/$v/out/x/kama"; cp -R "$ROOT/lib" "$tmp/$v/lib"
    ln -s "$ROOT/include" "$tmp/$v/include"; ln -s "$ROOT/prelude" "$tmp/$v/prelude"
done
python3 - "$ROOT/bench/jobs/pool_kama.kama" "$tmp/b/lib/std/concurrent/pool.kama" <<'PY'
import sys
src = open(sys.argv[1]).read()
body = src[src.index("fnptr void Run"):src.index("static int32 leaf = 10;")]
body += src[src.index("type resource WorkerSetup"):src.index("type value RootCtx")]
body = body.replace("count = this.n; leaf = this.cut;", "count = this.n;")
open(sys.argv[2], "w").write("import { std::collections::DynamicArray };\n\n" + body)
PY
printf 'import { std::concurrent::Atomic };\nfn uint8 main() {\n    Atomic<int32> a = Atomic.make(value: 7i32);\n    return cast<uint8>(a.load());\n}\n' >"$tmp/hello.kama"
echo "stand-in: $(wc -l <"$tmp/b/lib/std/concurrent/pool.kama") lines"
cp "$tmp/b/lib/std/concurrent/pool.kama" "$tmp/pool.keep"
echo 'fn int32 broken() { return "x"; }' >>"$tmp/b/lib/std/concurrent/pool.kama"
if "$tmp/b/out/x/kama" build "$tmp/hello.kama" -o "$tmp/hb" >/dev/null 2>&1; then echo "a type error in it: the build passes, so the import does not compile it"
else echo "a type error in it: the build FAILS, so the import compiles it"; fi
cp "$tmp/pool.keep" "$tmp/b/lib/std/concurrent/pool.kama"
python3 - "$tmp" <<'PY'
import subprocess, sys, time, statistics
T = sys.argv[1]
res = {v: {"check": [], "build": []} for v in "ab"}
for r in range(25):
    for v in ("a", "b") if r % 2 == 0 else ("b", "a"):
        k = f"{T}/{v}/out/x/kama"
        for what, args in (("check", [k, "check", f"{T}/hello.kama"]),
                           ("build", [k, "build", f"{T}/hello.kama", "-o", f"{T}/h{v}", "--no-cache"])):
            t0 = time.perf_counter_ns(); subprocess.run(args, capture_output=True, check=True)
            res[v][what].append((time.perf_counter_ns() - t0) / 1e6)
for what in ("check", "build"):
    a, b = res["a"][what], res["b"][what]
    print(f"{what}, 25 interleaved: without the file median {statistics.median(a):.1f} ms (min {min(a):.1f}), "
          f"with it {statistics.median(b):.1f} ms (min {min(b):.1f})")
PY
