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
  lm3s6965evb) MACHINE=lm3s6965evb; CPU=cortex-m3 ;;
  *) echo "mcu/run-qemu.sh: unknown board '$BOARD'" >&2; exit 2 ;;
esac
command -v qemu-system-arm >/dev/null || { echo "mcu/run-qemu.sh: qemu-system-arm not found (use the kama-mcu image)" >&2; exit 1; }

# `-semihosting` routes the program's SYS_EXIT_EXTENDED (and any console I/O) to the host; `-nographic`
# keeps it headless. QEMU exits with the value the firmware passed to semihost_exit (see mcu/startup.c).
exec qemu-system-arm -M "$MACHINE" -cpu "$CPU" -nographic -semihosting -kernel "$ELF"
