# Running kama on a microcontroller

kama's no-GC / RAII / transpile-to-C model is a natural fit for bare-metal MCUs: deterministic, no runtime,
no allocator unless you ask for one. This guide shows how to turn a `.kama` program into firmware that boots
on a Cortex-M — emulated (QEMU, no hardware needed) or on a real board.

The compiler does the language half; the **board link** (startup / vector table / linker script / vendor
HAL) is toolchain work kept deliberately separate, so kama emits portable freestanding C and any C toolchain
can finish the job. This directory (`mcu/`) bundles a reference startup + a board linker script + two thin
scripts that wire the two halves into one command.

## The two halves

1. **kama → freestanding object.** `kama build --target embedded` emits `-ffreestanding -nostdlib` C (no libc,
   no OS, `main` never returns, a panic runs the board's `@panicHandler` or traps) and stops at an object, or an
   archive with `--select OUTPUT=STATIC` when the program imports std. Name the board's triple with `--target` and pass it
   to the C compiler with `--cc`; the board's clocks are `@monotonicClock`, `@wallClock` and `@bootClock` declarations
   (SPEC *Building*).
2. **object → bootable image.** A C toolchain links that object against a startup (reset vector + `.data`/`.bss`
   init), a linker script (the chip's memory map), and a libc — producing a flashable/emulable ELF.

## Turnkey: build + run under QEMU (no hardware)

Everything below runs in the opt-in `kama-mcu` toolchain image (arm-none-eabi-gcc + qemu-system-arm, and
riscv64-unknown-elf-gcc + qemu-system-riscv32 for the RISC-V board; clang is in the base image):

```sh
tools/cdev build-image-mcu          # once — builds the kama-mcu image (kept separate so the base stays lean)

# build a kama program into a Cortex-M firmware ELF, then run it on an emulated core:
KAMA_IMAGE=kama-mcu tools/cdev exec sh -c '
  mcu/build.sh tests/embedded_blink.kama -o /tmp/blink.elf --qemu &&
  mcu/run-qemu.sh /tmp/blink.elf; echo "exit=$?"'          # -> exit=22 (the program'\''s result)
```

- `mcu/build.sh <in.kama> [-o out.elf] [--board <name>] [--march <isa>] [--qemu] [--unprivileged]` — the turnkey
  build (kama object → linked ELF). `--qemu` builds the *emulator harness*: it runs `main` once and reports the return
  value through ARM **semihosting** so QEMU exits with it (real firmware instead loops forever — omit `--qemu`).
  `--unprivileged` starts `main` in unprivileged Thread mode, as an RTOS task runs; `--march` picks the RISC-V board's ISA.
- `mcu/run-qemu.sh <firmware.elf> [--board <name>]` — boots the ELF headless under QEMU and propagates the
  semihosting exit code.
- `tools/check-mcu.sh` — the CI proof: builds `tests/embedded_blink.kama` (returns 22 on every leg), runs it
  on QEMU's `lm3s6965evb` (Cortex-M3), and asserts exit 22; then std with a board's hooks, and `Atomic` at every width
  on all three boards (below). It's wired into `run_tests.sh` but **SKIPs**
  cleanly when the cross toolchain is absent, so it only really exercises under `kama-mcu`.

This proves the whole chain end-to-end with no board: `kama → C → Cortex-M object → link (startup + vector
table + linker script + newlib) → boots on the core → runs the real MMIO loop → exits with the right value`.

## Targeting a different board

Three boards ship: `lm3s6965evb` (the reference, QEMU Cortex-M3), `microbit` (QEMU nRF51822, **Cortex-M0,
no FPU** — the soft-float proving board; on a no-FPU core the emitted `float`/`double` ops become
compiler-rt/libgcc soft-float libcalls, exercised end-to-end by `tools/check-softfloat.sh`), and `riscv32-virt` (QEMU's
`virt` machine in machine mode, linked by `riscv64-unknown-elf-gcc` with no C library; an emulator harness only, with its
own startup in `mcu/boards/riscv32-virt/`). To add another (real or emulated):

1. Copy `mcu/boards/lm3s6965evb/linker.ld` to `mcu/boards/<yourboard>/linker.ld` and edit the two `MEMORY`
   origins/lengths to the chip's datasheet (e.g. **STM32F103**: `FLASH 0x08000000/64K`, `RAM 0x20000000/20K`).
   The sections are chip-independent.
2. Add a `case` for the board in `mcu/build.sh` (its `-mcpu` + clang triple) and, for emulation, in
   `mcu/run-qemu.sh` (its QEMU machine). CPU/triple examples: Cortex-M0+ → `cortex-m0plus` / `thumbv6m-none-eabi`;
   Cortex-M4F → `cortex-m4` / `thumbv7em-none-eabi`.

`mcu/startup.c` (the reset handler + vector table) is vendor-neutral and usually needs no change; a real board
more often uses its SDK's startup instead — see below.

## `Atomic` on a chip without the instruction

`Atomic<T>` takes 1, 2, 4 and 8-byte cells, and not every core has an atomic of each width. What the C compiler reports
for the core (`__GCC_ATOMIC_*_LOCK_FREE`, 2 meaning always lock-free) decides, so name the core in `--cc`: one triple,
`riscv32-unknown-none-elf`, covers both rv32imc and rv32imac.

| core | 1, 2, 4 bytes | 8 bytes |
|---|---|---|
| `thumbv6m`: Cortex-M0, M0+ (an RP2040) | loads and stores native; read-modify-writes masked | masked |
| `thumbv7m`, `thumbv7em`, `thumbv8m.main`: M3, M4, M7, M33; `thumbv8m.base`: M23 | native (`ldrex`/`strex`) | masked |
| `riscv32 -march=rv32imac` | native (`amo*`, `lr`/`sc`) | masked |
| `riscv32 -march=rv32imc` (an ESP32-C3) | loads and stores native; read-modify-writes masked | masked |
| Xtensa: an ESP32, an ESP32-S3 | native (`s32c1i`) | masked |
| Xtensa: an ESP32-S2, or any of them built `-mdisable-hardware-atomics` | loads and stores native; read-modify-writes masked | masked |
| `wasm32`, `armv7a`, every 64-bit arch | native | native |

**Masked** means interrupts are off for the operation's own few instructions and restored to what they were, so an
operation inside an `@interrupt` handler or a masked region leaves it masked. It is sound because kama runs on one core
of a board; a second core is KR-346, which replaces the mask with a cross-core lock. On a Cortex-M0 a masked 4-byte
`fetchAdd` is `mrs; cpsid i; mrs; cmp; beq; ldr; adds; str; msr`, 30 bytes as a function against 28 for an M3's native
`ldrex`/`strex` loop; a plain load is `ldr; dmb sy`. On RISC-V the mask is `csrrci mstatus, 8` and `csrs`. On Xtensa it
is `rsil a, 15`, which raises the level above every maskable one, and `wsr.ps a; rsync`: a masked 8-byte `fetchAdd` is 34
bytes as a function on an ESP32, and an S2's masked 4-byte one is 22 bytes, against 27 for the ESP32's native `s32c1i`
loop. ESP-IDF builds `-mdisable-hardware-atomics` when the heap is in external RAM, where `s32c1i` does not work, and
there every read-modify-write masks.

**Privileged mode.** On an Arm M-profile core `cpsid i` is silently ignored in unprivileged Thread mode, which is how
an RTOS runs a task (FreeRTOS-MPU, Zephyr's user mode), and the operation would stop being atomic. So every masked
operation reads PRIMASK back, and panics if the mask did not take: the second `mrs`, `cmp` and `beq` above, 8 bytes.
RISC-V needs machine mode, as an ESP32-C3's code runs; outside it the CSR access traps by itself. An ESP32, S2 or S3
core has no unprivileged mode: it keeps no ring, so `rsil` always takes.

On a core kama has no mask for, an Arm core older than v6 such as an ARM7TDMI, the C compiler refuses each width the
chip lacks when the program is built, naming the cell, and a program that declares none builds. On a hosted target a
width with no lock-free atomic is refused too, by kama, since there the C compiler's answer is a lock.
`tools/check-mcu.sh` runs every width and operation on the Cortex-M3, the Cortex-M0, the RISC-V board built both ways,
and an ESP32 (built both ways) and an ESP32-S3 on Espressif's QEMU. It also runs an `Atomic<uint64>` that SysTick and
`main` add to together and a program started unprivileged that must panic, and builds for an ESP32-S2, which QEMU
lacks. `tools/check-embedded.sh` holds every such archive to no `__atomic_*` call, and an ARMv4T to its refusal.

## Real hardware

kama emits portable C, so the practical path on a real board is to hand that C to the board's own SDK/toolchain
(which already owns the correct startup, linker script, and clock/peripheral init). Two ways in:

- **Object link** (Cortex-M, matches the flow above): `kama build --target embedded --cc "<vendor-gcc flags>"`
  → an object; link it with the vendor startup + linker script. The object exposes `kama_main()` (your
  `fn uint8 main()`); call it from the SDK's `main`/`app_main`.
- **Transpile** (any target): `kama transpile app.kama -o app.c` → drop `app.c` into the SDK project as a source
  file and call `kama_main()` from the vendor entry point.

Board-by-board:

- **Raspberry Pi Pico / RP2040 (Cortex-M0+).** Use pico-sdk: add the transpiled `app.c` (or the
  `--cc "clang --target=thumbv6m-none-eabi -mcpu=cortex-m0plus"` object) to a pico-sdk CMake project, call
  `kama_main()` from `main()`, `cmake && make`, and flash the `.uf2` by dragging it onto the Pico's mass-storage
  bootloader (BOOTSEL). Peripheral access is `hardware UnsafePtr<T>` over the RP2040 register map (or FFI to pico-sdk).
- **STM32 (Cortex-M0/3/4/7).** Use STM32CubeIDE / arm-none-eabi-gcc with the chip's HAL: add the transpiled C,
  call `kama_main()` from `main()`, build, flash with `st-flash` / OpenOCD / the CubeProgrammer. Or reuse this
  repo's flow with a board linker script edited to the chip's memory map.
- **ESP32 / ESP32-C3/C6 (Xtensa or RISC-V).** Use esp-idf: `kama transpile` the program, add `app.c` to an
  esp-idf component, call `kama_main()` from `app_main()`, `idf.py build flash monitor`. (esp-idf brings its own
  Xtensa/RISC-V gcc — the Cortex-M triple above does not apply; the transpiled C is portable.) To emulate one
  without a board, `mcu/build.sh --board esp32` (or `esp32s2`, `esp32s3`) builds with Espressif's GCC and its
  simulator kit, and `mcu/run-qemu.sh --board esp32` runs it on Espressif's QEMU (the `kama-mcu` image has both).
- **Arduino.** For **SAMD / Cortex-M** Arduino boards (e.g. Nano 33 BLE) the Cortex-M path applies (transpile +
  the core's toolchain, call `kama_main()` from `setup()`). For **classic AVR** Arduinos (Uno/Nano/Mega) note
  that kama's AVR support (Harvard `PROGMEM`, `ISR(VECTOR)` macros, `avr-gcc`) is **not yet shipped** — see
  [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §5; the portable compute transpiles, but AVR-specific peripheral/flash idioms don't
  emit yet.

If you flash a real board, the exact `--cc` flags, linker script, and flash command are the vendor SDK's — the
kama side is just "produce the object / the `.c`, and call `kama_main()`."

## Licensing (MIT-clean)

kama is MIT (GOALS #8), and this MCU flow keeps user firmware and the kama distribution unencumbered:

- **arm-none-eabi-gcc / libgcc** are GPLv3 **with the GCC Runtime Library Exception** — code *compiled and
  linked* by them is **not** placed under the GPL (the basis on which all commercial embedded firmware ships).
- **newlib / librdimon** (the semihosting libc linked into the image) are permissive **BSD-style**.
- **clang/LLVM** (the object compiler) is **Apache-2.0** with the LLVM exception.
- **QEMU** is GPLv2 but is only an *emulator we run* — never linked into or shipped with kama; it lives solely
  in the opt-in `kama-mcu` container image.
- The bundled `mcu/startup.c`, `mcu/boards/*/linker.ld`, and the scripts are original to this repo and MIT.
- ARM **semihosting** (`bkpt 0xAB`, `SYS_EXIT_EXTENDED`) is a published ABI — using it is neither a copyright
  nor a patent concern.

So both your firmware and kama itself stay MIT-compatible; the only copyleft components are build/emulation
*tools* whose licenses do not reach their output, confined to the opt-in image.
