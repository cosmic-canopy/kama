# kama-bin.sh — resolve the compiler binary in a dev checkout. SOURCED, never run:
#
#   ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
#   . "$ROOT/tools/kama-bin.sh"      # sets $KAMA
#
# The Makefile builds into out/<os>-<arch>/ so a host (mach-o) and a container (ELF) build coexist,
# and leaves the root ./kama a symlink to whichever platform built LAST. Prefer this platform's binary
# so a host run and a `tools/cdev` run can interleave without a rebuild in between; fall back to the
# root symlink (an installed/packaged tree has no out/ at all). An externally set $KAMA always wins.
if [ -z "${KAMA:-}" ]; then
    KAMA="$ROOT/out/$(uname -s)-$(uname -m)/kama"
    [ -x "$KAMA" ] || KAMA="$ROOT/kama"
fi

# Spell an absolute path the way a NATIVE binary reads it. Use it for any path handed to kama through the
# ENVIRONMENT rather than as an argument.
#
# The distinction is the whole point: msys2 rewrites POSIX-looking absolute paths when it spawns a native
# child, so `"$KAMA" build "$tmp/x.kama"` arrives as `C:/msys64/tmp/…` and just works — which is why almost
# every guard passes on Windows without thinking about this. Environment variables get no such treatment
# (only a fixed list: PATH, TMP, TEMP, HOME, …). So `KAMA_STORE=/tmp/x` reached kama.exe verbatim, _fullpath
# read it as `\tmp\x` on the CURRENT DRIVE, and the package store was created in a directory unrelated to
# the one the guard had built its fixtures in. Everywhere else this is the identity function.
kama_native_path() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) cygpath -m "$1" ;;
        *)                    printf '%s' "$1" ;;
    esac
}
