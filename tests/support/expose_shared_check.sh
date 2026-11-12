#!/usr/bin/env bash
# Smoke-test `kama build --shared` + `expose`: build a native shared library from a kama module
# that has NO `main` (a reload module), then dlopen/dlsym its exported C-ABI symbols from a C host and
# assert the call returns the expected value. Then the same for `OUTPUT=STATIC`, linked by a C host and by
# a kama program. run_tests.sh is exit-code-only, so this lives apart.
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
if [ "$got" != 42 ]; then
    echo "FAIL expose_shared_check (got $got, expected 42)"
    exit 1
fi

# --- The same, as a STATIC library (KR-381) ---------------------------------------------------------------------
# A C host links the archive and calls through the header kama writes beside it. A function that prints reaches the
# runtime's slots (the panic hook among them), which only the entry TU, the one with `main`, used to define: a
# library with none linked with `kama_panic_hook` and three more undefined. They now travel as an archive member of
# their own, so the host pulls them and a kama program that defines them itself does not.
cat > "$tmp/smod.kama" <<'KAMA'
import { core::println };
expose fn int32 add(int32 a, int32 b) { return a + b; }
expose fn int32 shout(int32 n) { println(s: "static ${n}"); return n; }
KAMA
"$KAMA" build "$tmp/smod.kama" --select OUTPUT=STATIC -o "$tmp/libsmod.a"
cat > "$tmp/shost.c" <<'C'
#include "smod.h"
int main(void) { return smod_add(40, 1) + smod_shout(1); }   /* 42 */
C
"$CC" -I"$tmp" "$tmp/shost.c" "$tmp/libsmod.a" -o "$tmp/shost"
set +e
sout=$("$tmp/shost"); sgot=$?
set -e
if [ "$sgot" != 42 ] || [ "$sout" != "static 1" ]; then
    echo "FAIL expose_shared_check (static: got $sgot and \"$sout\", expected 42 and \"static 1\")"
    exit 1
fi
# ...and a kama program, whose entry TU defines the slots, links the same archive without a duplicate.
cat > "$tmp/app.kama" <<'KAMA'
extern "smod.h";
extern fn int32 smod_add(int32 a, int32 b);
unsafe fn int32 viaLibrary() { return smod_add(a: 40, b: 2); }
fn uint8 main() { return cast<uint8>(viaLibrary()); }
KAMA
LIBRARY_PATH="$tmp" "$KAMA" build "$tmp/app.kama" --link smod -o "$tmp/app"
set +e
"$tmp/app"; agot=$?
set -e
if [ "$agot" != 42 ]; then
    echo "FAIL expose_shared_check (a kama program linking the static library: got $agot, expected 42)"
    exit 1
fi
# ...and as one OBJECT, which a host links whole, so its one unit defines the slots as a --shared module's does.
"$KAMA" build "$tmp/smod.kama" --select OUTPUT=OBJECT -o "$tmp/smod.o"
"$CC" -I"$tmp" "$tmp/shost.c" "$tmp/smod.o" -o "$tmp/ohost"
set +e
oout=$("$tmp/ohost"); ogot=$?
set -e
if [ "$ogot" != 42 ] || [ "$oout" != "static 1" ]; then
    echo "FAIL expose_shared_check (object: got $ogot and \"$oout\", expected 42 and \"static 1\")"
    exit 1
fi
echo "PASS expose_shared_check (dlsym mod_add/mod_mul/renamed -> $got; a C host and a kama program link the static library -> $sgot; a C host links the object -> $ogot)"
