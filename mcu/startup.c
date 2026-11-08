/* kama bare-metal Cortex-M startup — the reset vector + minimal C runtime bring-up that the freestanding
 * `--target embedded` object links against to become a bootable firmware image. Deliberately tiny and
 * vendor-neutral: a real board usually replaces this with its SDK's startup (pico-sdk / STM32 HAL / Arduino
 * core) — see docs/mcu.md — but for the reference QEMU flow (and any board with the same memory map) this is
 * the whole runtime.
 *
 * Two entry behaviors, one flag:
 *   default                    — call kama's freestanding `main()` (which runs the user's `main` then loops
 *                                forever), the correct shape for real firmware.
 *   -DKAMA_MCU_SEMIHOST_EXIT   — run the user's `kama_main()` ONCE and exit through ARM semihosting with its
 *                                return value, so an emulator (QEMU `-semihosting`) stops with that exit code.
 *                                This is how mcu/build.sh --qemu + tools/check-mcu.sh assert behavior with no
 *                                real board. Never use it in shipped firmware.
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int kama_main(void);   /* the user's `fn uint8 main()` (external in the emitted C) */
extern int main(void);        /* kama's freestanding wrapper: runs kama_main then `for(;;)` */

#ifdef KAMA_MCU_SEMIHOST_EXIT
/* ARM semihosting SYS_EXIT_EXTENDED (op 0x20): QEMU exits with `code`. `bkpt 0xAB` is the Cortex-M trap. */
static void semihost_exit(int code) {
    uint32_t block[2] = { 0x20026u /*ADP_Stopped_ApplicationExit*/, (uint32_t)code };
    register uint32_t r0 __asm__("r0") = 0x20u;
    register void*    r1 __asm__("r1") = (void*)block;
    __asm__ volatile ("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    for (;;) {}
}
/* The same exit, for a program that must end the emulator from somewhere other than `main`'s return: a
   `@panicHandler` under test (tests/support/embedded_panic/main.kama, through tests/support/embedded_panic/kama_mcu.h). */
void kama_mcu_exit(int code) { semihost_exit(code); }
#endif

void Reset_Handler(void) {
    uint32_t *src = &_sidata, *dst = &_sdata;   /* copy .data from flash to RAM */
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;   /* zero .bss */
#ifdef KAMA_MCU_UNPRIVILEGED
    /* Unprivileged Thread mode, as an RTOS task runs (FreeRTOS-MPU, Zephyr's user mode): `cpsid i` is ignored there, so an
       `Atomic` that masks interrupts must notice and panic rather than silently stop being atomic (KR-349). */
    __asm__ volatile("msr control, %0\n\tisb" : : "r"(1u) : "memory");
#endif
#ifdef KAMA_MCU_SEMIHOST_EXIT
    semihost_exit(kama_main());
#else
    main();
#endif
    for (;;) {}
}

void Default_Handler(void) { for (;;) {} }
/* The core's exceptions, each a weak alias a program overrides by defining the name, as kama's
   `@linkName("SysTick_Handler") @interrupt expose fn` does. */
void NMI_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)   __attribute__((weak, alias("Default_Handler")));

/* The Cortex-M vector table: [0] = initial stack pointer, [1] = reset handler, [2..15] the core's exceptions. The
 * remaining entries are zero; a real firmware fills in its peripherals' ISRs (kama's `@interrupt` handlers) there. */
__attribute__((section(".isr_vector"), used))
uint32_t *g_vectors[64] = {
    (uint32_t*)&_estack,
    (uint32_t*)Reset_Handler,
    (uint32_t*)NMI_Handler,
    (uint32_t*)HardFault_Handler,
    [11] = (uint32_t*)SVC_Handler,
    [14] = (uint32_t*)PendSV_Handler,
    [15] = (uint32_t*)SysTick_Handler,
};
