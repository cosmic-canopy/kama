#!/usr/bin/env bash
# Smoke-test `kama build --shared` + `expose`: build a native shared library from a kama module
# that has NO `main` (a reload module), then dlopen/dlsym its bare C-ABI symbol from a C host and
# assert the call returns the expected value. run_tests.sh is exit-code-only, so this lives apart.
#
# Usage: tools/cdev exec tests/support/expose_shared_check.sh
set -euo pipefail

KAMA="${KAMA:-./kama}"
CC="${HOSTCC:-cc}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

case "$(uname -s)" in
  Darwin) ext=dylib ;;
  MINGW*|MSYS*|CYGWIN*) ext=dll ;;
  *) ext=so ;;
esac

# A reload-module: exposed entry points, no `main`.
cat > "$tmp/mod.kama" <<'KAMA'
expose fn int add(int a, int b) { return a + b; }
expose fn int mul(int a, int b) { return a * b; }
KAMA

"$KAMA" build --shared "$tmp/mod.kama" -o "$tmp/libmod.$ext"

# A tiny C host that loads the library and calls the bare symbols.
cat > "$tmp/host.c" <<'C'
#include <dlfcn.h>
#include <stdio.h>
typedef int (*binop)(int, int);
int main(void) {
    void* h = dlopen(LIBPATH, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }
    binop add = (binop)dlsym(h, "add");
    binop mul = (binop)dlsym(h, "mul");
    if (!add || !mul) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 3; }
    int r = add(40, 2) + mul(0, 0);   /* 42 */
    dlclose(h);
    return r;
}
C

"$CC" -DLIBPATH="\"$tmp/libmod.$ext\"" "$tmp/host.c" -ldl -o "$tmp/host"
set +e
"$tmp/host"; got=$?
set -e
if [ "$got" = 42 ]; then
    echo "PASS expose_shared_check (dlsym add/mul -> $got)"
    exit 0
fi
echo "FAIL expose_shared_check (got $got, expected 42)"
exit 1
