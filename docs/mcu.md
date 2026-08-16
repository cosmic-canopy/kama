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
   no OS, `main` never returns, a weak `kama_panic_handler`) and stops at an object. Pass the Cortex-M triple
   to the C compiler with `--cc` (kama stays board-agnostic).
2. **object → bootable image.** A C toolchain links that object against a startup (reset vector + `.data`/`.bss`
   init), a linker script (the chip's memory map), and a libc — producing a flashable/emulable ELF.

## Turnkey: build + run under QEMU (no hardware)

Everything below runs in the opt-in `kama-mcu` toolchain image (arm-none-eabi-gcc + qemu-system-arm; clang is
in the base image):

```sh
tools/cdev build-image-mcu          # once — builds the kama-mcu image (kept separate so the base stays lean)

# build a kama program into a Cortex-M firmware ELF, then run it on an emulated core:
KAMA_IMAGE=kama-mcu tools/cdev exec sh -c '
  mcu/build.sh tests/embedded_blink.kama -o /tmp/blink.elf --qemu &&
  mcu/run-qemu.sh /tmp/blink.elf; echo "exit=$?"'          # -> exit=22 (the program'\''s result)
```

- `mcu/build.sh <in.kama> [-o out.elf] [--board <name>] [--qemu]` — the turnkey build (kama object → linked
  ELF). `--qemu` builds the *emulator harness*: it runs `main` once and reports the return value through ARM
  **semihosting** so QEMU exits with it (real firmware instead loops forever — omit `--qemu`).
- `mcu/run-qemu.sh <firmware.elf> [--board <name>]` — boots the ELF headless under QEMU and propagates the
  semihosting exit code.
- `tools/check-mcu.sh` — the CI proof: builds `tests/embedded_blink.kama` (returns 22 on every leg), runs it
  on QEMU's `lm3s6965evb` (Cortex-M3), and asserts exit 22. It's wired into `run_tests.sh` but **SKIPs**
  cleanly when the cross toolchain is absent, so it only really exercises under `kama-mcu`.

This proves the whole chain end-to-end with no board: `kama → C → Cortex-M object → link (startup + vector
table + linker script + newlib) → boots on the core → runs the real MMIO loop → exits with the right value`.

## Targeting a different board

Two boards ship: `lm3s6965evb` (the reference, QEMU Cortex-M3) and `microbit` (QEMU nRF51822, **Cortex-M0,
no FPU** — the soft-float proving board; on a no-FPU core the emitted `float`/`double` ops become
compiler-rt/libgcc soft-float libcalls, exercised end-to-end by `tools/check-softfloat.sh`). To add another
(real or emulated):

1. Copy `mcu/boards/lm3s6965evb/linker.ld` to `mcu/boards/<yourboard>/linker.ld` and edit the two `MEMORY`
   origins/lengths to the chip's datasheet (e.g. **STM32F103**: `FLASH 0x08000000/64K`, `RAM 0x20000000/20K`).
   The sections are chip-independent.
2. Add a `case` for the board in `mcu/build.sh` (its `-mcpu` + clang triple) and, for emulation, in
   `mcu/run-qemu.sh` (its QEMU machine). CPU/triple examples: Cortex-M0+ → `cortex-m0plus` / `thumbv6m-none-eabi`;
   Cortex-M4F → `cortex-m4` / `thumbv7em-none-eabi`.

`mcu/startup.c` (the reset handler + vector table) is vendor-neutral and usually needs no change; a real board
more often uses its SDK's startup instead — see below.

## Real hardware

kama emits portable C, so the practical path on a real board is to hand that C to the board's own SDK/toolchain
(which already owns the correct startup, linker script, and clock/peripheral init). Two ways in:

- **Object link** (Cortex-M, matches the flow above): `kama build --target embedded --cc "<vendor-gcc flags>"`
  → an object; link it with the vendor startup + linker script. The object exposes `kama_main()` (your
  `fn int32 main()`); call it from the SDK's `main`/`app_main`.
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
  Xtensa/RISC-V gcc — the Cortex-M triple above does not apply; the transpiled C is portable.)
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
