#!/bin/sh
# check-short-write.sh — a whole-file write is all of the file or an error, never `Ok` with a short file.
#
# `std::fs::writeText` made ONE `write()` — the partial-write primitive — so when the OS took fewer bytes than
# asked (a full disk, a file-size limit, Linux's ~2 GiB cap on one write) it returned `Ok(<count written>)` and
# left the file cut short: silent data loss for any caller that read `Ok` as "written". Under a 1 KiB file-size
# limit, 3000 bytes came back `Ok(1024)`. It now loops through `std::io::writeAll`, as `writeFile` does.
#
# A short write is made observable the way the shell does it: `ulimit -f` caps the file size, and SIGXFSZ is
# ignored so the write past the cap fails with EFBIG instead of killing the process (an ignored disposition
# survives `exec`). A fixture under `tests/` cannot set that up from inside the program it tests. POSIX only:
# Windows has no file-size limit to set, and SKIPS visibly.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-short-write: $KAMA not built" >&2; exit 1; fi
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "SKIP check-short-write (Windows has no per-process file-size limit to set)"; exit 0 ;;
esac
command -v bash > /dev/null 2>&1 || { echo "SKIP check-short-write (no bash: its \`ulimit -f\` unit is 1 KiB)"; exit 0; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-short-write: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }

# Exit 10 = Err (what a short write must be); 20 + n/100 = Ok(n), so a short Ok is told apart from a whole one.
cat > "$tmp/w.kama" <<'EOF'
import { core::args, std::fs::writeText, std::fs::writeFile, std::io::IoError, std::collections::DynamicArray };

fn int32 verdict(Result<isize, IoError> r) {
    return match (r) { case Ok(value: n): 20 + cast<int32>(n / 100); case Err(error: e): 10; };
}

fn int32 main() {
    string s = "";
    int32 i = 0;
    while (i < 3000) { s = "${s}x"; i = i + 1; }
    if (args().count() == 0) { return verdict(r: writeText(path: "out.txt", text: s)); }   // `./w` text, `./w file`
    DynamicArray<uint8> b = DynamicArray.empty();
    i = 0;
    while (i < 3000) { b.add(item: 120ui8); i = i + 1; }
    return verdict(r: writeFile(path: "out.bin", bytes: b));
}
EOF
"$KAMA" build "$tmp/w.kama" -o "$tmp/w" > "$tmp/build.log" 2>&1 || fail "the probe did not build:" "$tmp/build.log"

for form in Text File; do
    arg=; [ "$form" = File ] && arg=file
    # Unlimited first: the whole file, `Ok(3000)` — so a pass below is not an always-failing write.
    rc=0; (cd "$tmp" && ./w $arg) || rc=$?
    [ "$rc" = 50 ] || fail "write$form of 3000 bytes with no limit exited $rc, want 50 (Ok(3000))"
    rc=0; (cd "$tmp" && bash -c 'ulimit -f 1; trap "" XFSZ; exec ./w $1' _ "$arg") || rc=$?
    [ "$rc" = 10 ] || fail "write$form of 3000 bytes under a 1 KiB file-size limit exited $rc, want 10 (Err) — \
a short write was reported as success"
done

echo "check-short-write: PASS (writeText and writeFile report a short write as Err)"
