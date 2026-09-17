#!/bin/sh
# check-stats.sh — `kama stats` (KR-60). The report is only worth having if its numbers are EXACT, so this
# guard builds a fixture whose every line is accounted for by hand and asserts the counts, rather than
# asserting the report merely runs.
#
# The cases that matter are the ones a line counter gets wrong, and they are the reason this lives in the
# compiler at all:
#   * `//` inside a string is NOT a comment — the lexer is in the string state there;
#   * a blank-looking line INSIDE a multi-line string is CODE (it is string content);
#   * the continuation lines of a `/* … */` block are COMMENT (a `^\s*//` grep misses them);
#   * a line with code and a trailing comment is CODE (the usual convention).
# Plus the numbers no text tool has at all: generic templates vs the monomorphs actually emitted, the
# unsafe/FFI surface, and what `@compileFor` leaves out of this build.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-stats: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-stats: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }

# `jget <file> <section>.<key>` (or `<section>.0.<key>`, the first element of an array section): read one number
# out of the --json envelope with sed, without a JSON dependency. Every section is a flat object of numbers, which
# is all this needs. (It was python3, which a Windows msys2 shell does not have, so the guard never ran there.)
jget() {
    sed "s/.*\"${2%%.*}\":\[*{\([^}]*\)}.*/\1/" "$1" | tr , '\n' | sed -n "s/^\"${2##*.}\"://p"
}

# ---- 1. lines, counted by hand ---------------------------------------------------------------------
# 14 lines exactly. Hand tally, which is the whole point of this fixture:
#   comment: 1 (`// one`), 9, 10, 11 (the block and its continuations)        = 4
#   code:    2, 3 (trailing comment), 4, 5 (blank INSIDE the string), 6, 7, 12, 13, 14 = 9
#   blank:   8                                                                 = 1
cat > "$tmp/lines.kama" <<'KAMA'
// one
fn int32 main() {
    string url = "https://example.com/x";   // not a comment inside that string
    string block = @"first

third";
    int32 n = 1;

    /* a block comment
       continues here
       and ends here */
    println(s: url); println(s: block);
    return n - 1;
}
KAMA
"$KAMA" stats "$tmp/lines.kama" --json > "$tmp/lines.json" 2>"$tmp/lines.err" \
    || fail "stats failed on the line fixture:" "$tmp/lines.err"
for pair in 'lines.total:14' 'lines.code:9' 'lines.comment:4' 'lines.blank:1'; do
    got=$(jget "$tmp/lines.json" "${pair%%:*}"); want=${pair#*:}
    [ "$got" = "$want" ] || fail "${pair%%:*} is $got, hand-counted $want — the lexer's classification drifted"
done

# ---- 2. declarations by kind ------------------------------------------------------------------------
cat > "$tmp/kinds.kama" <<'KAMA'
type value Point { public int32 x; public ctor make(int32 x) { this.x = x; } }
type resource Handle { public int32 fd; public ctor make(int32 fd) { this.fd = fd; }
                       public fn int32 get() { return this.fd; } ~Handle() { } }
type contract Shape for value { fn int32 area(); }
type enum Color { Red, Green }
type value Box<T> { public T v; public ctor make(T v) { this.v = give v; } }
comptime int32 LIMIT = 4;
unsafe fn int32 raw() { return 1; }
extern "<string.h>";
extern fn isize strlen(UnsafeConstPtr<uint8> s);
fn int32 main() { Box<int32> b = Box.make(v: 1); return b.v - 1; }
KAMA
"$KAMA" stats "$tmp/kinds.kama" --json > "$tmp/kinds.json" 2>"$tmp/kinds.err" \
    || fail "stats failed on the kinds fixture:" "$tmp/kinds.err"
# ⚠️ `method` is 2: `Handle.get` AND the contract's `area()` — a contract member is a method declaration
# like any other, which is what makes "how many methods does this project declare" answerable at all.
# ⚠️ `value` is 2: `Point` and `Box<T>`. A type counts once under its KIND, and `generic` is a SUBSET of
# those (a template is still a value/resource/…), so the kind column adds up to `types.total`.
for pair in 'types.value:2' 'types.total:5' 'types.resource:1' 'types.contract:1' 'types.enum:1' 'types.generic:1' \
            'functions.ctor:3' 'functions.dtor:1' 'functions.method:2' \
            'ffi.externFns:1' 'ffi.externHeaders:1' 'ffi.unsafeFns:1'; do
    got=$(jget "$tmp/kinds.json" "${pair%%:*}"); want=${pair#*:}
    [ "$got" = "$want" ] || fail "${pair%%:*} is $got, expected $want"
done
# The number no source text carries: ONE template, and the monomorph the program actually produced.
[ "$(jget "$tmp/kinds.json" generics.templates)" = "1" ] \
    || fail "generics.templates is $(jget "$tmp/kinds.json" generics.templates), expected the one Box<T> template"
[ "$(jget "$tmp/kinds.json" generics.instances)" -ge 1 ] \
    || fail "generics.instances is 0 — Box<int32> was emitted, so the analysis half of the report is dead"

# ---- 3. what `@compileFor` leaves out of THIS build --------------------------------------------------
cat > "$tmp/gated.kama" <<'KAMA'
@compileFor(ARCH_WASM32)
fn int32 onlyWasm() {
    return 23;
}
fn int32 main() { return 0; }
KAMA
"$KAMA" stats "$tmp/gated.kama" --json > "$tmp/gated.json" 2>"$tmp/gated.err" \
    || fail "stats failed on the gated fixture:" "$tmp/gated.err"
[ "$(jget "$tmp/gated.json" gates.sites)" = "1" ]         || fail "gates.sites is not 1"
[ "$(jget "$tmp/gated.json" gates.gatedOutDecls)" = "1" ] || fail "the gated-out declaration was not counted"
[ "$(jget "$tmp/gated.json" gates.gatedOutLines)" = "4" ] || \
    fail "gates.gatedOutLines is $(jget "$tmp/gated.json" gates.gatedOutLines), expected the 4 lines of onlyWasm (gate line included)"
[ "$(jget "$tmp/gated.json" gates.configurations)" = "2" ] || \
    fail "gates.configurations is not 2 — the cover needs this build plus one that activates ARCH_WASM32"

# ---- 4. the largest declarations are ranked by their REAL span ---------------------------------------
# `onlyWasm` is 3 lines (its `fn` line, its body, its brace) while `main` is 1: a declaration's own endLine
# is its SIGNATURE (the AST says so), so ranking by that alone reported every function as 1 line. The body's
# block carries the real end. ⚠️ Deliberately 3 and not 4: the gated-out LINE count above includes the
# `@compileFor` line (it is equally absent from the build), while this ranks the declaration itself.
top=$(jget "$tmp/gated.json" largest.0.lines)
[ "$top" = "3" ] || fail "the largest declaration is $top line(s), expected 3 — the span is back to the signature"

# ---- 5. a project operand reports its modules, and the human report runs -----------------------------
"$KAMA" stats "$ROOT/tests/file_gate.d/kama.json" > "$tmp/proj.out" 2>"$tmp/proj.err" \
    || fail "stats failed on a project:" "$tmp/proj.err"
grep -q "3 files" "$tmp/proj.out" || fail "a project report did not count its files:" "$tmp/proj.out"
grep -q "gates" "$tmp/proj.out"   || fail "a project with file gates reported no gate line:" "$tmp/proj.out"

echo "check-stats: PASS (lines hand-verified incl. string/comment traps; kinds, generics, FFI, gates, spans, project)"
