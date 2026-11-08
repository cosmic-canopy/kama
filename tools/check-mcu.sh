#!/bin/sh
# check-mcu.sh — end-to-end MCU proof: build a kama program into a real Cortex-M firmware and RUN it on an
# emulated core, asserting the exit code. Where check-embedded.sh proves the freestanding *object* is
# correct host-side, this proves the whole chain: kama -> C -> Cortex-M object -> link (startup + vector
# table + linker script + newlib) -> boots on QEMU -> runs the actual MMIO loop -> exits with the right value.
#
# OPT-IN: needs the cross toolchain (arm-none-eabi-gcc + qemu-system-arm) from the `kama-mcu` image. When
# they're absent (the base `kama-dev` image / a plain host), it SKIPS with exit 0 — so run_tests.sh stays
# green everywhere and this only really exercises under `KAMA_IMAGE=kama-mcu`.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# Resolve the platform binary, not the root ./kama symlink: tools/check-no-inheritance.sh repoints that
# symlink at a DIFFERENT compiler while it builds, and this guard would then silently test the wrong one.
# mcu/build.sh honors an exported $KAMA for the same reason.
. "$ROOT/tools/kama-bin.sh"
export KAMA
FIXTURE="$ROOT/tests/embedded_blink.kama"   # returns 22 on every leg (native/wasm/embedded) — the shared oracle
EXPECT=22

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1 || ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "SKIP check-mcu (no arm-none-eabi-gcc / qemu-system-arm — build the kama-mcu image: tools/cdev build-image-mcu)"
    exit 0
fi
if [ ! -x "$KAMA" ]; then echo "check-mcu: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
elf="$tmp/blink.elf"

# 1. turnkey build -> a QEMU-runnable firmware (semihosting-exit harness).
if ! "$ROOT/mcu/build.sh" "$FIXTURE" -o "$elf" --qemu >"$tmp/build.log" 2>&1; then
    echo "check-mcu: FAIL — mcu/build.sh did not produce a firmware ELF" >&2
    sed 's/^/  /' "$tmp/build.log" >&2; exit 1
fi

# 2. run on the emulated Cortex-M and assert the exit code.
set +e
"$ROOT/mcu/run-qemu.sh" "$elf" >"$tmp/run.log" 2>&1
got=$?
set -e
if [ "$got" -ne "$EXPECT" ]; then
    echo "check-mcu: FAIL — QEMU exit $got, expected $EXPECT (embedded_blink)" >&2
    sed 's/^/  /' "$tmp/run.log" >&2; exit 1
fi

# 3. KR-341 — std on the board, with its hooks declared in kama, booted and RUN: a `@monotonicClock` that `sleep` and
#    `Atomic.waitUntil` spin on, a collection and `sqrt` (embedded_board, exits 42), and a `@panicHandler` that ends the
#    emulator from a bounds miss (embedded_panic, exits 57). Each twice: through mcu/build.sh, which links newlib, and
#    linked with NO C library at all, only libgcc's helpers — a board with none still links (KR-341's R1).
for case in embedded_board:42 embedded_panic:57; do
    name=${case%:*}; want=${case#*:}
    src="$ROOT/tests/support/$name.kama"
    if ! "$ROOT/mcu/build.sh" "$src" -o "$tmp/$name.elf" --qemu >"$tmp/$name.build" 2>&1; then
        echo "check-mcu: FAIL — $name did not build into a firmware" >&2; sed 's/^/  /' "$tmp/$name.build" >&2; exit 1
    fi
    if ! "$KAMA" build "$src" --target thumbv7m-none-eabi --select OUTPUT=STATIC \
            --cc "clang --target=thumbv7m-none-eabi -mcpu=cortex-m3" -o "$tmp/$name-nolibc.a" >"$tmp/$name.kn" 2>&1 \
       || ! arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -ffreestanding -nostartfiles -nostdlib -DKAMA_MCU_SEMIHOST_EXIT \
            -T "$ROOT/mcu/boards/lm3s6965evb/linker.ld" -Wl,--gc-sections "$ROOT/mcu/startup.c" "$tmp/$name-nolibc.a" -lgcc \
            -o "$tmp/$name-nolibc.elf" >"$tmp/$name.link" 2>&1; then
        echo "check-mcu: FAIL — $name did not link with no C library" >&2; sed 's/^/  /' "$tmp/$name.kn" "$tmp/$name.link" >&2; exit 1
    fi
    for elf in "$tmp/$name.elf" "$tmp/$name-nolibc.elf"; do
        set +e; "$ROOT/mcu/run-qemu.sh" "$elf" >"$tmp/$name.run" 2>&1; got=$?; set -e
        if [ "$got" -ne "$want" ]; then
            echo "check-mcu: FAIL — $(basename "$elf") exited $got on QEMU, expected $want" >&2; sed 's/^/  /' "$tmp/$name.run" >&2; exit 1
        fi
    done
done

echo "PASS check-mcu (kama -> Cortex-M firmware boots on QEMU lm3s6965evb + exits $EXPECT; std with a board's clock and panic handler declared in kama, with newlib and with no C library)"
