#!/bin/sh
# check-release-line.sh — an error only the C compiler finds names the kama line in a release build too (KR-256).
#
# Some errors are the C compiler's to find, because only it reads the header: an `extern const` or an extern enum
# constant the header does not define, an extern type's layout. kama writes `#line` so they name the kama line. Until
# `--release` kept `#line` it emitted none, and the same program's error read `main.c:679` there and `main.kama:7` in
# the debug build. The fixture suite builds debug only, so it could not see it. `#line` changes no byte of a release
# binary (measured: no `-g`, and nothing reads `__FILE__`), so release keeps it.
#
# Each xfail below must report, built with `--release`, the position tests/xfail/DIAGNOSTIC_LINES records for its
# debug build. A two-file project checks the release fold, where every module lands in one translation unit and each
# one's `#line` must still name its own file.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-release-line: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# expect <case> <program> <position the release build's error must name>
expect() {
    if "$KAMA" build "$2" --release -o "$tmp/$1/prog" >"$tmp/$1.log" 2>&1; then
        echo "check-release-line: FAIL — $1: the release build succeeded; it must be refused" >&2; fail=1; return
    fi
    if ! grep -qF "$3: error" "$tmp/$1.log"; then
        echo "check-release-line: FAIL — $1: no error names $3:" >&2; sed 's/^/  /' "$tmp/$1.log" >&2; fail=1
    fi
}

for n in extern_const_missing extern_enum_missing_constant; do
    mkdir -p "$tmp/$n"
    at=$(awk -v n="$n" '$1 == n { print $2 }' "$ROOT/tests/xfail/DIAGNOSTIC_LINES")
    [ -n "$at" ] || { echo "check-release-line: FAIL — $n has no DIAGNOSTIC_LINES row" >&2; exit 1; }
    expect "$n" "$ROOT/tests/xfail/$n.d/main.kama" "$at"
done

# The release fold: the missing constant is used in the SECOND file.
p="$tmp/two"
mkdir -p "$p/src" "$tmp/fold"
cat > "$p/kama.json" <<'EOF'
{ "name": "two", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { ".": { "visibility": "internal" } } }
EOF
printf '#define KEY_ENTER 13\n' > "$p/src/keys.h"
cat > "$p/src/main.kama" <<'EOF'
import { spacebar };
fn uint8 main() { return spacebar() == 32 ? 0 : 1; }
EOF
cat > "$p/src/keys.kama" <<'EOF'
export { spacebar };
extern "keys.h";
extern const int32 KEY_SPACEBAR;
fn int32 spacebar() {
    return KEY_SPACEBAR;
}
EOF
expect fold "$p/kama.json" "keys.kama:5:19"

[ "$fail" = 0 ] || exit 1
echo "check-release-line: OK (a C compiler error names the kama line in a release build: two xfails and the one-TU fold)"
