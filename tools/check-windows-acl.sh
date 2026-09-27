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
# entries a private file does. An EXISTING file given a private list — the other half of the seam, where
# `SetKernelObjectSecurity` rewrites a list the file inherited — must come out the same: protected, nothing
# inherited. Its sibling plain create proves the directory hands out inherited entries to begin with.
#
# A deny entry is judged by Windows' own access check: under `0o077` Everyone may read and the owner, who is one
# of Everyone, may not — only a deny entry ahead of Everyone's grant says that, and the owner's read must fail.
#
# And Win32-OpenSSH (System32's, by full path) is asked directly: it must load the private key kama wrote and
# refuse the SAME bytes at `0o644`, so what it accepts is the list and nothing else. msys2's own ssh-keygen is
# never the one asked: on a `noacl` mount its key check is skipped, and it accepts anything.
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

# The bytes the private and public files hold: a real private key where Win32-OpenSSH is present, so the same
# files can be handed to it below.
sshkg="$(cygpath -S)/OpenSSH/ssh-keygen.exe"
if [ -x "$sshkg" ]; then
    "$sshkg" -q -t ed25519 -N '' -C kama-acl -f "$(cygpath -w "$tmp/src.key")" </dev/null >/dev/null \
        || fail "Win32-OpenSSH could not generate the key the probe copies"
else
    sshkg=; printf 'not a key\n' > "$tmp/src.key"
fi

cat > "$tmp/acl.kama" <<'EOF'
import { std::fs::File, std::fs::OpenMode, std::fs::Permissions, std::fs::createDirWith, std::fs::readFile,
         std::fs::readText, std::io::IoError, std::collections::DynamicArray };

unsafe fn int32 put(const ref string path, Permissions p, const ref DynamicArray<uint8> bytes) {
    match (File.openWith(path: path, mode: OpenMode::Write, permissions: p)) {
        case Ok(value: f): { match (f.writeAll(bytes: bytes)) { case Ok(value: n): { } case Err(error: e): { return 1; } }; }
        case Err(error: e): { return 1; }
    };
    return 0;
}

unsafe fn int32 run() {
    Permissions priv = Permissions::OwnerRead | Permissions::OwnerWrite;
    Permissions pub = priv | Permissions::GroupRead | Permissions::OtherRead;
    match (readFile(path: "src.key")) {
        case Ok(value: key): {
            if (put(path: "private.key", p: priv, bytes: key) != 0) { return 1; }
            if (put(path: "public.txt", p: pub, bytes: key) != 0) { return 2; }
        }
        case Err(error: e): { return 9; }
    };
    match (createDirWith(path: "private.d", permissions: priv | Permissions::OwnerExecute)) {
        case Ok(value: u): { }
        case Err(error: e): { return 3; }
    };
    // Two plain creates, inheriting this directory's list: one kept as the witness, one given a private list.
    match (File.open(path: "plain.txt", mode: OpenMode::Write)) { case Ok(value: f): { } case Err(error: e): { return 4; } };
    match (File.open(path: "remode.key", mode: OpenMode::Write)) { case Ok(value: f): { } case Err(error: e): { return 4; } };
    match (File.openWith(path: "remode.key", mode: OpenMode::Write, permissions: priv)) {
        case Ok(value: f): { }
        case Err(error: e): { return 5; }
    };
    // ---rwxrwx: Everyone may read, and the owner may not.
    Permissions others = Permissions::GroupRead | Permissions::GroupWrite | Permissions::GroupExecute
                       | Permissions::OtherRead | Permissions::OtherWrite | Permissions::OtherExecute;
    match (File.openWith(path: "deny.txt", mode: OpenMode::Write, permissions: others)) {
        case Ok(value: f): { }
        case Err(error: e): { return 6; }
    };
    match (readText(path: "deny.txt")) {
        case Ok(value: t): { return 7; }
        case Err(error: e): { match (e) { case PermissionDenied: { } case _: { return 8; } }; }
    };
    return 0;
}

fn int32 main() { return run(); }
EOF
"$KAMA" build "$tmp/acl.kama" -o "$tmp/acl.exe" > "$tmp/build.log" 2>&1 \
    || { sed -n '1,20p' "$tmp/build.log" >&2; fail "the probe did not build"; }
rc=0; ( cd "$tmp" && ./acl.exe ) || rc=$?
case $rc in
    0) ;;
    7) fail "the owner READ a 0o077 file — Everyone's grant reached it, so the deny entry is missing or misplaced" ;;
    8) fail "the owner's read of a 0o077 file failed, but not as PermissionDenied" ;;
    *) fail "the probe exited $rc — the \`return $rc\` in the probe above names the call that was refused" ;;
esac

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
s=$(sddl "$tmp/plain.txt")
printf '%s\n' "$s" | grep -q ';ID;' || fail "a plain create here inherits nothing, so the re-mode case proves nothing: $s"
r=$(verdict "$tmp/remode.key" "")
[ -z "$r" ] || fail "an existing file given a private list is not owner + SYSTEM, protected: $r"

# Win32-OpenSSH, on the same bytes under two lists. Before the self-check below widens private.key.
sshnote="no Win32-OpenSSH, so key acceptance NOT checked"
if [ -n "$sshkg" ]; then
    "$sshkg" -y -f "$(cygpath -w "$tmp/private.key")" </dev/null >"$tmp/ssh.out" 2>"$tmp/ssh.err" \
        || fail "Win32-OpenSSH refused the private key kama wrote: $(tr -d '\r' < "$tmp/ssh.err")"
    [ "$(tr -d '\r' < "$tmp/ssh.out" | cut -d' ' -f1,2)" = "$(tr -d '\r' < "$tmp/src.key.pub" | cut -d' ' -f1,2)" ] \
        || fail "Win32-OpenSSH read back a different key: $(tr -d '\r' < "$tmp/ssh.out")"
    if "$sshkg" -y -f "$(cygpath -w "$tmp/public.txt")" </dev/null >/dev/null 2>&1; then
        fail "Win32-OpenSSH accepted the same key readable by Everyone — its acceptance says nothing about the list"
    fi
    sshnote="Win32-OpenSSH loads the private key and refuses the same bytes at 0o644"
fi

# The verdict must be able to fail: widen the private file behind kama's back and ask again.
MSYS2_ARG_CONV_EXCL='*' icacls "$(cygpath -w "$tmp/private.key")" /grant '*S-1-5-32-544:(R)' >/dev/null \
    || fail "could not add the Administrators entry the self-check needs"
r=$(verdict "$tmp/private.key" "")
[ -n "$r" ] || fail "the verdict ACCEPTED a private file with an extra Administrators entry — it cannot tell private from not"

echo "check-windows-acl: PASS (a private file and directory, and an existing file given a private list, hold exactly"
echo "                   owner + SYSTEM, protected; 0o644 adds group + Everyone; a deny entry keeps the owner out of"
echo "                   0o077; $sshnote; the verdict refuses an extra entry)"
