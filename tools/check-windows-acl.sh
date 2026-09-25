#!/bin/sh
# check-windows-acl.sh — a private file kama writes is private BY WINDOWS' OWN READING (KR-92).
#
# `File.openWith(… permissions: rw-------)` builds an access list (include/kama_os.h: the owner and SYSTEM,
# protected, nothing inherited), and tests/fs_permissions.kama proves kama reads back the bits it wrote. That is
# kama agreeing with itself. This asks Windows: the file's security descriptor, as SDDL (SIDs, so no locale
# renames "Everyone" out from under the check), must hold exactly the owner and SYSTEM (`SY`) — protected
# (`D:P`), no inherited (`ID`) entry, no Everyone (`WD`), Users (`BU`), Authenticated Users (`AU`) or
# Administrators (`BA`). That is the set Win32-OpenSSH accepts for a private key — it refuses a key anyone else
# can read — and a relay's signing key is the file this row was filed for (the peer project's KG-37).
#
# A `0o644` file must add Everyone and the file's group, and nothing else; a private directory holds the same two
# entries a private file does.
#
# The verdict is itself checked: the private file is then given an extra Administrators entry with icacls, and
# the same verdict must refuse it — a list comparison only ever seen passing proves nothing.
#
# Windows only — the oracle is Windows. Everywhere else it SKIPS with exit 0, like check-winargv.
#
# check-legs: native
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "SKIP check-windows-acl (Windows only: it reads kama's access lists back through Windows)"; exit 0 ;;
esac
. "$ROOT/tools/kama-bin.sh"
command -v powershell.exe >/dev/null 2>&1 || { echo "check-windows-acl: no powershell.exe on PATH" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-windows-acl: FAIL — $*" >&2; exit 1; }

cat > "$tmp/acl.kama" <<'EOF'
import { std::fs::File, std::fs::OpenMode, std::fs::Permissions, std::fs::createDirWith };

unsafe fn int32 run() {
    Permissions priv = Permissions::OwnerRead | Permissions::OwnerWrite;
    Permissions pub = priv | Permissions::GroupRead | Permissions::OtherRead;
    match (File.openWith(path: "private.key", mode: OpenMode::Write, permissions: priv)) {
        case Ok(value: f): { }
        case Err(error: e): { return 1; }
    };
    match (File.openWith(path: "public.txt", mode: OpenMode::Write, permissions: pub)) {
        case Ok(value: f): { }
        case Err(error: e): { return 2; }
    };
    match (createDirWith(path: "private.d", permissions: priv | Permissions::OwnerExecute)) {
        case Ok(value: u): { }
        case Err(error: e): { return 3; }
    };
    return 0;
}

fn int32 main() { return run(); }
EOF
"$KAMA" build "$tmp/acl.kama" -o "$tmp/acl.exe" > "$tmp/build.log" 2>&1 \
    || { sed -n '1,20p' "$tmp/build.log" >&2; fail "the probe did not build"; }
( cd "$tmp" && ./acl.exe ) || fail "the probe failed — File.openWith/createDirWith refused on this volume"

# The descriptor as SDDL: `O:<owner>G:<group>D:<flags>(ace)(ace)…`.
sddl() { powershell.exe -NoProfile -NonInteractive -Command "(Get-Acl -LiteralPath '$(cygpath -w "$1")').Sddl" | tr -d '\r'; }
owner_of() { printf '%s' "$1" | sed -n 's/^O:\([^G]*\)G:.*/\1/p'; }
group_of() { printf '%s' "$1" | sed -n 's/.*G:\([^D]*\)D:.*/\1/p'; }
aces()     { printf '%s' "$1" | sed 's/.*D:[A-Z]*//' | grep -o '([^)]*)' || true; }

# Empty when `$1`'s descriptor is exactly the owner + SYSTEM (plus the SIDs in `$2`), protected and uninherited;
# otherwise the reason, with the SDDL so the next person can read what Windows holds.
verdict() {
    s=$(sddl "$1"); o=$(owner_of "$s"); list=$(aces "$s"); want="$o SY $2"
    case "$s" in *D:P*) ;; *) echo "the list is not protected, so a parent's entries can widen it: $s"; return ;; esac
    if printf '%s\n' "$list" | grep -q ';ID;'; then echo "an entry is inherited: $s"; return; fi
    n=$(printf '%s\n' "$list" | grep -c '(' || true)
    w=$(echo $want | wc -w | tr -d ' ')
    [ "$n" -eq "$w" ] || { echo "it holds $n entries where $w were written ($want): $s"; return; }
    for sid in $want; do
        printf '%s\n' "$list" | grep -q ";;;$sid)\$" || { echo "\`$sid\` holds no entry: $s"; return; }
    done
    echo ""
}

r=$(verdict "$tmp/private.key" "")
[ -z "$r" ] || fail "a private file is not owner + SYSTEM: $r"
r=$(verdict "$tmp/private.d" "")
[ -z "$r" ] || fail "a private directory is not owner + SYSTEM: $r"
ps=$(sddl "$tmp/public.txt")
g=$(group_of "$ps")
[ "$g" != "$(owner_of "$ps")" ] || g=""   # one SID for both: kama writes no separate group entry
r=$(verdict "$tmp/public.txt" "WD $g")
[ -z "$r" ] || fail "a 0o644 file is not owner + group + Everyone + SYSTEM: $r"

# The verdict must be able to fail: widen the private file behind kama's back and ask again.
MSYS2_ARG_CONV_EXCL='*' icacls "$(cygpath -w "$tmp/private.key")" /grant '*S-1-5-32-544:(R)' >/dev/null \
    || fail "could not add the Administrators entry the self-check needs"
r=$(verdict "$tmp/private.key" "")
[ -n "$r" ] || fail "the verdict ACCEPTED a private file with an extra Administrators entry — it cannot tell private from not"

echo "check-windows-acl: PASS (a private file and directory hold exactly owner + SYSTEM, protected; 0o644 adds group + Everyone; the verdict refuses an extra entry)"
