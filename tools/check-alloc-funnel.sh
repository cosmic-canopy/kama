#!/bin/sh
# check-alloc-funnel.sh — heap memory is obtained and released in exactly ONE place (KR-48).
#
# `kama_alloc(n, align)` / `kama_free(p, n, align)` in include/kama_runtime.h are the funnel, and they are the only
# code in the compiler's output, the runtime headers, the prelude and the stdlib allowed to call the C allocator.
# Everything else goes through them, carrying the block's layout. Two defects this holds down:
#   * MIXED FAMILIES. Before the funnel, ~20 blocks were allocated by one family (a raw emitted `malloc`, the
#     runtime's `kama_alloc`, an `Allocator`) and released by another. That was correct only because every family
#     happened to be libc, and wrong the moment one is replaced — which is what a declared `@globalAllocator` does.
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
# `[(]`, not `\(`: the pattern reaches awk through `-v`, which processes escapes, and gawk turns `\(` into a bare `(`
# — an unbalanced regex that is fatal (mawk keeps `\(`, so it passed on Linux and failed on Windows' msys2 gawk).
CALL="(^|[^A-Za-z0-9_.>])($NAMES)[[:space:]]*[(]"

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

# --- 2. the FOREIGN allocators the compiler knows about (KR-74) ---------------------------------------------
# Section 1 proves the C allocator is reached only through the funnel. It cannot prove the same of the OTHER
# way a runtime header obtains memory the program must release: a platform entry point that hands back a block
# with its own release (`getaddrinfo`/`freeaddrinfo`, `opendir`/`closedir`, `GetEnvironmentStringsW`). Those are
# not in NAMES and never will be — there is no closed list of them — so the compiler carries the judgement, in
# `CEmitter::foreignAllocators` (src/kama.cemit.cpp), and treats a call to one as an allocation fact ALWAYS,
# because no declared `@globalAllocator` can serve memory its funnel never handed out.
#
# A list in the compiler is a list that can go stale, which is the failure mode this whole row exists to end.
# So it is checked the only way it can be: every name on it must still be reached from `include/`, and every
# such name reached from `include/` must be on it. The second half is what catches "a new OS call arrived in a
# header and nothing noticed" — the way `kama_resolve_host`'s `getaddrinfo` went unmarked for months.
FOREIGN=$(sed -n '/kForeign = {/,/};/p' src/kama.cemit.cpp | grep -oE '"[A-Za-z_][A-Za-z0-9_]*"' | tr -d '"' | sort -u)
[ -n "$FOREIGN" ] || fail "could not read CEmitter::foreignAllocators out of src/kama.cemit.cpp — did it move or get renamed?"
# The platform entries a header actually calls, `//` comments and the funnel's own block stripped as above.
HDR=$(awk '
    /THE ALLOCATION FUNNEL\./           { funnel = 1 }
    funnel && /static inline void  kama_copy/ { funnel = 0 }
    { line = $0; sub(/\/\/.*/, "", line); if (!funnel) print line }' include/*.h)
for n in getaddrinfo GetEnvironmentStringsW opendir pthread_create CreateThread CreateProcessW; do
    reached=$(printf '%s\n' "$HDR" | grep -cE "(^|[^A-Za-z0-9_.>])$n[[:space:]]*\(" || true)
    known=$(printf '%s\n' "$FOREIGN" | grep -cx "$n" || true)
    if [ "$reached" -gt 0 ] && [ "$known" -eq 0 ]; then
        fail "include/ calls \`$n\`, which hands back memory the program must release, but CEmitter::foreignAllocators does not list it — a --no-heap build would silently accept a program that reaches it"
    fi
    if [ "$reached" -eq 0 ] && [ "$known" -gt 0 ]; then
        fail "CEmitter::foreignAllocators lists \`$n\`, but no header calls it any more — drop it, or the list becomes a place where stale entries hide"
    fi
done

# --- 3. the capacity-guarded drop is still the ONLY one (KR-74) ---------------------------------------------
# `kama_string__dtor` frees under `if (self->kama_cap)`, and the header scan is flow-insensitive, so the
# compiler excludes it BY NAME (isCapacityGuardedDrop) or every no-heap body holding a string literal is
# refused — SPEC promises that literal. A name is exactly the shape this row deleted 55 of, so it is allowed to
# be one only while it is the one. A second guarded free must be a decision, not a surprise.
guarded=$(grep -cE 'if \(self->kama_cap\)[[:space:]]*kama_free' include/*.h | awk -F: '{s+=$2} END {print s+0}')
[ "$guarded" -eq 1 ] || fail "expected exactly ONE capacity-guarded kama_free in include/ (kama_string__dtor), found $guarded — CEmitter::isCapacityGuardedDrop names only that one, so a new one is silently a fact and will refuse programs SPEC says are legal"

echo "check-alloc-funnel: PASS (the C allocator is called only inside the kama_alloc/kama_free funnel: include/, prelude/, lib/, and the C the emitter writes; the compiler's foreign-allocator list agrees with what include/ reaches; one capacity-guarded drop)"
