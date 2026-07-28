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
# tests/query/mono/ is a workspace: the root declares `"packages": ["packages/*"]`, each member declares
# `"sources": ["src"]`, and `outside/stray.kama` declares a same-named `Gear` that nothing ever claims.
# Because the scope is DECLARED rather than inferred, no directory walk of the repo happens, the file cap
# does not apply, and the stray type cannot collide with the workspace's.
FIXTURE="$ROOT/tests/query/mono/packages/core/src/gearcore.kama"
if [ ! -f "$FIXTURE" ]; then echo "check-query: missing $FIXTURE" >&2; exit 1; fi

echo "check-query: M3.5 declared project scope (sources + packages)"
# The consuming package is reached even though the declaring one never imports it — and reached WITHOUT an
# editor workspace root, because an ancestor manifest explicitly owns this file.
expect --project --refs 9:11 -- "gearcore.kama:9:11"        # the declaration
expect --project --refs 9:11 -- "gearapp.kama:7:4"          # a SIBLING PACKAGE's use
reject --project --refs 9:11 -- "stray.kama"                # ... and never the undeclared decoy
# Without --project the sibling package is invisible again (the closure is one file).
reject --refs 9:11 -- "gearapp.kama"

if [ "$fail" != 0 ]; then echo "check-query: FAILED" >&2; exit 1; fi
echo "check-query: OK"
