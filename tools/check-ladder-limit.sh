#!/bin/sh
# check-ladder-limit.sh — an `else if` ladder of 10,000 arms builds and runs, and one of 10,001 is refused (KR-307).
#
# A ladder is a chain: each `else if` is a link, held to the 10,000 SPEC states for every chain, and the parser's stack
# stays flat. Past 64 arms the emitter writes one C `if` an arm, each taken arm jumping past the rest, because the nested
# C it writes for a short ladder segfaults clang 21, clang 23 and emcc somewhere between 10,000 and 20,000 arms (emcc's
# flat `else if` at 5,000), and what builds must not depend on which C compiler is installed. Generated here rather than
# checked in, since 10,000 arms is about 250 KB of source. Most arms return and some assign: a check that asks at every
# arm whether the rest of the ladder leaves is quadratic only when the arms do leave (10,000 such arms once took 15 s).
#
# check-legs: native wasm
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-ladder-limit: FAIL — $1" >&2; sed -n '1,20p' "$tmp/log" >&2; exit 1; }

# ladder <arms> <file>: arm k answers k (every seventh by assigning, the rest by returning), and main returns 42 when the
# last arm and the final `else` each answer what they should.
ladder() {
    awk -v n="$1" 'BEGIN {
        print "fn int32 pick(int32 a) {"; print "    int32 r = -1;";
        for (k = 0; k < n; k++)
            printf "    %sif (a == %d) { %s }\n", (k ? "else " : ""), k, (k % 7 ? "return " k ";" : "r = " k ";");
        print "    else { r = -2; }"; print "    return r;"; print "}";
        printf "fn uint8 main() { return pick(a: %d) == %d && pick(a: %d) == -2 ? 42 : 1; }\n", n - 1, n - 1, n;
    }' > "$2"
}
ladder 10000 "$tmp/limit.kama"
ladder 10001 "$tmp/over.kama"

if "$KAMA" check "$tmp/over.kama" > "$tmp/log" 2>&1; then fail "a ladder of 10,001 arms was accepted"; fi
grep -q 'chains more than 10,000' "$tmp/log" || fail "a ladder of 10,001 arms was refused, but not by the chain limit"

"$KAMA" build "$tmp/limit.kama" -o "$tmp/native" > "$tmp/log" 2>&1 || fail "a ladder of 10,000 arms does not build"
st=0; "$tmp/native" > "$tmp/log" 2>&1 || st=$?
[ "$st" = 42 ] || fail "a ladder of 10,000 arms answered $st, not 42"
said="native"

if command -v "${EMCC:-emcc}" >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
    "$KAMA" build "$tmp/limit.kama" --target wasm --cc "${EMCC:-emcc}" -o "$tmp/wasm.js" > "$tmp/log" 2>&1 \
        || fail "a ladder of 10,000 arms does not build for wasm"
    st=0; node "$tmp/wasm.js" > "$tmp/log" 2>&1 || st=$?
    [ "$st" = 42 ] || fail "a ladder of 10,000 arms answered $st on wasm, not 42"
    said="native and wasm"
fi
echo "check-ladder-limit: PASS (10,000 arms build and run, $said; 10,001 are refused by the chain limit)"
