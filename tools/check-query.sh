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

# expect <flags...> -- <substring>: run `kama query FIXTURE <flags>` and assert the output contains <substring>.
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

if [ "$fail" != 0 ]; then echo "check-query: FAILED" >&2; exit 1; fi
echo "check-query: OK"
