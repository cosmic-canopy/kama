#!/usr/bin/env bash
# Smoke-test `kama build --shared` + `expose`: build a native shared library from a kama module
# that has NO `main` (a reload module), then dlopen/dlsym its exported C-ABI symbols from a C host and
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

# A reload-module: exposed entry points, no `main`. A loose file is in no module, so its FILE name
# qualifies each symbol: `add` exports as `mod_add`, and only `@linkName` fixes a symbol verbatim.
cat > "$tmp/mod.kama" <<'KAMA'
expose fn int32 add(int32 a, int32 b) { return a + b; }
expose fn int32 mul(int32 a, int32 b) { return a * b; }
@linkName("renamed") expose fn int32 sub(int32 a, int32 b) { return a - b; }   // exported as `renamed`
KAMA

"$KAMA" build --shared "$tmp/mod.kama" -o "$tmp/libmod.$ext"

# A tiny C host that loads the library and calls the exported symbols.
#
# ⚠️ Windows has no <dlfcn.h> and no -ldl: mingw-w64 ships neither, so the POSIX spelling does not fail
# at load time — it fails to COMPILE, and the guard reads as "the --shared proof did not pass" when what
# it means is "this host was written for one of the two platforms". LoadLibrary/GetProcAddress is the
# same three operations under different names, so the branch is in the host rather than in the script.
cat > "$tmp/host.c" <<'C'
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define OPEN(p)     ((void*)LoadLibraryA(p))
#define SYM(h, n)   ((void*)GetProcAddress((HMODULE)(h), (n)))
#define CLOSE(h)    FreeLibrary((HMODULE)(h))
#define WHY         "GetLastError %lu", (unsigned long)GetLastError()
#else
#include <dlfcn.h>
#define OPEN(p)     dlopen((p), RTLD_NOW)
#define SYM(h, n)   dlsym((h), (n))
#define CLOSE(h)    dlclose(h)
#define WHY         "%s", dlerror()
#endif
typedef int (*binop)(int, int);
int main(void) {
    void* h = OPEN(LIBPATH);
    if (!h) { fprintf(stderr, "load failed: "); fprintf(stderr, WHY); fprintf(stderr, "\n"); return 2; }
    binop add = (binop)SYM(h, "mod_add");
    binop mul = (binop)SYM(h, "mod_mul");
    binop sub = (binop)SYM(h, "renamed");   /* the `@linkName`, not the kama name `sub` */
    if (!add || !mul || !sub) { fprintf(stderr, "symbol lookup failed: "); fprintf(stderr, WHY); fprintf(stderr, "\n"); return 3; }
    if (SYM(h, "sub") || SYM(h, "mod_sub")) { fprintf(stderr, "`sub` is exported under a kama-derived name despite @linkName\n"); return 4; }
    if (SYM(h, "add")) { fprintf(stderr, "`add` is exported BARE — the module (here the file) must qualify it\n"); return 5; }
    int r = add(40, 2) + mul(0, 0) + sub(5, 5);   /* 42 */
    CLOSE(h);
    return r;
}
C

# -ldl is glibc's, and only glibc's: mingw-w64 resolves LoadLibrary out of kernel32 with no extra library.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) dl= ;;
  *) dl=-ldl ;;
esac
"$CC" -DLIBPATH="\"$tmp/libmod.$ext\"" "$tmp/host.c" ${dl:+$dl} -o "$tmp/host"
set +e
"$tmp/host"; got=$?
set -e
if [ "$got" = 42 ]; then
    echo "PASS expose_shared_check (dlsym mod_add/mod_mul/renamed -> $got)"
    exit 0
fi
echo "FAIL expose_shared_check (got $got, expected 42)"
exit 1
