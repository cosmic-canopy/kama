#!/bin/sh
# mcu/build.sh — turnkey bare-metal build: a kama program -> a bootable Cortex-M firmware ELF, in one step.
#
# It wires the two halves the compiler keeps deliberately separate (kama emits portable freestanding C; the
# board link is toolchain work): (1) `kama build --target embedded` with a Cortex-M clang triple -> a
# freestanding object, then (2) arm-none-eabi-gcc links it against this repo's startup + the board linker
# script + newlib (semihosting) -> a flashable/emulable ELF.
#
# Opt-in: needs the `kama-mcu` toolchain image (arm-none-eabi-gcc; clang is in the base). See docs/mcu.md.
#
#   mcu/build.sh <input.kama> [-o out.elf] [--board <name>] [--qemu]
#     --board   target board (default: lm3s6965evb). Adds a board under mcu/boards/<name>/linker.ld.
#     --qemu    build the emulator harness (runs main ONCE and exits via semihosting) instead of real
#               firmware (which loops forever). Used by mcu/run-qemu.sh + tools/check-mcu.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KAMA="${KAMA:-$ROOT/../kama}"   # honor a caller-supplied binary (tools/check-mcu.sh exports the platform one)
BOARD=lm3s6965evb
OUT=
QEMU=
IN=

while [ $# -gt 0 ]; do
  case "$1" in
    -o)       OUT=$2; shift 2 ;;
    --board)  BOARD=$2; shift 2 ;;
    --qemu)   QEMU=1; shift ;;
    -*)       echo "mcu/build.sh: unknown option $1" >&2; exit 2 ;;
    *)        IN=$1; shift ;;
  esac
done
[ -n "$IN" ] || { echo "usage: mcu/build.sh <input.kama> [-o out.elf] [--board <name>] [--qemu]" >&2; exit 2; }

# Board table: CPU (arm-none-eabi -mcpu / clang -mcpu) + clang bare-metal triple.
case "$BOARD" in
  lm3s6965evb) CPU=cortex-m3; TRIPLE=thumbv7m-none-eabi ;;
  microbit)    CPU=cortex-m0; TRIPLE=thumbv6m-none-eabi ;;   # nRF51822, no FPU -> soft-float libcalls
  *) echo "mcu/build.sh: unknown board '$BOARD' (add mcu/boards/$BOARD/linker.ld + a case here)" >&2; exit 2 ;;
esac

LD="$ROOT/boards/$BOARD/linker.ld"
[ -f "$LD" ] || { echo "mcu/build.sh: missing linker script $LD" >&2; exit 1; }
[ -x "$KAMA" ] || { echo "mcu/build.sh: kama not built ($KAMA)" >&2; exit 1; }
command -v arm-none-eabi-gcc >/dev/null || { echo "mcu/build.sh: arm-none-eabi-gcc not found (use the kama-mcu image)" >&2; exit 1; }

[ -n "$OUT" ] || OUT=$(printf '%s' "$IN" | sed 's/\.kama$//').elf
obj="${OUT%.elf}.o"

# 1. kama -> freestanding Cortex-M object (clang accepts kama's warning flags; arm-none-eabi-gcc does not).
"$KAMA" build "$IN" --target embedded --cc "clang --target=$TRIPLE -mcpu=$CPU" -o "$obj"

# 2. link -> bootable ELF (startup + vector table + linker script + newlib/rdimon semihosting).
semi=; [ -n "$QEMU" ] && semi="-DKAMA_MCU_SEMIHOST_EXIT"
arm-none-eabi-gcc -mcpu="$CPU" -mthumb -ffreestanding -nostartfiles -T "$LD" --specs=rdimon.specs $semi \
  "$ROOT/startup.c" "$obj" -o "$OUT"

echo "mcu/build.sh: wrote $OUT ($(arm-none-eabi-size "$OUT" | awk 'NR==2{print $1+$2" bytes flash, "$2+$3" bytes RAM"}'))"
