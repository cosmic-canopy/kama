#!/bin/sh
# check-closure-pruning.sh — a directory-module import loads the files defining the named symbols plus
# their intra-directory reference closure, NOT every .kama in the directory.
#
# The assertions are ordered by what they protect, and several exist because the thing they protect
# actually broke during the campaign:
#   1. it prunes, and not to zero
#   2. behaviour is identical pruned vs hatched, across every fixture that imports a directory module
#   3. the unit set does not depend on a program's position in a `--each` batch (the parse cache serves
#      the SAME unit to a later analysis, and @compileFor pruning rewrites the decl list in place)
#   4. every bail-out valve still produces today's diagnostics byte for byte
#   5. a self-import re-enters per-symbol (lib/std/net/net.kama imports from inside its own namespace)
#   6. the process-global singletons still exist exactly once
#   7. `--release` unity is unaffected in behaviour
#   8. a nameless decl with a program-wide effect is never pruned away
#   9. a `html"…"` string tag still resolves (it is an unqualified reference no `import` mentions)
#
# NOT asserted: byte-identical emitted C. Per-symbol re-entry appends a module's files at several points
# instead of one, so the relative order of survivors within a module can change, which changes emit order
# and the `_F<file>` mangling of namespace-less imported files. Self-consistent within a build, so this
# guard compares BEHAVIOUR — exit codes and stdout — never bytes.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-closure-pruning: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

units_of() { "$KAMA" check "$1" 2>&1 | sed -n 's/.*OK (\([0-9]*\) unit.*/\1/p'; }

# --- 1. it prunes, and not to zero ------------------------------------------------------------------
# A RANGE, deliberately not an equality. The derived closure is smaller than the hand-prune the design
# brief measured (it reached 17; this reaches 10), and it will move again as the stdlib changes. The
# ceiling catches pruning silently regressing to a no-op; the floor catches a resolver that drops
# everything and only appears to work because the fixture needs little. Raised 17 -> 20 at `0.9.362`, when
# httpd sat exactly at 17 and `std::time`'s calendar became a second file that `Timestamp.date()` reaches
# (39 hatched, so the headroom is nowhere near a no-op).
HTTPD="$ROOT/examples/httpd/httpd.kama"
if [ ! -f "$HTTPD" ]; then
    bad "examples/httpd/httpd.kama is gone — pick another multi-module program"
else
    n_pruned=$(units_of "$HTTPD")
    n_full=$(KAMA_NO_PRUNE=1 "$KAMA" check "$HTTPD" 2>&1 | sed -n 's/.*OK (\([0-9]*\) unit.*/\1/p')
    if [ -z "$n_pruned" ] || [ -z "$n_full" ]; then
        bad "httpd did not analyze cleanly (pruned='${n_pruned:-}' hatched='${n_full:-}')"
    elif [ "$n_pruned" -ge "$n_full" ]; then
        bad "httpd: $n_pruned units pruned vs $n_full hatched — pruning is a no-op"
    elif [ "$n_pruned" -gt 20 ] || [ "$n_pruned" -lt 5 ]; then
        bad "httpd: $n_pruned units, expected 5..20 (was $n_full unpruned)"
    else
        ok "httpd resolves to $n_pruned units, not $n_full"
    fi
fi

# --- 2. behaviour identical, corpus-wide ------------------------------------------------------------
# stdout, not stderr: `check` writes its per-file verdict to stdout under --each, while stderr carries
# the unit count, which legitimately differs. Restricted to fixtures that import a directory module,
# because those are the only ones pruning can touch.
set +e
# ⚠️ The selector follows the IMPORT SYNTAX. It used to be `^import a::b::{`, which stopped matching the
# day the scope moved inside the braces — and an empty selection is silent, which is why the emptiness
# check below exists. Both spellings of an entry naming a nested module are matched: the one-line block
# and an indented entry inside a multi-line one.
grep -lE 'import \{ *[a-z_][a-z_]*::[a-z_][a-z_]*::|^[[:space:]]+[a-z_][a-z_]*::[a-z_][a-z_]*::[A-Za-z_]' \
    "$ROOT"/tests/*.kama 2>/dev/null | head -120 >"$tmp/corpus"
set -e
if [ ! -s "$tmp/corpus" ]; then
    bad "found no fixture importing a directory module — the corpus check is not running"
else
    # shellcheck disable=SC2046
                      "$KAMA" check --each $(cat "$tmp/corpus") >"$tmp/pruned.out"  2>/dev/null || true
    KAMA_NO_PRUNE=1   "$KAMA" check --each $(cat "$tmp/corpus") >"$tmp/hatched.out" 2>/dev/null || true
    if cmp -s "$tmp/pruned.out" "$tmp/hatched.out"; then
        ok "$(wc -l <"$tmp/corpus" | tr -d ' ') directory-module fixtures: verdicts identical pruned vs hatched"
    else
        bad "check verdicts differ pruned vs hatched"
        diff "$tmp/hatched.out" "$tmp/pruned.out" | head -10 >&2
    fi
fi

# --- 3. the unit set is not batch-position dependent ------------------------------------------------
# `check --each` turns the parse cache ON, and @compileFor pruning rewrites codeDeclarationList IN PLACE,
# so a second analysis of a cached unit sees a decl list that has already lost its gated names. Indexing
# off that would make a program's unit count depend on what was checked before it. The facts the closure
# reads are harvested at parse time precisely so this cannot happen; this is what proves it.
cat >"$tmp/gated.kama" <<'EOF'
import { std::collections::sort };

fn int32 main() { return 0; }
EOF
"$KAMA" check --each "$tmp/gated.kama" "$tmp/gated.kama" >/dev/null 2>"$tmp/twice.err" || true
counts=$(sed -n 's/.*OK (\([0-9]*\) unit.*/\1/p' "$tmp/twice.err" | sort -u | wc -l | tr -d ' ')
if [ "$counts" = 1 ]; then
    ok "same file twice in one batch analyzes to the same unit count"
else
    bad "unit count depends on batch position ($(sed -n 's/.*OK (\([0-9]*\) unit.*/\1/p' "$tmp/twice.err" | tr '\n' ' '))"
fi

# --- 4. the valves keep today's diagnostics ---------------------------------------------------------
# Anything the closure does not fully understand must load the whole module, so the existing tailored
# message still fires. Two that matter: a symbol that does not exist, and one @compileFor dropped (which
# must still say "not available in this build configuration", never "cannot resolve module").
cat >"$tmp/nosuch.kama" <<'EOF'
import { std::collections::NoSuchThingAtAll };

fn int32 main() { return 0; }
EOF
rc_p=0; "$KAMA" check "$tmp/nosuch.kama" >/dev/null 2>"$tmp/nosuch.p" || rc_p=$?
rc_f=0; KAMA_NO_PRUNE=1 "$KAMA" check "$tmp/nosuch.kama" >/dev/null 2>"$tmp/nosuch.f" || rc_f=$?
if [ "$rc_p" = "$rc_f" ] && cmp -s "$tmp/nosuch.p" "$tmp/nosuch.f"; then
    ok "unknown imported symbol: exit code and stderr identical pruned vs hatched"
else
    bad "unknown imported symbol diagnoses differently under pruning (rc $rc_p vs $rc_f)"
    diff "$tmp/nosuch.f" "$tmp/nosuch.p" | head -6 >&2
fi

rc_p=0; "$KAMA" check --no-heap "$tmp/gated.kama" >/dev/null 2>"$tmp/gate.p" || rc_p=$?
rc_f=0; KAMA_NO_PRUNE=1 "$KAMA" check --no-heap "$tmp/gated.kama" >/dev/null 2>"$tmp/gate.f" || rc_f=$?
if [ "$rc_p" = "$rc_f" ] && cmp -s "$tmp/gate.p" "$tmp/gate.f"; then
    ok "@compileFor-gated import: exit code and stderr identical pruned vs hatched"
else
    bad "a @compileFor-gated import diagnoses differently under pruning (rc $rc_p vs $rc_f)"
    diff "$tmp/gate.f" "$tmp/gate.p" | head -6 >&2
fi

# --- 5. self-import re-enters per-symbol ------------------------------------------------------------
# lib/std/net/net.kama imports from INSIDE namespace std::net. Today that is short-circuited only
# because the whole directory loads at once; under pruning `provided` has to be per-symbol or the file
# defining the symbol is never loaded.
cat >"$tmp/selfimp.kama" <<'EOF'
import { std::net::TcpListener };

fn int32 main() { return 0; }
EOF
if "$KAMA" check "$tmp/selfimp.kama" >/dev/null 2>&1; then
    ok "a module that imports from its own namespace still resolves"
else
    bad "self-import broke: per-symbol re-entry is not pulling in the defining file"
    "$KAMA" check "$tmp/selfimp.kama" 2>&1 | head -5 >&2
fi

# --- 6. the process-global singletons still exist exactly once --------------------------------------
# The panic hook, the log sink and the argv slots are defined ONCE, in the entry TU. Pruning changes
# which TUs exist, so a duplicate or a missing definition is the failure mode — and both are LINK
# errors, which is what makes this assertion cheap: if it links and runs, there is exactly one of each.
cat >"$tmp/single.kama" <<'EOF'
import {
    std::collections::DynamicArray,
    std::log::logInfo,
};

fn int32 main() {
    DynamicArray<int32> d = DynamicArray.empty();
    d.add(item: 7);
    logInfo(tag: "guard", msg: "one");
    return d[0];
}
EOF
if "$KAMA" build "$tmp/single.kama" -o "$tmp/single" >"$tmp/single.log" 2>&1; then
    rc=0; "$tmp/single" >/dev/null 2>&1 || rc=$?
    [ "$rc" = 7 ] && ok "multi-TU singletons: links and runs (exactly one of each)" \
                  || bad "singleton fixture ran but returned $rc, expected 7"
else
    bad "multi-TU singleton fixture failed to build/link under pruning"
    tail -8 "$tmp/single.log" >&2
fi

# --- 7. --release unity is unaffected in behaviour --------------------------------------------------
"$KAMA" build "$tmp/single.kama" -o "$tmp/rel_p" --release >/dev/null 2>&1 || true
KAMA_NO_PRUNE=1 "$KAMA" build "$tmp/single.kama" -o "$tmp/rel_f" --release >/dev/null 2>&1 || true
if [ -x "$tmp/rel_p" ] && [ -x "$tmp/rel_f" ]; then
    rc_p=0; "$tmp/rel_p" >"$tmp/rel_p.out" 2>&1 || rc_p=$?
    rc_f=0; "$tmp/rel_f" >"$tmp/rel_f.out" 2>&1 || rc_f=$?
    if [ "$rc_p" = "$rc_f" ] && cmp -s "$tmp/rel_p.out" "$tmp/rel_f.out"; then
        ok "--release unity behaves identically pruned vs hatched"
    else
        bad "--release differs pruned vs hatched (rc $rc_p vs $rc_f)"
    fi
else
    bad "--release build produced no binary"
fi

# --- 8. a nameless decl with a program-wide effect survives -----------------------------------------
# Two kinds, both unreachable by any reference and so both unprunable:
#   `type intrinsic <int32> implements Parseable` — registers a conformance for a PRIMITIVE, under no name.
#   `extern "kama_isolate.h";`                  — the seam `spawn`/`parallel_for` require while naming
#                                                 nothing in the file that provides it.
cat >"$tmp/nameless.kama" <<'EOF'
import { std::fmt::parse, std::fmt::ParseError };

fn int32 main() {
    Result<int32, ParseError> r = parse::<int32>(s: "7");
    return match (r) { case Ok(value: v): v; case Err(error: e): 1; };
}
EOF
if "$KAMA" build "$tmp/nameless.kama" -o "$tmp/nameless" >"$tmp/nameless.log" 2>&1; then
    rc=0; "$tmp/nameless" >/dev/null 2>&1 || rc=$?
    [ "$rc" = 7 ] && ok "intrinsic conformance survives pruning (parse::<int32>)" \
                  || bad "intrinsic fixture returned $rc, expected 7"
else
    bad "a 'type intrinsic' conformance was pruned away"
    tail -6 "$tmp/nameless.log" >&2
fi

cat >"$tmp/seam.kama" <<'EOF'
import { std::concurrent::Atomic };

fn void bump(ref Atomic<int32> c) { c.fetchAdd(delta: 1i32); }

fn int32 main() {
    Atomic<int32> a = Atomic.make(value: 0i32);
    scope { spawn bump(c: ref a); }
    return a.load();
}
EOF
if "$KAMA" check "$tmp/seam.kama" >"$tmp/seam.log" 2>&1; then
    ok "the isolate seam survives an import that never names it (spawn + {Atomic})"
else
    bad "spawn lost its seam: the file holding \`extern \"kama_isolate.h\"\` was pruned away"
    head -4 "$tmp/seam.log" >&2
fi

# --- 9. a string tag still resolves -----------------------------------------------------------------
# `html"…"` is an unqualified reference to a sibling top-level fn, lexed by its own rule that bypasses
# getToken. If that rule stops recording the identifier, this is what breaks.
cat >"$tmp/tagged.kama" <<'EOF'
import { std::fmt::html };

fn int32 main() {
    string name = "x";
    string s = html"<b>${name}</b>";
    return s.length() > 0 ? 0 : 1;
}
EOF
if "$KAMA" check "$tmp/tagged.kama" >"$tmp/tagged.log" 2>&1; then
    ok "a tagged string still resolves its tag under pruning"
else
    bad "string tag broke — the STRING_TAG lexer rule is not recording its identifier"
    head -4 "$tmp/tagged.log" >&2
fi

[ "$fail" = 0 ] && { echo "check-closure-pruning: PASS"; exit 0; }
echo "check-closure-pruning: FAIL" >&2; exit 1
