#ifndef KAMA_MCU_H
#define KAMA_MCU_H
/* The QEMU harness's semihosting exit (mcu/startup.c, built with KAMA_MCU_SEMIHOST_EXIT), for a test program that ends the
   emulator from somewhere other than main's return: tests/support/embedded_panic.kama's `@panicHandler`. */
void kama_mcu_exit(int code);
#endif
