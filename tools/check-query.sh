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

# runq <flags>: `kama query $FIXTURE <flags>`, ONCE per distinct (fixture, flags) pair.
#
# This guard is the most expensive one in the tree by a wide margin, and after the guards were
# parallelized it IS the guard block — everything else finishes inside its runtime. The reason is that
# every assertion below used to be its own process, and every process re-parses the fixture's entire
# transitive `std` import closure before it can answer anything: ~210ms for the import-heavy fixtures,
# against ~1ms to actually answer the question off the built index.
#
# The assertions ask far fewer questions than they make claims — 233 assertions, 131 distinct queries,
# because several `expect`s (and the `reject`s paired with them) interrogate one output. So memoize it.
# Keyed on fixture + flags; the cksum keeps the filename short and the sanitized prefix keeps it legible.
# Failures are cached too, deliberately: the output IS the result, exit status was already discarded.
qcache=$(mktemp -d); trap 'rm -rf "$tmp" "$qcache"' EXIT
qkey() { printf '%s|%s' "$1" "$2" | cksum | tr -cd '0-9'; }

# The SCOPE operand for a fixture, replacing the deleted `--project` flag. `kama query` is
# target-addressed: a manifest operand says which scope to search and the .kama file stays the thing
# being asked about, so the two compose (§2g.35). This walks up for the scope the flag used to infer —
# a workspace file if one is above (which is what lets the mono assertions reach a SIBLING member, a
# scope `--project` could never express) else the nearest project manifest.
#
# `--project` survives below only as this guard's own shorthand for "ask at project scope"; it is
# translated here and never reaches the compiler.
scope_for() {
    _d=$(dirname "$1"); _s=""
    while [ "$_d" != "/" ] && [ "$_d" != "." ]; do
        if [ -f "$_d/kama_workspace.json" ]; then echo "$_d/kama_workspace.json"; return; fi
        if [ -z "$_s" ] && [ -f "$_d/kama.json" ]; then _s="$_d/kama.json"; fi
        _d=$(dirname "$_d")
    done
    echo "$_s"
}

runq() {
    _f="$qcache/$(qkey "$FIXTURE" "$1")"
    _a="$1"; _scope=""
    # Position-independent: `expect` accumulates args with a leading space, so a prefix match misses.
    case " $_a " in
        *" --project "*) _a=$(printf '%s' "$_a" | sed 's/ *--project//'); _scope=$(scope_for "$FIXTURE") ;;
    esac
    # shellcheck disable=SC2086
    [ -f "$_f" ] || "$KAMA" query $_scope "$FIXTURE" $_a >"$_f" 2>&1 || true
    cat "$_f"
}

# ---- batch preload -------------------------------------------------------------------------------
# Memoizing collapsed 233 assertions to 131 distinct queries, but 131 queries were still 131 PROCESSES,
# each re-parsing its fixture's whole `std` import closure to answer one question. `kama query` now takes
# many questions per invocation and answers them from ONE analysis, in argv order. Asked per FIXTURE
# instead of per assertion, those 131 analyses become ~15.
#
# The question list is DERIVED FROM THIS FILE rather than written out beside it. A hand-maintained list
# would drift the moment someone added an assertion, and drift here is invisible — the guard would still
# pass, just slowly. The scan is deliberately conservative: anything it cannot read literally (a shell
# variable, a quoted argument, an indented line inside a subshell) is simply left out, and `runq` answers
# it the old way. A miss therefore costs time and never correctness, which is the property that makes
# deriving-by-parsing acceptable at all.
#
# CHECK_QUERY_NO_PRELOAD=1 turns it off, which is how the reconstruction is verified — with it set every
# assertion goes through runq one process at a time, and the guard's output must be byte-identical to a
# preloaded run. That is the only proof that a batched answer is the same answer. Run it after touching
# either side of this:
#
#     sh tools/check-query.sh                     2>&1 | grep -v '^check-query: preloaded' >/tmp/a
#     CHECK_QUERY_NO_PRELOAD=1 sh tools/check-query.sh 2>&1 | grep -v '^check-query: preloaded' >/tmp/b
#     diff /tmp/a /tmp/b        # must be empty (the preloaded-count line is the one expected difference)
preload() {
    [ -z "${CHECK_QUERY_NO_PRELOAD:-}" ] || return 0
    plan="$tmp/preload.plan"
    # fixture \t --project? \t the args string runq would key on \t the args to actually pass
    awk -v root="$ROOT" '
        function emit(  i, toks, n, proj, key, qa) {
            n = split(argsrc, toks, " ")
            if (n == 0 || fixture == "") return
            proj = (toks[1] == "--project") ? 1 : 0
            key = ""; qa = ""
            for (i = 1; i <= n; i++) {
                key = key " " toks[i]
                if (!(i == 1 && proj)) qa = qa (qa == "" ? "" : " ") toks[i]
            }
            if (qa == "") return                       # --project alone is not a question
            printf "%s\t%d\t%s\t%s\n", fixture, proj, key, qa
        }
        # Only column-1 assignments: an indented one is inside a subshell whose value we cannot resolve.
        /^FIXTURE=/ {
            f = $0; sub(/^FIXTURE=/, "", f); gsub(/"/, "", f)
            if (f ~ /\$ROOT/) { sub(/\$ROOT/, root, f); fixture = f }
            else              { fixture = "" }         # e.g. "$dep/..." — unresolvable here
            next
        }
        # Only column-1 assertions, for the same reason.
        /^(expect|reject) / {
            line = $0
            sub(/^(expect|reject)[ \t]+/, "", line)
            i = index(line, " -- ")
            if (i == 0) next
            argsrc = substr(line, 1, i - 1)
            if (argsrc ~ /[$"'"'"'`]/) next            # a substitution or a quoted arg: leave it to runq
            emit()
        }
    ' "$0" | sort -u >"$plan"
    [ -s "$plan" ] || return 0

    # One group per (fixture, --project): --project widens the unit set, so it is a different analysis.
    awk -F'\t' -v d="$tmp/plg_" '
        { g = $1 SUBSEP $2
          if (!(g in n)) { n[g] = ++c; printf "%s\t%s\n", $1, $2 >(d n[g]) }
          printf "%s\t%s\n", $3, $4 >>(d n[g]) }
    ' "$plan"

    for g in "$tmp"/plg_*; do
        [ -f "$g" ] || continue
        pf=$(head -1 "$g" | cut -f1)
        pp=$(head -1 "$g" | cut -f2)
        nq=$(( $(wc -l <"$g") - 1 ))
        [ "$nq" -ge 2 ] || continue                    # one question gains nothing from a batch
        [ -f "$pf" ] || continue
        qargs=$(tail -n +2 "$g" | cut -f2 | tr '\n' ' ')
        pscope=""
        [ "$pp" = 1 ] && pscope=$(scope_for "$pf")
        # shellcheck disable=SC2086
        "$KAMA" query $pscope "$pf" $qargs >"$tmp/plout" 2>&1 || true

        # Split on the `## <question>` delimiter lines. Anything BEFORE the first one is stderr the
        # analysis wrote once (a warning); a solo run would have shown it with every answer, so it is
        # prepended to each chunk — the reconstruction has to match what runq would have cached, not
        # merely resemble it.
        rm -f "$tmp"/plc_* "$tmp/plpre"
        awk -v d="$tmp/plc_" -v pre="$tmp/plpre" '
            /^## / { k++; next }
            { if (k == 0) print >pre; else print >(d k) }
            END { print k >"/dev/stderr" }
        ' "$tmp/plout" 2>"$tmp/plcount"
        [ "$(cat "$tmp/plcount")" = "$nq" ] || continue   # not the shape we expect: leave it to runq

        i=0
        tail -n +2 "$g" | cut -f1 | while IFS= read -r ka; do
            i=$((i + 1))
            { [ -f "$tmp/plpre" ] && cat "$tmp/plpre"; [ -f "$tmp/plc_$i" ] && cat "$tmp/plc_$i"; } \
                >"$qcache/$(qkey "$pf" "$ka")" 2>/dev/null || true
        done
    done
}
preload
# Say how much was preloaded. A silent fallback — a changed assertion shape the scan stops recognizing,
# an unreadable "$0" under some runner — would otherwise look exactly like everything working, just slow.
echo "check-query: preloaded $(find "$qcache" -type f | wc -l | tr -d ' ') answers in $(find "$tmp" -name 'plg_*' | wc -l | tr -d ' ') batched analyses"

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
    out=$(runq "$args")
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
    out=$(runq "$args")
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
expect --type 18:5  -- "value Point"        # 'Point m' inside midpoint's body
expect --def  25:5  -- "shapes.kama:6:11"   # 'Point p' inside main's body -> Point decl

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
expect --def  30:11 -- "scopes.kama:29:10"  # local use -> its declaration
expect --def  23:20 -- "scopes.kama:19:17"  # 'this.limit' -> the field declaration
expect --def  53:22 -- "scopes.kama:14:4"   # 'Code::Ok'  -> the enum member declaration
expect --def  55:13 -- "scopes.kama:14:4"   # 'case Ok:'  -> the same enum member

echo "check-query: M3.4 find-references"
expect --refs 29:10 -- "scopes.kama:30:11"  # local 'seeded' decl -> its one use
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
# tests/query/ws/ is a three-file package in TWO modules: `src/` holds app.kama and widget.kama, and
# `src/parts/` holds part.kama, which the manifest lists as its own module.
#
# ⚠️ The asymmetry this pins has MOVED TWICE, and pretending otherwise would each time have left a guard
# that passes without testing anything. It began as "app.kama imports widget.kama and widget.kama imports
# nothing, so widget's unit set is JUST ITSELF" — true when a file's module was the `namespace` it
# declared and app.kama declared none. 2d made a module a FOLDER (§2b), so the two became one module and
# a bare query on either saw the other, through the same-directory sibling scan.
#
# §2i.40 has now closed that scan for a loose build, and a bare `kama query <file>` IS one: no manifest
# operand, so no manifest participates — including in deciding what else to load. So the unit set is the
# file again, and this time for the reason the model states rather than for a declaration's sake. The
# scope operand is how you ask the other question, and that is §2g.35's asymmetry exactly: the CLI takes
# its operand at its word, the EDITOR walks (check-lsp measures the editor half over real JSON-RPC).
FIXTURE="$ROOT/tests/query/ws/src/widget.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.5 workspace indexing"
# Without --project: the operand, and nothing the operand did not name.
# (`--refs` prints absolute paths under --project and the given path without it, so match on the
# basename+position, which both forms carry.)
expect --refs 12:11 -- "widget.kama:12:11"          # the declaration itself
expect --refs 12:11 -- "widget.kama:22:3"           # 'fn Widget defaultWidget()' return type
expect --refs 12:11 -- "widget.kama:22:35"          # ... and the 'Widget.of(...)' call in its body
reject --refs 12:11 -- "app.kama"                   # ... but NOT its folder-sibling: nothing sneaks into
                                                    # a loose build that the operand list did not name
reject --refs 12:11 -- "part.kama"                  # ... and still not another module (the M3.3 blind spot)
# With --project: the same query reaches every module in the package.
expect --project --refs 12:11 -- "part.kama:9:18"   # 'fn int32 partSize(Widget w)' in the `parts` module
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
count=$("$KAMA" query "$(scope_for "$FIXTURE")" "$FIXTURE" --refs 12:11 2>&1 | grep -c "widget.kama:12:11" || true)
if [ "$count" = 1 ]; then
    echo "  ok: the declaration is reported exactly once (no self-reference duplicate)"
else
    echo "  FAIL: expected the decl at widget.kama:12:11 once, got $count" >&2
    fail=1
fi

# ---------------------------------------------------------------------------------------------------
# --search NAME — the BY-NAME entry point.
#
# Every other mode takes an L:C, which suits a caller holding a caret and nobody else. A script or an
# agent knows what a thing is CALLED, so without this it has to run --symbols, parse it, and come back —
# and across files it cannot get there at all. Scope is the files asked about, exactly as --refs: the one
# named file, or the whole package under --project.
FIXTURE="$ROOT/tests/query/ws/src/app.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: --search (by name, no cursor)"
# app.kama USES Widget but does not declare it, so the unscoped search must not reach the declaring file.
reject --search Widget -- "widget.kama"
# ... and with --project it does — the capability that has no L:C equivalent, since you cannot point a
# cursor at a file you have not opened.
expect --project --search Widget -- "widget.kama:12:11 value Widget"
expect --project --search Widget -- "widget.kama:15:16 ctor Widget.of"
# Matching is case-insensitive and on a SUBSTRING, so a half-remembered name still lands.
expect --project --search widget -- "widget.kama:12:11 value Widget"
expect --project --search Widg   -- "widget.kama:12:11 value Widget"
# It reaches every kind, not just types.
expect --project --search defaultSize -- "function defaultSize"
# A miss says so rather than printing nothing, so a caller can tell "no match" from "command broke".
expect --project --search Zzzz -- "no symbols"
# The package boundary holds: std is loaded in the index and must never be offered as the user's own.
reject --project --search string -- "lib/std"
# An EMPTY needle lists everything in scope — the whole-package outline that --symbols cannot give.
# (Tested directly: the expect helper word-splits its args, so an empty one cannot survive it.)
if "$KAMA" query "$(scope_for "$FIXTURE")" "$FIXTURE" --search "" 2>&1 | grep -qF "widget.kama:12:11 value Widget" &&
   "$KAMA" query "$(scope_for "$FIXTURE")" "$FIXTURE" --search "" 2>&1 | grep -qF "app.kama"; then
    echo "  ok: --search '' lists every symbol in the package"
else
    echo "  FAIL: --search '' should list the whole package" >&2
    fail=1
fi

echo "check-query: --diagnostics"
# Same list `kama check` prints, on stdout, without a pass/fail exit.
expect --project --diagnostics -- "no diagnostics"
dbad="$tmp/diagbad.kama"
printf 'fn int32 main() {\n    nope(a: 1);\n    return 0;\n}\n' > "$dbad"
if "$KAMA" query "$dbad" --diagnostics 2>/dev/null | grep -q "error: call to unknown function"; then
    echo "  ok: --diagnostics reports an analysis error on stdout"
else
    echo "  FAIL: --diagnostics missed the unknown-function error" >&2
    fail=1
fi
# `query` reports, `check` judges: an error must NOT turn into a non-zero exit here.
if "$KAMA" query "$dbad" --diagnostics >/dev/null 2>&1; then
    echo "  ok: --diagnostics exits 0 even with errors (query reports, check judges)"
else
    echo "  FAIL: --diagnostics must not fail the process on a diagnostic" >&2
    fail=1
fi
# THE BOUNDARY GUARD. `kama check` type-checks by KIND — a `string` cannot initialize an `int32` — and
# by WIDTH: kama has no implicit numeric conversion, so `int8 a = big` is an error wanting
# `cast<int8>(big)`. A LITERAL is typed by its destination and is therefore NOT a conversion, which is
# the other end of the width boundary and the one a blunt fix would break. All three are documented in
# usage(), in docs/agents.md and in AGENTS.md, and all three are pinned here so none can quietly become
# a lie. (check-agents.sh asserts the same set; docs/agents.md names both guards.)
tbad="$tmp/typebad.kama"
printf 'fn int32 main() {\n    int32 x = "oops";\n    return 0;\n}\n' > "$tbad"
if "$KAMA" check "$tbad" >/dev/null 2>&1; then
    echo "  FAIL: \`check\` no longer catches a kind mismatch — the docs promise it does" >&2
    fail=1
else
    echo "  ok: \`check\` catches a kind mismatch (\`int32 x = \"oops\"\`)"
fi
twide="$tmp/typewidth.kama"
printf 'fn int32 main() {\n    int32 big = 300;\n    int8 a = big;\n    return 0;\n}\n' > "$twide"
if "$KAMA" check "$twide" >/dev/null 2>&1; then
    echo "  FAIL: \`check\` no longer catches a width mismatch — the docs promise no implicit conversion" >&2
    fail=1
else
    echo "  ok: \`check\` catches a width mismatch (\`int8 a = big\`)"
fi
tlit="$tmp/typelit.kama"
printf 'fn int32 main() {\n    int8 a = 100;\n    float32 f = 3;\n    int8 b = 2 + 3;\n    return 0;\n}\n' > "$tlit"
if "$KAMA" check "$tlit" >/dev/null 2>&1; then
    echo "  ok: \`check\` still accepts a literal typed by its destination (\`int8 a = 100\`)"
else
    echo "  FAIL: \`check\` now rejects a literal at its destination's type — the width rule must" >&2
    echo "        exempt contextually-typed literals (docs/agents.md: what is NOT a conversion)." >&2
    fail=1
fi

# ---------------------------------------------------------------------------------------------------
# --json — ONE envelope for every mode.
#
# The text forms above are four different shapes (`L:C kind name`, `path:L:C`, `key=value`, tab-separated
# rows) plus six magic empties. That is fine for a human with grep and hostile to a parser, so --json is
# the machine format: same envelope every time, `results` always an array, `[]` where the text says "no
# definition". Everything above this line is the regression proof that adding it changed no text output.
echo "check-query: --json envelope"
JQ_FIXTURE="$ROOT/tests/query/shapes.kama"
# mode name : flags. Every mode must be here — a new one without a --json arm would emit no `results`.
for spec in \
    "symbols:--symbols" \
    "search:--search Point" \
    "def:--def 17:20" \
    "type:--type 6:11" \
    "refs:--refs 6:11" \
    "complete:--complete 24:5" \
    "sighelp:--sighelp 17:20" \
    "coverage:--coverage" \
    "diagnostics:--diagnostics" \
; do
    mode=${spec%%:*}
    flags=${spec#*:}
    # shellcheck disable=SC2086
    out=$("$KAMA" query "$JQ_FIXTURE" $flags --json 2>/dev/null || true)
    ok=1
    printf '%s' "$out" | grep -qF '"schema":1'        || ok=0
    printf '%s' "$out" | grep -qF "\"mode\":\"$mode\"" || ok=0
    printf '%s' "$out" | grep -qF '"results":'        || ok=0
    # Exactly one line: the envelope is a record, so a pipeline can read it line by line.
    [ "$(printf '%s\n' "$out" | wc -l | tr -d ' ')" = 1 ] || ok=0
    if [ "$ok" = 1 ]; then
        echo "  ok: --json $mode carries schema+mode+results on one line"
    else
        echo "  FAIL: --json $mode envelope wrong, got: $out" >&2
        fail=1
    fi
done

# A miss is an EMPTY ARRAY, never one of the text form's magic strings. This is the whole reason a caller
# can skip a per-mode parser, so it is asserted rather than assumed.
for spec in "def:--def 1:0" "type:--type 1:0" "sighelp:--sighelp 1:0" "search:--search Zzzz"; do
    mode=${spec%%:*}; flags=${spec#*:}
    # shellcheck disable=SC2086
    out=$("$KAMA" query "$JQ_FIXTURE" $flags --json 2>/dev/null || true)
    if printf '%s' "$out" | grep -qF '"results":[]' &&
       ! printf '%s' "$out" | grep -qE 'no (definition|type|signature|symbols)'; then
        echo "  ok: --json $mode reports a miss as [] (not a magic string)"
    else
        echo "  FAIL: --json $mode should report a miss as [], got: $out" >&2
        fail=1
    fi
done

# `check --json` puts the verdict in the document AND keeps the exit code, so neither a script reading
# stdout nor one testing `$?` has to change when --json is added.
cjbad="$tmp/checkjson.kama"
printf 'fn int32 main() {\n    nope(a: 1);\n    return 0;\n}\n' > "$cjbad"
out=$("$KAMA" check "$cjbad" --json 2>/dev/null || true)
rc=0; "$KAMA" check "$cjbad" --json >/dev/null 2>&1 || rc=$?
if printf '%s' "$out" | grep -qF '"ok":false' && [ "$rc" != 0 ]; then
    echo "  ok: check --json reports ok:false AND still exits non-zero"
else
    echo "  FAIL: check --json verdict/exit wrong (rc=$rc): $out" >&2
    fail=1
fi
out=$("$KAMA" check "$JQ_FIXTURE" --json 2>/dev/null || true)
if printf '%s' "$out" | grep -qF '"ok":true'; then
    echo "  ok: check --json reports ok:true on a clean file"
else
    echo "  FAIL: check --json should report ok:true, got: $out" >&2
    fail=1
fi

# `subject` — the NAME a diagnostic is about, carried as data rather than left in the prose. The LSP's
# auto-import quick fix acts on this field, and the reason it is a field is the house rule: `message` is
# written for a human and gets reworded, so a tool recovering the name by parsing it would turn any
# wording edit into a silent tooling regression.
#
# BOTH directions, and the second is the one that gives the first its meaning: it must be OMITTED where
# there is no name, or "has a subject" stops distinguishing anything and every diagnostic looks
# actionable to an editor.
sjbad="$tmp/subject.kama"
printf 'fn int32 main() {\n    Nonexistent thing;\n    return 0;\n}\n' > "$sjbad"
out=$("$KAMA" check "$sjbad" --json 2>/dev/null || true)
if printf '%s' "$out" | grep -qF '"subject":"Nonexistent"'; then
    echo "  ok: --json carries a diagnostic's \`subject\` (the name a quick fix acts on)"
else
    echo "  FAIL: --json dropped the diagnostic subject, got: $out" >&2
    fail=1
fi
# `local … has no initializer` is about a local, not about an importable name — no subject.
if printf '%s' "$out" | grep -qF '"subject":"thing"'; then
    echo "  FAIL: a diagnostic with no actionable name carried a subject anyway: $out" >&2
    fail=1
else
    echo "  ok: ...and omits it where there is no name to act on"
fi

# Real parseability, not just the shape. Skipped rather than failed where python3 is absent, so the guard
# stays runnable on a bare box — the grep assertions above still hold the line there.
if command -v python3 >/dev/null 2>&1; then
    pfail=0
    for spec in "--symbols" "--search Point" "--complete 24:5" "--coverage" "--diagnostics"; do
        # shellcheck disable=SC2086
        if ! "$KAMA" query "$JQ_FIXTURE" $spec --json 2>/dev/null | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
            echo "  FAIL: --json $spec is not valid JSON" >&2
            pfail=1; fail=1
        fi
    done
    [ "$pfail" = 0 ] && echo "  ok: every --json mode parses as valid JSON"
else
    echo "  skip: python3 absent — JSON parsed only by shape"
fi

# ---------------------------------------------------------------------------------------------------
# M3.5 — DECLARED project scope (`kama_workspace.json` + each member's `source`).
#
# tests/query/mono/ is a workspace whose members sit at two different DEPTHS. The root file lists
# `libs/*` and `group/libs/*`; each member's `source` root is the default `src/`; and
# `outside/stray.kama` declares a same-named `Gear` that nothing ever claims. Because the scope is
# DECLARED rather than inferred, no directory walk of the repo happens, the file cap does not apply, the
# deeper member is still reached, and the stray type cannot collide with the workspace's.
#
#   mono/kama_workspace.json        projects: libs/* and group/libs/*, both mandatory
#     libs/core/kama.json           source: src/ (default)  <- declares Gear
#     libs/app/kama.json            source: src/ (default)  <- uses Gear
#     group/libs/plugin/kama.json   source: src/ (default)  <- uses Gear, TWO levels down
#     outside/stray.kama            claimed by nobody       <- must never appear
#
# There is NO manifest at `group/`, and keeping that member is the point: depth is spelled by a deeper
# GLOB, because a workspace does not nest and neither does a project. This used to be a monorepo inside a
# monorepo, which made "which manifest owns this file" a cycle-broken tree walk over ancestors.
#
# The member directory is `libs/`, NOT `packages/`: kama.lock uses `packages` for resolved dependencies,
# so a folder of that name beside a workspace file would teach exactly the confusion the naming avoids.
FIXTURE="$ROOT/tests/query/mono/libs/core/src/gearcore.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.5 declared project scope (kama_workspace.json + source)"
# The consuming member is reached even though the declaring one never imports it — and reached WITHOUT
# an editor workspace root, because the workspace file explicitly lists both.
expect --project --refs 9:11 -- "gearcore.kama:9:11"        # the declaration
expect --project --refs 9:11 -- "gearapp.kama:7:4"          # a SIBLING member's use
expect --project --refs 9:11 -- "gearplugin.kama:6:4"       # a member TWO levels down (the deeper glob)
reject --project --refs 9:11 -- "stray.kama"                # ... and never the undeclared decoy
# Without --project the sibling member is invisible again (the closure is one file).
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
mkdir -p "$dep/geo/src" "$dep/app/src"
cat > "$dep/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library" }
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
  "dependencies": { "geo": { "path": "../geo" } } }
JSON
cat > "$dep/app/src/app.kama" <<'KAMA'
import { geo::Point };
fn int32 main() {
    Point p = Point.of(x: 7);
    return p.x;
}
KAMA
if (cd "$dep/app" && "$KAMA" pkg install >/dev/null 2>&1); then
    FIXTURE="$dep/app/src/app.kama"
    echo "check-query: M3.5 installed dependency"
    # The dep's own source is where its declaration lives — go-to-def crosses the package boundary.
    expect --project --def  3:4 -- "/.kama/deps/geo/src/geo.kama:3:11"
    expect --project --type 3:4 -- "value Point"
    expect --project --refs 3:4 -- "app.kama:3:4"                  # our use
    expect --project --refs 3:4 -- "/.kama/deps/geo/src/geo.kama:3:11"  # ... and the dep's declaration
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
expect --complete 110:21 -- "trigger=dot recv=c "                    # c.|value
expect --complete 112:27 -- "trigger=dot recv=h.cell "               # h.cell.|value — a chained receiver
expect --complete 113:33 -- "trigger=dot recv=makeHolder() "         # a CALL receiver, canonicalized to `()`
expect --complete 117:30 -- "trigger=dot recv=cells[] "              # an INDEX receiver, canonicalized to `[]`
expect --complete 119:27 -- "trigger=dot recv=owned "                # a smart-pointer receiver
expect --complete 134:24 -- "trigger=scope recv=Level "              # Level::|High
expect --complete 134:24 -- "active=-1"                              # `(a == b)` is a GROUPING paren, not a call
expect --complete 122:27 -- "trigger=arg-label recv= callee=blend prefix= active=0 filled="   # blend(|lo: …)
expect --complete 122:34 -- "trigger=arg-label recv= callee=blend prefix= active=1 filled=lo" # …, |hi: 4)
# ⚠️ BOTH POSITIONS ARE `import-symbol` NOW, and that is the syntax change rather than a weakening. With
# the scope inside the braces every position in the block is inside them, so the trigger no longer
# distinguishes "naming a module" from "naming a symbol" — the RECEIVER does, and it is what the completer
# keys on (it offers sub-modules and symbols of `recv` together, since `std::|` could want either).
expect --complete 15:14  -- "trigger=import-symbol recv=std "                # import { std::|collections
expect --complete 15:31  -- "trigger=import-symbol recv=std::collections "   # import { std::collections::|Dyn
# Literals and comments hold no code — and an interpolation HOLE does, so it must still complete.
expect --complete 123:21 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a string body
expect --complete 125:17 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a // comment
expect --complete 126:15 -- "trigger=bare recv= callee= prefix= active=-1 filled="   # inside a /* block */
expect --complete 124:30 -- "trigger=dot recv=c "                    # inside "interp ${c.|value} hole"

# ---------------------------------------------------------------------------------------------------
# M4.1 — member completion after `.`. Output is one `kind<TAB>label<TAB>detail` line per candidate.
echo "check-query: M4.1 member completion (receivers)"
expect --complete 110:21 -- "field	value	int32"                     # c.| — a public field, with its type
expect --complete 110:21 -- "method	doubled	fn int32 doubled()"       # ... and its methods
reject --complete 110:21 -- "secret"                                  # ... but NEVER a private one from outside
expect --complete 112:27 -- "field	value	int32"                     # h.cell.| — chained through a field's type
expect --complete 113:33 -- "field	cell	Cell"                       # makeHolder().| — a call's return type
expect --complete 114:40 -- "field	value	int32"                     # makeHolder().get().| — a CALLED tail segment
expect --complete 117:30 -- "method	doubled	fn int32 doubled()"       # cells[0].| — a generic instance's ELEMENT
expect --complete 119:27 -- "method	read	fn int32 read()"            # owned.| — through Owned<Node>'s Deref
expect --complete 118:33 -- "ctor	make	fn Node make(tag: int32)"     # Node.| — a TYPE receiver offers ctors
reject --complete 119:27 -- "ctor"                                    # ... and an INSTANCE receiver never does
expect --complete 53:16  -- "method	size	fn int32 size()"            # item.| where `T: Sized` — via the BOUND

echo "check-query: M4.1 generic-instance substitution + visibility"
# A generic instance's members are stored with the TEMPLATE's spellings; they must read as the INSTANCE's.
expect --complete 116:10 -- "method	add	fn void add(item: Cell)"      # DynamicArray<Cell>.add takes a Cell, not a T
expect --complete 116:10 -- "method	pop	fn Optional<Cell> pop()"      # ... including a nested generic return
# Inside a method, `this.` sees what THAT type may see — private included, inherited protected included,
# a base class's privates never.
expect --complete 23:44  -- "field	secret	int32"                     # own private field, from inside
expect --complete 36:59  -- "method	baseOnly	fn int32 baseOnly()"   # inherited PROTECTED method, from a subclass
reject --complete 36:59  -- "hidden"                                  # ... but not the base's privates
reject --complete 36:59  -- "shared"

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
expect --complete 100:32 -- "field	value	int32"    # scope
expect --complete 103:70 -- "field	value	int32"    # a match arm's payload binding (type from the variant case)

# A type may declare a FIELD and a METHOD under one name (std::process::Command has both spellings of
# `args`). Which one a path segment names depends on whether the source CALLED it — resolving `this.args.`
# through the void-returning method would silently offer nothing.
echo "check-query: M4.1 field-vs-method precedence on a real stdlib type"
FIXTURE="$ROOT/lib/std/process/process.kama"
# The two probe positions are DERIVED from the source text, not hardcoded. Pinning a line number into a
# live stdlib file makes every edit to that file — even adding a comment — fail this guard for a reason
# that has nothing to do with what it tests. The columns are still literal: they point INSIDE the line
# (just past `this.args.` / `this.`), which is the thing under test.
qline() { grep -n -m1 -F "$1" "$FIXTURE" | cut -d: -f1; }
expect --complete "$(qline 'this.args.add(item: give a)'):45" -- "method	add	fn void add(item: string)"   # this.args.| is the DynamicArray FIELD
expect --complete "$(qline 'return this.signal == 0'):43"     -- "field	code	int32"                       # ... and a plain `this.` still works

# ---------------------------------------------------------------------------------------------------
# M4.2 — after `::`. A `::` head is always a TYPE or a NAMESPACE (SPEC forbids `::` on a value, and the
# emitter rejects it), so there are exactly two answers.
FIXTURE="$ROOT/tests/query/complete.kama"

# ---------------------------------------------------------------------------------------------------
# Go-to-definition on the PRELUDE. `Optional`, `Result`, `Ordering`, the language contracts and the
# smart-pointer triad are ordinary kama declarations in real files; they are EMBEDDED into the binary,
# which is how they lost their path, and the answer used to be `no definition` for the most-navigated
# names in the language.
#
# ⚠️ THE LINE NUMBERS ARE THE ASSERTION, not decoration. tools/embed_prelude.sh wrapped each source as
# `R"KAMASRC(\n<file>` — that newline was line 1 of the embedded copy, so every declaration sat one line
# below where it does on disk. Nothing could see it while nothing pointed at the file; the moment
# go-to-definition did, `Optional` opened the blank line under itself. Pinning the exact line/column here
# is what stops that returning — and any real edit to global.kama's head fails this and should. It has
# done its job once already: `Optional` moved from line 8 to line 15 when it gained `@generate`, and this
# is the assertion that said so. Update the number WITH the edit; never relax it to a line range.
echo "check-query: go-to-definition reaches the prelude"
expect --def 102:4  -- "prelude/global.kama:15:10"    # `Optional` -> its declaration, not `no definition`
expect --def 102:27 -- "prelude/global.kama:15:10"    # ...and from the `Optional::` qualifier too
expect --def 118:4  -- "std/memory/owned.kama:21:14" # `Owned` -> the embedded built-in MODULE's own file
# The prelude stays READ-ONLY, and that is the point of `DefSite::file` being a second field rather than
# a `unit`. Not owning a declaration is a reason to refuse to REWRITE it; it was never a reason to refuse
# to OPEN it, and conflating the two is what sent this navigation nowhere.
reject --refs 102:4 -- "prelude/global.kama"

echo "check-query: M4.2 completion after \`::\`"
expect --complete 134:24 -- "enum-member	High"          # Level::| — a plain enum's members
expect --complete 134:24 -- "enum-member	Low"
expect --complete 102:37 -- "variant	Some	(value: T)"   # Optional::| — a generic variant's cases + payload
expect --complete 102:37 -- "variant	None"
expect --complete 149:28 -- "method	zero	fn int32 zero()"   # Stat::| — a static method
expect --complete 149:28 -- "ctor	of	fn Stat of(n: int32)" # ... a named ctor (the `::` bridge reaches them)
expect --complete 149:28 -- "constant	LIMIT	int32"          # ... and a type-associated `comptime` constant
# A MODULE head lists what that module declares, one level deep. The head is an IMPORTED module, and that
# is not a weakening of the assertion — it is the only spelling that exists. A file's identity is its
# FOLDER (SPEC.md §Modules), so a loose single-file fixture like this one has no module of its
# own to qualify with; `completeprobe::blend` used to work here only because the file DECLARED a namespace.
expect --complete 177:23 -- "type	DynamicArray"
expect --complete 177:23 -- "type	View"
reject --complete 177:23 -- "	Cell	"          # ... never the naming FILE's own symbols
reject --complete 177:23 -- "	blend	"

# ---------------------------------------------------------------------------------------------------
# M4.3 — bare names. The flooding guard is the whole milestone: `_classes` and `_funcs` span the entire
# import closure, so `bareNameOf` is the exact INVERSE of resolveUserNameImpl's lookup order and anything
# it cannot spell must not appear.
echo "check-query: M4.3 names in scope"
expect --complete 162:4 -- "local	near	Cell"                 # locals, with their declared types
expect --complete 162:4 -- "param	seed	int32"                # ... and parameters
expect --complete 162:4 -- "type	Cell"                       # a type declared in this file
expect --complete 162:4 -- "type	DynamicArray"               # ... one reached through an import
expect --complete 162:4 -- "function	print	fn void print(s: string)"   # the always-in-scope FLOOR
expect --complete 162:4 -- "function	args	fn Args args()"
expect --complete 162:4 -- "function	main	fn int32 main()"    # `main` is the one name the resolver rewrites
expect --complete 162:4 -- "keyword	foreach"                  # the lexer's own keyword table
# The C-ABI plumbing behind the floor is spellable but is NOT language surface. A user's own extern is.
expect --complete 162:4 -- "function	myOwnFfi"
reject --complete 162:4 -- "	free	"
reject --complete 162:4 -- "kama_args_at"
reject --complete 162:4 -- "kama_ctrl_release_strong"
reject --complete 162:4 -- "kama_main"                        # ... and never a mangled spelling
# A deeper namespace than any `using` reaches is unspellable here, mangled instances doubly so.
reject --complete 162:4 -- "DynamicArray_"
reject --complete 162:4 -- "collections__"
# Inside a method a field is spellable bare — which is exactly why a local may not shadow one.
expect --complete 23:38 -- "field	secret	int32"               # own private field
expect --complete 23:38 -- "method	size	fn int32 size()"

# ---------------------------------------------------------------------------------------------------
# M4.7 — import paths. These answer from the module RESOLVER, not the index: a module the file does not
# import yet is by definition absent from the index. Root order mirrors loadProgramUnits exactly, so what
# completes is what would actually resolve.
#
# ⚠️ Which is why the second line below is a REJECT now and used to be an expect. `shapes.kama` sits
# beside this file and was offered as a module, because a file WAS one (§2b.9). 2d deleted the
# file-module and the directory search with it, so offering `shapes` would offer a name that no longer
# resolves — the worst kind of completion, and the reason this list is now read off manifests.
echo "check-query: M4.7 import paths"
expect --complete 15:7  -- "module	std"           # `import |` -> the stdlib, always resolvable
reject --complete 15:7  -- "module	shapes"        # ... but NOT a sibling file: a file is not a module
reject --complete 15:7  -- "module	complete"      # ... and never the file itself
expect --complete 15:14 -- "module	collections"   # `import { std::|` -> the stdlib's modules
expect --complete 15:14 -- "module	process"
expect --complete 15:31 -- "type	DynamicArray	std::collections"   # `…::collections::|` -> its export manifest
expect --complete 15:31 -- "type	Deque	std::collections"

# ---------------------------------------------------------------------------------------------------
# M4.8 — `global::` names the ROOT scope. Its completion payoff is why the alias waited for an LSP: the
# always-in-scope floor is otherwise undiscoverable, since there is no module to import that would list it.
echo "check-query: M4.8 global:: completion"
expect --complete 168:12 -- "function	println	fn void println(s: string)"   # the floor
expect --complete 168:12 -- "function	args	fn Args args()"
expect --complete 168:12 -- "type	Optional"
reject --complete 168:12 -- "	Cell	"          # ... never a namespaced symbol, even this file's own
reject --complete 168:12 -- "	blend	"
reject --complete 168:12 -- "keyword"           # ... and `global::while` is not a thing
reject --complete 168:12 -- "kama_args_at"      # ... nor the C-ABI plumbing behind the floor

# ---------------------------------------------------------------------------------------------------
# M4.4 — signature help + argument labels. kama has NO positional arguments (kama.y's `argument`
# productions are all `IDENTIFIER COLON …`), so "which parameter am I on" and "what labels may I type"
# are one question with one answer.
echo "check-query: M4.4 signature help"
expect --sighelp 122:27 -- "sig=blend(lo: int32, hi: int32) -> int32 active=0"
expect --sighelp 122:34 -- "active=1"                                  # ... the next slot
expect --sighelp 116:14 -- "sig=cells.add(item: Cell)"                 # a generic INSTANCE's method: T -> Cell
expect --sighelp 102:42 -- "sig=Optional::Some(value: T)"              # a variant case, payload as labels
expect --sighelp 149:33 -- "sig=Stat::zero() -> int32 active=-1"       # a static with no parameters
expect --sighelp 152:24 -- "sig=Stat.of(n: int32) -> Stat"             # a named ctor, dot-on-type
expect --sighelp 135:52 -- "sig=measure(item: T) -> int32"             # a generic free fn
expect --sighelp 121:36 -- "sig=derived.total() -> int32"              # reached THROUGH Owned<Derived>'s Deref
expect --sighelp 162:4  -- "no signature"                              # not inside a call at all

echo "check-query: M4.4 argument-label completion"
expect --complete 122:27 -- "label	lo:	int32"                        # an empty slot admits every label
expect --complete 122:27 -- "label	hi:	int32"
expect --complete 122:34 -- "label	hi:	int32"
reject --complete 122:34 -- "label	lo:"                              # ... but never one already supplied

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
FIXTURE="$ROOT/tests/query/labels/src/lib.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M6 A2 argument labels"
# A free function's parameter: declaration, its body use, and the label in the OTHER unit.
expect --project --refs 14:23 -- "lib.kama:14:23"       # the declaration
expect --project --refs 14:23 -- "lib.kama:14:40"       # its body use (M3.1)
expect --project --refs 14:23 -- "use.kama:10:22"       # the call-site LABEL, across units (M6 A2)
# A METHOD's parameter reaches its label too — a different ParamSig path through emitReorderedCall.
expect --project --refs 19:31 -- "lib.kama:19:31"
expect --project --refs 19:31 -- "use.kama:14:12"

FIXTURE="$ROOT/tests/query/labels/src/use.kama"
# From the label's side: go-to-definition lands on the parameter, and hover names it as a param rather
# than echoing the bare spelling.
expect --project --def 10:22  -- "lib.kama:14:23"
expect --project --type 10:22 -- "param factor"
expect --project --def 14:12  -- "lib.kama:19:31"
expect --project --type 14:12 -- "param amount"
# A label is NOT the argument expression: the value after the colon keeps answering as itself, so the
# label's span cannot have swallowed it.
reject --project --type 10:22 -- "param 3"

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
FIXTURE="$ROOT/tests/query/generics/src/lib.kama"
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
n=$("$KAMA" query "$(scope_for "$FIXTURE")" "$FIXTURE" --refs 15:13 2>&1 | grep -c "lib.kama:15:13")
if [ "$n" = 1 ]; then echo "  ok: two instantiations yield ONE def-site for the field"
else echo "  FAIL: expected 1 def-site line for the field, got $n" >&2; fail=1; fi
# A generic type's METHOD, likewise — and these are CALL sites, which is B3a.
expect --project --refs 17:16 -- "lib.kama:17:16"
expect --project --refs 17:16 -- "use.kama:13:17"
expect --project --refs 17:16 -- "use.kama:18:16"

FIXTURE="$ROOT/tests/query/generics/src/use.kama"
# From the call site: go-to-definition lands on the template's declaration, not on any instance, and hover
# names the member through the TEMPLATE (`Box.get`, never `Box_int32.get`).
expect --project --def 13:17  -- "lib.kama:17:16"
expect --project --type 13:17 -- "method Box.get"
expect --project --def 12:7   -- "lib.kama:15:13"
expect --project --type 12:7  -- "field v"
# A generic method's PARAMETER reaches its call-site label too (the A2 path, through a template body).
expect --project --def 19:15  -- "lib.kama:19:29"

FIXTURE="$ROOT/tests/query/generics/src/lib.kama"
# M6 B3g: an `import`'s symbol list is a REFERENCE. Renaming `Box` used to rewrite its declaration and its
# uses and leave `import genericprobe::{Box, …}` spelling the old name — the module then imports a symbol that no
# longer exists, so the rename breaks a file it did edit. Same class as B3a, across units.
expect --project --refs 13:11 -- "use.kama:7:23"
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
expect --def 59:14 -- "spellings.kama:14:10"     # `Color` in `Color::Green` -> the enum declaration
expect --def 62:15 -- "spellings.kama:16:10"     # `Shape` in `Shape::Circle(r: 7)`
expect --def 56:14 -- "spellings.kama:34:11"    # `Point` in `Point::origin()` — a static call's TYPE
expect --type 59:14 -- "enum Color"
# The qualifier is a USE of the type, so it must be in the type's reference set — that is what makes
# renaming the enum rewrite it. The member `Green` keeps its own separate symbol.
expect --refs 14:10 -- "spellings.kama:59:14"
expect --refs 14:10 -- "spellings.kama:59:4"     # the type ANNOTATION, indexed since M0
reject --refs 14:10 -- "spellings.kama:59:21"    # `Green` belongs to the enum MEMBER, not the enum

# ---------------------------------------------------------------------------------------------------
# M6 B3f — a MODULE path is a navigation target, never a rename target.
#
# In kama a module IS a folder (SPEC.md §Modules), so renaming one is a directory move rather
# than a symbol rename. `module:` keys therefore name NO def-site — which is what makes rename and
# find-references skip them with no extra flag — while go-to-definition opens the module, the same
# gesture clangd gives `#include` and gopls gives an import path.
FIXTURE="$ROOT/tests/query/imports.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

# ⚠️ The COLUMNS moved with the syntax, though the lines did not: `import { ` is nine characters where
# `import ` was seven, so every position inside an import shifted right by two.
echo 'check-query: M6 B3f module paths'
expect --def 6:14 -- "/lib/std/collections/"     # `collections` opens the module it names
expect --type 6:14 -- "module std::collections"
expect --type 6:9  -- "module std"               # a path PREFIX no file declares...
expect --def 6:9   -- "no definition"            # ...has nothing to open, as clangd answers a partial include
# There used to be two assertions here for the file naming its OWN module — `--type`/`--def` on its
# `namespace importsprobe;` declaration. Phase 2e deleted the keyword: a file's module is its folder, so
# there is no token in the source to hover or jump from, and the feature is gone rather than moved. What
# survives is above — a module path in an `import`, which is a real token and still navigates.
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
expect --refs 10:43 -- "dispatch.kama:10:43"    # the contract declaration
expect --refs 10:43 -- "dispatch.kama:15:20"    # Cat's implementation
expect --refs 10:43 -- "dispatch.kama:20:20"    # Dog's implementation
expect --refs 10:43 -- "dispatch.kama:34:21"    # c.speak() — a concrete call
expect --refs 10:43 -- "dispatch.kama:34:33"    # d.speak() — the other concrete call
expect --refs 10:43 -- "dispatch.kama:38:26"    # s.speak() — through the fat pointer
# ...and the same set from ONE implementation, which is the direction the rename actually comes from.
expect --refs 15:20 -- "dispatch.kama:10:43"
expect --refs 15:20 -- "dispatch.kama:20:20"
expect --refs 15:20 -- "dispatch.kama:38:26"
# Go-to-definition does NOT collapse onto the contract: a concrete-typed call lands on THAT type's
# implementation, and only a fat-pointer call — whose static type IS the contract — lands on the contract.
# This is the whole reason the group is a separate relation rather than a shared def-site.
expect --def 34:21 -- "dispatch.kama:15:20"
expect --def 34:33 -- "dispatch.kama:20:20"
expect --def 38:26 -- "dispatch.kama:10:43"
expect --type 10:43 -- "method speak"

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

# ---------------------------------------------------------------------------------------------------
# MANY QUESTIONS, ONE ANALYSIS.
#
# `kama query` takes any number of modes per invocation and answers them from a single analysis. That is
# what the preload at the top of this file rides on, and it is a capability an agent uses directly, so it
# is pinned here rather than only implied by the guard getting faster.
#
# These run their own processes on purpose: each asserts something about the SHAPE of a whole invocation
# (delimiters, ordering, exit codes), which is not a thing runq's per-question cache can hold.
echo "check-query: multi-query (N questions, one analysis)"
MQ="$ROOT/tests/query/shapes.kama"

# Argv order IS answer order, and each answer is delimited by the question that produced it. Without the
# delimiter the six magic empties ("no definition", "no type", …) would be unattributable.
mq=$("$KAMA" query "$MQ" --def 6:11 --type 6:11 --refs 7:17 2>&1)
mq_order=$(printf '%s\n' "$mq" | grep '^## ' | tr '\n' '|')
if [ "$mq_order" = "## --def 6:11|## --type 6:11|## --refs 7:17|" ]; then
    echo "  ok: three questions answered in argv order, each delimited"
else
    echo "  FAIL: expected three '## ' delimiters in argv order, got: $mq_order" >&2; fail=1
fi
printf '%s\n' "$mq" | grep -qF "value Point" \
    && echo "  ok: the --type answer rides along in the batch" \
    || { echo "  FAIL: --type answer missing from the batch" >&2; fail=1; }

# A repeated flag is TWO questions, not last-wins. Before the question list existed, the second silently
# vanished — the bug this whole change is downstream of.
mq2=$("$KAMA" query "$MQ" --type 6:11 --type 7:17 2>&1)
if [ "$(printf '%s\n' "$mq2" | grep -c '^## ')" = 2 ] \
   && printf '%s\n' "$mq2" | grep -qF "value Point" \
   && printf '%s\n' "$mq2" | grep -qF "field x"; then
    echo "  ok: a repeated flag asks twice (not last-wins)"
else
    echo "  FAIL: --type 6:11 --type 7:17 did not answer both:" >&2
    printf '%s\n' "$mq2" | sed 's/^/      /' >&2; fail=1
fi

# ONE question prints exactly what it always did — no delimiter. Every assertion above, and any script a
# user already wrote, depends on this.
if "$KAMA" query "$MQ" --symbols 2>&1 | grep -q '^## '; then
    echo "  FAIL: a single question must not print a '## ' delimiter" >&2; fail=1
else
    echo "  ok: a single question is undelimited (output unchanged)"
fi

# Every coordinate is validated BEFORE any answer is printed: a partial batch that exits nonzero is worse
# than no batch, because a caller who checks the exit code has already consumed output that looks whole.
# `x=$(cmd)` carries cmd's status, so under `set -e` a deliberately-failing query would kill the guard
# outright. `|| mqrc=$?` is what keeps the nonzero exit observable instead of fatal.
mqrc=0
mqbad=$("$KAMA" query "$MQ" --def 6:11 --type bogus 2>/dev/null) || mqrc=$?
if [ "$mqrc" = 2 ] && [ -z "$mqbad" ]; then
    echo "  ok: a malformed L:C anywhere exits 2 with NO partial output"
else
    echo "  FAIL: --def 6:11 --type bogus gave rc=$mqrc and stdout '$mqbad' (want rc=2, empty)" >&2; fail=1
fi

# --json: a batch is the same envelope one level up, and each record carries the question it answers.
mqj=$("$KAMA" query "$MQ" --def 6:11 --symbols --json 2>/dev/null)
if printf '%s' "$mqj" | grep -qF '"mode":"batch"' \
   && printf '%s' "$mqj" | grep -qF '"ask":"--def 6:11"' \
   && printf '%s' "$mqj" | grep -qF '"ask":"--symbols"'; then
    echo "  ok: --json batch envelope carries an 'ask' echo per record"
else
    echo "  FAIL: --json batch envelope wrong: $mqj" >&2; fail=1
fi
if command -v python3 >/dev/null 2>&1; then
    if printf '%s' "$mqj" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["mode"]=="batch"; assert len(d["results"])==2; assert d["results"][0]["mode"]=="def"; assert d["results"][1]["mode"]=="symbols"' 2>/dev/null; then
        echo "  ok: --json batch parses, two records, in argv order"
    else
        echo "  FAIL: --json batch did not parse into two ordered records: $mqj" >&2; fail=1
    fi
fi
# A single question keeps the flat per-mode envelope — no "batch", no "ask".
mqj1=$("$KAMA" query "$MQ" --def 6:11 --json 2>/dev/null)
if printf '%s' "$mqj1" | grep -qF '"mode":"def"' && ! printf '%s' "$mqj1" | grep -qF '"ask"'; then
    echo "  ok: a single --json question keeps the flat envelope"
else
    echo "  FAIL: single --json question envelope changed: $mqj1" >&2; fail=1
fi

if [ "$fail" != 0 ]; then echo "check-query: FAILED" >&2; exit 1; fi
echo "check-query: OK"
