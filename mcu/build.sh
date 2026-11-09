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
#   mcu/build.sh <input.kama> [-o out.elf] [--board <name>] [--march <isa>] [--cflags <flags>] [--qemu] [--unprivileged]
#     --board   target board (default: lm3s6965evb). Adds a board under mcu/boards/<name>/linker.ld. `esp32`, `esp32s2` and
#               `esp32s3` are Xtensa cores built and linked by Espressif's GCC, with its simulator kit (mcu/boards/esp32).
#     --march   riscv32-virt only: the ISA the core is built for (default rv32imac; rv32imc has no atomics, KR-349).
#     --cflags  more flags for the C compiler that builds the program (`-mdisable-hardware-atomics` on an ESP32).
#     --qemu    build the emulator harness (runs main ONCE and exits via semihosting) instead of real
#               firmware (which loops forever). Used by mcu/run-qemu.sh + tools/check-mcu.sh.
#     --unprivileged  Cortex-M harness only: start `main` in unprivileged Thread mode, as an RTOS task runs.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KAMA="${KAMA:-$ROOT/../kama}"   # honor a caller-supplied binary (tools/check-mcu.sh exports the platform one)
BOARD=lm3s6965evb
OUT=
QEMU=
UNPRIV=
MARCH=rv32imac
XFLAGS=
IN=

while [ $# -gt 0 ]; do
  case "$1" in
    -o)       OUT=$2; shift 2 ;;
    --board)  BOARD=$2; shift 2 ;;
    --march)  MARCH=$2; shift 2 ;;
    --cflags) XFLAGS=$2; shift 2 ;;
    --qemu)   QEMU=1; shift ;;
    --unprivileged) UNPRIV=1; shift ;;
    -*)       echo "mcu/build.sh: unknown option $1" >&2; exit 2 ;;
    *)        IN=$1; shift ;;
  esac
done
[ -n "$IN" ] || { echo "usage: mcu/build.sh <input.kama> [-o out.elf] [--board <name>] [--march <isa>] [--cflags <flags>] [--qemu] [--unprivileged]" >&2; exit 2; }

# Board table: the bare-metal triple, the C compiler (clang with the core's flags, or Espressif's GCC for an Xtensa core,
# which no clang generates code for), and the linker (arm-none-eabi-gcc, riscv64-unknown-elf-gcc with the rv32 multilib of
# libgcc that matches the ISA, or the Xtensa GCC itself).
CC=
LD="$ROOT/boards/$BOARD/linker.ld"
case "$BOARD" in
  lm3s6965evb)  TRIPLE=thumbv7m-none-eabi; CPUFLAGS=-mcpu=cortex-m3; LINKER=arm-none-eabi-gcc ;;
  microbit)     TRIPLE=thumbv6m-none-eabi; CPUFLAGS=-mcpu=cortex-m0; LINKER=arm-none-eabi-gcc ;;   # nRF51822, no FPU -> soft-float libcalls
  riscv32-virt) TRIPLE=riscv32-unknown-none-elf; CPUFLAGS="-march=$MARCH -mabi=ilp32"; LINKER=riscv64-unknown-elf-gcc ;;
  esp32|esp32s2|esp32s3) TRIPLE=xtensa-$BOARD-none-elf; CC=xtensa-$BOARD-elf-gcc; LINKER=$CC; LD= ;;   # its kit's map
  *) echo "mcu/build.sh: unknown board '$BOARD' (add mcu/boards/$BOARD/linker.ld + a case here)" >&2; exit 2 ;;
esac

[ -n "$CC" ] || CC="clang --target=$TRIPLE $CPUFLAGS"
[ -z "$LD" ] || [ -f "$LD" ] || { echo "mcu/build.sh: missing linker script $LD" >&2; exit 1; }
[ -x "$KAMA" ] || { echo "mcu/build.sh: kama not built ($KAMA)" >&2; exit 1; }
command -v "$LINKER" >/dev/null || { echo "mcu/build.sh: $LINKER not found (use the kama-mcu image)" >&2; exit 1; }

[ -n "$OUT" ] || OUT=$(printf '%s' "$IN" | sed 's/\.kama$//').elf
lib="${OUT%.elf}.a"

# 1. kama -> a freestanding archive (clang accepts kama's warning flags; arm-none-eabi-gcc does not). The
#    board's own triple, not `--target embedded`, which is THIS host's arch: kama lays a type out for the target it is
#    told and asserts each layout in the C, so a host arch handed a Cortex-M compiler stops there once a program
#    reaches std (KR-341). An archive, since a program importing std is several units.
"$KAMA" build "$IN" --target "$TRIPLE" --select OUTPUT=STATIC --cc "$CC $XFLAGS" -o "$lib"

# 2. link -> bootable ELF (startup + vector table + linker script + newlib/rdimon semihosting). --gc-sections drops
#    what nothing reaches (kama builds bare metal with -ffunction-sections), as a board's own link should.
#    The RISC-V board is an emulator harness only, with no C library: its startup ends QEMU through the test device and
#    supplies the allocation funnel's four functions, and libgcc the rest (rv32imc's objects link against the rv32im multilib).
if [ "$BOARD" = riscv32-virt ]; then
  [ -n "$QEMU" ] || { echo "mcu/build.sh: riscv32-virt is an emulator harness; pass --qemu" >&2; exit 2; }
  linkmarch=$(printf '%s' "$MARCH" | sed 's/c$//')_zicsr   # rv32imc -> rv32im, the multilib; the startup writes mtvec
  riscv64-unknown-elf-gcc -march="$linkmarch" -mabi=ilp32 -ffreestanding -nostdlib -nostartfiles -T "$LD" \
    -Wl,--gc-sections "$ROOT/boards/$BOARD/startup.c" "$lib" -lgcc -o "$OUT"
  echo "mcu/build.sh: wrote $OUT"
  exit 0
fi
#    An Xtensa core links through its simulator kit, also an emulator harness only: newlib, the core's memory map, its
#    register-window vectors, and an `_exit` that ends Espressif's QEMU through semihosting.
if [ -z "$LD" ]; then
  [ -n "$QEMU" ] || { echo "mcu/build.sh: $BOARD is an emulator harness; pass --qemu" >&2; exit 2; }
  "$CC" -specs=sim.elf.specs -specs=sys.qemu.specs -Wl,--gc-sections,--wrap=main "$ROOT/boards/esp32/startup.c" "$lib" -o "$OUT"
  echo "mcu/build.sh: wrote $OUT"
  exit 0
fi
semi=; [ -n "$QEMU" ] && semi="-DKAMA_MCU_SEMIHOST_EXIT"
[ -n "$UNPRIV" ] && semi="$semi -DKAMA_MCU_UNPRIVILEGED"
arm-none-eabi-gcc $CPUFLAGS -mthumb -ffreestanding -nostartfiles -T "$LD" --specs=rdimon.specs $semi \
  -Wl,--gc-sections "$ROOT/startup.c" "$lib" -o "$OUT"

echo "mcu/build.sh: wrote $OUT ($(arm-none-eabi-size "$OUT" | awk 'NR==2{print $1+$2" bytes flash, "$2+$3" bytes RAM"}'))"
