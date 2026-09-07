#!/bin/sh
# platform.sh — print the name of the out/ subdirectory this host's build artifacts belong in.
#
# RUN, not sourced, so every consumer shares ONE derivation. It used to be spelled independently in
# FOUR places — the Makefile, tools/kama-bin.sh, tools/check-no-inheritance.sh and
# tools/check-compiler-asan.sh — which was safe only for as long as all four stayed the same
# one-liner. The moment the rule gained the $MSYSTEM arm below, that stopped being true, and
# check-no-inheritance failed immediately and misleadingly: `make` built into
# out/UCRT64-x86_64-noinherit, the guard looked for the old uname-derived name, found no binary, and
# reported that the compiler had stopped rejecting `extends`. A build landing in a directory the
# harness never looks in IS the stale-binary trap `./dev` exists to make unrepresentable.
#
# ⚠️ So: if you need this name, call this script. Do not inline it. (The VS Code extension is the one
# deliberate exception — it globs `out/*/` and takes the newest, because it cannot run a shell.)
#
# ⚠️ WHY $MSYSTEM, on msys2 only. On an ARM64 Windows host, UCRT64 and CLANGARM64 select DIFFERENT
# compilers for the same source tree and are INDISTINGUISHABLE to uname. Measured on one such box:
#
#     UCRT64      uname -s=MINGW64_NT-10.0-26200-ARM64  uname -m=x86_64   /ucrt64/bin/clang     (x86-64, emulated)
#     CLANGARM64  uname -s=MINGW64_NT-10.0-26200-ARM64  uname -m=x86_64   /clangarm64/bin/clang (ARM64, native)
#
# Identical on both keys — `uname -m` says x86_64 in BOTH, because bash.exe is itself the emulated
# x86_64 binary reporting on itself, not on the compiler it is about to invoke. So the two builds
# shared one out/ directory: building one and testing the other passed silently against the wrong
# compiler, and `PLATFORM=<name>` did not help because the harness resolved $KAMA from uname and
# found the OTHER environment's binary still sitting at that path.
#
# $MSYSTEM is what actually selects the toolchain, and msys2 passes it to child processes, so it is
# the honest key. Everywhere else it is unset and this is exactly the old expression.
#
# NOT keyed off $MSYSTEM: the Makefile's `-static` rule, which asks the different question "are we on
# msys2 at all" and is correctly answered by `uname -s` matching MINGW* in every environment.
set -eu

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # The arch half stays `uname -m` so a non-msys2 cross-check still reads naturally; MSYSTEM
        # alone is the discriminator that matters, and it is already unique per toolchain.
        printf '%s-%s' "${MSYSTEM:-$(uname -s)}" "$(uname -m)"
        ;;
    *)
        printf '%s-%s' "$(uname -s)" "$(uname -m)"
        ;;
esac
