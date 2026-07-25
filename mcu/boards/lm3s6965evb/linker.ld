/* Linker script for the QEMU `lm3s6965evb` (Stellaris LM3S6965, Cortex-M3) — the reference bare-metal board
 * for kama's MCU flow. Flash is at 0x0 (the vector table boots from there), SRAM at 0x20000000.
 *
 * To target a real chip, copy this file and edit the two MEMORY origins/lengths to the chip's datasheet
 * (e.g. STM32F103: FLASH 0x08000000/64K, RAM 0x20000000/20K) — the sections below are chip-independent. */
ENTRY(Reset_Handler)

MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
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
