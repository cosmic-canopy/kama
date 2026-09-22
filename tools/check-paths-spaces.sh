#!/bin/sh
# check-paths-spaces.sh — a path with a SPACE in it builds, and the binary runs.
#
# Why this guard exists. Three of the C compiler's `-I` entries were written UNQUOTED while every
# other `-I`, `--sysroot`, input and `-o` on the same line was quoted (`kama.driver.cpp`, fixed
# 0.9.302). A space anywhere in them tore the command line and the tail became a bare input operand:
#
#     $ cd "…/my project" && kama build app.kama -o "$PWD/app.exe"
#     clang: error: no such file or directory: 'project'
#
# It is NOT Windows-only — `sh -c` word-splits exactly the same way — but Windows is where a spaced
# path is the default rather than an oddity.
#
# ⚠️ The shape that made this urgent was not a spaced PROJECT. It was `runtimeDir`, which is the
# INSTALL PREFIX: the documented install is `~/.kama` (docs/GETTING_STARTED.md, both installers), so a
# Windows account named `John Smith` got
#
#     clang: error: no such file or directory: 'Smith/.kama/include'
#
# and could not build hello-world AT ALL — on the happy path, with no unusual choice made. `runtimeDir`
# resolves relative to the binary, so there was no user-side workaround. Shape D below is that case,
# and it is the reason this guard stages a whole install tree rather than only spacing a project dir.
#
# The shapes, and what each was MEASURED to do against the unfixed compiler (this guard was run against
# a reverted `kama.driver.cpp`, which is the only proof a guard guards anything):
#
#   A. spaced project dir, single module      RED: no such file or directory: 'project'
#   B. spaced project dir, MULTI-module       RED: '…/src/helper' AND '…/out' — the second is
#                                                  headerDir, which A never populates
#   C. -o into a DIFFERENT spaced dir         RED: 'dir', twice — headerDir, independently of the
#                                                  source directory
#   D. spaced INSTALL PREFIX, project clean   RED: 'Files/kama/include' — runtimeDir
#   E. a space in the source FILE name        ⚠️ GREEN even unfixed, and kept deliberately:
#                                                  the -I is built from dirName(), which strips the
#                                                  file name, so this shape never tore. It is NOT a
#                                                  regression witness for that bug — it holds down the
#                                                  separate, cheap invariant that a spaced FILE name
#                                                  builds at all (the operand quoting, not the -I).
#                                                  Do not cite it as evidence the -I fix works.
#
# ⚠️ Every build here is the DEFAULT (debug) one, deliberately. `--release` folds the program into one
# translation unit and never sets `headerDir`, so a release-only version of B/C would silently assert
# nothing about that entry.
#
# ⚠️ It asserts the binary RUNS, never that `kama: built <path>` was printed. The driver reports
# success on the C compiler's exit status without stat-ing its own output, so the build message is not
# evidence a file exists (docs/platforms/windows.md § Where the remaining work is). Exit codes are the
# instrument: each program returns a distinctive value.
#
# Hermetic: everything happens under one private mktemp dir, so it writes nothing into the worktree and
# cannot be perturbed by a developer's scratch files. Reaches the compiler through $KAMA (run-checks.sh
# exports an absolute one) rather than the ./kama symlink, which check-no-inheritance.sh repoints while
# it builds. No `cd` outside a subshell — the guards run in parallel.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu
exec </dev/null

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-paths-spaces: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

note() { echo "check-paths-spaces: FAIL — $1" >&2; fail=1; }

# Build in $dir (subshell, so the cwd never leaks), then RUN the output and compare its exit code.
# $1 label  $2 cwd for the build  $3 compiler operand  $4 -o path  $5 expected exit code
try() {
    _label=$1; _cwd=$2; _in=$3; _out=$4; _want=$5
    _exe=${6:-$KAMA}
    rm -f "$_out"
    if ! ( cd "$_cwd" && "$_exe" build "$_in" -o "$_out" ) >"$tmp/build.log" 2>&1; then
        note "$_label: the build failed"
        sed 's/^/      /' "$tmp/build.log" >&2
        return
    fi
    # The build message is not evidence — stat it.
    if [ ! -f "$_out" ]; then
        note "$_label: kama reported success but produced no file at $_out"
        sed 's/^/      /' "$tmp/build.log" >&2
        return
    fi
    _rc=0; "$_out" >"$tmp/run.log" 2>&1 || _rc=$?
    if [ "$_rc" != "$_want" ]; then
        note "$_label: the binary ran with exit $_rc, wanted $_want"
        sed 's/^/      /' "$tmp/run.log" >&2
    fi
}

# ---- A. a spaced PROJECT directory, single module -------------------------------------------------
a="$tmp/my project"
mkdir -p "$a"
cat >"$a/app.kama" <<'EOF'
import { core::print };
fn int32 main() { print(s: "spaced project\n"); return 31; }
EOF
try "A (spaced project dir, single module)" "$a" "app.kama" "$a/app.exe" 31

# ---- B. a spaced PROJECT directory, MULTI-module (populates headerDir) ----------------------------
# Two modules under a manifest, so the build takes the per-unit branch that emits a shared .gen.h.
b="$tmp/my project/multi"
mkdir -p "$b/src/helper"
cat >"$b/kama.json" <<'EOF'
{ "name": "spc", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama",
  "modules": { "helper": { "visibility": "internal" } } }
EOF
# No `module` statement: a module's name comes from its directory (src/<module>/<file>.kama), the same
# shape tests/friend_cross_module.d uses.
cat >"$b/src/helper/helper.kama" <<'EOF'
export { bump };
fn int32 bump(int32 v) { return v + 1; }
EOF
cat >"$b/src/main.kama" <<'EOF'
import { core::print, spc::helper::bump };
fn int32 main() { print(s: "spaced multi\n"); return bump(v: 31); }
EOF
try "B (spaced project dir, multi-module)" "$b" "kama.json" "$b/out/app.exe" 32

# ---- C. -o into a DIFFERENT spaced directory, source directory CLEAN ------------------------------
c_src="$tmp/clean-src"
c_out="$tmp/out dir"
mkdir -p "$c_src/src/helper" "$c_out"
cp "$b/kama.json" "$c_src/kama.json"
cp "$b/src/helper/helper.kama" "$c_src/src/helper/helper.kama"
cp "$b/src/main.kama" "$c_src/src/main.kama"
try "C (-o into a spaced dir, clean source dir)" "$c_src" "kama.json" "$c_out/app.exe" 32

# ---- D. a spaced INSTALL PREFIX, project path CLEAN ----------------------------------------------
# The shape with no user-side workaround. A staged install needs the binary plus the runtime headers
# beside it: resolveRuntimeDir probes <exeDir>/include, which is also how the `out/lsp` snapshot works
# (docs/platforms/windows.md § The language server).
d_inst="$tmp/Program Files/kama"
d_proj="$tmp/clean-proj"
mkdir -p "$d_inst" "$d_proj"
exe_suffix=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe_suffix=".exe" ;; esac
cp "$KAMA" "$d_inst/kama$exe_suffix"
cp -r "$ROOT/include" "$d_inst/"
cat >"$d_proj/app.kama" <<'EOF'
import { core::print };
fn int32 main() { print(s: "spaced install\n"); return 33; }
EOF
if [ ! -x "$d_inst/kama$exe_suffix" ]; then
    note "D: could not stage an install tree at $d_inst"
else
    try "D (spaced INSTALL PREFIX, clean project)" "$d_proj" "app.kama" "$d_proj/app.exe" 33 \
        "$d_inst/kama$exe_suffix"
fi

# ---- E. a space in the source FILE name ----------------------------------------------------------
e="$tmp/filename"
mkdir -p "$e"
cat >"$e/my app.kama" <<'EOF'
import { core::print };
fn int32 main() { print(s: "spaced filename\n"); return 34; }
EOF
try "E (spaced source FILE name)" "$e" "my app.kama" "$e/app.exe" 34

# ---- verdict --------------------------------------------------------------------------------------
[ "$fail" = 0 ] || {
    echo "" >&2
    echo "  A path with a space must survive into the C compiler's command line. Every path kama puts" >&2
    echo "  on that line needs quoting — including the -I entries built from runtimeDir (the INSTALL" >&2
    echo "  PREFIX), the input's own directory, and headerDir. See kama.driver.cpp's comment at the -I" >&2
    echo "  block, and this file's own header for which entry each shape covers." >&2
    exit 1
}

echo "check-paths-spaces: PASS (spaced project dir single + multi-module, spaced -o dir, spaced INSTALL prefix, spaced filename — all build and run)"
