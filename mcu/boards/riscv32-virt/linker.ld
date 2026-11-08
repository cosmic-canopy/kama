/* QEMU `virt`, rv32: RAM at 0x80000000, where the core starts with `-bios none`. Paired with startup.c beside it. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16M }
SECTIONS {
    .text   : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
    .data   : { *(.data .data.* .sdata .sdata.*) } > RAM
    .bss (NOLOAD) : { _sbss = .; *(.bss .bss.* .sbss .sbss.* COMMON) _ebss = .; } > RAM
    . = ALIGN(16);
    _heap = .;
    _estack = ORIGIN(RAM) + LENGTH(RAM);
}
