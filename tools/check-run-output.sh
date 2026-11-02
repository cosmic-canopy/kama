#!/bin/sh
# check-run-output.sh — `kama run` refuses anything that is not an executable, before building it.
#
# `run` forces its output to a throwaway `kama-run-<pid>[.exe]` and execs whatever the build left there, and it
# did so whatever the OUTPUT was: a library project, or `--select OUTPUT=STATIC`/`SHARED` on a loose file, built
# an archive or a DLL under the executable's name and ran it. Windows answered "This version of …\kama-run-<pid>.exe
# is not compatible with the version of Windows you're running" for the archive and "Access is denied" for the DLL
# (0.9.803). And `kama run` on a workspace named a library member as the one to run, which only led to the first
# refusal. `cargo run` refuses with "a bin target must be available", `go run` with "is not a main package".
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-run-output: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# `run`'s throwaway binary lands in the temp directory: a private one, so a leftover is this guard's to see.
mkdir -p "$tmp/t"
TMPDIR=$(kama_native_path "$tmp/t"); export TMPDIR

mkdir -p "$tmp/ws/geo/src" "$tmp/ws/app/src"
printf '{ "projects": { "geo": { "optional": false }, "app": { "optional": false } } }\n' > "$tmp/ws/kama_workspace.json"
printf '{ "name": "geo", "version": "0.1.0", "kind": "library", "source": "src", "modules": { ".": { "visibility": "public" } } }\n' > "$tmp/ws/geo/kama.json"
printf 'fn int32 answer() { return 42; }\n' > "$tmp/ws/geo/src/geo.kama"
printf '{ "name": "app", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "modules": { ".": { "visibility": "internal" } } }\n' > "$tmp/ws/app/kama.json"
printf 'fn uint8 main() { return 0; }\n' > "$tmp/ws/app/src/app.kama"
printf 'fn uint8 main() { return 0; }\n' > "$tmp/loose.kama"

# refuses <label> <expected stderr fragment> <kama run args...>: exit 2, the fragment, nothing left behind.
refuses() {
    label=$1; want=$2; shift 2
    set +e
    out=$("$KAMA" run "$@" 2>&1); rc=$?
    set -e
    if [ "$rc" -ne 2 ] || ! printf '%s' "$out" | grep -qF -- "$want"; then
        echo "check-run-output: FAIL — kama run $label: exit $rc, wanted 2 and \"$want\":" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        exit 1
    fi
    if ls "$tmp/t" | grep -q '^kama-run-'; then
        echo "check-run-output: FAIL — kama run $label left $(ls "$tmp/t") in the temp directory" >&2; exit 1
    fi
}

refuses "on a library project"  "builds a static library (OUTPUT=STATIC), which cannot be run" "$tmp/ws/geo/kama.json"
refuses "OUTPUT=STATIC"         "builds a static library (OUTPUT=STATIC), which cannot be run" "$tmp/loose.kama" --select OUTPUT=STATIC
refuses "OUTPUT=SHARED"         "builds a shared library (OUTPUT=SHARED), which cannot be run" "$tmp/loose.kama" --select OUTPUT=SHARED
refuses "on a workspace"        "app/kama.json" "$tmp/ws/kama_workspace.json"
set +e
wsout=$("$KAMA" run "$tmp/ws/kama_workspace.json" 2>&1)
set -e
if printf '%s' "$wsout" | grep -qF "geo/kama.json"; then
    echo "check-run-output: FAIL — kama run on a workspace names its library member as the one to run:" >&2
    printf '%s\n' "$wsout" | sed 's/^/    /' >&2
    exit 1
fi
rm -rf "$tmp/ws/app"
printf '{ "projects": { "geo": { "optional": false } } }\n' > "$tmp/ws/kama_workspace.json"
refuses "on a workspace of libraries" "none of them executable" "$tmp/ws/kama_workspace.json"

# …and the executable still runs.
if ! "$KAMA" run "$tmp/loose.kama" >/dev/null 2>&1; then
    echo "check-run-output: FAIL — kama run of an executable no longer runs" >&2; exit 1
fi
echo "check-run-output: OK (a library project, OUTPUT=STATIC/SHARED and a workspace of libraries are refused before a build; a workspace names only executables)"
