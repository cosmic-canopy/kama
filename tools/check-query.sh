#!/bin/sh
# check-query.sh — LSP query-index guard (M0 T4/T5). Drives the `kama query` debug harness over a stable
# fixture and asserts the semantic query surface the LSP server (M1) is built on:
#   1. documentSymbols — the outline lists every USER decl with its accurate name position + kind, and
#      NOTHING from the prelude/std (namespace-ownership filtering).
#   2. definitionAt — go-to-definition on a signature type reference replays name resolution at the cursor
#      and lands on the type's declaration.
#   3. typeAtPosition — hover returns "<kind> <name>" for a decl name and a resolved type reference.
#   4. referencesAt (M3) — find-references lists every use of a symbol INCLUDING body use-sites, and a
#      cursor on a use returns the same set as a cursor on the declaration.
# Positions are tied to tests/query/shapes.kama; edit both together. Fails (exit 1) with a diagnostic.
# Run standalone or from run_tests.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/query/shapes.kama"

if [ ! -x "$KAMA" ]; then echo "check-query: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# expect <flags...> -- <substring>: run `kama query $FIXTURE <flags>` and assert the output contains
# <substring>. $FIXTURE is reassigned partway down for the M3.4 block — the helpers read it at call time.
expect() {
    want=""
    args=""
    seen_sep=0
    for a in "$@"; do
        if [ "$a" = "--" ]; then seen_sep=1; continue; fi
        if [ "$seen_sep" = 1 ]; then want="$a"; else args="$args $a"; fi
    done
    # shellcheck disable=SC2086
    out=$("$KAMA" query "$FIXTURE" $args 2>&1 || true)
    if printf '%s\n' "$out" | grep -qF -- "$want"; then
        echo "  ok: query$args ~ '$want'"
    else
        echo "  FAIL: query$args expected '$want', got:" >&2
        printf '%s\n' "$out" | sed 's/^/      /' >&2
        fail=1
    fi
}

# reject <flags...> -- <substring>: assert the output does NOT contain <substring> (prelude leakage guard).
reject() {
    want=""; args=""; seen_sep=0
    for a in "$@"; do
        if [ "$a" = "--" ]; then seen_sep=1; continue; fi
        if [ "$seen_sep" = 1 ]; then want="$a"; else args="$args $a"; fi
    done
    # shellcheck disable=SC2086
    out=$("$KAMA" query "$FIXTURE" $args 2>&1 || true)
    if printf '%s\n' "$out" | grep -qF -- "$want"; then
        echo "  FAIL: query$args must NOT contain '$want', got:" >&2
        printf '%s\n' "$out" | sed 's/^/      /' >&2
        fail=1
    else
        echo "  ok: query$args !~ '$want'"
    fi
}

echo "check-query: documentSymbols (outline + kinds + accurate name positions)"
expect --symbols -- "6:11 value Point"
expect --symbols -- "11:14 resource Widget"
expect --symbols -- "13:16 ctor Widget.make"
expect --symbols -- "14:20 method Widget.originX"
expect --symbols -- "17:9 function midpoint"
expect --symbols -- "24:9 function main"
# Namespace-ownership filter: Optional/Result/Owned/Shared (prelude + std::memory) never leak into a
# user file's outline.
reject --symbols -- "Optional"
reject --symbols -- "Owned"

echo "check-query: definitionAt (go-to-definition, resolution replay)"
expect --def 17:3  -- "shapes.kama:6:11"    # 'Point' return type of midpoint -> Point decl
expect --def 17:22 -- "shapes.kama:6:11"    # 'Point' param type of midpoint  -> Point decl
expect --def 6:11  -- "shapes.kama:6:11"    # cursor ON the Point decl name    -> itself
expect --def 13:26 -- "shapes.kama:6:11"    # 'Point' ctor param type          -> Point decl

echo "check-query: typeAtPosition (hover)"
expect --type 17:22 -- "value Point"        # a Point type reference
expect --type 11:14 -- "resource Widget"    # the Widget decl name
expect --type 17:9  -- "function midpoint"  # a function decl name
# M3: hover/def now reach BODY use-sites too (they are indexed positions like any other).
expect --type 18:5  -- "value Point"        # 'Point m;' inside midpoint's body
expect --def  25:5  -- "shapes.kama:6:11"   # 'Point p;' inside main's body -> Point decl

echo "check-query: referencesAt (find-references, incl. body use-sites)"
# Every Point spelling: the decl, the Widget field + ctor param, midpoint's return + 2 params, and the
# two BODY declarations (18:5, 25:5) that only the M3 reference index can see.
expect --refs 6:11 -- "shapes.kama:6:11"    # includeDecl -> the declaration itself
expect --refs 6:11 -- "shapes.kama:12:4"    # 'Point origin' field type
expect --refs 6:11 -- "shapes.kama:13:21"   # 'Point at' ctor param
expect --refs 6:11 -- "shapes.kama:17:3"    # midpoint's return type
expect --refs 6:11 -- "shapes.kama:18:4"    # BODY: 'Point m;' in midpoint
expect --refs 6:11 -- "shapes.kama:25:4"    # BODY: 'Point p;' in main
# A cursor on a USE resolves to the same key, so it returns the same set as a cursor on the decl.
expect --refs 18:5 -- "shapes.kama:6:11"
expect --refs 18:5 -- "shapes.kama:25:4"
# Prelude/std symbols are never renameable targets and have no user references to report.
reject --refs 7:12 -- "shapes.kama"         # 'int32' (a builtin) -> "no references"

# ---- M3.4: locals, params, fields, enum members -----------------------------------------------------
# Second fixture. Positions are tied to tests/query/scopes.kama — edit both together, and APPEND to that
# file rather than inserting, so these line numbers stay valid.
FIXTURE="$ROOT/tests/query/scopes.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.4 outline (fields + enum members in; locals + params OUT)"
expect --symbols -- "14:4 enum-member Ok"
expect --symbols -- "15:4 enum-member Bad"
expect --symbols -- "19:17 field limit"
expect --symbols -- "20:17 field scale"
# An outline listing every local/param would be noise — documentSymbols filters those two kinds.
reject --symbols -- "local"
reject --symbols -- "param"

echo "check-query: M3.4 hover + go-to-definition on bindings"
expect --type 30:12 -- "local seeded"       # a local USE
expect --type 22:33 -- "param bias"         # a parameter declaration
expect --type 19:17 -- "field limit"
expect --type 14:4  -- "enum-member Ok"
expect --def  30:12 -- "scopes.kama:29:10"  # local use -> its declaration
expect --def  23:20 -- "scopes.kama:19:17"  # 'this.limit' -> the field declaration
expect --def  53:22 -- "scopes.kama:14:4"   # 'Code::Ok'  -> the enum member declaration
expect --def  55:13 -- "scopes.kama:14:4"   # 'case Ok:'  -> the same enum member

echo "check-query: M3.4 find-references"
expect --refs 29:10 -- "scopes.kama:30:12"  # local 'seeded' decl -> its one use
expect --refs 22:33 -- "scopes.kama:23:41"  # param 'bias' -> its use in the body
expect --refs 19:17 -- "scopes.kama:23:20"  # field 'limit' -> 'this.limit'
expect --refs 14:4  -- "scopes.kama:53:22"  # enum member 'Ok' -> the 'Code::Ok' read
expect --refs 14:4  -- "scopes.kama:55:13"  # ... and the 'case Ok:' match arm
expect --refs 64:19 -- "scopes.kama:65:20"  # a foreach loop variable is a local too
# SHADOWING: two `shadow` locals in sibling scopes are DISTINCT symbols, keyed by declaration site.
expect --refs 39:14 -- "scopes.kama:40:20"
reject --refs 39:14 -- "scopes.kama:44:20"  # ... and must NOT return the sibling's use
expect --refs 43:14 -- "scopes.kama:44:20"
reject --refs 43:14 -- "scopes.kama:40:20"

# ---------------------------------------------------------------------------------------------------
# M3.5 — WORKSPACE INDEXING (`--project`).
#
# tests/query/ws/ is a two-file package: app.kama imports widget.kama, and widget.kama imports nothing.
# So widget.kama's own import closure is JUST ITSELF — app.kama's three uses of `Widget` are invisible to
# it. That asymmetry is the whole reason M3.3 had to refuse cross-file rename, and it is what --project
# fixes by widening the unit set from one closure to every .kama under the nearest kama.json.
FIXTURE="$ROOT/tests/query/ws/widget.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.5 workspace indexing"
# Without --project: only widget.kama's own uses are visible. (`--refs` prints absolute paths under
# --project and the given path without it, so match on the basename+position, which both forms carry.)
expect --refs 12:11 -- "widget.kama:12:11"          # the declaration itself
expect --refs 12:11 -- "widget.kama:15:33"          # 'Widget r;' inside the ctor
reject --refs 12:11 -- "app.kama"                   # ... and app.kama is INVISIBLE (the M3.3 blind spot)
# With --project: the same query reaches every file in the package.
expect --project --refs 12:11 -- "app.kama:5:3"     # 'fn Widget make(...)' return type
expect --project --refs 12:11 -- "app.kama:6:4"     # 'Widget w = Widget.of(...)'
expect --project --refs 12:11 -- "app.kama:11:4"    # 'Widget w = make(...)' in main
expect --project --refs 12:11 -- "widget.kama:12:11"  # ... without losing the declaring file's own uses
# A free function crosses the boundary the same way.
expect --project --refs 18:9 -- "app.kama:11:23"    # 'defaultSize()' called from app.kama
# The declaration must be reported ONCE. A type that declares a `ctor` used to be listed twice: the
# ctor's implicit result type resolves through the class's own decl identifier, so the decl name was
# recorded as a reference to itself. Rename replaces every range it is handed, so a duplicate meant two
# identical TextEdits over one range — which the LSP spec forbids within a file.
count=$("$KAMA" query "$FIXTURE" --project --refs 12:11 2>&1 | grep -c "widget.kama:12:11" || true)
if [ "$count" = 1 ]; then
    echo "  ok: the declaration is reported exactly once (no self-reference duplicate)"
else
    echo "  FAIL: expected the decl at widget.kama:12:11 once, got $count" >&2
    fail=1
fi

# ---------------------------------------------------------------------------------------------------
# M3.5 — DECLARED project scope (`sources` / `packages` in kama.json).
#
# tests/query/mono/ is a NESTED monorepo. The root declares `"projects": ["libs/*", "group"]`; `group`
# declares projects of its OWN; each leaf declares `"sources": ["src"]`; and `outside/stray.kama` declares
# a same-named `Gear` that nothing ever claims. Because the scope is DECLARED rather than inferred, no
# directory walk of the repo happens, the file cap does not apply, the nested level is still reached, and
# the stray type cannot collide with the workspace's.
#
#   mono/kama.json                  projects: ["libs/*", "group"]
#     libs/core/kama.json           sources: ["src"]   <- declares Gear
#     libs/app/kama.json            sources: ["src"]   <- uses Gear
#     group/kama.json               projects: ["libs/*"]   <- a monorepo INSIDE a monorepo
#       group/libs/plugin/kama.json sources: ["src"]   <- uses Gear, one level deeper
#     outside/stray.kama            claimed by nobody  <- must never appear
#
# The member directory is `libs/`, NOT `packages/`: kama.lock uses `packages` for resolved dependencies,
# so a folder of that name next to a `projects` key would teach exactly the confusion the key avoids.
FIXTURE="$ROOT/tests/query/mono/libs/core/src/gearcore.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.5 declared project scope (sources + packages)"
# The consuming package is reached even though the declaring one never imports it — and reached WITHOUT an
# editor workspace root, because an ancestor manifest explicitly owns this file.
expect --project --refs 9:11 -- "gearcore.kama:9:11"        # the declaration
expect --project --refs 9:11 -- "gearapp.kama:7:4"          # a SIBLING project's use
expect --project --refs 9:11 -- "gearplugin.kama:6:4"       # a NESTED sub-project's use (projects recurses)
reject --project --refs 9:11 -- "stray.kama"                # ... and never the undeclared decoy
# Without --project the sibling package is invisible again (the closure is one file).
reject --refs 9:11 -- "gearapp.kama"

# ---------------------------------------------------------------------------------------------------
# M3.5 — an installed DEPENDENCY (not just first-party sub-projects).
#
# A dependency is materialized under `<project>/.kama/deps`, which the project walk PRUNES (dot-directory)
# yet analysis still loads, because the import pulls it in. So a dependency's symbols are queryable —
# go-to-definition jumps into the dep's source — while the dep's files are NOT part of the project's own
# file set, and rename must refuse to rewrite them (asserted in check-lsp.sh, which has the rename verb).
# Built here rather than committed: `.kama/deps` is install output, and a path dep needs no network.
dep="$tmp/depproj"
mkdir -p "$dep/geo" "$dep/app"
cat > "$dep/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "sources": ["."] }
JSON
cat > "$dep/geo/geo.kama" <<'KAMA'
namespace geo;
export { Point };
type value Point {
    public int32 x;
    public ctor of(int32 x) { Point r; r.x = x; return give r; }
}
KAMA
cat > "$dep/app/kama.json" <<'JSON'
{ "name": "app", "version": "0.1.0", "main": "app.kama", "sources": ["."],
  "dependencies": { "geo": { "path": "../geo" } } }
JSON
cat > "$dep/app/app.kama" <<'KAMA'
import geo::{Point};
fn int32 main() {
    Point p = Point.of(x: 7);
    return p.x;
}
KAMA
if (cd "$dep/app" && "$KAMA" pkg install >/dev/null 2>&1); then
    FIXTURE="$dep/app/app.kama"
    echo "check-query: M3.5 installed dependency"
    # The dep's own source is where its declaration lives — go-to-def crosses the package boundary.
    expect --project --def  3:4 -- "/.kama/deps/geo/geo.kama:3:11"
    expect --project --type 3:4 -- "value Point"
    expect --project --refs 3:4 -- "app.kama:3:4"                  # our use
    expect --project --refs 3:4 -- "/.kama/deps/geo/geo.kama:3:11"  # ... and the dep's declaration
    # The project's own file set stops at the package boundary: `.kama/` is pruned, so the outline is ours.
    expect --project --symbols -- "2:9 function main"
    reject --project --symbols -- "Point"
else
    echo "  SKIP: installed-dependency checks (kama pkg install failed)"
fi

# ---------------------------------------------------------------------------------------------------
# M4.0 — the completion LEXICAL layer.
#
# `--complete L:C` prints the context recovered from the file's RAW TEXT before any semantics run:
#   trigger=<bare|dot|scope|arg-label|import-path|import-symbol> recv=… callee=… prefix=… active=N filled=…
# Every probe points INSIDE valid source (the column of the character just after a `.`, or just inside a
# `(`), so the fixture parses and analyzes clean while the scanner sees exactly what a half-typed buffer
# would give it. That equivalence is the whole design: at completion time the buffer does NOT parse, so
# this context can never come from the index.
FIXTURE="$ROOT/tests/query/complete.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M4.0 completion context (lexical scan)"
expect --complete 112:21 -- "trigger=dot recv=c "                    # c.|value
expect --complete 114:27 -- "trigger=dot recv=h.cell "               # h.cell.|value — a chained receiver
expect --complete 115:33 -- "trigger=dot recv=makeHolder() "         # a CALL receiver, canonicalized to `()`
expect --complete 119:30 -- "trigger=dot recv=cells[] "              # an INDEX receiver, canonicalized to `[]`
expect --complete 121:27 -- "trigger=dot recv=owned "                # a smart-pointer receiver
expect --complete 136:24 -- "trigger=scope recv=Level "              # Level::|High
expect --complete 136:24 -- "active=-1"                              # `(a == b)` is a GROUPING paren, not a call
expect --complete 124:27 -- "trigger=arg-label recv= callee=blend prefix= active=0 filled="   # blend(|lo: …)
expect --complete 124:34 -- "trigger=arg-label recv= callee=blend prefix= active=1 filled=lo" # …, |hi: 4)
expect --complete 15:12  -- "trigger=import-path recv=std "          # import std::|collections
expect --complete 15:26  -- "trigger=import-symbol recv=std::collections "   # import …::{|DynamicArray}
# Literals and comments hold no code — and an interpolation HOLE does, so it must still complete.
expect --complete 125:21 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a string body
expect --complete 127:17 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a // comment
expect --complete 128:15 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a /* block */
expect --complete 126:30 -- "trigger=dot recv=c "                    # inside "interp ${c.|value} hole"

# ---------------------------------------------------------------------------------------------------
# M4.1 — member completion after `.`. Output is one `kind<TAB>label<TAB>detail` line per candidate.
echo "check-query: M4.1 member completion (receivers)"
expect --complete 112:21 -- "field	value	int32"                     # c.| — a public field, with its type
expect --complete 112:21 -- "method	doubled	fn int32 doubled()"       # ... and its methods
reject --complete 112:21 -- "secret"                                  # ... but NEVER a private one from outside
expect --complete 114:27 -- "field	value	int32"                     # h.cell.| — chained through a field's type
expect --complete 115:33 -- "field	cell	Cell"                       # makeHolder().| — a call's return type
expect --complete 116:40 -- "field	value	int32"                     # makeHolder().get().| — a CALLED tail segment
expect --complete 119:30 -- "method	doubled	fn int32 doubled()"       # cells[0].| — a generic instance's ELEMENT
expect --complete 121:27 -- "method	read	fn int32 read()"            # owned.| — through Owned<Node>'s Deref
expect --complete 120:33 -- "ctor	make	fn Node make(tag: int32)"     # Node.| — a TYPE receiver offers ctors
reject --complete 121:27 -- "ctor"                                    # ... and an INSTANCE receiver never does
expect --complete 53:16  -- "method	size	fn int32 size()"            # item.| where `T: Sized` — via the BOUND

echo "check-query: M4.1 generic-instance substitution + visibility"
# A generic instance's members are stored with the TEMPLATE's spellings; they must read as the INSTANCE's.
expect --complete 118:10 -- "method	add	fn void add(item: Cell)"      # DynamicArray<Cell>.add takes a Cell, not a T
expect --complete 118:10 -- "method	pop	fn Optional<Cell> pop()"      # ... including a nested generic return
# Inside a method, `this.` sees what THAT type may see — private included, inherited protected included,
# a base class's privates never.
expect --complete 23:44  -- "field	secret	int32"                     # own private field, from inside
expect --complete 36:42  -- "method	baseOnly	fn int32 baseOnly()"   # inherited PROTECTED method, from a subclass
reject --complete 36:42  -- "hidden"                                  # ... but not the base's privates
reject --complete 36:42  -- "shared"

echo "check-query: M4.1 collectBindings covers every block-bearing statement"
# Each probe reads a local declared inside one statement kind. A miss is silent everywhere else — it just
# means fewer suggestions — which is exactly why it needs a test per kind.
expect --complete 69:33  -- "field	value	int32"    # a bare nested block
expect --complete 73:31  -- "field	value	int32"    # if
expect --complete 76:31  -- "field	value	int32"    # else
expect --complete 80:32  -- "field	value	int32"    # while
expect --complete 84:29  -- "field	value	int32"    # do/while
expect --complete 88:30  -- "field	value	int32"    # for
expect --complete 94:29  -- "field	value	int32"    # the foreach BINDING
expect --complete 94:47  -- "field	value	int32"    # a local inside the foreach body
expect --complete 98:33  -- "field	value	int32"    # unsafe
expect --complete 102:32 -- "field	value	int32"    # scope
expect --complete 105:62 -- "field	value	int32"    # a match arm's payload binding (type from the variant case)

# A type may declare a FIELD and a METHOD under one name (std::process::Command has both spellings of
# `args`). Which one a path segment names depends on whether the source CALLED it — resolving `this.args.`
# through the void-returning method would silently offer nothing.
echo "check-query: M4.1 field-vs-method precedence on a real stdlib type"
FIXTURE="$ROOT/lib/std/process/process.kama"
expect --complete 166:45 -- "method	add	fn void add(item: string)"   # this.args.| is the DynamicArray FIELD
expect --complete 62:43  -- "field	code	int32"                       # ... and a plain `this.` still works

# ---------------------------------------------------------------------------------------------------
# M4.2 — after `::`. A `::` head is always a TYPE or a NAMESPACE (SPEC forbids `::` on a value, and the
# emitter rejects it), so there are exactly two answers.
FIXTURE="$ROOT/tests/query/complete.kama"
echo "check-query: M4.2 completion after \`::\`"
expect --complete 136:24 -- "enum-member	High"          # Level::| — a plain enum's members
expect --complete 136:24 -- "enum-member	Low"
expect --complete 104:37 -- "variant	Some	(value: T)"   # Optional::| — a generic variant's cases + payload
expect --complete 104:37 -- "variant	None"
expect --complete 151:28 -- "method	zero	fn int32 zero()"   # Stat::| — a static method
expect --complete 151:28 -- "ctor	of	fn Stat of(n: int32)" # ... a named ctor (the `::` bridge reaches them)
expect --complete 151:28 -- "constant	LIMIT	int32"          # ... and a type-associated `comptime` constant
# A NAMESPACE head lists what that namespace declares, one level deep.
expect --complete 153:33 -- "type	Cell"
expect --complete 153:33 -- "contract	Sized"
expect --complete 153:33 -- "function	blend"
reject --complete 153:33 -- "DynamicArray"                  # ... never another namespace's symbols

# ---------------------------------------------------------------------------------------------------
# M4.3 — bare names. The flooding guard is the whole milestone: `_classes` and `_funcs` span the entire
# import closure, so `bareNameOf` is the exact INVERSE of resolveUserNameImpl's lookup order and anything
# it cannot spell must not appear.
echo "check-query: M4.3 names in scope"
expect --complete 164:4 -- "local	near	Cell"                 # locals, with their declared types
expect --complete 164:4 -- "param	seed	int32"                # ... and parameters
expect --complete 164:4 -- "type	Cell"                       # a type declared in this file
expect --complete 164:4 -- "type	DynamicArray"               # ... one reached through an import
expect --complete 164:4 -- "function	print	fn void print(s: string)"   # the always-in-scope FLOOR
expect --complete 164:4 -- "function	args	fn Args args()"
expect --complete 164:4 -- "function	main	fn int32 main()"    # `main` is the one name the resolver rewrites
expect --complete 164:4 -- "keyword	foreach"                  # the lexer's own keyword table
# The C-ABI plumbing behind the floor is spellable but is NOT language surface. A user's own extern is.
expect --complete 164:4 -- "function	myOwnFfi"
reject --complete 164:4 -- "	free	"
reject --complete 164:4 -- "kama_args_at"
reject --complete 164:4 -- "kama_ctrl_release_strong"
reject --complete 164:4 -- "kama_main"                        # ... and never a mangled spelling
# A deeper namespace than any `using` reaches is unspellable here, mangled instances doubly so.
reject --complete 164:4 -- "DynamicArray_"
reject --complete 164:4 -- "collections__"
# Inside a method a field is spellable bare — which is exactly why a local may not shadow one.
expect --complete 23:38 -- "field	secret	int32"               # own private field
expect --complete 23:38 -- "method	size	fn int32 size()"

# ---------------------------------------------------------------------------------------------------
# M4.7 — import paths. These answer from the module RESOLVER, not the index: a module the file does not
# import yet is by definition absent from the index. Root order mirrors loadProgramUnits exactly, so what
# completes is what would actually resolve.
echo "check-query: M4.7 import paths"
expect --complete 15:7  -- "module	std"           # `import |` -> the stdlib root
expect --complete 15:7  -- "module	shapes"        # ... and sibling file-modules in this directory
reject --complete 15:7  -- "module	complete"      # ... but never the file itself
expect --complete 15:12 -- "module	collections"   # `import std::|` -> the stdlib's modules
expect --complete 15:12 -- "module	process"
expect --complete 15:26 -- "type	DynamicArray	std::collections"   # `import …::{|}` -> the export manifest
expect --complete 15:26 -- "type	Deque	std::collections"

# ---------------------------------------------------------------------------------------------------
# M4.8 — `global::` names the ROOT scope. Its completion payoff is why the alias waited for an LSP: the
# always-in-scope floor is otherwise undiscoverable, since there is no module to import that would list it.
echo "check-query: M4.8 global:: completion"
expect --complete 170:12 -- "function	println	fn void println(s: string)"   # the floor
expect --complete 170:12 -- "function	args	fn Args args()"
expect --complete 170:12 -- "type	Optional"
reject --complete 170:12 -- "	Cell	"          # ... never a namespaced symbol, even this file's own
reject --complete 170:12 -- "	blend	"
reject --complete 170:12 -- "keyword"           # ... and `global::while` is not a thing
reject --complete 170:12 -- "kama_args_at"      # ... nor the C-ABI plumbing behind the floor

# ---------------------------------------------------------------------------------------------------
# M4.4 — signature help + argument labels. kama has NO positional arguments (kama.y's `argument`
# productions are all `IDENTIFIER COLON …`), so "which parameter am I on" and "what labels may I type"
# are one question with one answer.
echo "check-query: M4.4 signature help"
expect --sighelp 124:27 -- "sig=blend(lo: int32, hi: int32) -> int32 active=0"
expect --sighelp 124:34 -- "active=1"                                  # ... the next slot
expect --sighelp 118:14 -- "sig=cells.add(item: Cell)"                 # a generic INSTANCE's method: T -> Cell
expect --sighelp 104:42 -- "sig=Optional::Some(value: T)"              # a variant case, payload as labels
expect --sighelp 151:33 -- "sig=Stat::zero() -> int32 active=-1"       # a static with no parameters
expect --sighelp 154:24 -- "sig=Stat.of(n: int32) -> Stat"             # a named ctor, dot-on-type
expect --sighelp 137:52 -- "sig=measure(item: T) -> int32"             # a generic free fn
expect --sighelp 123:36 -- "sig=derived.total() -> int32"              # reached THROUGH Owned<Derived>'s Deref
expect --sighelp 164:4  -- "no signature"                              # not inside a call at all

echo "check-query: M4.4 argument-label completion"
expect --complete 124:27 -- "label	lo:	int32"                        # an empty slot admits every label
expect --complete 124:27 -- "label	hi:	int32"
expect --complete 124:34 -- "label	hi:	int32"
reject --complete 124:34 -- "label	lo:"                              # ... but never one already supplied

# ---------------------------------------------------------------------------------------------------
# M6 A2 — NAMED-ARGUMENT LABELS are references to the callee's parameter.
#
# Every kama argument is named, so labels are most of the call syntax, not a niche gesture. Before A2 they
# were absent from the reference index: renaming a parameter rewrote its declaration and body uses and
# silently left every call site spelling the old label.
#
# The cross-unit direction is the one that matters. A label's key must come from the parameter's DECLARING
# unit; building it at the call site would embed the CALLER's unit instead — and same-file labels would
# still appear to work, which is the failure mode a test suite is least able to see.
FIXTURE="$ROOT/tests/query/labels/lib.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M6 A2 argument labels"
# A free function's parameter: declaration, its body use, and the label in the OTHER unit.
expect --project --refs 14:23 -- "lib.kama:14:23"       # the declaration
expect --project --refs 14:23 -- "lib.kama:14:40"       # its body use (M3.1)
expect --project --refs 14:23 -- "use.kama:10:22"       # the call-site LABEL, across units (M6 A2)
# A METHOD's parameter reaches its label too — a different ParamSig path through emitReorderedCall.
expect --project --refs 19:31 -- "lib.kama:19:31"
expect --project --refs 19:31 -- "use.kama:14:12"

FIXTURE="$ROOT/tests/query/labels/use.kama"
# From the label's side: go-to-definition lands on the parameter, and hover names it as a param rather
# than echoing the bare spelling.
expect --def 10:22  -- "lib.kama:14:23"
expect --type 10:22 -- "param factor"
expect --def 14:12  -- "lib.kama:19:31"
expect --type 14:12 -- "param amount"
# A label is NOT the argument expression: the value after the colon keeps answering as itself, so the
# label's span cannot have swallowed it.
reject --type 10:22 -- "param 3"

# ---------------------------------------------------------------------------------------------------
# M6 B3a/B3b — METHOD CALL SITES, and the inside of a GENERIC body.
#
# Before B3 a method's uses were in the index for no type at all, generic or not, while its DECLARATION was
# — so rename offered itself and then rewrote the declaration alone, leaving a buffer that no longer
# compiled. And nothing inside a generic body was indexed, because instances are emitted from
# emitHeaderContent, before the loop that sets `_refUnit`.
#
# TWO instantiations, in a unit that is not the declaring one. That combination is the whole test: a
# one-file, one-instantiation fixture passes under designs that canonicalize the instance key onto the
# template, and those break the moment a second key shape exists.
FIXTURE="$ROOT/tests/query/generics/lib.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M6 B3 generic members + method calls"
# A generic type's FIELD is ONE symbol: its declaration, both uses inside the template body, and the use
# through EACH instantiation. `Box<int32>` and `Box<bool>` resolve through different instances
# (`Box_int32__v`, `Box_bool__v`); only the template's own declaration node is common to both.
expect --project --refs 15:13 -- "lib.kama:15:13"        # the declaration
expect --project --refs 15:13 -- "lib.kama:17:36"        # `this.v` inside the generic BODY (B3b)
expect --project --refs 15:13 -- "lib.kama:19:42"        # and in a second method of the same template
expect --project --refs 15:13 -- "use.kama:12:7"         # through Box<int32>
expect --project --refs 15:13 -- "use.kama:17:7"         # through Box<bool> — the SAME symbol
# ONE def-site however many instantiations exist: exactly one line names the declaration itself.
n=$("$KAMA" query "$FIXTURE" --project --refs 15:13 2>&1 | grep -c "lib.kama:15:13")
if [ "$n" = 1 ]; then echo "  ok: two instantiations yield ONE def-site for the field"
else echo "  FAIL: expected 1 def-site line for the field, got $n" >&2; fail=1; fi
# A generic type's METHOD, likewise — and these are CALL sites, which is B3a.
expect --project --refs 17:16 -- "lib.kama:17:16"
expect --project --refs 17:16 -- "use.kama:13:17"
expect --project --refs 17:16 -- "use.kama:18:16"

FIXTURE="$ROOT/tests/query/generics/use.kama"
# From the call site: go-to-definition lands on the template's declaration, not on any instance, and hover
# names the member through the TEMPLATE (`Box.get`, never `Box_int32.get`).
expect --def 13:17  -- "lib.kama:17:16"
expect --type 13:17 -- "method Box.get"
expect --def 12:7   -- "lib.kama:15:13"
expect --type 12:7  -- "field v"
# A generic method's PARAMETER reaches its call-site label too (the A2 path, through a template body).
expect --def 19:15  -- "lib.kama:19:29"

FIXTURE="$ROOT/tests/query/generics/lib.kama"
# M6 B3g: an `import`'s symbol list is a REFERENCE. Renaming `Box` used to rewrite its declaration and its
# uses and leave `import lib::{Box, …}` spelling the old name — the module then imports a symbol that no
# longer exists, so the rename breaks a file it did edit. Same class as B3a, across units.
expect --project --refs 13:11 -- "use.kama:7:13"
# M6 B3f: and the matching `export { Box, … };`, which was the other half of that same rename. This line
# was a `reject` from B3g until the grammar carried per-segment positions for the `::`-separated name
# lists — it was pinned as a FACT precisely so that closing the gap could not be silent.
expect --project --refs 13:11 -- "lib.kama:11:9"

# ---------------------------------------------------------------------------------------------------
# M6 B3f — the QUALIFIER of a `::`-separated name.
#
# `Color` in `Color::Green` had no position at all: a qualifier is a list of plain STRINGS, so there was
# no node to anchor an index entry to. Renaming the enum rewrote its declaration and every `case` arm and
# left every `Color::` spelling behind — the same silent under-apply as B3a, one level up. The grammar
# now carries a span per segment (CodeGenContext::listSegPos).
FIXTURE="$ROOT/tests/query/coverage/spellings.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo 'check-query: M6 B3f :: qualifier positions'
expect --def 59:14 -- "spellings.kama:14:5"     # `Color` in `Color::Green` -> the enum declaration
expect --def 62:15 -- "spellings.kama:16:5"     # `Shape` in `Shape::Circle(r: 7)`
expect --def 56:14 -- "spellings.kama:34:11"    # `Point` in `Point::origin()` — a static call's TYPE
expect --type 59:14 -- "enum Color"
# The qualifier is a USE of the type, so it must be in the type's reference set — that is what makes
# renaming the enum rewrite it. The member `Green` keeps its own separate symbol.
expect --refs 14:5 -- "spellings.kama:59:14"
expect --refs 14:5 -- "spellings.kama:59:4"     # the type ANNOTATION, indexed since M0
reject --refs 14:5 -- "spellings.kama:59:21"    # `Green` belongs to the enum MEMBER, not the enum

# ---------------------------------------------------------------------------------------------------
# M6 B3f — a MODULE path is a navigation target, never a rename target.
#
# In kama the namespace IS the module path IS the directory path (SPEC § Modules / namespaces:
# `import a::b::c` resolves to a/b/c.kama or a/b/c/), so renaming a namespace is a file-and-directory
# move rather than a symbol rename. `module:` keys therefore name NO def-site — which is what makes
# rename and find-references skip them with no extra flag — while go-to-definition opens the module,
# the same gesture clangd gives `#include` and gopls gives an import path.
FIXTURE="$ROOT/tests/query/imports.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo 'check-query: M6 B3f module paths'
expect --def 6:12 -- "/lib/std/collections/"     # `collections` opens the module it names
expect --type 6:12 -- "module std::collections"
expect --type 6:7  -- "module std"               # a path PREFIX no file declares...
expect --def 6:7   -- "no definition"            # ...has nothing to open, as clangd answers a partial include
expect --type 4:11 -- "module importsprobe"      # the file's own `namespace` declaration
expect --def 4:11  -- "imports.kama:4:10"
# The imported SYMBOL is a real symbol and keeps its own def-site (B3g) — the module key must not
# swallow it.
expect --type 6:32 -- "generic-type DynamicArray"

# ---------------------------------------------------------------------------------------------------
# M6 B3c — a contract method and its implementations are ONE renameable name.
#
# `fn int32 speak();` inside a `type contract` was indexed nowhere, and neither was any call dispatched
# through the contract's fat pointer, so renaming an implementation rewrote that one method and left the
# contract and every sibling implementation saying the old name.
#
# The fixture dispatches BOTH ways on purpose — `c.speak()` on the concrete type and `s.speak()` through
# `Owned<Speaker>` — because a fixture with only one of them passes under a design that gets the other
# wrong. The members keep SEPARATE def-sites (go-to-definition stays precise); only references and rename
# consult the group.
FIXTURE="$ROOT/tests/query/coverage/dispatch.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo 'check-query: M6 B3c contract methods'
# From the CONTRACT's declaration: the contract, both implementations, both direct calls, the fat-pointer
# call. Renaming any one of them has to move all six or the program stops compiling.
expect --refs 10:42 -- "dispatch.kama:10:42"    # the contract declaration
expect --refs 10:42 -- "dispatch.kama:15:20"    # Cat's implementation
expect --refs 10:42 -- "dispatch.kama:20:20"    # Dog's implementation
expect --refs 10:42 -- "dispatch.kama:34:21"    # c.speak() — a concrete call
expect --refs 10:42 -- "dispatch.kama:34:33"    # d.speak() — the other concrete call
expect --refs 10:42 -- "dispatch.kama:38:26"    # s.speak() — through the fat pointer
# ...and the same set from ONE implementation, which is the direction the rename actually comes from.
expect --refs 15:20 -- "dispatch.kama:10:42"
expect --refs 15:20 -- "dispatch.kama:20:20"
expect --refs 15:20 -- "dispatch.kama:38:26"
# Go-to-definition does NOT collapse onto the contract: a concrete-typed call lands on THAT type's
# implementation, and only a fat-pointer call — whose static type IS the contract — lands on the contract.
# This is the whole reason the group is a separate relation rather than a shared def-site.
expect --def 34:21 -- "dispatch.kama:15:20"
expect --def 34:33 -- "dispatch.kama:20:20"
expect --def 38:26 -- "dispatch.kama:10:42"
expect --type 10:42 -- "method speak"

# M6 B3h — a generic ARGUMENT is a reference to the type it names. `Speaker` inside `Owned<Speaker>`
# resolves in mangleElem and nowhere else, and mangleElem passed no `site`, so renaming the contract
# rewrote its declaration and its `implements` clauses and left every `Owned<Speaker>` spelling behind.
# Found by pulling on a `-` line the coverage oracle left over after B3c, not by anyone remembering it.
expect --refs 10:14 -- "dispatch.kama:37:10"    # `Owned<Speaker>` is a use of Speaker
expect --def 37:10  -- "dispatch.kama:10:14"    # ...and go-to-definition from it reaches the contract

# ---------------------------------------------------------------------------------------------------
# M6 B3 — the reference index's COVERAGE ORACLE.
#
# Every assertion above tests a spelling somebody thought of. That is exactly how a method's call sites
# stayed unindexed for the whole campaign: nobody thought of them, so nothing failed. `--coverage` asks the
# other question — for EVERY identifier the source spells, what does the index know? — and the answer is
# frozen in a checked-in table, so a gap is a diff rather than a discovery.
#
# The table is compared WHOLE, not by substring: a line silently disappearing is as much a regression as a
# line changing. Regenerate deliberately (never to "make the test pass") with:
#     ./kama query tests/query/coverage/<name>.kama --coverage > tests/query/coverage/<name>.coverage
# and justify every changed line in the commit message.
echo "check-query: M6 B3 index coverage"
for cov in "$ROOT"/tests/query/coverage/*.kama; do
    want="${cov%.kama}.coverage"
    name=$(basename "$cov")
    if [ ! -f "$want" ]; then
        echo "  FAIL: $name has no checked-in coverage table ($want)" >&2
        fail=1
        continue
    fi
    got="$tmp/$(basename "$want")"
    "$KAMA" query "$cov" --coverage > "$got" 2>/dev/null || true
    if diff -u "$want" "$got" > "$tmp/cov.diff" 2>&1; then
        echo "  ok: $name coverage table unchanged ($(wc -l < "$want" | tr -d ' ') identifiers)"
    else
        echo "  FAIL: $name coverage table changed — read the diff, then regenerate it ON PURPOSE:" >&2
        sed 's/^/      /' "$tmp/cov.diff" >&2
        fail=1
    fi
done

if [ "$fail" != 0 ]; then echo "check-query: FAILED" >&2; exit 1; fi
echo "check-query: OK"
