#!/bin/sh
# check-alloc-funnel.sh — heap memory is obtained and released in exactly ONE place (KR-48).
#
# `kama_alloc(n, align)` / `kama_free(p, n, align)` in include/kama_runtime.h are the funnel, and they are the only
# code in the compiler's output, the runtime headers, the prelude and the stdlib allowed to call the C allocator.
# Everything else goes through them, carrying the block's layout. Two defects this holds down:
#   * MIXED FAMILIES. Before the funnel, ~20 blocks were allocated by one family (a raw emitted `malloc`, the
#     runtime's `kama_alloc`, an `Allocator`) and released by another. That was correct only because every family
#     happened to be libc, and wrong the moment one is replaced — which is what KR-49's global allocator does.
#   * HIDDEN ALLOCATORS. `strdup` returns `malloc` memory, so a `strdup` is a raw allocation that no grep for
#     `malloc` finds.
# The sanitizer leg proves the other half: under KAMA_ALLOC_CHECK every release is checked against the layout its
# block was allocated with (run_tests.sh). This guard proves nothing escapes the funnel for that check to miss.
#
# Scanned: include/*.h (outside the funnel's own block), prelude/ and lib/ kama sources, and the STRING LITERALS
# of src/kama.cemit*.cpp — the C the compiler writes. Not scanned: tests/ and examples/, whose `extern fn malloc`
# is a program calling C, which is FFI and not the runtime's business. A foreign release that is not one of these
# names (`freeaddrinfo`, `LocalFree`, `FreeEnvironmentStringsW`) passes, and must stay paired with its own source.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-alloc-funnel: FAIL — $*" >&2; exit 1; }

NAMES='malloc|calloc|realloc|free|strdup|_strdup|strndup|aligned_alloc|_aligned_malloc|_aligned_free|posix_memalign'
CALL="(^|[^A-Za-z0-9_.>])($NAMES)[[:space:]]*\\("

# Print `file:line: text` for every call to a C allocator in C/kama source, `//` comments stripped. The funnel's
# own block in kama_runtime.h (from its banner to `kama_copy`) is the one exemption.
scan_source() {
    for f in "$@"; do
        awk -v file="$f" -v re="$CALL" '
            /THE ALLOCATION FUNNEL\./           { funnel = 1 }
            funnel && /static inline void  kama_copy/ { funnel = 0 }
            {
                line = $0; sub(/\/\/.*/, "", line)
                if (!funnel && line ~ re) printf "%s:%d: %s\n", file, NR, $0
            }' "$f"
    done
}
# The same over only the string literals of a C++ file: the C the emitter writes.
scan_emitter() {
    for f in "$@"; do
        awk -v file="$f" -v re="$CALL" '
            {
                code = $0; sub(/^[[:space:]]*\/\/.*/, "", code)
                lits = ""; s = code
                while (match(s, /"([^"\\]|\\.)*"/)) { lits = lits " " substr(s, RSTART + 1, RLENGTH - 2); s = substr(s, RSTART + RLENGTH) }
                if (lits ~ re) printf "%s:%d: %s\n", file, NR, $0
            }' "$f"
    done
}

# --- 0. the scanners catch what they are for (a guard that never fires proves nothing) ----------------------
printf 'static inline void* f(void) { return malloc(4); }\n' > "$tmp/planted.h"
printf 'fn void g() { p = strdup(s: x); }\n' > "$tmp/planted.kama"
printf '    *_out << "x = (T*)malloc(sizeof(T));\\n";\n' > "$tmp/planted.cpp"
printf '// a comment may say malloc(n) freely\nstatic int ok(void) { return freeaddrinfo_count(); }\n' > "$tmp/clean.h"
[ -n "$(scan_source "$tmp/planted.h")" ]    || fail "the header scan missed a planted malloc("
[ -n "$(scan_source "$tmp/planted.kama")" ] || fail "the kama scan missed a planted strdup("
[ -n "$(scan_emitter "$tmp/planted.cpp")" ] || fail "the emitter scan missed a planted malloc( in a string literal"
[ -z "$(scan_source "$tmp/clean.h")" ]      || fail "the header scan fired on a comment or a foreign release"

# --- 1. the tree ------------------------------------------------------------------------------------------------
cd "$ROOT"
{
    scan_source include/*.h
    scan_source $(find prelude lib -name '*.kama' | sort)
    scan_emitter src/kama.cemit*.cpp
} > "$tmp/hits"
if [ -s "$tmp/hits" ]; then
    sed 's/^/  /' "$tmp/hits" >&2
    fail "heap memory obtained or released outside the funnel — call kama_alloc(n, align) / kama_free(p, n, align) (include/kama_runtime.h), with the block's layout"
fi
echo "check-alloc-funnel: PASS (the C allocator is called only inside the kama_alloc/kama_free funnel: include/, prelude/, lib/, and the C the emitter writes)"
