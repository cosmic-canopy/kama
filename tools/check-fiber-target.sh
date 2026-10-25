#!/bin/sh
# check-fiber-target.sh — `spawn fiber` where kama has no fiber switch is refused with a sentence, never left to the C
# compiler (KR-3). The switch (include/kama_fiber.h) is written for x86-64, arm64, x86 and riscv64, and Windows has
# its own fibers; wasm runs fibers over JSPI, a later step (docs/design/fibers.md decision 4), and bare metal is
# KR-233. No fixture leg sees this: the wasm leg skips every fiber fixture, and none builds for bare metal or for an
# arch without a switch — so the refusals, and the targets that must NOT be refused, are held down here.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-fiber-target: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-fiber-target: FAIL — $1" >&2; [ $# -lt 2 ] || sed 's/^/  /' "$2" >&2; exit 1; }

cat > "$tmp/f.kama" <<'KAMA'
import { std::concurrent::Fiber };
fn void tick(int32 n) { }
fn uint8 main() { scope { spawn fiber tick(n: 1); } return 0; }
KAMA

for t in x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu riscv64gc-unknown-linux-gnu i686-unknown-linux-gnu \
         x86_64-pc-windows-gnu; do
    "$KAMA" check "$tmp/f.kama" --target "$t" >/dev/null 2>"$tmp/ok.err" || fail "--target $t refused a fiber spawn:" "$tmp/ok.err"
done

refused() {   # <target> <the sentence it must say>
    if "$KAMA" check "$tmp/f.kama" --target "$1" >/dev/null 2>"$tmp/t.err"; then fail "--target $1 accepted a fiber spawn"; fi
    grep -qF "$2" "$tmp/t.err" || fail "--target $1 refused it without saying why:" "$tmp/t.err"
}
refused WASM "fibers on wasm run over JSPI"
refused thumbv7em-none-eabihf "fibers on bare metal are not built yet (KR-233)"
refused powerpc64le-unknown-linux-gnu "kama has no fiber switch for \`powerpc64le\`"

echo "check-fiber-target: PASS (five switched targets build a fiber spawn; wasm, bare metal and an unswitched arch refuse it)"
