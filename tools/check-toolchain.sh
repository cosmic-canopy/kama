#!/bin/sh
# check-toolchain.sh — M1 toolchain-selector guard. NETWORK-FREE: the live installer hits GitHub, so this
# never installs. It hand-populates a fake versioned store with STUB `kama` binaries that just announce which
# version ran, then proves the PATH selector + resolution order — not the download. HOME (kamaHome() =
# $HOME/.kama) and KAMA_STORE point at a throwaway dir, so the real ~/.kama is never touched. It proves:
#   1. `toolchain list` shows the installed versions and marks the global default.
#   2. an unpinned directory resolves to the default; `toolchain default <v>` switches it.
#   3. a project `"toolchain"` pin in kama.json beats the default.
#   4. `KAMA_VERSION` beats the default but NOT a project pin (precedence: pin > env > default).
#   5. a pin to a missing version → a clear, actionable error (names the version + `kama toolchain install`).
#   6. `toolchain pin` writes the manifest; `toolchain uninstall` refuses the default, removes a non-default.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-toolchain: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Total isolation: a throwaway HOME (so kamaHome() = $HOME/.kama) + store; drop any inherited selector env.
export HOME="$(kama_native_path "$tmp")/home"
export KAMA_STORE="$HOME/.kama/store"
unset KAMA_VERSION 2>/dev/null || true
unset KAMA_NO_SELECT 2>/dev/null || true
mkdir -p "$HOME/.kama/bin"

# `.exe` where the OS requires one, matching selectorPath()/versionBin() in the driver. The selector
# recognizes itself by comparing its own executable path against selectorPath(), so a stub named `kama`
# on Windows is not the selector and not an installed version either — it is simply invisible.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
    *)                    EXE=""     ;;
esac

# The selector on PATH is a copy of a real kama (here, the repo build). It re-execs the resolved version.
cp -f "$KAMA" "$HOME/.kama/bin/kama$EXE"
SEL="$HOME/.kama/bin/kama$EXE"

# Two "installed" versions: stub binaries that just announce which one ran (stand in for real toolchains).
#
# On Windows the stub must be a REAL EXECUTABLE. The selector hands off with _spawnv, which starts a PE
# image and nothing else — a `#!/bin/sh` file is not a program it can run, so every hand-off failed. kama
# is right here and builds one in a second, which also makes the stub a genuine native binary rather than
# something only a shell would honour.
mkstub() {
    d="$HOME/.kama/versions/$1/bin"; mkdir -p "$d"
    case "$EXE" in
        .exe)
            printf 'fn int32 main() {\n    println(s: "TOOLCHAIN %s");\n    return 0;\n}\n' "$1" > "$tmp/stub-$1.kama"
            "$KAMA" build "$tmp/stub-$1.kama" -o "$d/kama.exe" >/dev/null 2>&1 \
                || { echo "check-toolchain: could not build the $1 stub" >&2; exit 1; }
            ;;
        *)
            printf '#!/bin/sh\necho "TOOLCHAIN %s"\n' "$1" > "$d/kama"
            chmod +x "$d/kama"
            ;;
    esac
}
mkstub vA
mkstub vB

fail() { echo "check-toolchain: FAIL — $1" >&2; [ -f "$tmp/out" ] && sed 's/^/  /' "$tmp/out" >&2; exit 1; }
run()  { if "$@" >"$tmp/out" 2>&1; then RC=0; else RC=$?; fi; }

# ---- 1. toolchain list: both versions, with the default marked ---------------------------------------
run "$KAMA" toolchain default vA
[ "$RC" = 0 ] || fail "toolchain default vA errored"
run "$KAMA" toolchain list
[ "$RC" = 0 ] || fail "toolchain list errored"
grep -q 'vA' "$tmp/out" && grep -q 'vB' "$tmp/out" || fail "toolchain list did not show both versions"
grep -q '\* vA' "$tmp/out" || fail "toolchain list did not mark vA as the default"

# ---- 2. an unpinned dir resolves to the default; `default` switches it --------------------------------
mkdir -p "$tmp/plain"                          # no kama.json anywhere up the tree
run sh -c "cd '$tmp/plain' && '$SEL' build x.kama"
grep -q "TOOLCHAIN vA" "$tmp/out" || fail "unpinned dir did not resolve to the default vA"
run "$KAMA" toolchain default vB
[ "$RC" = 0 ] || fail "toolchain default vB errored"
run sh -c "cd '$tmp/plain' && '$SEL' build x.kama"
grep -q "TOOLCHAIN vB" "$tmp/out" || fail "toolchain default vB did not switch the unpinned resolution"

# ---- 3. a project pin beats the default (default is now vB) -------------------------------------------
#         The project is NAMED, because the operand is what the selector reads. It no longer walks up from
#         an input file or from the CWD — see 4c, which is where that change is stated and asserted.
mkdir -p "$tmp/pinned"
printf '{ "name": "p", "kind": "executable", "toolchain": "vA" }\n' > "$tmp/pinned/kama.json"
run sh -c "cd '$tmp/pinned' && '$SEL' build kama.json"
grep -q "TOOLCHAIN vA" "$tmp/out" || fail "project pin vA did not beat the default vB"

# ---- 4. KAMA_VERSION beats the default, but NOT a project pin (pin > env > default) -------------------
run sh -c "cd '$tmp/plain'  && KAMA_VERSION=vA '$SEL' build x.kama"   # no manifest named, env vA -> vA
grep -q "TOOLCHAIN vA" "$tmp/out" || fail "KAMA_VERSION did not override the default"
run sh -c "cd '$tmp/pinned' && KAMA_VERSION=vB '$SEL' build kama.json"   # pin vA, env vB -> vA
grep -q "TOOLCHAIN vA" "$tmp/out" || fail "KAMA_VERSION wrongly overrode a project pin"

# ---- 4b. a kama.local.json toolchain override beats the kama.json pin (M5.3, dev-local) ---------------
printf '{ "toolchain": "vB" }\n' > "$tmp/pinned/kama.local.json"
run sh -c "cd '$tmp/pinned' && '$SEL' build kama.json"                # kama.json pin vA, local vB -> vB
grep -q "TOOLCHAIN vB" "$tmp/out" || fail "kama.local.json toolchain override did not beat the kama.json pin"
rm -f "$tmp/pinned/kama.local.json"

# ---- 4c. THE PIN COMES FROM THE OPERAND, and there is no walk left at all ----------------------------
#         The selector runs BEFORE argument parsing — it must, since its job is choosing which binary does
#         the parsing — so it used to scan raw argv for "an existing file ending in .kama" and then walk UP
#         from it. Hand it `kama build ../legacy/kama.json` and nothing matched, so the pin came from the
#         CURRENT DIRECTORY and ../legacy was built by whatever the CWD pinned, silently.
#
#         Now it reads the named manifest. `pinned` pins vA; the default is vB. Standing OUTSIDE it and
#         naming its manifest must give vA — the case that used to be the silent one.
printf 'fn int32 main() { return 0; }\n' > "$tmp/pinned/real.kama"
run sh -c "cd '$tmp' && '$SEL' build pinned/kama.json"
grep -q "TOOLCHAIN vA" "$tmp/out" \
    || fail "the pin did not follow the named manifest: building pinned/kama.json from outside used the CWD's toolchain"

#         ⚠️ And the deliberate BEHAVIOR CHANGE, asserted so it cannot regress by accident: naming no
#         manifest inherits NO project's pin, even standing inside one. A loose build is not a project
#         build (§2g.33), so it takes the env/global default — here vB — rather than the vA it is sitting
#         in. This is the one place the operand rule takes something away, and it is on purpose.
mkdir -p "$tmp/pinned/deep/deeper"
run sh -c "cd '$tmp/pinned/deep/deeper' && '$SEL' build nosuchfile.kama"
grep -q "TOOLCHAIN vB" "$tmp/out" \
    || fail "a loose build inside a pinned project no longer takes the global default"

# ---- 4d. A WORKSPACE RUNS IN PLACE AND RE-EXECS PER MEMBER, so each member keeps its own pin ----------
#         The selector exports KAMA_NO_SELECT immediately before it execs, as its loop-stopper, and a
#         child inherits it. If a workspace operand made the driver select a version for ITSELF, every
#         member would then skip selection and be built by that one compiler. Never exec'ing for a
#         workspace is what lets each member start clean — which is why no `toolchain` key may live in
#         kama_workspace.json in the first place.
#
#         Two members pinned to DIFFERENT versions, so one compiler for the pair would be visible: both
#         announcements must appear.
mkdir -p "$tmp/wsp/a" "$tmp/wsp/b"
printf '{ "projects": { "a": { "optional": false }, "b": { "optional": false } } }\n' > "$tmp/wsp/kama_workspace.json"
printf '{ "name": "a", "kind": "library", "toolchain": "vA" }\n' > "$tmp/wsp/a/kama.json"
printf '{ "name": "b", "kind": "library", "toolchain": "vB" }\n' > "$tmp/wsp/b/kama.json"
run sh -c "cd '$tmp' && '$SEL' build wsp/kama_workspace.json"
grep -q "TOOLCHAIN vA" "$tmp/out" || fail "the workspace fan-out did not use member a's own pin (vA)"
grep -q "TOOLCHAIN vB" "$tmp/out" || fail "the workspace fan-out did not use member b's own pin (vB)"

# ---- 5. a pin to a missing version → a clear, actionable error ---------------------------------------
mkdir -p "$tmp/missing"
printf '{ "name": "m", "kind": "executable", "toolchain": "v9" }\n' > "$tmp/missing/kama.json"
run sh -c "cd '$tmp/missing' && '$SEL' build kama.json"
[ "$RC" != 0 ] || fail "a pin to a missing version did not error"
grep -qi "not installed" "$tmp/out" && grep -q "toolchain install" "$tmp/out" \
    || fail "missing-version error was not clear/actionable"

# ---- 6. `toolchain pin` writes the manifest; `uninstall` guards the default ---------------------------
mkdir -p "$tmp/proj"
printf '{ "name": "proj", "version": "0.1.0", "kind": "executable" }\n' > "$tmp/proj/kama.json"
run sh -c "cd '$tmp/proj' && '$KAMA' toolchain pin vA kama.json"
[ "$RC" = 0 ] || fail "toolchain pin errored"
grep -q '"toolchain": "vA"' "$tmp/proj/kama.json" || fail "toolchain pin did not write the manifest"
run "$KAMA" toolchain uninstall vB             # vB is the current default → must refuse
[ "$RC" != 0 ] || fail "uninstall of the default version was not refused"
run "$KAMA" toolchain uninstall vA             # vA is not the default → removed
[ "$RC" = 0 ] || fail "uninstall of a non-default version errored"
[ ! -d "$HOME/.kama/versions/vA" ] || fail "uninstall did not remove the version dir"

echo "check-toolchain: PASS (list; default switch; kama.local.json > pin > KAMA_VERSION > default; missing-version error; pin writes manifest; uninstall guards default; the pin is READ from the named manifest, a loose build takes the default, and a workspace re-execs per member)"
