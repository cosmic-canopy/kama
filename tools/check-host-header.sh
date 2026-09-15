#!/bin/sh
# check-host-header.sh — the header `kama build` writes for a program's `expose fn`s (KR-52).
#
# tests/expose_host_header.d proves the header WORKS: its C includes it and calls through every kind of type.
# A fixture is one build, exit-code-only, so three properties of the header live here instead:
#   1. it is clean C AND clean C++ under -Wall -Wextra -Werror — a host header that warns in the host's
#      build is a header the host stops including, and C++ is where most hosts that embed a kama module live;
#   2. it never overwrites a file kama did not generate. A loose `kama build shim.kama` writes beside the
#      source, so its header would be `shim.h` — the name of the C header such a file `extern`s;
#   3. a build that no longer exposes anything removes the header an earlier build wrote, so a host cannot
#      keep compiling against a prototype whose definition is gone.
#
# Native only: the header is plain C and target-independent, and a wasm host is JavaScript.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"   # sets $KAMA
CC=${HOSTCC:-cc}
CXX=${HOSTCXX:-c++}
if ! command -v "$CC" >/dev/null 2>&1 || ! command -v "$CXX" >/dev/null 2>&1; then
    echo "check-host-header: SKIP (needs $CC and $CXX on PATH)"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-host-header: FAIL — $*" >&2; exit 1; }

# --- 1. clean C and C++ -----------------------------------------------------------------------------------
fixture="$ROOT/tests/expose_host_header.d"
mkdir -p "$tmp/a"
"$KAMA" build "$fixture/kama.json" -o "$tmp/a/x" >"$tmp/a/log" 2>&1 || { cat "$tmp/a/log" >&2; fail "the fixture did not build"; }
[ -f "$tmp/a/hosthdr.h" ] || fail "no hosthdr.h beside the output of a program that exposes functions"
printf '#include "hosthdr.h"\nint main(void) { return 0; }\n' > "$tmp/a/use.c"
"$CC" -std=c11 -Wall -Wextra -Werror -I "$tmp/a" -I "$fixture/csrc" -c "$tmp/a/use.c" -o "$tmp/a/use_c.o" \
    || fail "the generated header is not clean C11 under -Wall -Wextra -Werror"
"$CXX" -x c++ -std=c++17 -Wall -Wextra -Werror -I "$tmp/a" -I "$fixture/csrc" -c "$tmp/a/use.c" -o "$tmp/a/use_cxx.o" \
    || fail "the generated header is not clean C++17 under -Wall -Wextra -Werror"

# --- 2. a file kama did not generate is never overwritten --------------------------------------------------
mkdir -p "$tmp/b"
printf 'int users_own_declaration;\n' > "$tmp/b/eb.h"
if "$KAMA" build "$ROOT/tests/expose_basic.kama" -o "$tmp/b/eb" >"$tmp/b/log" 2>&1; then
    fail "a build overwrote (or ignored) a pre-existing eb.h that kama did not generate"
fi
grep -q "kama did not generate it" "$tmp/b/log" || { cat "$tmp/b/log" >&2; fail "the refusal does not say why"; }
[ "$(cat "$tmp/b/eb.h")" = "int users_own_declaration;" ] || fail "the user's eb.h was modified"

# --- 3. a header for functions no longer exposed is removed -----------------------------------------------
mkdir -p "$tmp/c"
"$KAMA" build "$ROOT/tests/expose_basic.kama" -o "$tmp/c/p" >/dev/null 2>&1 || fail "expose_basic did not build"
[ -f "$tmp/c/p.h" ] || fail "no p.h after building a program that exposes functions"
sed 's/^expose fn/fn/' "$ROOT/tests/expose_basic.kama" > "$tmp/c/plain.kama"
"$KAMA" build "$tmp/c/plain.kama" -o "$tmp/c/p" >/dev/null 2>&1 || fail "the unexposed variant did not build"
[ ! -f "$tmp/c/p.h" ] || fail "p.h survived a build that exposes nothing — a stale prototype a host could still use"

echo "check-host-header: PASS (clean C11 + C++17, a user's file is refused, a stale header is removed)"
