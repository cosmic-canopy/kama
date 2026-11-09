/* kama bare-metal harness for an ESP32, ESP32-S2 or ESP32-S3 core, run on Espressif's QEMU (`qemu-system-xtensa -M esp32
 * -semihosting -kernel`): the Xtensa half of the reference flow mcu/startup.c is for Cortex-M. The reset code, the memory map,
 * the register-window vectors and the semihosting `_exit` are the toolchain's own simulator kit (`-specs=sim.elf.specs
 * -specs=sys.qemu.specs`, with newlib), one per chip, so this file only runs the user's `kama_main()` ONCE and ends QEMU
 * with its return value, as mcu/startup.c does through Arm's semihosting. The kit's startup calls `main`, and a bare-metal
 * kama program's own `main` runs it and then spins, as firmware does; mcu/build.sh links with `-Wl,--wrap=main`, so the
 * kit reaches `__wrap_main` below instead and `exit`s with what it returns.
 *
 * Built only as the emulator harness. Never use it in shipped firmware: on a board, esp-idf boots the chip (docs/mcu.md).
 */
#include <stdlib.h>

extern int kama_main(void);

/* For a test program that ends the emulator from somewhere other than main's return (tests/support/<name>/kama_mcu.h). */
void kama_mcu_exit(int code) { exit(code); }

int __wrap_main(void) {
    /* What the chip's boot ROM does before any program, which `-kernel` skips: ATOMCTL to 0x15, so `s32c1i` (the native
       1-, 2- and 4-byte atomics) works on memory the core does not cache, which on these chips is all of it. Read back from
       Espressif's QEMU booted through the ROM; its reset value, 0x28, makes `s32c1i` raise LoadStoreError. An S2 has
       neither the instruction nor the register. */
#if __XCHAL_HAVE_S32C1I
    __asm__ volatile("wsr.atomctl %0\n\trsync" : : "r"(0x15) : "memory");
#endif
    return kama_main();
}
