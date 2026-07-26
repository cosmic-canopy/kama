/* Linker script for the QEMU `microbit` (BBC micro:bit v1 — Nordic nRF51822, Cortex-M0, NO FPU) — kama's
 * reference no-FPU board. It exists to prove the soft-float path: on a core with no FPU the emitted C's
 * `float`/`double` ops become compiler-rt/libgcc libcalls, exercised end-to-end on emulated silicon.
 *
 * nRF51822 (aa variant): 256K flash at 0x0 (the vector table boots from there), 16K SRAM at 0x20000000.
 * Only the two MEMORY origins/lengths differ from the lm3s6965evb script; the sections below are
 * chip-independent (identical to every other board). */
ENTRY(Reset_Handler)

MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial stack pointer (top of RAM) */

SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH          /* vector table first — CPU boots from it */
  .text       : { *(.text*) *(.rodata*) } > FLASH
  .ARM.exidx  : { *(.ARM.exidx*) } > FLASH

  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
  .bss  : { _sbss = .; *(.bss* COMMON) . = ALIGN(4); _ebss = .; } > RAM

  . = ALIGN(8);
  end = .;        /* newlib sbrk heap base */
}
