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
    src="$ROOT/tests/support/$name/main.kama"   # a project whose kama.json names its board, built here as a file with explicit flags
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

# 4. KR-349 — `Atomic<T>` on a chip with no atomic of T's width, RUN: every width and operation on a Cortex-M3 (8 bytes masked),
#    a Cortex-M0 (every read-modify-write masked) and a RISC-V core built rv32imc (the same) and rv32imac (native), each
#    exiting 49; the M3 started unprivileged, where `cpsid i` is ignored, must reach the panic handler instead (58); and
#    SysTick adding to an `Atomic<uint64>` that `main` adds to, which keeps firing only if each operation restores its mask (50).
mcurun() {   # <name> <expected exit> <program under tests/support> <board> [mcu/build.sh flags]
    name=$1; want=$2; prog=$3; board=$4; shift 4
    if ! "$ROOT/mcu/build.sh" "$ROOT/tests/support/$prog/main.kama" --board "$board" "$@" -o "$tmp/$name.elf" --qemu \
            >"$tmp/$name.build" 2>&1; then
        echo "check-mcu: FAIL — $name did not build into a firmware" >&2; sed 's/^/  /' "$tmp/$name.build" >&2; exit 1
    fi
    set +e; timeout 120 "$ROOT/mcu/run-qemu.sh" "$tmp/$name.elf" --board "$board" >"$tmp/$name.run" 2>&1; got=$?; set -e
    if [ "$got" -ne "$want" ]; then
        echo "check-mcu: FAIL — $name exited $got on QEMU, expected $want" >&2; sed 's/^/  /' "$tmp/$name.run" >&2; exit 1
    fi
}
mcurun atomic-m3 49 embedded_atomic lm3s6965evb
mcurun atomic-m0 49 embedded_atomic microbit
mcurun atomic-unprivileged 58 embedded_atomic lm3s6965evb --unprivileged
mcurun systick 50 embedded_systick lm3s6965evb
riscv="RISC-V rv32imc and rv32imac"
if command -v qemu-system-riscv32 >/dev/null 2>&1 && command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    mcurun atomic-rv32imc 49 embedded_atomic riscv32-virt --march rv32imc
    mcurun atomic-rv32imac 49 embedded_atomic riscv32-virt --march rv32imac
else
    riscv="RISC-V SKIPPED: no qemu-system-riscv32 / riscv64-unknown-elf-gcc, rebuild the kama-mcu image"
    echo "check-mcu: $riscv"
fi

# 5. KR-354 — the same on an Xtensa core, on Espressif's QEMU: an ESP32 (1 to 4 bytes native through `s32c1i`, 8 masked), the
#    same ESP32 built `-mdisable-hardware-atomics` (every read-modify-write masked, as on an S2, or on an ESP32 whose heap is in
#    external RAM), and an S3, each exiting 49. QEMU has no S2, so it is only built. Every archive must name no `__atomic_*` or
#    `__sync_*`, and the mask must be there, read off an S2's 4-byte fetchAdd: a run on one core would pass without it.
xtensa="Xtensa ESP32, ESP32-S3 and (built) ESP32-S2"
if command -v qemu-system-xtensa >/dev/null 2>&1 && command -v xtensa-esp32-elf-gcc >/dev/null 2>&1; then
    mcurun atomic-esp32 49 embedded_atomic esp32
    mcurun atomic-esp32-nohw 49 embedded_atomic esp32 --cflags -mdisable-hardware-atomics
    mcurun atomic-esp32s3 49 embedded_atomic esp32s3
    if ! "$ROOT/mcu/build.sh" "$ROOT/tests/support/embedded_atomic/main.kama" --board esp32s2 -o "$tmp/atomic-esp32s2.elf" --qemu \
            >"$tmp/atomic-esp32s2.build" 2>&1; then
        echo "check-mcu: FAIL — atomic-esp32s2 did not build into a firmware" >&2; sed 's/^/  /' "$tmp/atomic-esp32s2.build" >&2; exit 1
    fi
    for a in esp32:esp32 esp32-nohw:esp32 esp32s3:esp32s3 esp32s2:esp32s2; do
        if "xtensa-${a#*:}-elf-nm" "$tmp/atomic-${a%:*}.a" | grep -qE ' U _?_(_atomic|_sync)_'; then
            echo "check-mcu: FAIL — Atomic on atomic-${a%:*} calls into a libatomic no board has:" >&2
            "xtensa-${a#*:}-elf-nm" "$tmp/atomic-${a%:*}.a" | grep -E ' U _?_(_atomic|_sync)_' | sort -u | sed 's/^/  /' >&2; exit 1
        fi
    done
    printf '#include "kama_atomic.h"\nuint32_t f(uint32_t* c) { return kama__atomic_add4(c, 1, 5); }\n' \
        | xtensa-esp32s2-elf-gcc -O2 -ffreestanding -DKAMA_TARGET_EMBEDDED -I "$ROOT/include" -S -x c - -o "$tmp/xtensa-mask.s"
    for want in 'rsil[[:space:]]+a[0-9]+, *15' 'wsr\.ps' 'rsync'; do
        grep -qE "$want" "$tmp/xtensa-mask.s" || { echo "check-mcu: FAIL — an ESP32-S2 fetchAdd has no '$want': its mask is gone" >&2; exit 1; }
    done
else
    xtensa="Xtensa SKIPPED: no qemu-system-xtensa / xtensa-esp32-elf-gcc, rebuild the kama-mcu image"
    echo "check-mcu: $xtensa"
fi

echo "PASS check-mcu (kama -> Cortex-M firmware boots on QEMU lm3s6965evb + exits $EXPECT; std with a board's clock and panic handler declared in kama, with newlib and with no C library; Atomic at every width on Cortex-M3, Cortex-M0, $riscv and $xtensa, refused unprivileged)"
