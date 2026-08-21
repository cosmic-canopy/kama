#!/bin/sh
# check-manifest.sh — the `kama.json` SCHEMA guard.
#
# Every rule here is a rejection, and a rejection has no guard until something proves it fires — the
# house rule this repo learned the hard way (a SPEC claim that `foreach (char c in s)` was "a type
# error" went unenforced for months because no fixture asserted the rejection). The positive shapes are
# already covered everywhere else in the suite; what needs holding down is that a MALFORMED manifest is
# refused, with the message that names the actual problem.
#
# ⚠️ These cannot live in tests/xfail/. run_tests.sh's xfail leg runs `kama build <one .kama file>`, and
# a manifest rejection needs a DIRECTORY TREE around that file. Worse, dropping a kama.json into
# tests/xfail/ would change manifest discovery for all ~439 fixtures in it — the subdirectories already
# there (cola/, epriv/, …) are import targets, not fixture roots. So the trees get built in a temp dir,
# the way check-packages.sh builds its package fixtures.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-manifest: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# A project tree with one buildable source file and the manifest given on stdin.
#   proj <name> <<'JSON' … JSON
proj() {
    d="$tmp/$1"; rm -rf "$d"; mkdir -p "$d/src"
    printf 'fn int32 main() { return 7; }\n' > "$d/src/app.kama"
    cat > "$d/kama.json"
}

# The manifest must be REFUSED, non-zero, with no artifact, and the error must contain $2.
reject() {
    d="$tmp/$1"; want="$2"; what="$3"
    rm -f "$d/out.bin"
    if "$KAMA" build "$d/src/app.kama" -o "$d/out.bin" >"$tmp/o" 2>"$tmp/e"; then
        bad "$what — accepted, but must be REJECTED"; return
    fi
    if [ -f "$d/out.bin" ]; then bad "$what — rejected but still wrote an artifact"; return; fi
    if ! grep -qF "$want" "$tmp/e"; then
        bad "$what — rejected, but the error is missing \"$want\""; head -2 "$tmp/e" >&2; return
    fi
    ok "$what"
}

# The manifest must be ACCEPTED and the program must build and run. Silence is the claim, so every
# rejection above is paired with one of these — otherwise a rule that rejects EVERYTHING would pass.
accept() {
    d="$tmp/$1"; what="$2"
    rm -f "$d/out.bin"
    if ! "$KAMA" build "$d/src/app.kama" -o "$d/out.bin" >"$tmp/o" 2>"$tmp/e"; then
        bad "$what — rejected, but must be accepted"; head -3 "$tmp/e" >&2; return
    fi
    # `|| rc=$?`, not `; rc=$?` — the fixture exits 7 on purpose and `set -e` would kill the guard.
    rc=0; "$d/out.bin" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 7 ] || { bad "$what — built but ran with exit $rc, expected 7"; return; }
    ok "$what"
}

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: an unknown key is an error, not a silent skip"

# The whole reason this rule exists: `"sourses"` used to be accepted and ignored, so a typo'd key looked
# exactly like a key that did nothing — and the manifest is about to carry the module map, where a
# swallowed key would mean a swallowed visibility decision.
proj typo <<'JSON'
{ "name": "typo", "version": "0.1.0", "kind": "executable", "sourses": ["src"] }
JSON
reject typo 'unknown key `sourses`' "a misspelled key names itself"

proj typo <<'JSON'
{ "name": "typo", "version": "0.1.0", "kind": "executable" }
JSON
accept typo "the correctly spelled key builds"

# Recognition is split from storage, so a key the READER was not asked to capture must still be
# recognized. `kama build` never captures `entry`/`version`, and before the split those fell down the
# same path as a typo — this is the case that would regress if the two were re-fused.
proj unasked <<'JSON'
{ "name": "unasked", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "toolchain": "v1",
  "out": "artifacts" }
JSON
accept unasked "keys this command does not read are still recognized"

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: the pre-1.0 \`main\` key names its rename"

proj legacy <<'JSON'
{ "name": "legacy", "version": "0.1.0", "kind": "executable", "main": "src/app.kama" }
JSON
reject legacy '`main` is now `entry`' "the legacy \`main\` key is refused by name"

# Every command, not just `kama run` — which is what moving the check into the reader bought. Before
# this, `kama build` accepted the manifest silently and only `kama run` mentioned the rename.
if (cd "$tmp/legacy" && "$KAMA" run >"$tmp/o" 2>"$tmp/e"); then
    bad "\`kama run\` accepted a manifest with the legacy \`main\` key"
elif grep -qF '`main` is now `entry`' "$tmp/e"; then
    ok "\`kama run\` reports the rename too"
else
    bad "\`kama run\` rejected it without naming the rename"; head -2 "$tmp/e" >&2
fi

proj legacy <<'JSON'
{ "name": "legacy", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama" }
JSON
accept legacy "the renamed \`entry\` key builds"

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: \`kind\` is required, and its value set is closed"

proj nokind <<'JSON'
{ "name": "nokind", "version": "0.1.0" }
JSON
reject nokind 'no `kind`' "a project with no \`kind\` is refused"

# The value set is closed, and it is checked in the READER — so a near-miss is caught even on a command
# that never reads the key. Without that, `"libary"` would be a well-formed string nobody looked at.
proj badkind <<'JSON'
{ "name": "badkind", "version": "0.1.0", "kind": "libary" }
JSON
reject badkind '`kind` must be "library" or "executable"' "a misspelled \`kind\` value names the two"

proj badkind <<'JSON'
{ "name": "badkind", "version": "0.1.0", "kind": "library" }
JSON
accept badkind "\`kind\`: library builds"

proj badkind <<'JSON'
{ "name": "badkind", "version": "0.1.0", "kind": "executable" }
JSON
accept badkind "\`kind\`: executable builds"

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: \`source\` names one real subdirectory"

# The default. Every fixture above already leans on it — this one says so out loud, because the absence
# of a key is the easiest claim in the file to break without noticing.
proj dflt <<'JSON'
{ "name": "dflt", "version": "0.1.0", "kind": "library" }
JSON
accept dflt "an absent \`source\` defaults to src/"

# `.` is refused, and this is the rule that keeps "no kama.json under source" exemption-free: with `.`,
# the manifest itself, .kama/deps, out/ and any vendored project would all sit INSIDE the source root.
proj dot <<'JSON'
{ "name": "dot", "version": "0.1.0", "kind": "library", "source": "." }
JSON
reject dot '`source` must name a subdirectory' "\`source\`: \".\" is refused"

proj esc <<'JSON'
{ "name": "esc", "version": "0.1.0", "kind": "library", "source": "../elsewhere" }
JSON
reject esc 'may not reach outside the project' "\`source\` may not escape with .."

proj abs <<'JSON'
{ "name": "abs", "version": "0.1.0", "kind": "library", "source": "/etc" }
JSON
reject abs 'must be relative to the manifest' "\`source\` may not be absolute"

# A source root that is simply not there. The build says so rather than resolving to nothing, which is
# the trap `kama seed` exists to prevent: a project that builds for its author and is empty to everyone.
proj gone <<'JSON'
{ "name": "gone", "version": "0.1.0", "kind": "library", "source": "lib" }
JSON
reject gone 'does not exist' "a \`source\` naming a missing directory is named"

# A MALFORMED manifest must not stop module resolution. The editor sits above a tree it does not own, and
# refusing to resolve over a JSON typo somewhere up that tree would strip hover and go-to-definition from
# code that is itself fine. The build still refuses — that is the split being asserted here.
#
# `geo/` is FLAT on purpose: with an unparseable manifest it is not a package root at all, so the only
# thing that can find its sources is the plain directory-module listing. That is precisely the fallback
# under test — had the sources been under geo/src/, a pass would prove nothing about it.
mkdir -p "$tmp/broken/geo"
printf 'namespace geo;\nexport { v };\nfn int32 v() { return 7; }\n' > "$tmp/broken/geo/geo.kama"
printf '{ "name": "geo", oops\n' > "$tmp/broken/geo/kama.json"
printf 'import geo::{v};\nfn int32 main() { return v(); }\n' > "$tmp/broken/app.kama"
rm -f "$tmp/broken/out.bin"
if "$KAMA" build "$tmp/broken/app.kama" -o "$tmp/broken/out.bin" >"$tmp/o" 2>"$tmp/e"; then
    rc=0; "$tmp/broken/out.bin" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 7 ] && ok "a package with an unparseable manifest still resolves as a directory-module" \
                    || bad "resolved, but ran with exit $rc"
else
    bad "an unparseable manifest on a DEPENDENCY stopped resolution"; head -2 "$tmp/e" >&2
fi

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-manifest: PASS" || echo "check-manifest: FAIL" >&2
exit "$fail"
