#!/bin/sh
# check-lsp.sh — LSP server guard (M1 diagnostics + M2 interactive features). Drives the `kama lsp` server
# over stdio with a scripted JSON-RPC 2.0 session and asserts, end-to-end:
#   1. initialize        -> capabilities advertising full-document sync + hover/definition/documentSymbol.
#   2. didOpen (bad buf) -> a publishDiagnostics carrying the syntax error at the right (0-based) range.
#   3. didChange (fixed) -> a publishDiagnostics with an EMPTY array (squiggles cleared).
#   4. documentSymbol    -> the outline (Point value + mid function) with accurate name ranges + kinds.
#   5. definition        -> go-to-def on a type reference lands on the decl (a file:// Location).
#   6. hover             -> "<kind> <name>" on a type reference; null on a body use-site (M3 scope).
# The buffers are inlined (tiny; avoids fragile file-content JSON-escaping in POSIX sh). Mirrors
# check-query.sh style; run standalone or from run_tests.sh. Fails (exit 1) with a diagnostic dump.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-lsp: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
session="$tmp/session"; : > "$session"

# frame <json-body>: append one LSP-framed message (Content-Length header + CRLFCRLF + body) to the
# session. %s never interprets the body's backslashes, so the JSON `\n` escapes stay two literal bytes —
# exactly what Content-Length must count and what the server's JSON parser decodes back to newlines.
frame() {
    body="$1"
    len=$(printf '%s' "$body" | wc -c | tr -d ' ')
    printf 'Content-Length: %s\r\n\r\n%s' "$len" "$body" >> "$session"
}

URI="file:///t.kama"
BAD='fn int32 main() {\n    return 0\n}\n'     # missing semicolon -> syntax error on line 3 (LSP line 2)
GOOD='fn int32 main() {\n    return 0;\n}\n'   # fixed

# M2 fixture: a decl-rich buffer (its own URI) driving hover / go-to-definition / document symbols. The
# layout is fixed — the query positions below are tied to it (kama line 1-based/col 0-based -> LSP 0/0):
#   L2 `type value Point { ... }`   -> "Point" name at kama 2:11  (LSP 1:11, the def target)
#   L3 `fn Point mid(Point a) ...`  -> return-type "Point" at kama 3:3 (LSP 2:3); param-type at kama 3:13 (LSP 2:13)
#   L4 (M3) a BODY: two `Point` locals at kama 4:17 / 4:26 (LSP 3:17 / 3:26) and a CALL to `mid` at kama
#      4:36 (LSP 3:41). Only the M3 reference index sees these — M0's signature walk never enters a body.
#      Appended, so every position above is unchanged.
# M4.6 fixture: a buffer that has NEVER parsed — `p.` on line 5 is a syntax error, so there is no
# last-good index at all. This is the state a NEW file is in the first time completion is wanted.
NURI="file:///new.kama"
NEWB='type value P { public int32 x; public fn int32 twice() { return this.x * 2; } }\nfn int32 main() {\n    P p;\n    p.\n    return 0;\n}\n'

# M4.9 fixture: the campaign-exit STAMP_LOC checklist. A generic type's decl name and a named ctor's
# name are both built mid-action from a raw IDENTIFIER, so without an explicit stamp they inherit their
# whole production's span. That is a DATA-LOSS bug, not cosmetics: rename REPLACES the range it is handed,
# so renaming `Box` would have overwritten `Box<T>` and deleted the type-parameter list.
SPURI="file:///span.kama"
SPAN='type value Box<T> { public T v; public ctor of(T v) { this.v = v; } }\ntype resource R { public ctor make() { } ~R() { } }\n'

# Contract-model M4 fixture: `type intrinsic <…> implements C`. The enclosing type of a method here has no
# ClassDeclarationNode at all — the shape the retroactive block used to have, and `enclosingCallable` had an arm for
# that one and none for this, so everything downstream of it (completion, signature help, the in-scope
# bindings) went dead inside these bodies. That cost nothing while the spelling lived only in tests/;
# it costs real files the moment lib/std/fmt/parse.kama and lib/std/math/scalar.kama migrate onto it.
# TWO member lists, deliberately: the block's SHARED body and a per-target `<…>` section. A member lives in
# exactly one of them, so a fixture that only probed the shared body would pass with the section arm absent.
#   LSP L3 = the shared body, local `wshared`;  LSP L6 = the `<float32>` section, local `wsection`.
# The names are deliberately unique across the whole session, because `expect` matches the transcript as
# one string and cannot scope a substring to the response that produced it.
IIURI="file:///intrinsic.kama"
IIB='type contract Weighable { fn int32 weight(ref This wpeer); }\ntype intrinsic <int8, int16> implements Weighable {\n    public fn int32 weight(ref This wpeer) { int32 wshared = 1; return wshared + cast<int32>(wpeer); }\n}\ntype intrinsic <float32, float64> implements Weighable {\n    <float32> { public fn int32 weight(ref This wpeer) { int32 wsection = 2; return wsection + cast<int32>(wpeer); } }\n    <float64> { public fn int32 weight(ref This wpeer) { return 4; } }\n}\n'

# M5.3/M5.4 fixture: THREE independent syntax errors at three grains — a malformed class member (LSP
# line 2), and a missing semicolon in each of two DIFFERENT functions (LSP lines 6 and 10). Before error
# recovery this file produced exactly ONE diagnostic and no index at all; the whole milestone is that it
# now produces three and still answers queries. The `H`/`x` outline is the second half of the claim: the
# broken member is discarded but the TYPE stays declared, which is what keeps a half-typed member from
# cascading "undeclared type" over every use of it.
RURI="file:///recover.kama"
RECOV='type value H {\n    public int32 x;\n    public int32 = ;\n}\nfn int32 a() {\n    int32 v = 1\n    return v;\n}\nfn int32 main() {\n    int32 w = 2\n    return w;\n}\n'

# M5.4 fixture: the one recovery outcome that MUST suppress semantics. A malformed type HEAD is dropped
# whole by the top-level arm while the functions after it survive and still use the name — so every use
# reads as an undeclared type. Measured: without the droppedTopLevelDecl guard this buffer publishes the
# 1 real parse error plus 2 false "unknown type `Widget`" errors, and a real file with 20 uses would get
# 20. The finer arms lose a statement or a member and cascade barely at all, so they publish normally.
XURI="file:///drop.kama"
XDROP='type value ! Widget {\n    public int32 w;\n}\nfn int32 use() {\n    Widget a;\n    Widget b;\n    return 0;\n}\n'

# M6 B2 fixtures: semantic tokens. TOKB is deliberately a type that declares a ctor — that is the shape
# whose implicit result type resolves through the class's OWN decl identifier, so `_positions` holds TWO
# entries at `P`'s decl-name range. The protocol FORBIDS overlapping tokens, and the existing de-duplication
# lives only in `_refIndex`, never in `_positions`, so an exact-array assertion here is what proves the
# facade's own overlap filter runs.
TOKURI="file:///semtok.kama"
TOKB='type value P {\n    int32 x;\n    public ctor make(int32 v) { this.x = v; }\n}\nfn int32 main() { P p = P.make(v: 1); return p.x; }\n'

# TOKG is the GENERIC-body fixture. It documented the M6 A2-era gap (nothing inside a generic type's body
# reached the reference index) as an exact array, precisely so that closing it in B3 could not be silent —
# and B3 closed it. Two things are asserted through it now: that a template's members are indexed at all,
# and that one declaration stays ONE symbol however many instantiations exist.
# M6 B3a fixtures: RENAMING A METHOD — the assertion that would have caught the bug. A `--refs` query
# returning nothing was survivable; rename half-applying was not, because prepareRename still OFFERED F2
# (the method HAS a def-site, from the _classes loop — it was only its uses that were missing) and the
# resulting WorkspaceEdit rewrote the declaration while every call kept the old name. That does not fail
# loudly: it produces a buffer that no longer compiles.
#
# Layout (LSP 0-based lines, 0-based chars):
#   L2 `    public fn int32 get() { return this.x; }` -> `get` decl at 20..23
#   L4 `fn int32 main() { P p = P.zero(); p.x = 1; return p.get(); }` -> the CALL `get` at 52..55
MRURI="file:///methodrename.kama"
MREN='type value P {\n    public int32 x;   public ctor zero() { this.x = 0; }\n    public fn int32 get() { return this.x; }\n}\nfn int32 main() { P p = P.zero(); p.x = 1; return p.get(); }\n'

# The generic half. TWO instantiations, each with its OWN call, on purpose: a single-instantiation fixture
# passes under designs that canonicalize the instance key onto the template, which would break the moment a
# second key shape appeared. `Box<int32>` and `Box<bool>` resolve through different instances and must land
# on ONE symbol — three edits, no duplicate over any range, and none at the instances' mangled names.
#   L2 `    public fn T get() { return this.v; }` -> `get` decl at 16..19
#   L4 `... return bs.get() ? bi.get() : 0; }`   -> the two calls at 108..111 and 119..122
MGURI="file:///genrename.kama"
MGEN='type value Box<T> {\n    public T v;   public ctor of(T v) { this.v = v; }\n    public fn T get() { return this.v; }\n}\nfn int32 main() { Box<int32> bi = Box::<int32>.of(v: 1); Box<bool> bs = Box::<bool>.of(v: false); return bs.get() ? bi.get() : 0; }\n'

# M6 B3f — the `::` QUALIFIER of a name. Before the grammar carried per-segment positions a qualifier was a
# list of plain STRINGS, so `Color` in `Color::Green` was indexed nowhere: renaming the enum rewrote its
# declaration, its type annotations and its `case` arms, and left every `Color::` spelling behind. Both
# spellings are in this buffer on purpose, since a fixture with only the annotation passes while broken.
# Layout (LSP 0-based lines, 0-based chars):
#   L0 `type enum Color { Red, Green, Blue }`                  -> `Color` decl at 10..15
#   L1 `fn int32 main() { Color c = Color::Green; ...`         -> annotation at 18..23, QUALIFIER at 28..33
QRURI="file:///qualrename.kama"
QREN='type enum Color { Red, Green, Blue }\nfn int32 main() { Color c = Color::Green; return match (c) { case Red: 1; case Green: 2; case Blue: 3; }; }\n'

TOKGURI="file:///semtokgen.kama"
TOKG='type value Box<T> {\n    T v;   public ctor of(T v) { this.v = v; }\n    public fn T get() { return this.v; }\n}\nfn int32 main() { Box<int32> b = Box::<int32>.of(v: 7); return b.get(); }\n'

QURI="file:///shapes.kama"
SHP='type value Point { public int32 x;   public ctor zero() { this.x = 0; } }\nfn Point mid(Point a) { return a; }\nfn int32 use() { Point p = Point.zero(); Point q = mid(a: p); return q.x; }\n'

# A `file://` URI the SERVER can resolve back to a real file. The in-memory buffers above use invented
# paths and never get opened, but the two below are read off disk, so their URI has to name the file the
# way the OS does: under msys2 $ROOT is `/c/Users/…`, which a native kama resolves against the current
# drive as `C:\c\Users\…` and cannot open. `cygpath -m` gives `C:/Users/…`, and the extra slash makes it
# the conventional `file:///C:/…` that uriToPath strips back off.
furi() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) printf 'file:///%s' "$(cygpath -m "$1")" ;;
        *)                    printf 'file://%s'  "$1" ;;
    esac
}

# Module-loading fixture: a REAL on-disk file that imports a std module. Unlike the in-memory buffers above
# (fake paths -> single-file fallback), this exercises loadProgramUnits pulling std::collections off disk so
# the imported DynamicArray resolves (no false "does not export"), and cross-module go-to-def into the std
# source. URI must be the real path so imports resolve relative to it + the stdlib.
IURI=$(furi "$ROOT/tests/query/imports.kama")
IMP='import { std::collections::DynamicArray };\nfn int32 useit(DynamicArray<int32> a) { return 0; }\n'

# M3.5 workspace fixture: the DECLARING half of the tests/query/ws package. app.kama (on disk, never
# opened here) imports it and uses `Widget` three times; widget.kama imports nothing, so its own closure
# is just itself. Opening it and renaming `Widget` is exactly the case M3.3 had to refuse.
WWURI=$(furi "$ROOT/tests/query/ws/src/widget.kama")
WW='export { Widget, defaultSize };\ntype value Widget {\n    public int32 size;\n    public ctor of(int32 size) { this.size = size; }\n}\nfn int32 defaultSize() { return 7; }\n'

# Semantic-diagnostic fixture: an undeclared type in a body (kama line 2 -> LSP line 1).
SURI="file:///sem.kama"
SEM='fn int32 main() {\n    Nonexistent thing;\n    return 0;\n}\n'

# ---- auto-import quick fix (textDocument/codeAction) -----------------------------------------------
#
# All three buffers are OVERLAYS on real on-disk files, and they have to be: the whole gesture is "which
# module exports this name?", which is answered from the filesystem and the module resolver, so an
# invented path resolves nothing and every fixture would pass by returning an empty list.
#
# CAW is `tests/query/ws/src/app.kama` WITHOUT its import — the case the module campaign created, where
# `Widget` is a sibling in this file's own module (`wsproj`) and needed no import before. The offered fix
# is the BARE spelling, and that spelling was checked by building the project with it, not by reading the
# grammar.
CAWURI=$(furi "$ROOT/tests/query/ws/src/app.kama")
CAW='fn Widget make(int32 n) {\n    Widget w = Widget.of(size: n);\n    return w;\n}\n'
# The other half: a name from another module entirely, which the diagnostic canNOT name (it says only
# "unknown type `DynamicArray`"), so the fix has to search for it. Two spellings of the same buffer —
# without an import block and with one — because they take the two different edits, and the second is
# the one that must not produce a SECOND `import` statement: two blocks is a parse error, so an edit
# that appended one would hand back a file that no longer compiles.
CANOBLK='fn int32 useit() {\n    DynamicArray<int32> a = DynamicArray.empty();\n    return 0;\n}\n'
CABLK='import { std::collections::View };\nfn int32 useit() {\n    DynamicArray<int32> a = DynamicArray.empty();\n    return 0;\n}\n'

# Go-to-definition on the PRELUDE. Also an overlay on a real path, because the answer is a real file the
# driver resolves relative to the compiler binary.
PRELB='fn Optional<int32> pick() {\n    return Optional::None;\n}\n'

# A compiler BUILT-IN, which unlike the prelude has no source anywhere — `string` is a reserved word, not
# a declaration. LSP L1 char 4 = the `string` annotation IN A BODY (the road that needed indexing at all);
# L2 char 13 = the `.length()` call, an intrinsic with no declaration node of its own.
# ⚠️ No string LITERAL in here: every buffer on this page is inlined into a JSON body, so a `"` would end
# the string and the server would answer questions about a file that never parsed.
BLTB='fn isize measure(string a) {\n    string s = a;\n    return s.length();\n}\n'

# M3.4 fixture: one of each binding kind, each WITH the trailing syntax whose span used to be swallowed.
# The prepareRename ranges below are the DATA-LOSS GUARD — rename replaces the range it is given, so a
# range that ran past the name would rewrite `seeded = 7` (or `Code::Ok`, or `Bad = 2`) as the new name.
# LSP 0-based lines/chars:
#   L1 `type enum Code { Ok, Bad = 2 }`                -> `Bad` 21..24  (NOT 21..28)
#   L3 `    public int32 scale = 3;`                   -> `scale` 17..22 (NOT 17..26)
#   L4 `    public fn int32 twice(int32 bias) { … }`   -> `bias` 32..36 (NOT 26..36, which starts at the type)
#      ... its body `this.scale` -> `scale` at 52..57 (NOT 47.., which starts at the receiver)
#   L7 `    int32 seeded = 7;`                         -> `seeded` 10..16 (NOT 10..20)
#   L8 `    Code c = Code::Ok;`                        -> `Ok` 19..21 (NOT 13..21, which eats `Code::`)
MURI="file:///bindings.kama"
M34='type enum Code { Ok, Bad = 2 }\ntype value Cfg {\n    public int32 scale = 3;\n    public fn int32 twice(int32 bias) { return this.scale * bias; }\n}\nfn int32 run() {\n    int32 seeded = 7;\n    Code c = Code::Ok;\n    return seeded + cast<int32>(c);\n}\n'

# M6 A2 fixture: a named-argument LABEL, single-file so it is renameable under the open-file rule. The
# span is the whole point — rename REPLACES the range it is handed, so a span running past the label would
# eat the argument expression with it. Layout (LSP 0-based lines, 0-based chars):
#   L0 `fn int32 add(int32 lhs, int32 rhs) { return lhs + rhs; }` -> `lhs` decl 19..22, body use 44..47
#   L2 `    return add(lhs: 1, rhs: 2);`                          -> the `lhs` LABEL 15..18, NOT 15..21
LURI="file:///labels.kama"
LSRC='fn int32 add(int32 lhs, int32 rhs) { return lhs + rhs; }\nfn int32 useIt() {\n    return add(lhs: 1, rhs: 2);\n}\n'

# M3.5 dependency fixture: a real installed path dependency, so `.kama/deps` is populated. Built here
# rather than committed — `.kama/deps` is install output, and a path dep needs no network. The guard under
# test is that rename REFUSES a symbol whose definition lives in a dependency: `DefSite.unit == nullptr`
# filters only the built-in prelude, so a dep's units look like ordinary user code and would otherwise be
# rewritten. Previously this guard was only ever exercised against std.
dep="$tmp/depproj"
mkdir -p "$dep/geo/src" "$dep/app/src"
cat > "$dep/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$dep/geo/src/geo.kama" <<'KAMA'
export { Point };
type value Point {
    public int32 x;
    public ctor of(int32 x) { this.x = x; }
}
KAMA
cat > "$dep/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "dependencies": { "geo": { "path": "../geo" } }, "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$dep/app/src/app.kama" <<'KAMA'
import { geo::Point };
fn int32 main() {
    Point p = Point.of(x: 7);
    return p.x;
}
KAMA
depok=0
(cd "$dep/app" && "$KAMA" pkg install >/dev/null 2>&1) && depok=1

# NOTE — a fixture that USED to live here. It declared `"sources": [".", "../shared"]` and proved that
# ownership is the project's FILE SET rather than a path prefix, in the OUTWARD direction: a declared
# source outside the project directory is still ours to rename. `source` is now ONE subdirectory, so a
# project cannot reach outside itself at all and that shape is unrepresentable by design. The invariant
# is unchanged and `ownsFile` still implements it; only the inward direction is testable now, which the
# `depproj` fixture above covers (a dependency INSIDE the root that must never be rewritten). Do not
# re-add the outward case without a `source` that can express it.

# 2d fixture: ONE project, TWO modules, and a cross-module import between them. This is the case §2i
# leaves to the editor alone: `kama check src/alpha/a.kama` is a LOOSE build — the operand's basename is
# the mode — so it has no manifest and cannot resolve `twomod::beta`, while the server walks up from the
# buffer to the project and does. The CLI half is asserted as a NEGATIVE CONTROL below, because "the
# editor reports no diagnostics" is worth nothing if the import was never the hard part.
tm="$tmp/twomod"
mkdir -p "$tm/src/alpha" "$tm/src/beta"
cat > "$tm/kama.json" <<'JSON'
{ "name": "twomod", "version": "0.1.0", "kind": "library",
  "modules": { ".":     { "visibility": "internal" },
               "alpha": { "visibility": "public" },
               "beta":  { "visibility": "public" } } }
JSON
printf 'export { bval };\nfn int32 bval() { return 41; }\n' > "$tm/src/beta/b.kama"
TMSRC='import { twomod::beta::bval };\nexport { aval };\nfn int32 aval() { return bval() + 1; }\n'
printf "$TMSRC" > "$tm/src/alpha/a.kama"
TMURI=$(furi "$tm/src/alpha/a.kama")
tmcli=$("$KAMA" check "$tm/src/alpha/a.kama" 2>&1 || true)

# M6 B3c fixture: a project resource implementing a STD contract. `write` here and `Writer.write` in
# lib/std/io/streams.kama are ONE renameable name, and the contract's half is not ours to rewrite — so
# rename must refuse outright rather than rewrite the implementation and silently break conformance.
# Before B3c the two were unrelated symbols and this rename went through.
# Layout (LSP 0-based): L4 `    public fn Result<usize, IoError> write(...` -> `write` at 37..42.
mkdir -p "$tmp/impl/src"
cat > "$tmp/impl/kama.json" <<'JSON'
{ "name": "impl", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$tmp/impl/src/sink.kama" <<'KAMA'
import { std::io::Writer, std::io::IoError, std::collections::ConstView };
type resource Sink implements Writer {
    int32 n;
    public fn Result<usize, IoError> write(ConstView<uint8> bytes) { this.n = 1; return Result::Ok(value: cast<usize>(this.n)); }
    public fn Result<Unit, IoError> flush() { return Result::Ok(value: Unit::Unit); }
}
KAMA
CIURI=$(furi "$tmp/impl/src/sink.kama")
CISRC='import { std::io::Writer, std::io::IoError, std::collections::ConstView };\ntype resource Sink implements Writer {\n    int32 n;\n    public fn Result<usize, IoError> write(ConstView<uint8> bytes) { this.n = 1; return Result::Ok(value: cast<usize>(this.n)); }\n    public fn Result<Unit, IoError> flush() { return Result::Ok(value: Unit::Unit); }\n}\n'

DURI=$(furi "$dep/app/src/app.kama")
DSRC='import { geo::Point };\nfn int32 main() {\n    Point p = Point.of(x: 7);\n    return p.x;\n}\n'

# M6 A3 fixture: a FREE-RIDING workspace member. `libs/net` imports `config` without declaring it — so
# net builds where it sits and nowhere else, and nothing in an editor said so. A build
# makes this a hard error; the editor path stays lenient (refusing to analyze over a *manifest* problem
# would strip cross-module hover and definitions while the code itself resolves fine), so the finding has
# to arrive as a DIAGNOSTIC on the import statement instead.
frws="$tmp/frws"
mkdir -p "$frws/libs/net/src" "$frws/libs/config/src"
cat > "$frws/kama_workspace.json" <<'JSON'
{ "projects": { "libs/*": { "optional": false } } }
JSON
cat > "$frws/libs/config/kama.json" <<'JSON'
{ "name": "config", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$frws/libs/config/src/config.kama" <<'KAMA'
export { limit };
fn int32 limit() { return 5; }
KAMA
printf 'import { config::limit };\nexport { cap };\nfn int32 cap() { return limit(); }\n' > "$frws/libs/net/src/net.kama"
# Two steps, because the check only fires for an import that RESOLVES through a dependency view: declare
# `config` and install (which materializes net/.kama/deps), then remove the declaration while the view
# remains. That is a real editing state — someone dropped the line from the manifest — and it is the state
# in which the editor must speak up, since the code still resolves and builds where it sits.
cat > "$frws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library",
  "dependencies": { "config": { "path": "../config" } }, "modules": { ".": { "visibility": "public" } } }
JSON
frok=0
"$KAMA" pkg install "$frws/libs/net" >/dev/null 2>&1 && frok=1
cat > "$frws/libs/net/kama.json" <<'JSON'
{ "name": "net", "version": "0.1.0", "kind": "library", "modules": { ".": { "visibility": "public" } } }
JSON
FRURI=$(furi "$frws/libs/net/src/net.kama")
FRSRC='import { config::limit };\nexport { cap };\nfn int32 cap() { return limit(); }\n'
FRSRC2='import { config::limit };\nexport { cap };\nfn int32 cap() { return limit() + 0; }\n'

frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file://'"$ROOT"'/tests/query","capabilities":{}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$URI"'","languageId":"kama","version":1,"text":"'"$BAD"'"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$URI"'","version":2},"contentChanges":[{"text":"'"$GOOD"'"}]}}'
# --- M2: open the decl-rich buffer, then query it ---
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$QURI"'","languageId":"kama","version":1,"text":"'"$SHP"'"}}}'
frame '{"jsonrpc":"2.0","id":3,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$QURI"'"}}}'
frame '{"jsonrpc":"2.0","id":4,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":3}}}'
frame '{"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":13}}}'
# A body use-site ("a" in `return a`, kama 3:31 -> LSP 2:31) resolves to null BY DESIGN (M0 indexes decls +
# signature type refs only; body use-sites are the M3 find-references walk).
frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":31}}}'
# --- M3: find-references + rename over the same buffer ---
# 8:  references on the Point DECL name (LSP 1:11), includeDeclaration -> decl + both signature refs + both body refs.
# 9:  the same query with includeDeclaration:false -> the decl's own range must be absent.
# 10: references from a BODY use (LSP 3:17) -> the same set (a use and its decl resolve to one key).
# 11: references on the `mid` call site (LSP 3:51) -> the fn decl + that call (the resolveFunc hook).
# 12: prepareRename on the Point decl -> its identifier range.
# 13: prepareRename on the local `a` (LSP 2:31) -> null (locals are M3.4).
# 14: rename Point -> Pnt: a WorkspaceEdit with one TextEdit per reference, all in this file.
# 15: rename to a KEYWORD must be refused (the lexer's own table decides, via kamaIsKeyword).
frame '{"jsonrpc":"2.0","id":8,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":0,"character":11},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":9,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":0,"character":11},"context":{"includeDeclaration":false}}}'
frame '{"jsonrpc":"2.0","id":10,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":17},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":11,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":51},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":12,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":0,"character":11}}}'
frame '{"jsonrpc":"2.0","id":13,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":1,"character":31}}}'
frame '{"jsonrpc":"2.0","id":14,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":0,"character":11},"newName":"Pnt"}}'
frame '{"jsonrpc":"2.0","id":15,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":0,"character":11},"newName":"return"}}'
# --- module loading: open the import-using file; its imports resolve, so diagnostics are empty and
#     go-to-def on DynamicArray (kama 3:15 -> LSP 2:15) jumps into the std::collections source ---
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$IURI"'","languageId":"kama","version":1,"text":"'"$IMP"'"}}}'
frame '{"jsonrpc":"2.0","id":7,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":15}}}'
# 16: the M3.3 SAFETY GUARD. DynamicArray is declared in std and used across several std files, so a rename
#     here would rewrite files the user can't see — and the loaded-unit set still isn't every user of the
#     symbol. Rename must REFUSE rather than half-rewrite. (Lifted by workspace indexing in M3.5.)
frame '{"jsonrpc":"2.0","id":16,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":15},"newName":"Foo"}}'
# --- SEMANTIC diagnostics: an undeclared type in a BODY. Everything above asserts on a PARSE error,
#     because semantic diagnostics used to be too incomplete to test — an unknown body type resolved to
#     nothing and was emitted verbatim, so `kama check` said OK and the editor showed a clean file that
#     then failed in the C compiler. checkTypeResolves closes that; this asserts the squiggle is live.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$SURI"'","languageId":"kama","version":1,"text":"'"$SEM"'"}}}'
# --- M3.4: locals / params / fields / enum members ---
# 20-24: prepareRename on each kind, asserting the EXACT range (the data-loss guard described above).
# 25:    references on the field `scale` -> its `this.scale` use, proving the member-access span is the
#        name and not the receiver. 26: a full rename of a local. 27: hover reports the binding kind.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$MURI"'","languageId":"kama","version":1,"text":"'"$M34"'"}}}'
frame '{"jsonrpc":"2.0","id":20,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":6,"character":12}}}'
frame '{"jsonrpc":"2.0","id":21,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":3,"character":33}}}'
frame '{"jsonrpc":"2.0","id":22,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":2,"character":18}}}'
frame '{"jsonrpc":"2.0","id":23,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":0,"character":22}}}'
frame '{"jsonrpc":"2.0","id":24,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":7,"character":20}}}'
frame '{"jsonrpc":"2.0","id":25,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":2,"character":18},"context":{"includeDeclaration":false}}}'
frame '{"jsonrpc":"2.0","id":26,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":6,"character":12},"newName":"total"}}'
frame '{"jsonrpc":"2.0","id":27,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$MURI"'"},"position":{"line":8,"character":13}}}'
# --- M3.5: workspace indexing. Open widget.kama (a REAL on-disk file inside tests/query/ws, which holds
#     a kama.json) and rename `Widget`. rootUri is tests/query and the nearest manifest below it is ws/,
#     so the project is exactly those two files — and the rewrite must reach app.kama, which widget.kama
#     does NOT import. 28: references span both files. 29: the multi-file WorkspaceEdit. 30: a project-wide
#     symbol search. 31: didChangeWatchedFiles is accepted (a notification, so silence == success; the
#     assertion is that it does not come back as "method not found").
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$WWURI"'","languageId":"kama","version":1,"text":"'"$WW"'"}}}'
frame '{"jsonrpc":"2.0","id":28,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$WWURI"'"},"position":{"line":1,"character":11},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":29,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$WWURI"'"},"position":{"line":1,"character":11},"newName":"Gadget"}}'
frame '{"jsonrpc":"2.0","id":30,"method":"workspace/symbol","params":{"query":"efaultSi"}}'
frame '{"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":"'"$WWURI"'","type":2}]}}'
# --- M3.5: an installed DEPENDENCY. 32: rename on a type declared in .kama/deps must REFUSE (it is not
#     ours to rewrite). 33: the dep's symbols must not show up in the project symbol picker either.
if [ "$depok" = 1 ]; then
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$DURI"'","languageId":"kama","version":1,"text":"'"$DSRC"'"}}}'
frame '{"jsonrpc":"2.0","id":32,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$DURI"'"},"position":{"line":2,"character":4},"newName":"Pt"}}'
frame '{"jsonrpc":"2.0","id":33,"method":"workspace/symbol","params":{"query":"Point"}}'
fi
# --- 2d: a cross-MODULE import inside one project. No id: the assertion is the published diagnostics.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$TMURI"'","languageId":"kama","version":1,"text":"'"$TMSRC"'"}}}'
# --- M4: completion + signature help, over the M2 decl-rich buffer (`$SHP`, still open).
#     Line 4 is `fn int32 use() { slot Point p; Point q = mid(a: p); return q.x; }` (LSP line 3):
#       char 56 = the `x` of `q.x`  -> a Dot trigger on a `Point` local
#       char 40 = the `a` of `mid(` -> an argument slot with no label yet
#       char 17 = the `P` of the first `Point` -> a bare position
#     17: member completion.  18: argument-LABEL completion.  19: signature help, active parameter 0.
#     35: bare completion.    36: signature help outside any call -> null.
frame '{"jsonrpc":"2.0","id":17,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":71}}}'
frame '{"jsonrpc":"2.0","id":18,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":55}}}'
frame '{"jsonrpc":"2.0","id":19,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":55}}}'
frame '{"jsonrpc":"2.0","id":35,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":73}}}'
frame '{"jsonrpc":"2.0","id":36,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"'"$QURI"'"},"position":{"line":2,"character":73}}}' 
# --- M4.6: a buffer that never parsed. Completion repairs by blanking the CURSOR'S LINE and
#     re-analyzing — the lexical context still comes from the untouched text, so `p.` is not lost.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$NURI"'","languageId":"kama","version":1,"text":"'"$NEWB"'"}}}'
frame '{"jsonrpc":"2.0","id":37,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$NURI"'"},"position":{"line":3,"character":6}}}'
frame '{"jsonrpc":"2.0","id":38,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$NURI"'"},"position":{"line":3,"character":4}}}' 
# --- M4.7: import paths, over the imports.kama buffer, whose line 0 is
#     `import { std::collections::DynamicArray };`. ⚠️ The columns MOVED when the scope went inside the
#     braces: `import { ` is nine characters where `import ` was seven. char 14 is after `std::` (a module
#     position), char 31 is inside the symbol.
frame '{"jsonrpc":"2.0","id":39,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":14}}}'
frame '{"jsonrpc":"2.0","id":40,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":31}}}' 
# --- M4.9: the stamped spans. 41: a GENERIC type's decl name must stop before `<T>`. 42: a named ctor's
#     name spans just the name, not the declarator.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$SPURI"'","languageId":"kama","version":1,"text":"'"$SPAN"'"}}}'
frame '{"jsonrpc":"2.0","id":41,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$SPURI"'"},"position":{"line":0,"character":11}}}'
frame '{"jsonrpc":"2.0","id":42,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$SPURI"'"},"position":{"line":1,"character":32}}}'
# --- M5.2: the parse cache must be INVISIBLE. Every request above already ran against a warm cache
#     (the server enables it for its whole life), so 43/44 pin the two ways it could go wrong and not
#     be noticed: a stale entry surviving an eviction, and a served unit carrying the WRONG PATH
#     SPELLING. The cache is keyed by the spelling parseFile was handed, because a unit is named by
#     that string and unitForUri matches names exactly — key it by absolute path instead and one file
#     reachable by two spellings starts answering with the other's name, silently rewriting every
#     go-to-def URI. Both requests repeat id 7's query, so the expected Location is identical.
frame '{"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":"'"$IURI"'","type":2}]}}'
frame '{"jsonrpc":"2.0","id":43,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":15}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$IURI"'","version":2},"contentChanges":[{"text":"'"$IMP"'"}]}}'
frame '{"jsonrpc":"2.0","id":44,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":15}}}'
# --- M5.3/M5.4: error recovery reaches the editor. 45: the outline off a PARTIAL parse. 46: hover on a
#     type whose own body contained the error, proving the index is live rather than a stale last-good.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$RURI"'","languageId":"kama","version":1,"text":"'"$RECOV"'"}}}'
frame '{"jsonrpc":"2.0","id":45,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$RURI"'"}}}'
frame '{"jsonrpc":"2.0","id":46,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$RURI"'"},"position":{"line":0,"character":11}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$XURI"'","languageId":"kama","version":1,"text":"'"$XDROP"'"}}}'
# --- M6 A2: named-argument labels. 49 = prepareRename ON THE LABEL (the span guard); 53 = rename from the
#     PARAMETER (must reach the label); 54 = references from the label's own position.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$LURI"'","languageId":"kama","version":1,"text":"'"$LSRC"'"}}}'
frame '{"jsonrpc":"2.0","id":49,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$LURI"'"},"position":{"line":2,"character":15}}}'
frame '{"jsonrpc":"2.0","id":53,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$LURI"'"},"position":{"line":0,"character":19},"newName":"left"}}'
frame '{"jsonrpc":"2.0","id":54,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$LURI"'"},"position":{"line":2,"character":15},"context":{"includeDeclaration":true}}}'
# --- M6 A3: the free-ride finding is a DIAGNOSTIC, and it survives a keystroke. The second didChange is
#     the assertion that matters: the stderr message is warn-once per process, so a diagnostic sharing that
#     lifetime would vanish the moment you typed a character.
if [ "$frok" = 1 ]; then
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$FRURI"'","languageId":"kama","version":1,"text":"'"$FRSRC"'"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$FRURI"'","version":2},"contentChanges":[{"text":"'"$FRSRC2"'"}]}}'
fi
# --- M6 B2: semanticTokens/full, on a ctor-bearing type and on a generic type.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$TOKURI"'","languageId":"kama","version":1,"text":"'"$TOKB"'"}}}'
frame '{"jsonrpc":"2.0","id":55,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"'"$TOKURI"'"}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$TOKGURI"'","languageId":"kama","version":1,"text":"'"$TOKG"'"}}}'
frame '{"jsonrpc":"2.0","id":56,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"'"$TOKGURI"'"}}}'
# --- M6 B3a: RENAMING A METHOD. 57 = prepareRename on the declaration (F2 was already offered before B3 —
#     that is what made this a silent edit rather than a missing feature); 58 = the rename itself, which
#     must reach the CALL SITE; 59 = the same from a generic template, where two instantiations must still
#     leave one symbol.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$MRURI"'","languageId":"kama","version":1,"text":"'"$MREN"'"}}}'
frame '{"jsonrpc":"2.0","id":57,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$MRURI"'"},"position":{"line":2,"character":20}}}'
frame '{"jsonrpc":"2.0","id":58,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$MRURI"'"},"position":{"line":2,"character":20},"newName":"fetch"}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$MGURI"'","languageId":"kama","version":1,"text":"'"$MGEN"'"}}}'
frame '{"jsonrpc":"2.0","id":59,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$MGURI"'"},"position":{"line":2,"character":17},"newName":"fetch"}}'
# --- M6 B3f: rename the enum from its DECLARATION and require the `Color::` qualifier among the edits;
#     then go-to-definition and hover FROM the qualifier, which had no position to click on at all.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$QRURI"'","languageId":"kama","version":1,"text":"'"$QREN"'"}}}'
frame '{"jsonrpc":"2.0","id":60,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$QRURI"'"},"position":{"line":0,"character":10},"newName":"Hue"}}'
frame '{"jsonrpc":"2.0","id":61,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$QRURI"'"},"position":{"line":1,"character":30}}}'
frame '{"jsonrpc":"2.0","id":62,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$QRURI"'"},"position":{"line":1,"character":30}}}'
# --- M6 B3f: a module PATH segment. `$IURI` was opened above with the COMPACT $IMP buffer (three lines),
#     not the ten-line file on disk — take the coordinates from $IMP:
#     L1 `import { std::collections::DynamicArray };` -> `std` at 7, `collections` at 12.
frame '{"jsonrpc":"2.0","id":63,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":19}}}'
frame '{"jsonrpc":"2.0","id":64,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":19}}}'
frame '{"jsonrpc":"2.0","id":65,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":19}}}'
# --- M6 B3c: a project type implementing a STD contract. F2 on its `write` must REFUSE — the contract's
#     own declaration lives in lib/std and is not ours to rewrite, and renaming only our half would leave
#     the type no longer satisfying Writer. 66: prepareRename still OFFERS (it is a real symbol here).
#     67: the rename itself is refused, by the group-wide ownership check.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$CIURI"'","languageId":"kama","version":1,"text":"'"$CISRC"'"}}}'
frame '{"jsonrpc":"2.0","id":66,"method":"textDocument/references","params":{"textDocument":{"uri":"'"$CIURI"'"},"position":{"line":3,"character":37},"context":{"includeDeclaration":true}}}'
frame '{"jsonrpc":"2.0","id":67,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$CIURI"'"},"position":{"line":3,"character":37},"newName":"emit"}}'
# --- contract model M4: completion inside a `type intrinsic` body, in BOTH member lists. Character
#     positions are the cursor sitting just after the `pe` in `cast<int32>(pe` on each line.
#     69: the shared body -> its param `wpeer` and its local `wshared`.  70: the `<float32>` SECTION ->
#     `wsection`, which exists in no other body, so it can only come from the section's own member list.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$IIURI"'","languageId":"kama","version":1,"text":"'"$IIB"'"}}}'
frame '{"jsonrpc":"2.0","id":69,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$IIURI"'"},"position":{"line":2,"character":95}}}'
frame '{"jsonrpc":"2.0","id":70,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$IIURI"'"},"position":{"line":5,"character":109}}}'
# --- auto-import. 71: a SIBLING in the file's own module -> the bare spelling, and a whole new block.
#     72: a name from another module, which the diagnostic cannot name -> the search finds it. 73: the
#     same into a file that ALREADY has a block -> one entry joins it, and the range must be ZERO-WIDTH
#     (a one-character range REPLACES, which is how this first shipped: it ate the `std` it aimed at).
#     74: a clean buffer offers nothing.
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$CAWURI"'","languageId":"kama","version":1,"text":"'"$CAW"'"}}}'
frame '{"jsonrpc":"2.0","id":71,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"'"$CAWURI"'"},"range":{"start":{"line":1,"character":4},"end":{"line":1,"character":4}},"context":{"diagnostics":[]}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$IURI"'","version":3},"contentChanges":[{"text":"'"$CANOBLK"'"}]}}'
frame '{"jsonrpc":"2.0","id":72,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"'"$IURI"'"},"range":{"start":{"line":1,"character":4},"end":{"line":1,"character":4}},"context":{"diagnostics":[]}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$IURI"'","version":4},"contentChanges":[{"text":"'"$CABLK"'"}]}}'
frame '{"jsonrpc":"2.0","id":73,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"'"$IURI"'"},"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":4}},"context":{"diagnostics":[]}}}'
frame '{"jsonrpc":"2.0","id":74,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"'"$QURI"'"},"range":{"start":{"line":0,"character":0},"end":{"line":2,"character":0}},"context":{"diagnostics":[]}}}'
# --- go-to-definition on the PRELUDE. 75: F12 on `Optional` opens prelude/global.kama. 76/77: F2 on the
#     same name is still refused — the prelude is openable, never renameable, and those are two different
#     questions that a single `unit == nullptr` test used to answer with one "no".
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$IURI"'","version":5},"contentChanges":[{"text":"'"$PRELB"'"}]}}'
frame '{"jsonrpc":"2.0","id":75,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":3}}}'
frame '{"jsonrpc":"2.0","id":76,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":3}}}'
frame '{"jsonrpc":"2.0","id":77,"method":"textDocument/rename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":0,"character":3},"newName":"Maybe"}}'
# --- ...and on a compiler BUILT-IN, which has no source anywhere and so gets a documentation file
#     (prelude/builtin.kama) the way Go's `builtin.go` and Rust's `primitive_docs.rs` do. 78: the type.
#     79: an intrinsic METHOD, which has no declaration node either. tools/check-builtin-doc.sh is what
#     holds that file against the C++ registrations; these two prove the editor actually reaches it.
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$IURI"'","version":6},"contentChanges":[{"text":"'"$BLTB"'"}]}}'
frame '{"jsonrpc":"2.0","id":78,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":4}}}'
frame '{"jsonrpc":"2.0","id":79,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":2,"character":13}}}'
frame '{"jsonrpc":"2.0","id":80,"method":"textDocument/prepareRename","params":{"textDocument":{"uri":"'"$IURI"'"},"position":{"line":1,"character":4}}}'
frame '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
frame '{"jsonrpc":"2.0","method":"exit"}'

out=$("$KAMA" lsp < "$session" 2>/dev/null || true)

fail=0
# expect <substring> <description>: assert the server's framed stdout contains <substring>.
expect() {
    if printf '%s' "$out" | grep -qF -- "$1"; then
        echo "  ok: $2"
    else
        echo "  FAIL: $2 — expected substring: $1" >&2
        fail=1
    fi
}

echo "check-lsp: lifecycle + live diagnostics over stdio"
expect '"capabilities"'                              "initialize -> capabilities"
expect '"textDocumentSync":1'                        "advertises full-document sync"
expect '"hoverProvider":true'                        "advertises hoverProvider (M2)"
expect '"definitionProvider":true'                   "advertises definitionProvider (M2)"
expect '"documentSymbolProvider":true'               "advertises documentSymbolProvider (M2)"
expect '"referencesProvider":true'                   "advertises referencesProvider (M3)"
expect '"renameProvider":{"prepareProvider":true}'   "advertises renameProvider with prepareProvider (M3)"
expect '"completionProvider"'                        "advertises completionProvider (M4)"
expect '"triggerCharacters":[".",":"]'               "... triggered by . and :"
expect '"signatureHelpProvider":{"triggerCharacters":["(",","]}' "advertises signatureHelpProvider (M4)"
expect '"codeActionProvider":{"codeActionKinds":["quickfix"]}'   "advertises codeActionProvider for quickfix (auto-import)"
expect '"method":"textDocument/publishDiagnostics"'  "server publishes diagnostics"
expect '"message":"syntax error'                     "syntax error surfaced on the bad buffer"
expect '"start":{"line":2,"character":0}'            "error range mapped to LSP 0-based (kama 3:0 -> 2:0)"
expect '"diagnostics":[]'                            "didChange to a valid buffer clears the squiggles"

echo "check-lsp: M2 interactive features (hover / go-to-definition / document symbols)"
expect '"id":3,"result":[{"name":"Point","kind":5'            "documentSymbol: Point value -> SymbolKind.Class(5)"
expect '"selectionRange":{"start":{"line":0,"character":11}'  "documentSymbol: Point name range at LSP 1:11"
expect '"name":"mid","kind":12'                               "documentSymbol: mid -> SymbolKind.Function(12)"
expect '"id":4,"result":{"uri":"file:///shapes.kama"'         "definition: type ref -> file:// Location"
expect '"range":{"start":{"line":1,"character":11}'           "definition: lands on the Point decl name (LSP 1:11)"
expect '"id":5,"result":{"contents":{"kind":"plaintext","value":"value Point"}}'  "hover: 'value Point' on a type ref"
expect '"id":6,"result":{"contents":{"kind":"plaintext","value":"param a"}}'      "hover: 'param a' on a parameter use-site (M3.4)"

echo "check-lsp: M3 find-references"
expect '"id":8,"result":[{"uri":"file:///shapes.kama"'        "references: returns Locations in this file"
expect '"start":{"line":2,"character":17}'                    "references: BODY use-site 'Point p' (LSP 2:17) is indexed"
expect '"start":{"line":2,"character":41}'                    "references: BODY use-site 'Point q' (LSP 2:41) is indexed"
expect '"id":11,"result":[{"uri":"file:///shapes.kama","range":{"start":{"line":1,"character":9}' \
                                                              "references: a CALL resolves to the fn decl (resolveFunc hook)"
expect '"start":{"line":2,"character":51}'                    "references: the call site itself (LSP 2:51) is indexed"

echo "check-lsp: M3 rename"
expect '"id":12,"result":{"start":{"line":0,"character":11}'  "prepareRename: the Point identifier range"
expect '"id":13,"result":{"start":{"line":1,"character":31},"end":{"line":1,"character":32}}' \
                                                              "prepareRename: a parameter use is renameable and spans just the name (M3.4)"
expect '"id":14,"result":{"changes":{"file:///shapes.kama":[' "rename: a WorkspaceEdit keyed by this file's URI"
expect '"newText":"Pnt"'                                      "rename: each edit carries the new name"
expect '"id":15,"error"'                                      "rename: a reserved keyword is refused"

echo "check-lsp: module loading (imports resolve across files)"
expect 'imports.kama","diagnostics":[]'   "import-using file analyzes clean (std::collections loaded; no false 'does not export')"
expect 'dynamic_array.kama'               "cross-module go-to-def resolves DynamicArray into the std source"
expect '"id":16,"error"'                             "rename still REFUSES a symbol we do not own"
expect 'this name is also declared outside the project'   "...and now says WHY: DynamicArray is declared in std"

echo "check-lsp: M3.4 bindings (locals / params / fields / enum members)"
# Each of these asserts the FULL range. An `end` past the name is the data-loss bug the M3.4 grammar
# pass fixed — rename replaces this range verbatim, so a wrong end silently eats the initializer/value.
expect '"id":20,"result":{"start":{"line":6,"character":10},"end":{"line":6,"character":16}}' \
                                          "prepareRename: local 'seeded' spans the NAME, not 'seeded = 7'"
expect '"id":21,"result":{"start":{"line":3,"character":32},"end":{"line":3,"character":36}}' \
                                          "prepareRename: param 'bias' starts at the name, not at its type"
expect '"id":22,"result":{"start":{"line":2,"character":17},"end":{"line":2,"character":22}}' \
                                          "prepareRename: field 'scale' spans the NAME, not 'scale = 3'"
expect '"id":23,"result":{"start":{"line":0,"character":21},"end":{"line":0,"character":24}}' \
                                          "prepareRename: enum member 'Bad' spans the NAME, not 'Bad = 2'"
expect '"id":24,"result":{"start":{"line":7,"character":19},"end":{"line":7,"character":21}}' \
                                          "prepareRename: a 'Code::Ok' use spans 'Ok', not the qualifier too"
expect '"id":25,"result":[{"uri":"file:///bindings.kama","range":{"start":{"line":3,"character":52}' \
                                          "references: 'this.scale' is a field use at the NAME, not the receiver"
expect '"id":26,"result":{"changes":{"file:///bindings.kama":['  "rename: a local produces a WorkspaceEdit"
expect '"newText":"total"'                                       "rename: the local's edits carry the new name"
expect '"id":27,"result":{"contents":{"kind":"plaintext","value":"local seeded"}}' \
                                          "hover: 'local seeded' on a local use-site"

echo "check-lsp: semantic diagnostics (not just parse errors)"
expect 'unknown type `Nonexistent`'                        "undeclared body type surfaces as a live diagnostic"
expect 'sem.kama","diagnostics":[{"range":{"start":{"line":1'  "the squiggle lands on the decl line (kama 2 -> LSP 1)"

echo "check-lsp: M3.5 workspace indexing (cross-file rename)"
expect '"workspaceSymbolProvider":true'                 "advertises workspaceSymbolProvider (M3.5)"
expect '"workspace":{"workspaceFolders":{"supported":true}}' \
                                                        "advertises workspaceFolders support (M3.5)"
# THE MILESTONE: app.kama is not in widget.kama's import closure, so every one of these would be missing
# without the project-wide unit set.
expect '"id":28,"result":[{"uri":"'"$(furi "$ROOT/tests/query/ws/src/widget.kama")"'"' \
                                                        "references: starts with the declaring file"
expect '/tests/query/ws/src/app.kama","range":{"start":{"line":4,"character":3}' \
                                                        "references REACH app.kama, which widget.kama does not import"
expect '"id":29,"result":{"changes":{"'"$(furi "$ROOT/tests/query/ws/src/app.kama")"'":[' \
                                                        "rename: the WorkspaceEdit rewrites the OTHER file too"
# M6 B3f: the declaring file's edits now START with the `export { Widget, … };` mention, which sorts before
# the declaration. Until the grammar carried per-segment positions the export manifest was not a reference
# at all, so this rename left the module exporting a name that no longer existed — it broke a file it had
# just edited, the same class of silent under-apply as B3a.
expect '/tests/query/ws/src/widget.kama":[{"range":{"start":{"line":0,"character":9},"end":{"line":0,"character":15}},"newText":"Gadget"}' \
                                                        "rename: ... and the export manifest of the declaring file (B3f)"
expect '{"range":{"start":{"line":1,"character":11},"end":{"line":1,"character":17}},"newText":"Gadget"}' \
                                                        "rename: ... and the declaration itself, keyed separately"
expect '"newText":"Gadget"'                             "rename: the cross-file edits carry the new name"
expect '"id":30,"result":[{"name":"defaultSize","kind":12' \
                                                        "workspace/symbol: substring search finds a project symbol"
expect '"id":30,'                                       "workspace/symbol: answered (not method-not-found)"
# didChangeWatchedFiles is a notification: the proof it is handled is that no error came back for it.
if printf '%s' "$out" | grep -qF 'method not found: workspace/'; then
    echo "  FAIL: a workspace/* method was rejected as unknown" >&2
    fail=1
else
    echo "  ok: workspace/didChangeWatchedFiles is accepted (index invalidation, not an error)"
fi

if [ "$depok" = 1 ]; then
    echo "check-lsp: M3.5 installed dependency"
    expect '"id":32,"error"'                          "rename REFUSES a type declared in a dependency"
    expect 'declared outside the project'             "...naming the dependency source it lives in"
    expect '/.kama/deps/geo/src/geo.kama'                 "...which is under the project's package store"
    expect '"id":33,"result":[]'                      "workspace/symbol does not offer a dependency's symbols"
else
    echo "  SKIP: installed-dependency checks (kama pkg install failed)"
fi

echo "check-lsp: a project's own modules resolve for a buffer, not just for a build"
expect '"uri":"'"$TMURI"'","diagnostics":[]}' \
       "a file importing ANOTHER MODULE of its own project analyzes clean"
case "$tmcli" in
    *"cannot resolve module 'twomod::beta'"*)
        echo "  ok: ...and the same file as a LOOSE operand cannot, which is what makes that assertion mean something" ;;
    *)  echo "  FAIL: the negative control did not fire — a loose \`kama check\` of that file said:" >&2
        printf '%s\n' "$tmcli" | head -2 | sed 's/^/      /' >&2
        fail=1 ;;
esac

echo "check-lsp: M6 B2 semantic tokens"
# The legend's ORDER is the wire format — a token's type is sent as an INDEX into it, so this assertion is
# not cosmetic: reorder the array and every buffer in every client silently recolours.
expect '"semanticTokensProvider":{"legend":{"tokenTypes":["class","struct","interface","enum","enumMember","function","method","property","variable","parameter"],"tokenModifiers":["declaration"]},"full":true}' \
       "initialize advertises semanticTokensProvider with the legend, full-only"
# Exact array, because every interesting property of the encoding is positional. Decoded, 5 ints per token
# (deltaLine, deltaStartChar, length, type, modifiers), against
#   type value P {\n    int32 x;\n    public ctor make(int32 v) { this.x = v; }\n}\n
#   fn int32 main() { P p = P.make(v: 1); return p.x; }
#   L1c11 P     struct+decl     L2c10 x    property+decl  L3c16 make method+decl
#   L3c27 v     parameter+decl  L3c37 x    property       L3c41 v    parameter
#   L5c9  main  function+decl   L5c18 P    struct         L5c20 p    variable+decl
#   L5c24 P     STRUCT          L5c26 make METHOD         L5c31 v    parameter
#   L5c45 p     variable        L5c47 x    property
# Fourteen tokens, four fewer than when the ctor declared the value it built: `P`, its declarator and
# that declarator's three uses are gone, and `this` is a keyword, so it is not a token at all.
# Four of those carry a milestone. ONE token at `P`'s decl name, though `_positions` holds two entries
# there (the ctor's implicit result type resolves through the class's own decl identifier, and the existing
# de-duplication lives only in `_refIndex`) — the protocol forbids overlap, so the facade's own filter is
# what makes that true. `v` at L5c31 is an argument LABEL scoped as the callee's PARAMETER (M6 A2).
# `make` at L5c26 is M6 B3a: until B3 a method CALL was in the index for no type at all, so this token did
# not exist and F2 on `make` rewrote the declaration alone. And `P` at L5c24 is B3d — the type RECEIVER of
# a `Type.name(...)` call, which resolved without passing a site while the same spelling in an annotation
# (L5c18) indexed fine.
expect '"id":55,"result":{"data":[0,11,1,1,1,1,10,1,7,1,1,16,4,6,1,0,11,1,9,1,0,10,1,7,0,0,4,1,9,0,2,9,4,5,1,0,9,1,1,0,0,2,1,8,1,0,4,1,1,0,0,2,4,6,0,0,5,1,9,0,0,14,1,8,0,0,2,1,7,0]}}' \
       "semanticTokens/full -> delta-encoded tokens, deduped, non-overlapping, ascending"
# This assertion PINNED the pre-B3 gap as an exact array so that closing it could not be silent. B3 closed
# it, so it changed — which is the whole point. In
#   type value Box<T> {\n    T v;\n    public fn T get() { return this.v; }\n}\n
#   fn int32 main() { slot Box<int32> b; b.v = 7; return b.get(); }
# the tokens are now L1c11 Box (class+decl), L2c6 v (property+decl), L3c16 get (method+decl), L3c36 v
# (property, the `this.v` inside the generic BODY), L5c9 main, L5c18 Box, L5c29/32/48 b, L5c34 v, L5c50 get.
# Before B3 only Box, main, Box and the three `b`s were here: nothing inside a generic body reached the
# index (instances are emitted from emitHeaderContent, before the loop that sets `_refUnit`), and `b.v`
# resolved to `field:Box_int32::v`, a key with no def-site behind it.
#
# `T` still yields NO token, and that is correct rather than a residual gap: inside the instance it is
# substituted to `int32`, a builtin with no def-site, and semanticTokensFor emits nothing for a key it
# cannot resolve. One `v` in the template also yields ONE symbol however many instantiations exist.
expect '"id":56,"result":{"data":[0,11,3,0,1,1,6,1,7,1,0,17,2,6,1,0,5,1,9,1,0,10,1,7,0,0,4,1,9,0,1,16,3,6,1,0,20,1,7,0,2,9,4,5,1,0,9,3,0,0,0,11,1,8,1,0,4,3,0,0,0,13,2,6,0,0,3,1,9,0,0,14,1,8,0,0,2,3,6,0]}}' \
       "a generic type's BODY is indexed — declarations, uses, and one symbol per template (M6 B3b)"

echo "check-lsp: M6 B3a renaming a method"
# F2 was ALREADY offered before B3 — assert it, so the record shows this was never a refusal problem.
expect '"id":57,"result":{"start":{"line":2,"character":20},"end":{"line":2,"character":23}}' \
       "prepareRename on a method declaration spans the name"
# The pair that matters. Before B3 the reply held the FIRST edit and not the second, and the buffer that
# came back no longer compiled.
expect '"id":58,"result":{"changes":{"file:///methodrename.kama":[{"range":{"start":{"line":2,"character":20},"end":{"line":2,"character":23}},"newText":"fetch"}' \
       "renaming a method rewrites its declaration"
expect '{"range":{"start":{"line":4,"character":52},"end":{"line":4,"character":55}},"newText":"fetch"}]}}' \
       "...AND its call site, which was silently left behind before B3a"
# A generic template: the declaration is inside a body that is re-emitted once per instantiation, and the
# call resolves through the INSTANCE. One symbol, so exactly two edits — no duplicate over one range.
expect '"id":59,"result":{"changes":{"file:///genrename.kama":[{"range":{"start":{"line":2,"character":16},"end":{"line":2,"character":19}},"newText":"fetch"},{"range":{"start":{"line":4,"character":108},"end":{"line":4,"character":111}},"newText":"fetch"},{"range":{"start":{"line":4,"character":119},"end":{"line":4,"character":122}},"newText":"fetch"}]}}' \
       "renaming a generic method reaches BOTH instantiations' calls, as one symbol (M6 B3b)"

echo "check-lsp: M6 B3f the :: qualifier of a name"
# The whole edit set, asserted EXACTLY: the declaration, the type annotation, and — new in B3f — the
# `Color::` qualifier. The `Green` after it is the enum MEMBER, a separate symbol, and must not be touched.
expect '"id":60,"result":{"changes":{"file:///qualrename.kama":[{"range":{"start":{"line":0,"character":10},"end":{"line":0,"character":15}},"newText":"Hue"},{"range":{"start":{"line":1,"character":18},"end":{"line":1,"character":23}},"newText":"Hue"},{"range":{"start":{"line":1,"character":28},"end":{"line":1,"character":33}},"newText":"Hue"}]}}' \
       "renaming an enum rewrites the \`Color::\` qualifier, and only the qualifier (M6 B3f)"
expect '"id":61,"result":{"uri":"file:///qualrename.kama","range":{"start":{"line":0,"character":10},"end":{"line":0,"character":15}}}' \
       "go-to-definition FROM a qualifier lands on the enum declaration"
expect '"id":62,"result":{"contents":{"kind":"plaintext","value":"enum Color"}}' \
       "hover on a qualifier names the enum"
# A module PATH is a NAVIGATION target and never a rename target: in kama the namespace is the module path
# is the DIRECTORY path, so renaming one is a file move, not a symbol rename. `module:` keys name no
# def-site, which is exactly what makes prepareRename refuse without needing a new flag — the same line
# clangd draws for `#include` and gopls for an import path.
expect '"id":63,"result":{"uri":"file://'                "go-to-definition on \`collections\` opens the module"
expect '/lib/std/collections/'                           "...the module's own source, not the importer"
expect '"id":64,"result":{"contents":{"kind":"plaintext","value":"module std::collections"}}' \
       "hover on a module path segment names the module"
expect '"id":65,"result":null'                           "prepareRename REFUSES a module path segment"

echo "check-lsp: M6 B3c contract methods are one name with their implementations"
# find-references from the implementation reaches the CONTRACT's declaration in the stdlib, and the other
# implementation of it — that is the group, and it is why the rename below has to refuse.
expect '/lib/std/io/streams.kama","range":{"start":{"line":23,"character":30}' \
       "references from an impl reach the std contract's own declaration"
expect '/lib/std/io/streams.kama","range":{"start":{"line":92,"character":37}' \
       "...and StringWriter, the stdlib's other implementation of it"
expect '"id":67,"error"'                                "rename REFUSES a method that implements a std contract"
expect 'this name is also declared outside the project'  "...because the group straddles the project boundary"
expect '/lib/std/io/streams.kama'                        "...and it names the file it cannot rewrite"

echo "check-lsp: contract model M4 — a type intrinsic body is a callable the queries can see into"
expect '"label":"wpeer"'    "completion inside a type intrinsic body offers the method's parameter"
expect '"label":"wshared"'  "...and a local from the block's SHARED body"
expect '"label":"wsection"' "...and a local from a per-target <…> SECTION's own member list"

echo "check-lsp: M4 completion + signature help"
# The list is complete as sent: `isIncomplete:false` tells the client to filter it itself as the user
# keeps typing, so one `.` costs one request rather than one per character.
expect '"id":17,"result":{"isIncomplete":false,"items":[{"label":"x","kind":5,"detail":"int32"}]}' \
       "completion after a dot -> the receiver's field, as CompletionItemKind.Field (5)"
expect '"id":18,"result":{"isIncomplete":false,"items":[{"label":"a:","kind":10,"detail":"Point"}]}' \
       "completion in an empty argument slot -> the callee's unsupplied LABEL"
expect '"id":19,"result":{"signatures":[{"label":"mid(a: Point) -> Point"' \
       "signatureHelp -> the callee's rendered signature"
expect '"activeParameter":0' "... with the active parameter"
expect '"label":"Point"'     "bare completion -> a type in scope"
expect '"id":36,"result":null' "signatureHelp outside any call -> null"

echo "check-lsp: M4.6 completion on a buffer that has never parsed"
# Without the repair these are both empty — which is the state a NEW file is in, where completion is
# wanted most. The repair blanks only the cursor's line, so line/column geometry is preserved exactly.
expect '"id":37,"result":{"isIncomplete":false,"items":[{"label":"x","kind":5,"detail":"int32"},{"label":"twice"' \
       "a dotted receiver resolves even though the buffer does not parse"
expect '"id":38,"result":{"isIncomplete":false,"items":[{"label":"p","kind":6,"detail":"P"}' \
       "... and the bare position still sees the local"

echo "check-lsp: M4.7 import paths"
expect '{"label":"collections","kind":9}' \
       "import std:: -> the stdlib modules, as CompletionItemKind.Module (9)"
expect '"id":40,' "import ...::{} -> the module export manifest"
expect '{"label":"DynamicArray","kind":7,"detail":"std::collections"}' "... naming the module it comes from"

echo "check-lsp: M4.9 stamped declaration spans"
# Without the type_decl_head stamp this range ended at character 17 — i.e. it covered `Box<T>`, and a
# rename would have replaced the whole thing, deleting `<T>`.
expect '"id":41,"result":{"start":{"line":0,"character":11},"end":{"line":0,"character":14}}' \
       "a generic type decl name spans the NAME, not Name<T>"
expect '"id":42,"result":{"start":{"line":1,"character":30},"end":{"line":1,"character":34}}' \
       "a named ctor spans its name, not the declarator"

echo "check-lsp: M5.2 the parse cache is invisible"
expect '"id":43,' "go-to-def still answers after workspace/didChangeWatchedFiles evicts the cache"
expect '"id":44,' "...and after a didChange re-analyzes off cached units"
# The payload, not just the id: a wrong-spelling cache hit would still answer, just with another
# unit's name in the URI. Both must land in the std source exactly as id 7 did.
for id in 43 44; do
    got=$(printf '%s' "$out" | tr '\r' '\n' | grep -o '"id":'"$id"',"result":{"uri":"[^"]*"' || true)
    case "$got" in
        *dynamic_array.kama*) echo "  ok: id $id resolves into the std source (cache serves the right spelling)" ;;
        *) echo "  FAIL: id $id did not resolve into dynamic_array.kama — got: ${got:-<nothing>}" >&2; fail=1 ;;
    esac
done

echo "check-lsp: M5.3/M5.4 error recovery reaches the editor"
# The headline. Before recovery the parse aborted at error one, so this buffer produced exactly ONE
# diagnostic; all three must now be present, at their own ranges, in a single publishDiagnostics.
expect '"line":2,"character":17' "recovery reports the broken class member (error 1 of 3)"
expect '"line":6,"character":4'  "...and the missing semicolon in fn a (error 2 of 3)"
expect '"line":10,"character":4' "...and the one in fn main, a LATER declaration (error 3 of 3)"
# Count them, so a coincidental substring match elsewhere in the session cannot carry the assertion.
# `head -1` is load-bearing: the claim is "ONE publishDiagnostics carries three", and this buffer can now
# legitimately be republished (a manifest change re-analyzes every open document — M6 A1), which without
# it would count 6 and fail for a reason that has nothing to do with recovery.
n=$(printf '%s' "$out" | tr '\r' '\n' | grep -o 'recover.kama","diagnostics":\[[^]]*\]' | head -1 | grep -o '"code":"Parse"' | wc -l | tr -d ' ')
if [ "${n:-0}" -eq 3 ]; then
    echo "  ok: exactly 3 parse diagnostics published for one buffer (was 1 before recovery)"
else
    echo "  FAIL: expected 3 parse diagnostics on recover.kama, got ${n:-0}" >&2; fail=1
fi
# The other half: a partial parse still yields a LIVE index. The type survives even though the error was
# inside its own body — the member-level arm discards the member, not the shell.
expect '"id":45,"result":[{"name":"H"' "outline still answers off a partially-parsed buffer"
expect '"id":46,"result":{"contents"' "hover still answers on a type whose body held the error"
# ...and the one case where semantics MUST be suppressed: a dropped top-level decl turns every use of
# its name into a false "unknown type". The real parse error stays; the cascade does not.
drop=$(printf '%s' "$out" | tr '\r' '\n' | grep -o 'drop.kama","diagnostics":\[[^]]*\]' || true)
case "$drop" in
    *'unexpected !'*) echo "  ok: the real parse error on a dropped declaration is still reported" ;;
    *) echo "  FAIL: no parse error published for drop.kama — got: ${drop:-<nothing>}" >&2; fail=1 ;;
esac
case "$drop" in
    *'unknown type'*) echo "  FAIL: semantic cascade published from a dropped top-level decl" >&2; fail=1 ;;
    *) echo "  ok: no 'unknown type' cascade from the decl the top-level arm discarded" ;;
esac

echo "check-lsp: M6 A3 the undeclared-import finding reaches the Problems pane"
if [ "$frok" = 1 ]; then
    expect '"code":"undeclared-import"' "a free-riding package's missing declaration is a DIAGNOSTIC, not just stderr"
    expect '"severity":2'               "...as a warning (the code resolves; it is the manifest that is wrong)"
    # On the `import` statement itself. Its module-path segments carry no spans of their own, so statement
    # granularity is the honest limit — but it must be the import line, not the file's first line.
    expect '{"start":{"line":1,"character":0},"end":{"line":1,"character":23}},"severity":2,"code":"undeclared-import"' \
           "...positioned on the import statement, spanning exactly it"
    expect 'add it under \"dependencies\"' "...and it names the remedy"
    # THE LIFETIME ASSERTION. stderr is warn-once per process so a server does not repeat itself forever;
    # a diagnostic must be re-pushed every analysis or the squiggle disappears on the next keystroke.
    frn=$(printf '%s' "$out" | tr '\r' '\n' | grep -c '"code":"undeclared-import"' || true)
    if [ "${frn:-0}" -ge 2 ]; then
        echo "  ok: republished on every analysis (${frn}x), unlike the warn-once stderr message"
    else
        echo "  FAIL: the diagnostic was published ${frn:-0}x — it must survive a keystroke" >&2; fail=1
    fi
else
    echo "  skip: free-ride fixture did not install (no pkg support in this environment)"
fi

echo "check-lsp: M6 A2 argument labels"
# THE SPAN GUARD, and it comes first for a reason: rename REPLACES the range it is handed, so this asserts
# the WHOLE {start,end} object rather than a prefix. An end at char 21 instead of 18 would mean a rename
# silently deleted `: 1` along with the label.
expect '"id":49,"result":{"start":{"line":2,"character":15},"end":{"line":2,"character":18}}' \
       "prepareRename on a label spans the LABEL, not 'lhs: 1'"
# Renaming the PARAMETER must reach all three sites: its declaration, its body use, and the call-site label.
expect '"id":53,"result":{"changes":{"file:///labels.kama":[{"range":{"start":{"line":0,"character":19},"end":{"line":0,"character":22}},"newText":"left"}' \
       "renaming a param rewrites its declaration"
expect '{"range":{"start":{"line":0,"character":44},"end":{"line":0,"character":47}},"newText":"left"}' \
       "...and its body use"
expect '{"range":{"start":{"line":2,"character":15},"end":{"line":2,"character":18}},"newText":"left"}' \
       "...AND the call-site label, which was silently left behind before A2"
# From the label's own position, find-references sees the same symbol.
expect '"id":54,"result":[{"uri":"file:///labels.kama","range":{"start":{"line":0,"character":19}' \
       "references from a label finds the parameter declaration"

# --- M6 A1: the editor analyzes the program the BUILD analyzes ---------------------------------------
echo "check-lsp: auto-import quick fix"
# ⚠️ THE EDIT IS AN INSERTION, AND THAT IS WHAT THESE RANGES PIN. The first version emitted the anchor
# through the diagnostic range helper, whose "unknown end" arm widens a point to ONE CHARACTER so a
# squiggle is always visible — correct there, and destructive here, because a one-character TextEdit
# REPLACES. Measured before the fix: the new-block edit ate the file's first byte (`fn int32 main()` ->
# `n int32 main()`) and the join-a-block edit overwrote the `std` it aimed at. Both fixtures below assert
# `start` == `end`, which is the only thing that tells the two apart.
expect '"id":71,"result":[{"title":"add `import { Widget };`","kind":"quickfix"' \
    "codeAction: a sibling in the file's own module is offered BARE — the case imports-are-mandatory created"
expect '"newText":"import { Widget };\n"' \
    "codeAction: ...as a whole new import block (this file had none)"
expect '"id":72,"result":[{"title":"add `import { std::collections::DynamicArray };`","kind":"quickfix"' \
    "codeAction: a name from another module is FOUND — the diagnostic can only say \`unknown type\`, so the fix searches"
expect '"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"newText":"import { std::collections::DynamicArray };\n"' \
    "codeAction: ...inserted at 0:0 as a ZERO-WIDTH edit, not a one-character replacement"
expect '"range":{"start":{"line":0,"character":9},"end":{"line":0,"character":9}},"newText":"std::collections::DynamicArray, "' \
    "codeAction: an existing block gains ONE ENTRY at its head — a second \`import\` statement does not parse"
expect '"id":74,"result":[]' \
    "codeAction: a buffer with nothing to fix offers nothing"
# The quick fix ADDS to the diagnostic, it does not replace it: the message still names the exact line to
# paste, which is what works over a pipe, in CI, and in an editor with no kama extension. Assert both
# halves are present, or a later "tidy-up" of the message would silently remove the only answer a
# non-LSP user ever gets.
expect 'and this file does not import it — add `import { Widget };`' \
    "the DIAGNOSTIC still names the exact line to paste, alongside the code action"

echo "check-lsp: go-to-definition reaches the prelude"
# `Optional` is an ordinary kama declaration in a real file that got embedded into the binary and lost
# its path on the way in. Nothing is synthesized to answer this — the definition was always written.
expect '"id":75,"result":{"uri":"'"$(furi "$ROOT/prelude/global.kama")"'"' \
    "definition: F12 on \`Optional\` opens prelude/global.kama"
expect '"id":75,"result":{"uri":"'"$(furi "$ROOT/prelude/global.kama")"'","range":{"start":{"line":7,"character":10},"end":{"line":7,"character":18}}}' \
    "definition: ...on the declaration ITSELF (kama 8:10 — the embed used to shift every line by one)"
# ⚠️ AND IT IS STILL READ-ONLY. These two are why `DefSite` gained a `file` rather than a `unit`: not
# owning a declaration is a reason to refuse to REWRITE it, and was never a reason to refuse to OPEN it.
# A prelude that became renameable would be a regression the assertion above cannot see.
expect '"id":76,"result":null' \
    "prepareRename: F2 on a prelude name is still greyed out"
expect '"id":77,"error":' \
    "rename: ...and the rename itself is still refused"

echo "check-lsp: go-to-definition reaches a compiler built-in"
# A built-in has no source ANYWHERE — `string` is a reserved word, not a declaration — so unlike the
# prelude there is nothing to resolve and something had to be written. The exact lines are asserted by
# tools/check-builtin-doc.sh against the file itself; what these pin is that the EDITOR reaches it at all.
expect '"id":78,"result":{"uri":"'"$(furi "$ROOT/prelude/builtin.kama")"'"' \
    "definition: F12 on \`string\` opens prelude/builtin.kama"
expect '"id":79,"result":{"uri":"'"$(furi "$ROOT/prelude/builtin.kama")"'"' \
    "definition: ...and on \`s.length()\`, an intrinsic with no declaration node of its own"
expect '"id":80,"result":null' \
    "prepareRename: a built-in is read-only too — F2 stays greyed out"

# The build configuration is resolved ONCE PER PROCESS (the M5 parse cache holds units pruneInactiveDecls
# rewrote in place, so two configurations cannot share it), which is exactly why these cannot ride the
# session above: proving the outline CHANGES with the manifest needs a fresh server per configuration.
#
# Before A1 the server never called setBuildFlags, so with an empty `_activeFlags` it dropped every
# `@compileFor(FEATURE_A)` declaration and kept every `@compileFor(!FEATURE_A)` one — the exact inverse of
# a debug host build — no matter what any manifest said.
echo "check-lsp: M6 A1 build configuration"

# cfgsession <sessionfile> <root> <file>: a whole short server run over one buffer. id 47 = documentSymbol.
#
# ⚠️ furi(), not a bare `file://$path` — and this section is WHY it exists. Every buffer above is invented
# and never opened, so its URI can be anything; these are real files, and the server has to walk UP from
# the one it is told about to find the kama.json whose flags this whole section is about. Under msys2 a
# raw `/c/Users/…` reaches native kama as `C:\c\Users\…` (the current drive), the walk finds no manifest,
# and the server answers under permissive defaults — which is the exact INVERSE of what the manifest says,
# so case B still passed and cases A/C/E/F/I' failed. It looked like the server ignoring manifests.
cfgsession() {
    cfgsess="$1"; cfgrooturi=$(furi "$2"); cfgfile="$3"; cfgfileuri=$(furi "$3")
    : > "$cfgsess"
    cfgtext=$(sed 's/\\/\\\\/g; s/"/\\"/g' "$cfgfile" | awk '{printf "%s\\n", $0}')
    session="$cfgsess"      # frame() appends to $session
    frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"'"$cfgrooturi"'","capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$cfgfileuri"'","languageId":"kama","version":1,"text":"'"$cfgtext"'"}}}'
    frame '{"jsonrpc":"2.0","id":47,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$cfgfileuri"'"}}}'
    frame '{"jsonrpc":"2.0","id":48,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
    "$KAMA" lsp < "$cfgsess" 2>/dev/null || true
}

# cfgexpect <output> <substring> <description> / cfgreject: assert on one session's stdout.
cfgexpect() {
    if printf '%s' "$1" | grep -qF -- "$2"; then echo "  ok: $3"
    else echo "  FAIL: $3 — expected substring: $2" >&2; fail=1; fi
}
cfgreject() {
    if printf '%s' "$1" | grep -qF -- "$2"; then echo "  FAIL: $3 — must NOT contain: $2" >&2; fail=1
    else echo "  ok: $3"; fi
}

# A. The committed project. Its kama.json declares FEATURE_A `"default": true`, so a plain build keeps
#    `onlyWithA` and drops `onlyWithoutA` — and so must the editor.
CFGDIR="$ROOT/tests/query/cfg"
CFGA=$(cfgsession "$tmp/cfgA" "$ROOT/tests/query" "$CFGDIR/src/app.kama")
cfgexpect "$CFGA" '"name":"onlyWithA"'   "documentSymbol shows the decl the manifest's default flag KEEPS"
cfgreject "$CFGA" '"name":"onlyWithoutA"' "...and not the negated one a build would drop"
cfgexpect "$CFGA" '"name":"always"'      "the ungated decl is there either way"
cfgreject "$CFGA" '"severity":1'         "no phantom diagnostic from conditional compilation"

# A'. THE STRONGEST ASSERTION OF THE MILESTONE: the editor and the CLI must return the SAME symbol set.
#     Comparing the two sets, rather than each against a literal, is what makes this a statement about
#     agreement instead of two independent guesses that happen to match today.
# Scope the scrape to the id:47 frame — the initialize response carries `serverInfo:{"name":"kama"}`, which
# would otherwise join the symbol set and make this compare two different things.
cfglsp=$(printf '%s' "$CFGA" | tr '\r' '\n' | tr -d '\n' | sed 's/.*"id":47,"result"://' \
         | tr ',' '\n' | grep -o '"name":"[A-Za-z_]*"' | sed 's/.*:"//; s/"//' | sort | tr '\n' ' ')
# Named by its manifest AND its file: `query` is target-addressed, so the manifest sets the scope (and
# with it the flag universe this whole section is about) while the file stays the thing being asked about.
cfgcli=$("$KAMA" query "$CFGDIR/kama.json" "$CFGDIR/src/app.kama" --symbols 2>/dev/null | awk '{print $NF}' | sort | tr '\n' ' ')
if [ "$cfglsp" = "$cfgcli" ]; then
    echo "  ok: the LSP and \`kama query\` agree on the symbol set ($cfgcli)"
else
    echo "  FAIL: editor and CLI disagree — lsp: [$cfglsp] cli: [$cfgcli]" >&2; fail=1
fi

# B. Drop the default from a COPY's manifest -> the exact complement. This is what proves the flags come
#    from the manifest rather than from some hard-coded default that happens to match case A.
CFGSRC="$tmp/cfgcopy"
mkdir -p "$CFGSRC/src"
cp "$CFGDIR/src/app.kama" "$CFGSRC/src/app.kama"
printf '{"name":"cfgprobe","version":"0.1.0","kind":"library","flags":{"FEATURE_A":{}},"modules":{".":{"visibility":"public"}}}' > "$CFGSRC/kama.json"
CFGB=$(cfgsession "$tmp/cfgB" "$tmp" "$CFGSRC/src/app.kama")
cfgexpect "$CFGB" '"name":"onlyWithoutA"' "with the default off, the NEGATED decl is what survives"
cfgreject "$CFGB" '"name":"onlyWithA"'    "...and the gated one is dropped, as a build would"

# C. kama.local.json re-enables it over that manifest. This is the LSP's configuration override channel,
#    and because it is a file the compiler already reads, `kama build` in the same directory agrees — which
#    is why the F5 debug path needs no arguments of its own.
printf '{"flags":{"FEATURE_A":{"default":true}}}' > "$CFGSRC/kama.local.json"
CFGC=$(cfgsession "$tmp/cfgC" "$tmp" "$CFGSRC/src/app.kama")
cfgexpect "$CFGC" '"name":"onlyWithA"'    "kama.local.json is the override channel (flag back on)"
cfgreject "$CFGC" '"name":"onlyWithoutA"' "...and the complement is gone again"
cfgexpect "$CFGC" 'kama.local.json'       "the config log line names the local override that was applied"

# D. Visibility. This bug survived five milestones because nothing ever SAID what the server analyzed
#    under — a server quietly analyzing the wrong program looks exactly like one analyzing the right one.
cfgexpect "$CFGA" 'window/logMessage'   "the server announces its configuration"
cfgexpect "$CFGA" 'kama.json | target'  "...naming the manifest and the resolved target"
cfgexpect "$CFGA" '| strict |'          "...and that a manifest turned strict flag validation on"
cfgexpect "$CFGA" 'FEATURE_A'           "...and the active flag set it derived"

# E. Honest failure. A malformed override must not take the editor down with it: report it visibly, then
#    analyze under permissive defaults rather than a half-applied configuration.
printf '{"flags":{ this is not json' > "$CFGSRC/kama.local.json"
CFGBAD=$(cfgsession "$tmp/cfgE" "$tmp" "$CFGSRC/src/app.kama")
cfgexpect "$CFGBAD" 'window/showMessage' "a malformed kama.local.json is reported to the user"
cfgexpect "$CFGBAD" '"name":"always"'    "...and the editor keeps answering under permissive defaults"
rm -f "$CFGSRC/kama.local.json"

# F. Strict validation reaches the editor: the same typo the BUILD rejects must squiggle here. Editor and
#    build now agree about what is a valid flag name, not just about which decls survive.
printf '@compileFor(TELMETRY)\nfn int32 oops() { return 1; }\n' > "$CFGSRC/typo.kama"
CFGTYPO=$(cfgsession "$tmp/cfgF" "$tmp" "$CFGSRC/typo.kama")
cfgexpect "$CFGTYPO" 'undeclared flag' "a typo'd @compileFor flag is a diagnostic, as it is for a build"

# G. The reconfigure path: a manifest change must RE-RESOLVE, not merely evict the parse cache. Two config
#    log lines is the proof — one from the pin, one from the re-resolve.
: > "$tmp/cfgG"
session="$tmp/cfgG"
cfggtext=$(sed 's/\\/\\\\/g; s/"/\\"/g' "$CFGSRC/src/app.kama" | awk '{printf "%s\\n", $0}')
frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"'"$(furi "$tmp")"'","capabilities":{}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$(furi "$CFGSRC/src/app.kama")"'","languageId":"kama","version":1,"text":"'"$cfggtext"'"}}}'
frame '{"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":"'"$(furi "$CFGSRC/kama.json")"'","type":2}]}}'
frame '{"jsonrpc":"2.0","id":48,"method":"shutdown","params":null}'
frame '{"jsonrpc":"2.0","method":"exit"}'
CFGG=$("$KAMA" lsp < "$tmp/cfgG" 2>/dev/null || true)
cfgreject "$CFGG" 'method not found' "workspace/didChangeWatchedFiles on a manifest is handled"
cfgn=$(printf '%s' "$CFGG" | tr '\r' '\n' | grep -c 'config: ' || true)
if [ "${cfgn:-0}" -ge 2 ]; then
    echo "  ok: a manifest change RE-RESOLVED the configuration (${cfgn} config lines), not just evicted"
else
    echo "  FAIL: a manifest change did not re-resolve — saw ${cfgn:-0} config log line(s)" >&2; fail=1
fi

echo "check-lsp: M6 C0 dynamic watched-file registration"
# H. The watcher is registered BY THE SERVER when the client asks for dynamic registration. VS Code's
#    client-side `synchronize.fileEvents` list is a vscode-languageclient convenience, not a protocol
#    feature — Neovim, Helix and eglot all have dynamic registration and no static list, so without this
#    every non-VS-Code client silently never hears about a manifest change.
: > "$tmp/cfgH"
session="$tmp/cfgH"
frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"'"$(furi "$tmp")"'","capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":true}}}}}'
frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
# A RESPONSE to the registration we just sent. It has an id and no method; the dispatch loop must ignore
# it rather than answer `method not found` addressed to the client's own id, which is a protocol violation.
frame '{"jsonrpc":"2.0","id":1,"result":null}'
frame '{"jsonrpc":"2.0","id":68,"method":"shutdown","params":null}'
frame '{"jsonrpc":"2.0","method":"exit"}'
CFGDYN=$("$KAMA" lsp < "$tmp/cfgH" 2>/dev/null || true)
cfgexpect "$CFGDYN" 'client/registerCapability'          "a client offering dynamicRegistration gets the watcher registered"
cfgexpect "$CFGDYN" 'workspace/didChangeWatchedFiles'    "...for workspace/didChangeWatchedFiles"
cfgexpect "$CFGDYN" '"globPattern":"**/*.kama"'          "...watching every .kama (the workspace index)"
cfgexpect "$CFGDYN" '"globPattern":"**/kama.json"'       "...and the manifest"
cfgexpect "$CFGDYN" '"globPattern":"**/kama.local.json"' "...and the local override, which **/kama.json does NOT match"
cfgreject "$CFGDYN" 'method not found'                   "a RESPONSE arriving on stdin is ignored, not answered"
cfgexpect "$CFGDYN" '"id":68,"result":null'              "...and the session still completes normally"

# H'. A client that does not offer it must not be registered at — Neovim advertises FALSE on Linux/BSD on
#     purpose, and registering anyway would be noise the client is entitled to reject.
cfgreject "$CFGA" 'client/registerCapability' "a client that stays silent about dynamicRegistration is not registered at"

echo "check-lsp: M6 C1 kama/buildConfig"
# I. The configuration as DATA, not as a prose log line. A status bar parsing `describeConfig`'s English
#    would break silently the first time that string changed; this is the channel a client reads.
cfgexpect "$CFGA" '"method":"kama/buildConfig"' "the server announces its configuration as structured data"
cfgexpect "$CFGA" '"groups":'                   "...carrying the single-select groups"
cfgexpect "$CFGA" '"TARGET":{"values":'         "...TARGET, with the values a picker may offer"
cfgexpect "$CFGA" '"BUILD_TYPE":{"values":["DEBUG","RELEASE"'  "...BUILD_TYPE, in declaration order"
cfgexpect "$CFGA" '"selected":"DEBUG"'          "...and which value is actually in force"
cfgexpect "$CFGA" '"flags":['                   "...plus the resolved @compileFor set"

# I'. A project's OWN groups and targets reach the picker, or it could only ever offer the built-ins.
#     `"default": true` on a value is what the picker writes, so `selected` must follow it.
CFGSEL="$tmp/cfgsel"
mkdir -p "$CFGSEL/src"
cp "$CFGDIR/src/app.kama" "$CFGSEL/src/app.kama"
printf '{"name":"cfgsel","version":"0.1.0","kind":"library","flags":{"FEATURE_A":{}},"modules":{".":{"visibility":"public"}},"select":{"TARGET":{"RPI":{"triple":"aarch64-linux-gnu"}},"CONSOLE":{"XBOX":{"default":true},"PS5":{}}}}' > "$CFGSEL/kama.json"
CFGGRP=$(cfgsession "$tmp/cfgI" "$tmp" "$CFGSEL/src/app.kama")
cfgexpect "$CFGGRP" '"CONSOLE":{"values":["XBOX","PS5"],"selected":"XBOX"}' "a project's own select group reaches the picker, with its default selected"
cfgexpect "$CFGGRP" '"RPI"'                                                 "...and its own TARGET joins the built-in catalog"

# I''. The re-announcement. A picker writes kama.local.json and expects the status bar to follow without a
#      restart, which only works if a manifest change re-announces as well as re-analyzing.
cfgbcn=$(printf '%s' "$CFGG" | tr '\r' '\n' | grep -c 'kama/buildConfig' || true)
if [ "${cfgbcn:-0}" -ge 2 ]; then
    echo "  ok: a manifest change RE-ANNOUNCES the configuration (${cfgbcn} notifications), so a picker needs no restart"
else
    echo "  FAIL: a manifest change did not re-announce — saw ${cfgbcn:-0} kama/buildConfig notification(s)" >&2; fail=1
fi

if [ "$fail" != 0 ]; then
    echo "check-lsp: FAILED. Server stdout was:" >&2
    printf '%s\n' "$out" | sed 's/^/      /' >&2
    exit 1
fi
echo "check-lsp: OK"
