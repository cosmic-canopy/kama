/* kama bare-metal RISC-V startup for QEMU's `virt` machine (`qemu-system-riscv32 -M virt -bios none`): the RISC-V half of the
 * reference flow mcu/startup.c is for Cortex-M. The core starts at 0x80000000 in machine mode, as an ESP32-C3's code runs,
 * which is what `Atomic`'s interrupt mask needs on a chip without the A extension (KR-349). QEMU loads the ELF straight into
 * RAM, so there is no .data to copy, only .bss to zero.
 *
 * Built only as the emulator harness: it runs the user's `kama_main()` ONCE and ends QEMU with its return value through the
 * machine's test device (sifive_test), as mcu/startup.c does through semihosting. Never use it in shipped firmware.
 */
#include <stdint.h>
#include <stddef.h>

extern char _sbss[], _ebss[], _heap[], _estack[];
extern int kama_main(void);

/* sifive_test at 0x100000: 0x5555 ends QEMU with status 0, `code << 16 | 0x3333` with status `code`. */
void kama_mcu_exit(int code) {
    *(volatile uint32_t*)0x100000 = code == 0 ? 0x5555u : ((uint32_t)code << 16) | 0x3333u;
    for (;;) {}
}

/* Any trap (an illegal instruction, a fault) ends QEMU with 99 rather than spinning at address 0. */
static void trap(void) { kama_mcu_exit(99); }

void kama__mcu_start(void) {
    for (char* p = _sbss; p < _ebss; ) *p++ = 0;
    __asm__ volatile("csrw mtvec, %0" : : "r"(&trap));
    kama_mcu_exit(kama_main());
}

__attribute__((naked, section(".text.start"))) void _start(void) {
    __asm__ volatile("la sp, _estack\n\tj kama__mcu_start");
}

/* The allocation funnel's C functions (`Locked`, `Shared`), as a bump over the RAM above .bss. Nothing is given back, which a
   test program run once does not need; a real board brings its own allocator, or declares a `@globalAllocator`. */
static char* brk = _heap;
void* aligned_alloc(size_t align, size_t n) {
    uintptr_t p = ((uintptr_t)brk + align - 1) & ~(uintptr_t)(align - 1);
    brk = (char*)(p + n);
    return (void*)p;
}
void* malloc(size_t n) { return aligned_alloc(16, n); }
void free(void* p) { (void)p; }
void* realloc(void* p, size_t n) {
    char* q = malloc(n);
    if (p) for (size_t i = 0; i < n && (char*)p + i < q; ++i) q[i] = ((char*)p)[i];   /* the old block lies below the new */
    return q;
}
