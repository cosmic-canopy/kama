#!/bin/sh
# check-softfloat.sh — end-to-end soft-float proof: build a kama program doing float math into a real
# no-FPU Cortex-M0 firmware and RUN it on emulated silicon, asserting the exit code. On a core with no FPU
# the emitted C's `float`/`double` ops are lowered to compiler-rt/libgcc soft-float libcalls by the cross
# compiler — this proves that whole chain boots and computes correctly (kama -> C -> M0 object -> link ->
# QEMU `microbit` -> soft-float math -> semihosting exit). Sibling of check-mcu.sh (which proves the M3 blink).
#
# OPT-IN: needs the cross toolchain (arm-none-eabi-gcc + qemu-system-arm) from the `kama-mcu` image. When
# they're absent (the base `kama-dev` image / a plain host), it SKIPS with exit 0 — so run_tests.sh stays
# green everywhere and this only really exercises under `KAMA_IMAGE=kama-mcu`.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FIXTURE="$ROOT/tests/mcu_softfloat.kama"   # returns 33 on every leg (native/wasm/embedded) — the shared oracle
EXPECT=33
BOARD=microbit                             # QEMU nRF51822, Cortex-M0, no FPU

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1 || ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "SKIP check-softfloat (no arm-none-eabi-gcc / qemu-system-arm — build the kama-mcu image: tools/cdev build-image-mcu)"
    exit 0
fi
if [ ! -x "$ROOT/kama" ]; then echo "check-softfloat: $ROOT/kama not built" >&2; exit 1; fi

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
elf="$tmp/softfloat.elf"

# 1. turnkey build -> a QEMU-runnable no-FPU firmware (semihosting-exit harness).
if ! "$ROOT/mcu/build.sh" "$FIXTURE" -o "$elf" --board "$BOARD" --qemu >"$tmp/build.log" 2>&1; then
    echo "check-softfloat: FAIL — mcu/build.sh did not produce a firmware ELF" >&2
    sed 's/^/  /' "$tmp/build.log" >&2; exit 1
fi

# 2. run on the emulated Cortex-M0 and assert the exit code.
set +e
"$ROOT/mcu/run-qemu.sh" "$elf" --board "$BOARD" >"$tmp/run.log" 2>&1
got=$?
set -e
if [ "$got" -ne "$EXPECT" ]; then
    echo "check-softfloat: FAIL — QEMU exit $got, expected $EXPECT (mcu_softfloat)" >&2
    sed 's/^/  /' "$tmp/run.log" >&2; exit 1
fi

echo "PASS check-softfloat (kama -> Cortex-M0 soft-float firmware boots on QEMU $BOARD + exits $got)"
