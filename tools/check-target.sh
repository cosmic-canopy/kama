#!/bin/sh
# check-target.sh — build-configuration guard for the TARGET axis: the toolchain flags a build hands to
# the C compiler must come from the SELECTED TARGET, never from the machine the kama binary was built on.
#
# Until the build-configuration campaign, these were `#ifdef __APPLE__` / `#ifdef _WIN32` evaluated when
# the COMPILER itself was compiled. That is correct only while host == target, and it was the one hard
# blocker to cross-compilation — most sharply with `-lws2_32`, which a Windows build produced on Linux
# silently omitted, so every std::net program would fail to link.
#
# We prove the keying without needing a cross toolchain installed: `--cc "echo …"` substitutes the C
# compiler for `echo`, so the fully-assembled command line is printed instead of run.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/arith.kama"

if [ ! -x "$KAMA" ]; then echo "check-target: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-target: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The assembled cc command line for a given target, with the compiler stubbed out.
ccline() {
    "$KAMA" build --release --cc "echo" "$FIXTURE" --target "$1" -o "$tmp/out" 2>/dev/null
}

want() {   # want <target> <substring> <description>
    if ! ccline "$1" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 did not pass '$2' ($3)" >&2
        echo "  command line was:" >&2
        ccline "$1" | sed 's/^/    /' >&2
        exit 1
    fi
}

reject() {   # reject <target> <substring> <description>
    if ccline "$1" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 passed '$2' but must not ($3)" >&2
        exit 1
    fi
}

# 1. WINSOCK — the flagship cross-compilation case. `-lws2_32` must follow the TARGET being Windows,
#    from any host, and must not appear for any other target.
want   WINDOWS -lws2_32 "std::net needs Winsock on Windows"
reject LINUX   -lws2_32 "POSIX sockets need no extra library"
reject MACOS   -lws2_32 "POSIX sockets need no extra library"

# 2. SECTION GC — ld64 and GNU ld/lld spell dead-code stripping differently, and ld64 treats `-s` as
#    obsolete (it warned on every release link before this was target-keyed).
want   MACOS   -Wl,-dead_strip   "ld64 spells section GC -dead_strip"
reject MACOS   -Wl,--gc-sections "that is the GNU-style spelling"
reject MACOS   " -s "            "ld64 warns that -s is obsolete"
want   LINUX   -Wl,--gc-sections "GNU ld/lld spell section GC --gc-sections"
want   WINDOWS -Wl,--gc-sections "a mingw target links with a GNU-style linker"

# 3. SHARED-LIBRARY EXTENSION — the default output name follows the target's platform convention, so a
#    cross build does not produce a `.dylib` for Windows.
for spec in "WINDOWS .dll" "MACOS .dylib" "LINUX .so"; do
    set -- $spec
    out=$("$KAMA" build --shared --cc "echo" "$FIXTURE" --target "$1" 2>/dev/null | tr ' ' '\n' | grep -E "arith\\$2$" || true)
    if [ -z "$out" ]; then
        echo "check-target: FAIL — target $1 did not default its shared-library output to *$2" >&2
        exit 1
    fi
done

# 4. BARE METAL is a TRIPLE PROPERTY, not a magic target name. Any `os=none` triple must get the
#    freestanding treatment — that is what lets a real board triple work without the compiler knowing it.
for t in EMBEDDED riscv32-none-elf thumbv7em-none-eabihf; do
    line=$("$KAMA" build --cc "echo" "$FIXTURE" --target "$t" -o "$tmp/o.o" 2>/dev/null || true)
    for flag in -ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED; do
        if ! printf '%s' "$line" | grep -qF -- "$flag"; then
            echo "check-target: FAIL — os=none target $t did not get $flag" >&2
            exit 1
        fi
    done
done

# 5. A HOSTED triple must NOT get the freestanding treatment — the complement of case 4, so a mistake
#    that turns freestanding on unconditionally cannot pass.
if ccline aarch64-linux-gnu | grep -qF -- "-nostdlib"; then
    echo "check-target: FAIL — a hosted triple was compiled freestanding" >&2
    exit 1
fi

echo "check-target: PASS (link/compile flags follow the selected target, not the host: winsock, section GC,
  shared-library extension, and freestanding keyed on os=none rather than a target name)"
