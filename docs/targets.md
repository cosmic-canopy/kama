# Targets & toolchain setup

How to build kama for the platform you want, and what you need installed to do it. The language-level
rules — target triples, single-select groups and the `@compileFor` flags they derive — are in
[SPEC.md](SPEC.md#conditional-compilation--compileforflag-).

## The short version

```sh
kama build app.kama                              # this machine (the default)
kama build app.kama --release                    # optimized, stripped
kama build app.kama --target WASM                # the web
kama build app.kama --target LINUX               # cross-compile
kama build lib.kama --select OUTPUT=STATIC       # a static library
```

kama compiles to **ISO C11** and hands it to a C compiler. That is why cross-compiling is realistic:
the hard part is not code generation, it's having a C compiler that can reach the target. Everything
below is about that.

## Built-in targets

| `--target` | Triple | Needs |
|---|---|---|
| `HOST` *(default)* | this machine's | your system C compiler — already there |
| `MACOS` | `<arch>-macos-none` | the macOS SDK (free on a Mac; awkward elsewhere) |
| `WINDOWS` | `<arch>-windows-gnu` | a mingw-w64 toolchain, or zig |
| `LINUX` | `<arch>-linux-gnu` | a Linux sysroot, or zig |
| `WASM` | `wasm32-emscripten-none` | [Emscripten](https://emscripten.org) (`emcc` on PATH) |
| `EMBEDDED` | `<arch>-none-none` | nothing extra for the host's own arch |

A **named target follows the host's architecture** — `--target LINUX` on an Apple Silicon Mac means
`aarch64-linux-gnu`. When the arch matters, spell the triple: `--target x86_64-linux-gnu`. Names are
conveniences; triples are exact.

Anything containing a `-` is taken as a bare `<arch>-<os>-<abi>` triple and needs no configuration at
all, which is how you reach a target kama has never heard of:

```sh
kama build firmware.kama --target thumbv7em-none-eabihf --cc "arm-none-eabi-gcc"
kama build app.kama      --target riscv64-linux-musl
```

## Getting a cross toolchain

Three routes. **Try them in this order** — the first one that describes you is the least work.

### 1. You already have a cross toolchain

Use it. Declare it once in `kama.json` so it is checked in and the whole team gets it:

```json
{
  "select": {
    "TARGET": {
      "RPI": {
        "triple":  "aarch64-linux-gnu",
        "cc":      "aarch64-linux-gnu-gcc",
        "ar":      "aarch64-linux-gnu-ar",
        "sysroot": "/opt/rpi-sysroot",
        "cflags":  ["-mcpu=cortex-a72"],
        "ldflags": []
      }
    }
  }
}
```

```sh
kama build app.kama --target RPI
```

`ar` matters for `OUTPUT=STATIC`: an archive indexed by the host's `ar` may be unreadable to the
target's linker.

If the paths are personal (your own sysroot location), put the target in **`kama.local.json`**
instead — same shape, gitignored, overrides the committed manifest.

### 2. You have clang and a sysroot

clang has always been a cross compiler; what it lacks is the target's **headers and libraries**. Point
it at a sysroot and it works:

```sh
kama build app.kama --target aarch64-linux-gnu --cc "clang" \
                    --select OUTPUT=EXE   # plus a sysroot via kama.json, as above
```

kama detects that `clang` is a multi-target driver and passes `-target <triple>` for you. A
**per-target** compiler (`aarch64-linux-gnu-gcc`, a vendor ARM gcc, an NDK wrapper) has its triple in
its name, so kama deliberately does *not* pass `-target` — that would break it.

### 3. You have neither — use zig

[zig](https://ziglang.org/download/) ships the C libraries for essentially every target (musl, glibc
stubs, mingw-w64, wasi-libc), so `zig cc` cross-compiles with no sysroot hunt. One binary, no
per-target installs.

```sh
brew install zig        # or scoop/apt/pacman, or just unpack the tarball
kama build app.kama --target WINDOWS
```

**If `zig` is on your PATH, kama uses it automatically for a cross build** — you do not need `--cc`.
It is only ever chosen when the host compiler genuinely cannot do the job, so ordinary builds are
unaffected. To be explicit, or to use a zig that isn't on PATH:

```sh
kama build app.kama --target WINDOWS --cc "/path/to/zig cc"
```

Kama installs that ship without a system C compiler bundle zig for exactly this reason, and that copy
is used the same way — so on such a machine zig compiles *every* build, not just cross builds.

#### Is `zig cc` as fast as clang?

**Yes — because it *is* clang.** zig bundles LLVM and exposes `zig cc` as a drop-in clang driver, so
the optimizer and code generator are the same ones; `zig cc --version` reports a clang version. kama
passes both compilers identical flags (`-O3 -DNDEBUG -ffunction-sections -fdata-sections` in release)
and **no** `-march`/`-mcpu`, so neither tunes for a specific CPU — both target the architecture's
generic baseline.

Measured on this repo's benchmarks (`bench/src/kama/`), native release builds, best of 5:

| bench | clang | `zig cc` | ratio |
|---|---|---|---|
| collatz | 0.065 s | 0.065 s | **1.00×** |
| pi | 0.012 s | 0.012 s | 1.00× |
| math | 0.007 s | 0.007 s | 0.98× |

(The sub-10 ms rows are dominated by process startup; collatz is the only one long enough to mean
much.) zig's binaries are larger *as files* — but their `__TEXT` is actually **smaller** (1.4 KB vs
16 KB on collatz); the difference is linker metadata and segment padding, not more code.

So there is no performance reason to prefer one, and **your host build does not change** either way:
zig is only substituted when the host compiler genuinely cannot reach the target.

### Always available: emit the C

No toolchain here at all? Transpiling never needs one — it is pure code generation, so it accepts any
target and hands the C to whatever builds it downstream (a vendor IDE, a Yocto recipe, someone else's
CI):

```sh
kama transpile app.kama --target thumbv7em-none-eabihf -o app.c
```

## What "cross" actually means here

kama only treats a build as cross-compiling when the host compiler genuinely **cannot** do the job:

- a **different architecture**, or
- a **different OS for a hosted target**.

Bare metal on your own architecture is *not* cross — `--target EMBEDDED` is
`-ffreestanding -nostdlib -c` stopping at an object file, which your ordinary clang does perfectly
well. That is what has always made kama's bare-metal builds toolchain-agnostic, and it still holds for
any `os=none` triple on the host arch.

## Output kinds

```sh
kama build app.kama                            # EXE (default)
kama build app.kama --shared                   # SHARED  -> .dylib / .so / .dll
kama build lib.kama --select OUTPUT=STATIC     # STATIC  -> libapp.a
kama build lib.kama --select OUTPUT=OBJECT     # OBJECT  -> app.o
```

The extension follows the **target**, so a Windows build from a Mac produces `.dll`, not `.dylib`. A
bare-metal target defaults to `OBJECT` (there is no OS to link against); the board's startup object and
linker script own the final image, which is your link step.

`SHARED` exports only functions marked `expose` (everything else is hidden), so the library's surface
is exactly what you declared.

## Platform notes

- **macOS → anything**: fine with zig or a sysroot.
- **anything → macOS**: needs the macOS SDK, which Apple's licence keeps on Apple hardware. Practical
  answer: build macOS artifacts on a Mac (or a Mac CI runner).
- **→ Windows**: `WINDOWS` targets the **mingw-w64** ABI (`-gnu`), which zig covers. An MSVC-ABI build
  (`x86_64-windows-msvc`) needs the MSVC headers and libraries, so build it on Windows.
- **→ wasm**: uses Emscripten (`emcc`), not zig — it emits an `.html` + `.js` + `.wasm` harness, and
  `$EMCC` or `--cc` overrides which `emcc`.
- **→ bare metal**: for the host arch, nothing extra. For a real board, name its triple and give it a
  `cc` (`arm-none-eabi-gcc`, or `zig cc`). See [mcu.md](mcu.md) for the full firmware flow.

## Troubleshooting

**"cannot build for X — the default C compiler has no libc for it"**
kama will not guess a toolchain that cannot work. Pick a route from *Getting a cross toolchain* above,
or `kama transpile` and build the C elsewhere.

**I want to tune for a specific CPU.**
kama passes no `-march`/`-mcpu`/`-mtune`, so builds target the architecture's generic baseline — which
is what makes them portable. To override, put it in the target's `cflags`:
`"RPI": { "triple": "aarch64-linux-gnu", "cflags": ["-mcpu=cortex-a72"] }`. There is deliberately no
`--march=native` shorthand yet; it is a recorded follow-on (ROADMAP §10).

**A `@compileFor` gate isn't firing.**
Gate on the **derived** facts, not on how you spelled the build. `--target EMBEDDED` and
`--target xtensa-none-elf` both give you `OS_NONE`; neither gives you a flag called `EMBEDDED`, because
a built-in target name describes the invocation rather than the target. The flags a target contributes
are `ARCH_<arch>`, `OS_<os>`, `ABI_<abi>`, and `HOSTED` when the target has an OS.

**Two values of one group.**
`--select CONSOLE=PS5 --select CONSOLE=XBOX` is an error: a `select` group is single-select by
definition. Use `--define` / `flags` for things that combine.
