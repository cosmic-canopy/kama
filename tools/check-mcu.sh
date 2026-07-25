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
FIXTURE="$ROOT/tests/embedded_blink.kama"   # returns 22 on every leg (native/wasm/embedded) — the shared oracle
EXPECT=22

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1 || ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "SKIP check-mcu (no arm-none-eabi-gcc / qemu-system-arm — build the kama-mcu image: tools/cdev build-image-mcu)"
    exit 0
fi
if [ ! -x "$ROOT/kama" ]; then echo "check-mcu: $ROOT/kama not built" >&2; exit 1; fi

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

echo "PASS check-mcu (kama -> Cortex-M firmware boots on QEMU lm3s6965evb + exits $got)"
