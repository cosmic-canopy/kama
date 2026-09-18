#!/bin/sh
# check-c-keywords.sh — kama reserves every C keyword, and the table stays equal to the C11 + C23 sets.
#
# What this is about. kama lowers to C, so a kama name that is a C keyword emits C that does not compile.
# Measured at `0.9.80`, the exposure was not one position but seven, and two of them were missed by an
# earlier audit that reasoned about it instead of reading the emitted C:
#
#     int32_t switch;                                       /* struct field                    */
#     int32_t inline;                                       /* variant payload field           */
#     } Auto;                                               /* variant case union member       */
#     int32_t (*union)(void* self, int32_t static_assert);  /* vtable slot + parameter         */
#     int32_t volatile = (double + 1);                      /* local + parameter               */
#     KAMA_EXPORT int32_t static_assert(int32_t x);         /* an `expose`d name — silent      */
#
# Type names, function names and enum case constants are scope-prefixed and were never at risk.
#
# The alternative was to RENAME on collision (`switch` -> `k_switch`). ⚠️ KR-67 has since made that rename
# real for a different reason — every name kama owns reaches C as `k_<name>` so no header macro can rewrite
# it — which dissolves all three sub-problems the rename used to carry. The reservation is kept anyway:
# reserving now and relaxing later is source-compatible while the reverse is not, one lexer rule covers
# every position, and the DECLARED C surface (`extern`/`expose`, which is NOT prefixed) still requires it.
# See SPEC § C names, and tools/check-c-names.sh for the register premise.
#
# Three assertions. Each was checked by BREAKING the mechanism, not by reading it:
#
#   1. the table in kama.l equals the C11 + C23 keyword sets, both directions
#        — REAL, and it is the one that keeps this honest over time. The compiler's table is compared
#          against the STANDARDS' list written out below, so authority runs from C to kama and a future
#          addition is a visible failure rather than a silent hole.
#   2. every reserved spelling that would otherwise lex as an identifier is REFUSED as a name
#        — REAL. With the lexer check disabled, the ones that are not kama keywords compile clean.
#   3. `uint` is NOT reserved and still takes the type-position path
#        — REAL, and it exists because the asymmetry looks like an oversight. `uint` is not a C keyword,
#          so it is not part of this rule; `int`, `double` and `float` ARE, so they moved to the lexer and
#          took their width guidance with them. Someone tidying these into "consistency" breaks a
#          diagnostic, so both halves are pinned.
#
# ⚠️ There is deliberately NO "sweep the emitted C for a keyword declarator" assertion, though it looks
# like the obvious fourth. Every fixture in the suite already hands its generated C to clang, so a
# compiler-GENERATED name colliding with a keyword fails the native leg loudly and by name. A grep over C
# text cannot tell a declarator from a type (`int32_t x;` contains `int`), so it would be a weak assertion
# that reads as coverage while providing less than what already exists.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-c-keywords: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*" >&2; fail=1; }
ok()  { echo "  ok: $*"; }

# ---- 1. the table is exactly C11 + C23 ----------------------------------------------------------------
# The standards' sets, written out here so this file is the authority and src/kama.l is the thing checked.
# C11 §6.4.1 (44) plus the C23 additions (alignas alignof bool constexpr false nullptr static_assert
# thread_local true typeof typeof_unqual, and the _BitInt/_Decimal* type specifiers).
cat > "$tmp/standard" <<'EOF'
_Alignas
_Alignof
_Atomic
_BitInt
_Bool
_Complex
_Decimal128
_Decimal32
_Decimal64
_Generic
_Imaginary
_Noreturn
_Static_assert
_Thread_local
alignas
alignof
auto
bool
break
case
char
const
constexpr
continue
default
do
double
else
enum
extern
false
float
for
goto
if
inline
int
long
nullptr
register
restrict
return
short
signed
sizeof
static
static_assert
struct
switch
thread_local
true
typedef
typeof
typeof_unqual
union
unsigned
void
volatile
while
EOF
LC_ALL=C sort -u "$tmp/standard" -o "$tmp/standard"

# The compiler's own table, read out of the source rather than duplicated.
sed -n '/^static const char\* c_reserved\[\] = {/,/^};/p' "$ROOT/src/kama.l" \
    | grep -o '"[A-Za-z_][A-Za-z0-9_]*"' | tr -d '"' | LC_ALL=C sort -u > "$tmp/table"

# `grep -Fxv`, never `comm`: comm is locale-broken on macOS and silently reports nonsense.
missing=$(LC_ALL=C grep -Fxv -f "$tmp/table" "$tmp/standard" || true)
extra=$(LC_ALL=C   grep -Fxv -f "$tmp/standard" "$tmp/table" || true)
if [ -n "$missing" ]; then
    bad "src/kama.l's c_reserved is MISSING C keywords: $(printf '%s' "$missing" | tr '\n' ' ')"
elif [ -n "$extra" ]; then
    bad "src/kama.l's c_reserved has entries C does not reserve: $(printf '%s' "$extra" | tr '\n' ' ')"
else
    ok "the reserved table is exactly the C11 + C23 keyword set ($(wc -l < "$tmp/table" | tr -d ' ') words)"
fi

# ---- 2. each one is refused as a name ------------------------------------------------------------------
# Only the spellings that would otherwise LEX AS AN IDENTIFIER are this rule's population: the rest are
# already kama keywords, so they were never spellable and a "rejected" result would prove nothing about
# this rule. The split is computed, not listed, so it tracks kama's own keyword table.
refused=0; skipped=0
while read -r w; do
    printf 'type value Probe { public int32 %s; }\nfn int32 main() { return 0; }\n' "$w" > "$tmp/p.kama"
    if "$KAMA" check "$tmp/p.kama" > "$tmp/p.log" 2>&1; then
        bad "\`$w\` is a C keyword but is accepted as a field name — the emitted C will not compile"
        continue
    fi
    if grep -qF 'is a reserved word' "$tmp/p.log"; then
        refused=$((refused + 1))
    else
        # Rejected for some OTHER reason: it is already a kama keyword, so it is a syntax error instead.
        # That is fine — it cannot be used as a name either way — but it is not evidence for THIS rule,
        # so it is counted apart rather than folded in.
        skipped=$((skipped + 1))
    fi
done < "$tmp/table"

if [ "$refused" -lt 30 ]; then
    bad "only $refused spellings were refused AS RESERVED WORDS — expected the ~38 that are not already kama keywords"
else
    ok "$refused C keywords refused as a name with the reserved-word diagnostic ($skipped were already kama keywords)"
fi

# ---- 3. the `uint` asymmetry, both halves --------------------------------------------------------------
printf 'fn int32 main() { uint x = 5; return 0; }\n' > "$tmp/u.kama"
if "$KAMA" check "$tmp/u.kama" > "$tmp/u.log" 2>&1; then
    bad "\`uint\` is accepted"
elif grep -qF 'is a reserved word' "$tmp/u.log"; then
    bad "\`uint\` is being treated as a reserved word — it is not a C keyword, so it is not part of this rule"
elif grep -qF 'is not a kama type' "$tmp/u.log"; then
    ok "\`uint\` is not a C keyword, so it keeps the type-position diagnostic"
else
    bad "\`uint\` is rejected, but by neither path: $(head -1 "$tmp/u.log")"
fi

# ...and the three that DID move keep their width guidance, which is the whole reason they moved rather
# than simply becoming "reserved".
for pair in 'int:int32' 'double:float64' 'float:float32'; do
    w=${pair%%:*}; want=${pair##*:}
    printf 'fn int32 main() { %s x = 5; return 0; }\n' "$w" > "$tmp/t.kama"
    "$KAMA" check "$tmp/t.kama" > "$tmp/t.log" 2>&1 || true
    if grep -qF 'is a reserved word' "$tmp/t.log" && grep -qF "$want" "$tmp/t.log"; then
        ok "\`$w\` is reserved AND still names \`$want\`"
    else
        bad "\`$w\` lost its width guidance: $(head -1 "$tmp/t.log")"
    fi
done

[ "$fail" = 0 ] && echo "check-c-keywords: PASS" || { echo "check-c-keywords: FAIL" >&2; exit 1; }
