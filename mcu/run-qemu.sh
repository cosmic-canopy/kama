#!/bin/sh
# mcu/run-qemu.sh — run a kama Cortex-M firmware ELF under QEMU (headless, semihosting), propagating the
# program's semihosting exit code as the process exit code. Build the ELF with `mcu/build.sh --qemu`.
#
#   mcu/run-qemu.sh <firmware.elf> [--board <name>]
set -eu

BOARD=lm3s6965evb
ELF=
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD=$2; shift 2 ;;
    *)       ELF=$1; shift ;;
  esac
done
[ -n "$ELF" ] || { echo "usage: mcu/run-qemu.sh <firmware.elf> [--board <name>]" >&2; exit 2; }

case "$BOARD" in
  lm3s6965evb)  MACHINE=lm3s6965evb; CPU=cortex-m3 ;;
  microbit)     MACHINE=microbit;    CPU=cortex-m0 ;;
  riscv32-virt)
    command -v qemu-system-riscv32 >/dev/null || { echo "mcu/run-qemu.sh: qemu-system-riscv32 not found (use the kama-mcu image)" >&2; exit 1; }
    # Machine mode from 0x80000000, no firmware; the startup ends QEMU through the test device (mcu/boards/riscv32-virt).
    exec qemu-system-riscv32 -M virt -bios none -nographic -kernel "$ELF" ;;
  esp32|esp32s3)
    command -v qemu-system-xtensa >/dev/null || { echo "mcu/run-qemu.sh: qemu-system-xtensa not found (use the kama-mcu image)" >&2; exit 1; }
    # Espressif's QEMU loads the ELF in place of the chip's ROM boot; the simulator kit's `_exit` ends it through
    # semihosting with the program's code (mcu/boards/esp32). It has no ESP32-S2 machine.
    exec qemu-system-xtensa -M "$BOARD" -nographic -semihosting -kernel "$ELF" </dev/null ;;
  *) echo "mcu/run-qemu.sh: unknown board '$BOARD'" >&2; exit 2 ;;
esac
command -v qemu-system-arm >/dev/null || { echo "mcu/run-qemu.sh: qemu-system-arm not found (use the kama-mcu image)" >&2; exit 1; }

# Semihosting routes the program's SYS_EXIT_EXTENDED (and any console I/O) to the host; `-nographic` keeps it headless. QEMU
# exits with the value the firmware passed to semihost_exit (see mcu/startup.c). `userspace=on` lets a program started
# unprivileged (`mcu/build.sh --unprivileged`) exit too, as its panic handler must when `Atomic` refuses to run there.
exec qemu-system-arm -M "$MACHINE" -cpu "$CPU" -nographic -semihosting-config enable=on,target=native,userspace=on -kernel "$ELF"
