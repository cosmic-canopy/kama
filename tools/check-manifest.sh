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

# Same, for the cases whose manifest is deliberately broken in a way that stops it naming an entry.
# (An executable project states its `entry`; these are testing what happens before that is reached.)

# The manifest must be REFUSED, non-zero, with no artifact, and the error must contain $2.
#
# ⚠️ The project is named BY ITS MANIFEST, not by a file inside it. `kama build <d>/src/app.kama` is a
# LOOSE build now — it applies no manifest at all — so every assertion here would pass a broken manifest
# by never reading it. That is the whole operand rule (§2g.32) turned on the guard that tests it.
reject() {
    d="$tmp/$1"; want="$2"; what="$3"
    rm -f "$d/out.bin"
    if "$KAMA" build "$d/kama.json" -o "$d/out.bin" >"$tmp/o" 2>"$tmp/e"; then
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
#
# ⚠️ For an EXECUTABLE project only. A library has no `main`, so `kind` now picks OUTPUT=STATIC for it and
# the artifact is an archive with nothing to run — use acceptLib for those.
accept() {
    d="$tmp/$1"; what="$2"
    rm -f "$d/out.bin"
    if ! "$KAMA" build "$d/kama.json" -o "$d/out.bin" >"$tmp/o" 2>"$tmp/e"; then
        bad "$what — rejected, but must be accepted"; head -3 "$tmp/e" >&2; return
    fi
    # `|| rc=$?`, not `; rc=$?` — the fixture exits 7 on purpose and `set -e` would kill the guard.
    rc=0; "$d/out.bin" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 7 ] || { bad "$what — built but ran with exit $rc, expected 7"; return; }
    ok "$what"
}

# A LIBRARY manifest must be accepted, and what it produces is an archive: `kind` picks the OUTPUT
# default, so `kama build <lib>/kama.json` stops at the archive rather than failing at the linker looking
# for a `main` a library was never going to have.
acceptLib() {
    d="$tmp/$1"; what="$2"
    rm -f "$d/out.a"
    if ! "$KAMA" build "$d/kama.json" -o "$d/out.a" >"$tmp/o" 2>"$tmp/e"; then
        bad "$what — rejected, but must be accepted"; head -3 "$tmp/e" >&2; return
    fi
    [ -s "$d/out.a" ] || { bad "$what — accepted but produced no archive"; return; }
    ok "$what"
}

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: an unknown key is an error, not a silent skip"

# The whole reason this rule exists: `"sourses"` used to be accepted and ignored, so a typo'd key looked
# exactly like a key that did nothing — and the manifest is about to carry the module map, where a
# swallowed key would mean a swallowed visibility decision.
proj typo <<'JSON'
{ "name": "typo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "sourses": ["src"] }
JSON
reject typo 'unknown key `sourses`' "a misspelled key names itself"

proj typo <<'JSON'
{ "name": "typo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama" }
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
if (cd "$tmp/legacy" && "$KAMA" run kama.json >"$tmp/o" 2>"$tmp/e"); then
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
acceptLib badkind "\`kind\`: library builds, as an archive"

proj badkind <<'JSON'
{ "name": "badkind", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama" }
JSON
accept badkind "\`kind\`: executable builds"

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: \`source\` names one real subdirectory"

# The default. Every fixture above already leans on it — this one says so out loud, because the absence
# of a key is the easiest claim in the file to break without noticing.
proj dflt <<'JSON'
{ "name": "dflt", "version": "0.1.0", "kind": "library" }
JSON
acceptLib dflt "an absent \`source\` defaults to src/"

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

# A package root's `source` is the WHOLE answer. `geo/` here is a real package whose sources sit BESIDE
# its source root rather than inside it — that must not resolve. Before the gate the flat listing picked
# them up, so the package was importable by a layout it had never declared: the mirror of the bug the key
# exists to prevent, and invisible until a consumer's build changed shape underneath them.
mkdir -p "$tmp/gate/geo/src"
printf 'namespace geo;\nexport { v };\nfn int32 v() { return 7; }\n' > "$tmp/gate/geo/geo.kama"
printf '{ "name": "geo", "version": "1.0.0", "kind": "library" }\n' > "$tmp/gate/geo/kama.json"
printf 'import geo::{v};\nfn int32 main() { return v(); }\n' > "$tmp/gate/app.kama"
rm -f "$tmp/gate/out.bin"
if "$KAMA" build "$tmp/gate/app.kama" -o "$tmp/gate/out.bin" >"$tmp/o" 2>"$tmp/e"; then
    bad "a package resolved by a layout it never declared (the flat fallback is not gated)"
elif grep -qF "cannot resolve module 'geo'" "$tmp/e"; then
    ok "a package root's \`source\` is the whole answer — no fallback to a flat listing"
else
    bad "the ungated-layout import failed for the wrong reason"; head -2 "$tmp/e" >&2
fi

# ...and the same package resolves the moment its sources are where it says they are.
mv "$tmp/gate/geo/geo.kama" "$tmp/gate/geo/src/geo.kama"
rm -f "$tmp/gate/out.bin"
if "$KAMA" build "$tmp/gate/app.kama" -o "$tmp/gate/out.bin" >"$tmp/o" 2>"$tmp/e"; then
    rc=0; "$tmp/gate/out.bin" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 7 ] && ok "...and it resolves once they are under the source root" \
                    || bad "resolved, but ran with exit $rc"
else
    bad "the declared layout did not resolve"; head -2 "$tmp/e" >&2
fi

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: \`link\` names native libraries once, per project"

# `--cc echo` prints the command instead of running it, which is how check-target.sh inspects a link tail
# without needing the library to exist. There was no manifest key for this before: only per-target
# `ldflags` and the CLI `--link`, so a project needing -lm everywhere had nowhere to say so once.
mkdir -p "$tmp/lnk/src"
printf 'fn int32 main() { return 0; }\n' > "$tmp/lnk/src/app.kama"
cat > "$tmp/lnk/kama.json" <<'JSON'
{ "name": "lnk", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "link": ["m"] }
JSON
tail_out=$("$KAMA" build --cc "echo" "$tmp/lnk/kama.json" -o "$tmp/lnk/app" 2>/dev/null || true)
printf '%s' "$tail_out" | grep -qF -- "-lm" \
    && ok "a project's \`link\` reaches the link tail" \
    || { bad "\`link\` did not reach the link tail"; printf '%s\n' "$tail_out" | sed 's/^/    /' >&2; }

# A target OVERRIDES it wholesale rather than adding to it — the only way to say "not on this one".
cat > "$tmp/lnk/kama.json" <<'JSON'
{ "name": "lnk", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "link": ["m"],
  "select": { "TARGET": { "WINDOWS": { "link": ["ws2_32"] } } } }
JSON
tail_out=$("$KAMA" build --cc "echo" "$tmp/lnk/kama.json" --target WINDOWS -o "$tmp/lnk/app" 2>/dev/null || true)
if printf '%s' "$tail_out" | grep -qF -- "-lws2_32" && ! printf '%s' "$tail_out" | grep -qE -- '-lm( |$)'; then
    ok "a target's \`link\` REPLACES the project's, rather than adding to it"
else
    bad "target \`link\` did not override the project's"; printf '%s\n' "$tail_out" | sed 's/^/    /' >&2
fi

# ...and a target that says nothing about `link` still inherits the project's.
tail_out=$("$KAMA" build --cc "echo" "$tmp/lnk/kama.json" -o "$tmp/lnk/app" 2>/dev/null || true)
printf '%s' "$tail_out" | grep -qE -- '-lm( |$)' \
    && ok "...while a target that never mentions it inherits" \
    || { bad "a silent target lost the project's \`link\`"; printf '%s\n' "$tail_out" | sed 's/^/    /' >&2; }

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: projects do not nest"

proj nest <<'JSON'
{ "name": "nest", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama" }
JSON
mkdir -p "$tmp/nest/src/inner"
printf '{ "name": "inner", "version": "0.1.0", "kind": "library" }\n' > "$tmp/nest/src/inner/kama.json"
reject nest 'projects do not nest' "a kama.json inside \`source\` is refused"

# The passing twin, and the whole reason `source` may not be "." — a vendored project BESIDE the source
# root is a separate project, not a nested one, and needs no exemption to stay legal.
rm -rf "$tmp/nest/src/inner"
mkdir -p "$tmp/nest/vendor/inner/src"
printf '{ "name": "inner", "version": "0.1.0", "kind": "library" }\n' > "$tmp/nest/vendor/inner/kama.json"
printf 'namespace inner;\nexport { w };\nfn int32 w() { return 1; }\n' > "$tmp/nest/vendor/inner/src/inner.kama"
accept nest "...while one BESIDE it is just another project"

# ---------------------------------------------------------------------------------------------------
echo "check-manifest: the workspace is its own file, with its own schema"

# `projects` moved OUT of kama.json, and the rejection names where it went. This is the migration
# instruction, so it must fire on the ordinary build path, not only where a workspace is read.
proj oldws <<'JSON'
{ "name": "oldws", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "projects": ["libs/*"] }
JSON
reject oldws 'now lives in kama_workspace.json' "\`projects\` in a kama.json names the file it moved to"

# ---------------------------------------------------------------------------------------------------
# The workspace file is READ by `kama pkg install` — never by a build, because a project must compile
# identically whether or not its siblings are checked out (§2a's extractability invariant). So every
# assertion below drives install, and the fixture is a three-member workspace with path deps along a
# chain, which needs no network.
#
#   ws/kama_workspace.json
#   ws/libs/core/kama.json     <- a library
#   ws/libs/net/kama.json      <- depends on ../core BY PATH
#   ws/libs/app/kama.json      <- depends on ../net; installed FROM here
#
# Three links, not two, and that is load-bearing: the top-level-only rule exempts `<root manifest>`, so a
# dep the installed project declares itself never reaches the gate at all. `core` has to arrive as a
# TRANSITIVE request from `net` for the workspace to be what permits it.
ws="$tmp/ws"
mkws() {   # mkws <<'JSON' … JSON   — rebuild the tree, workspace file from stdin
    rm -rf "$ws"; mkdir -p "$ws/libs/core/src" "$ws/libs/net/src" "$ws/libs/app/src"
    printf '{ "name": "core", "version": "0.1.0", "kind": "library" }\n' > "$ws/libs/core/kama.json"
    printf 'namespace core;\nexport { v };\nfn int32 v() { return 7; }\n' > "$ws/libs/core/src/core.kama"
    printf '%s\n' '{ "name": "net", "version": "0.1.0", "kind": "library",' \
                  '  "dependencies": { "core": { "path": "../core" } } }' > "$ws/libs/net/kama.json"
    printf 'namespace net;\nimport core::{ v };\nexport { u };\nfn int32 u() { return v(); }\n' > "$ws/libs/net/src/net.kama"
    printf '%s\n' '{ "name": "app", "version": "0.1.0", "kind": "library",' \
                  '  "dependencies": { "net": { "path": "../net" } } }' > "$ws/libs/app/kama.json"
    printf 'namespace app;\nimport net::{ u };\nexport { w };\nfn int32 w() { return u(); }\n' > "$ws/libs/app/src/app.kama"
    cat > "$ws/kama_workspace.json"
}

# The workspace must be REFUSED by install, non-zero, with the error naming $1.
wsreject() {
    want="$1"; what="$2"
    if "$KAMA" pkg install "$ws/libs/app/kama.json" >"$tmp/o" 2>"$tmp/e"; then
        bad "$what — accepted, but must be REJECTED"; return
    fi
    grep -qF "$want" "$tmp/e" \
        || { bad "$what — rejected, but the error is missing \"$want\""; head -2 "$tmp/e" >&2; return; }
    ok "$what"
}

# Silence is the claim for every `optional: true` rule below, so each has one of these.
wsaccept() {
    what="$1"
    "$KAMA" pkg install "$ws/libs/app/kama.json" >"$tmp/o" 2>"$tmp/e" \
        && ok "$what" || { bad "$what — rejected, but must be accepted"; head -3 "$tmp/e" >&2; }
}

mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false } } }
JSON
wsaccept "a member may path-depend on a sibling the workspace lists"

# The gate is the DECLARATION, not adjacency: the same two directories with no workspace over them.
rm -f "$ws/kama_workspace.json"
wsreject 'only allowed at the top level' "...and may not, with no workspace file over them"

mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false }, "tools/codegen": { "optional": false } } }
JSON
wsreject 'no project at `tools/codegen`' "a missing MANDATORY member is refused, by path"

mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false }, "tools/codegen": { "optional": true } } }
JSON
wsaccept "...while an absent OPTIONAL one is silent"

# `optional` means something on a glob too, and it is what catches a mistyped root.
mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false }, "libz/*": { "optional": false } } }
JSON
wsreject '`libz/*` matched no project' "a MANDATORY glob matching nothing is refused"

mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false }, "libz/*": { "optional": true } } }
JSON
wsaccept "...while an OPTIONAL glob may match nothing"

# There is no default and no bare {}: which members may be absent is stated at every entry.
mkws <<'JSON'
{ "projects": { "libs/*": { } } }
JSON
wsreject 'does not say whether it is "optional"' "an entry with no \`optional\` is refused"

mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false, "kind": "library" } } }
JSON
wsreject 'unknown key `kind`' "an unknown key INSIDE an entry is refused"

# A different file with a different schema — not kama.json with extra keys tolerated.
mkws <<'JSON'
{ "name": "acme", "version": "0.1.0", "projects": { "libs/*": { "optional": false } } }
JSON
wsreject 'does not belong in kama_workspace.json' "a project key at the workspace top level is refused"

# `dependencies` is the ONE other legal key: build-time, host-built tooling. Parsed and validated so a
# typo cannot hide in it, though nothing consumes the entries yet.
mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false } },
  "dependencies": { } }
JSON
wsaccept "...while \`dependencies\` is legal there"

# A workspace root is not a project. Members sit BESIDE the file, so "projects do not nest" says nothing
# about them — but a root that is also a project would be a project composing projects, which is the one
# shape this whole split exists to remove.
mkws <<'JSON'
{ "projects": { "libs/*": { "optional": false } } }
JSON
printf '{ "name": "acme", "version": "0.1.0", "kind": "library" }\n' > "$ws/kama.json"
wsreject 'a workspace root is not a project' "a kama.json beside a kama_workspace.json is refused"

# ---------------------------------------------------------------------------------------------------
[ "$fail" -eq 0 ] && echo "check-manifest: PASS" || echo "check-manifest: FAIL" >&2
exit "$fail"
