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
