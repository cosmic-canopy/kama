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
kama build app.kama -j 4                         # cap concurrent C compiles (default: core count)
```

**Build parallelism.** A program that imports from `std` compiles several translation units — an import
brings in the files defining the names it asks for, plus their closure (see
[SPEC.md](SPEC.md#modules-); `examples/httpd` is 10) — and `kama build` compiles them
concurrently, `-j`/`--jobs` wide, defaulting to your core count. `$KAMA_BUILD_JOBS` sets the default;
`-j` on the command line beats it. It is purely a scheduling knob: the objects a build produces are
byte-identical at every width, which [`tools/check-build-jobs.sh`](../tools/check-build-jobs.sh) asserts.

Two cases stay a single compiler invocation, deliberately: `--release` native (which folds the whole
program into one translation unit so the C compiler can inline across modules), and a **bundled**
install, whose `zig cc` keeps its own object cache — one invocation re-compiles only what changed
(~0.1 s), which beats anything splitting them could do.

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
  "link": ["m"],
  "select": {
    "TARGET": {
      "RPI": {
        "triple":  "aarch64-linux-gnu",
        "cc":      "aarch64-linux-gnu-gcc",
        "ar":      "aarch64-linux-gnu-ar",
        "sysroot": "/opt/rpi-sysroot",
        "cflags":  ["-mcpu=cortex-a72"],
        "ldflags": [],
        "runtime": "static",
        "link":    ["m", "atomic"]
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

**`link`** names native libraries by their bare name — `["m"]` becomes `-lm`. It is the portable half
of linking, which is why it sits on the *project* as well as on a target: a project that needs `-lm`
everywhere says so once, at the top level, instead of repeating it per target or making every caller
remember `--link m`. `ldflags` remains the raw-linker-text escape hatch for anything that is not a
library name.

A target's `link` **replaces** the project's rather than adding to it — that is what makes it possible
to say *"not that one, here"*. This is deliberately the opposite of `cflags`/`ldflags`, which append
onto the built-in target they merge over. A target that never mentions `link` inherits the project's
list unchanged. The CLI `--link` is unaffected and still appends, after both.

**`cflags` and `ldflags` sit on the project too**, exactly like `link`:

```json
{
  "cflags":  ["-DFEATURE_X"],
  "ldflags": ["-Wl,--as-needed"],
  "select": { "TARGET": { "RPI": { "cflags": ["-mcpu=cortex-a72"] } } }
}
```

A project-level entry applies to **every** target; a target's list is **appended after** it, so the
command line reads project-then-target and the more specific list gets the last word wherever the C
compiler resolves a repeated flag last-wins. The honest limit of appending: a project-level flag cannot
be *removed* on one target the way `link` can be — override it with a later flag instead.

Two reasons it is on the project and not only on a target. A flag that is true of the artifact
(`-DFEATURE_X`) is not a fact about any one target, and saying it once beats repeating it in each.
More decisively: a target is matched **by name**, so `--target aarch64-linux-gnu` — an anonymous
triple — matches no `select.TARGET` entry at all, and a **dependency** cannot know how its consumer
spells the target. Without a project tier, a dependency's build settings could not reach such a build
at all; see [What a dependency contributes](#what-a-dependency-contributes).

### `no-heap`, `webgpu` and `reproducible-float` — the same shape, for the same reason

Three more project properties a target may override, spelled exactly like `link`:

```json
{
  "kind": "executable",
  "no-heap": true,
  "select": { "TARGET": { "HOST": { "no-heap": false } } }
}
```

**`no-heap`** forbids heap allocation program-wide, the same rule `--no-heap` applies. It wants a
manifest key more than either of the others do, because the FLAG fails **silently** when forgotten: the
build simply succeeds with allocation allowed, and a bare-metal target quietly gains a heap nobody asked
for. Saying it once in the manifest makes it a property of the project instead of something every
invocation has to remember.

What the rule proves is that the program never REACHES an allocation. `main`, every `expose fn` and every
`@foreignEntry` body are the roots (SPEC *No-heap subset*). It does not prove that the object never NAMES
`malloc`: a stdlib body that came in with an import and is never called may still reference it. So every no-heap
and bare-metal compile gets `-ffunction-sections -fdata-sections`, in debug as well as release, and **the board's
link step should pass `--gc-sections`**, which drops the unreached code. Measured on a `--release --target
embedded --no-heap` object importing `std::uuid`: `ld --gc-sections -e main` links it with no libc, and the same
link without `--gc-sections` fails on undefined references from bodies nothing calls.

**`webgpu`** is the `--webgpu` flag as a project property. It is also a *linking* decision, which is the
class `link` exists for.

The per-target override is the ordinary case for both, not an exotic one: no heap on the board and a
heap on the host that builds your tooling; WebGPU from the browser on wasm and from the wgpu-native SDK
natively. Both values are checked when the manifest is read, so `"no-heap": "yes"` is refused by name
rather than read as some truthiness nobody wrote down. `--no-heap` and `--webgpu` can only turn a
setting **on**, so they OR in with the manifest; a target is how you say *"not this one"*.

**`reproducible-float`** is the third of the shape, and it says: *this program's floating point must
give the same bits on every target.*

```json
{ "reproducible-float": true }
```

Without it, clang's default lets `a * b + c` contract into a single fused multiply-add — one rounding
step instead of two. A target with an FMA (arm64) then gives different bits from one without
(wasm32 MVP, baseline x86-64 SSE2), so the same source disagrees with itself browser-versus-native.
Measured on this repo's own fixture: **13 of 64 random triples differ** on aarch64-macos in a release
build, and **0** with the key.

It is named for the goal rather than for the `-ffp-contract=off` it emits today, because the flag is a
C-backend detail and the goal is not. It is a boolean because kama can honor exactly two states, and
"absent" already means *"I have not thought about it"* — a third spelling for that would be a second
way to say the same thing. Manifest-only, deliberately: a property that correctness depends on must not
be losable by forgetting a command-line flag.

⚠️ Two honest limits. The flag applies to a **build**, so it does not travel through `kama transpile`
into someone else's toolchain — pass it yourself there. And it changes nothing in a **debug** build
that was not already true: kama's own overflow-checking arithmetic already splits the expression, so
the difference only appears under `--release`.

**And one way it is NOT like the other two: it propagates from a dependency.** `no-heap` and `webgpu`
are read from your manifest only, because a dependency must not change what compiles in your program
or make your build demand an SDK. `reproducible-float` can only turn contraction *off*, and it states
a requirement of the dependency's own arithmetic — which you compile. So a dependency that declares it
gets it, in your build, and it is one-way: a dependency's `false` takes nothing from a consumer (or a
sibling) that asked. This was not true of `0.9.169` as shipped, where the key was grouped with the
other two — and a raw `-ffp-contract=off` in a dependency's `cflags` propagated while the first-class
key replacing it did not.

`runtime` is `"static"` or `"dynamic"` and says how the *language's own* runtime is linked — see
[Runtime linkage](#runtime-linkage) below. Every key is optional; omitting one takes the target's
default.

If the paths are personal (your own sysroot location), put the target in **`kama.local.json`**
instead — same shape, gitignored, overrides the committed manifest.

### `csources` — your own C, C++ and Objective-C, compiled by `kama build`

A project (or a package) can hand the build its own sources:

```json
{
  "csources":  ["csrc/adder.c", "csrc/ui.cpp", "csrc/window.m"],
  "cxxflags":  ["-fno-exceptions"],
  "objcflags": ["-fobjc-arc"]
}
```

Each entry is one translation unit, linked into the same artifact — no out-of-band Makefile. **Its
language is its extension**, the way cgo, CMake and the cc crate read it:

| extension | language | standard | flags it gets |
|---|---|---|---|
| `.c` | C | `-std=gnu11` | `cflags` |
| `.cpp`, `.cc`, `.cxx` | C++ | `-std=gnu++17` | `cflags`, then `cxxflags` |
| `.m` | Objective-C | `-std=gnu11` | `cflags`, then `objcflags` |
| `.mm` | Objective-C++ | `-std=gnu++17` | `cflags`, then `cxxflags`, then `objcflags` |

kama's own generated C stays ISO `c11`. A `.c` entry is somebody else's C, and the C the world writes is
GNU C (emscripten's `EM_ASM`, which libsodium's entropy source uses, refuses to compile in a `-std=c*`
mode). C++ is pinned to `gnu++17` — what clang and GCC 11+ already default to — so a compiler upgrade
cannot change your build; say `"cxxflags": ["-std=c++20"]` to move it, and since `cxxflags` come after
the pinned standard, yours wins. Each entry compiles as its own command, so its standard never reaches
another translation unit.

`cxxflags` and `objcflags` sit on the project and in a `select.TARGET` arm, and a dependency's reach
its consumer, exactly like `cflags` — with one difference that is the reason they exist: `cflags` reach
**every** translation unit, kama's own C included, while `cxxflags` reach only the C++ entries and
`objcflags` only the Objective-C ones. The C-only warning promotions kama adds for its own C
(`-Werror=incompatible-pointer-types` and friends) stay off C++.

**The C++ driver brings the C++ runtime.** A build holding a C++ or Objective-C++ entry compiles those
entries, and **links**, with the C++ spelling of your C compiler — `clang` → `clang++`, `gcc` → `g++`,
`cc` → `c++`, `zig cc` → `zig c++`, `emcc` → `em++`, keeping any prefix or version suffix
(`aarch64-linux-gnu-gcc` → `aarch64-linux-gnu-g++`, `clang-17` → `clang++-17`). That driver links the
right runtime on every target (`libc++` on macOS, `libstdc++` on Linux, zig's bundled libc++, emscripten's),
so there is no `-lc++`/`-lstdc++` to name per target. (On wasm the link stays with `emcc`, which links
C++ objects and their runtime itself.) A build **without** a C++ entry never uses it, so
a program that shares a package tree with a C++ UI does not link a C++ runtime. When your C compiler has
no C++ spelling kama knows, name one: `--cxx <compiler>` beside `--cc`, or `"cxx"` beside a target's
`"cc"`; otherwise the build is refused by name.

Paths are **relative to the manifest that declares them**, and each entry's directory goes on the
include path, so a header sitting beside the source is found both from that source and from the kama
file that `extern "adder.h";`s it. A C++ header shared with kama wants the usual `extern "C"` guard.

A dependency's `csources` are compiled too, which is what lets a package that wraps a C library ship
the shim that binds it. Two packages that both ship `csrc/shim.c` are fine — the object is named after
the package that owns the source — and so is `foo.c` beside `foo.cpp`.

Two smaller rules, both refusals with a message: an **absolute** path (a manifest must stay
relocatable, and a published package cannot name a directory nobody else has) and a path escaping the
project with `..`. A path that simply is not there is named by kama, not by the C compiler.

`OUTPUT=OBJECT` builds one translation unit by definition, so it refuses a build that has a `csources`
entry beside the program — `OUTPUT=STATIC` is the shape that takes several.

### A source for some targets only — `compileFor` on an entry

A `csources` or `cincludes` entry can be an object carrying a gate:

```json
{
  "csources": [
    "src/ui.cpp",
    { "path": "src/platform_mac.m", "compileFor": ["OS_MACOS"] },
    { "path": "src/audio_native.c", "compileFor": ["!ARCH_WASM32"] }
  ]
}
```

`compileFor` holds exactly the literals `@compileFor(...)` takes — flag names and `!FLAG`, all of which
must hold — and is judged by the same rule, over the same flags: the derived `ARCH_`/`OS_`/`ABI_`/`HOSTED`
facts, `DEBUG`/`RELEASE`, a declared `select` value, a `flags` entry. So gate on the fact, never on how
the build was spelled: `OS_MACOS` holds for a `HOST` build on a Mac, for `--target MACOS` and for a bare
`aarch64-macos-none`, while a `select.TARGET.MACOS` arm would match only the second. That is also why a
**dependency** can ship a macOS-only source: it cannot know what its consumer calls the target, but it
can state the fact, and its gate is judged against the build it compiles into.

The refusals are the `@compileFor` ones — a name that is neither a build-configuration fact nor declared,
and a gate no configuration can activate (`["DEBUG", "RELEASE"]`) — plus one of the manifest's own: an
entry whose gate leaves it **out** of this build must still exist, the way a gated-out kama file is
still parsed. A path that is wrong only on the platform nobody builds daily is exactly what a gate would
otherwise hide.

### `cincludes` — a package's include tree

A header beside its `.c` is found on its own (above). A library whose headers live in their own tree —
which is most of them — names the tree:

```json
{
  "csources":  ["third_party/foo/src/foo.c"],
  "cincludes": ["third_party/foo/include"]
}
```

Each entry is a directory, **relative to the manifest that declares it**, put on the include path for
every translation unit of the build — the package's own C and the kama file that `extern "foo/foo.h";`s
it alike. A dependency's `cincludes` reach its consumer's build the way its `csources` do, which is what
lets a package vendor a C library whole. The same three refusals as `csources` (absolute, escaping with
`..`, not there — a missing directory is named by kama, with the package that declared it, rather than
surfacing as a missing header three steps later). This is the structured route for what a raw `-I` in a
dependency's `cflags` cannot say, and why that is refused below.

### `jsLibraries` and `emSettings` — the emscripten pair

```json
{
  "jsLibraries": ["js/audio_glue.js"],
  "emSettings": {
    "EXPORTED_RUNTIME_METHODS": ["ccall", "cwrap"],
    "STACK_SIZE": "4MB",
    "ALLOW_MEMORY_GROWTH": true
  }
}
```

**`jsLibraries`** is a plain list of `--js-library` files, resolved against the manifest that declares
them. emcc accumulates them, so there is nothing to resolve between a project and its dependencies —
a package that binds a browser API ships the glue that binds it.

**`emSettings`** is an object, and the **shape of each value decides what it means**:

| value | kind | how it merges |
|---|---|---|
| an array of strings | **list** | **unions** — kama's values, then dependencies', then yours |
| a string, number or boolean | **scalar** | last-wins, and **you win** over kama and over every dependency |

That is the point of the key. emcc is last-wins on a repeated `-s`, and the settings kama emits for you
(`EXIT_RUNTIME`, and `EXPORTED_RUNTIME_METHODS` when your program uses `std::net::web`, and the pthread
settings when it uses isolates) reached the command line *after* anything you smuggled in through
`cflags` — so the only way to say this was also the way to lose. Now
`"EXPORTED_RUNTIME_METHODS": ["ccall"]` gives you `ccall` **and** the two names the stdlib's own glue
needs.

Two dependencies setting the same scalar to different values is **refused, naming both** — and stating
that setting in your own `emSettings` settles it, because you merge last.

Both keys are **inert on a non-wasm target**, not an error: one manifest builds every target, and
`subsystem` set that precedent. The *shape* is still validated everywhere, so a typo is caught by
whoever wrote it rather than by whoever ships to the web.

⚠️ You can override `EXIT_RUNTIME`, and it is load-bearing: without it an emscripten program returns
from `main` and leaves node to wind the runtime down, which has been observed to deadlock against V8's
own background threads. Turn it off only if you know why you need to.

### What a dependency contributes

A dependency's `cflags`, `ldflags` and `link` apply to your build. A package that needs `-lm`, or a
`-D` its own C expects, says so **once, in its own manifest**, and every consumer gets it — you do not
repeat the block, and it cannot drift between your copy and theirs.

The order is fixed: **dependencies first** (alphabetically by import name), **then your project**, then
the command line. So you always have the last word wherever the C compiler resolves a repeated flag
last-wins. `link` is deduped, because it is a list of library *names* and two packages both wanting `m`
is the ordinary case; `cflags`/`ldflags` are not, because they are raw text where a repeat can be
load-bearing.

Each manifest resolves on its own before the lists are joined. So a dependency's
`select.TARGET.<NAME>.link`, which **replaces**, replaces *that package's* list and never yours —
otherwise adding a dependency could delete your `-lm`.

What a dependency **cannot** do is redefine your build:

| it contributes | it does not |
|---|---|
| `cflags`, `cxxflags`, `objcflags`, `ldflags`, `link`, `csources`, `cincludes`, `jsLibraries`, `emSettings` | `cc`, `cxx`, `ar`, `sysroot`, `runtime`, `subsystem`, `triple` — your toolchain, your call |
| `reproducible-float` — it can only turn contraction off, and its own arithmetic is what you compile | `no-heap` — it changes what compiles, program-wide |
| | `webgpu` — it selects an SDK, and could make your build demand a download |

Two more rules worth knowing. A dependency's **relative** `-I`/`-L` is **refused**, because it would
resolve against *your* working directory rather than against the package — a package's own include tree
is `cincludes`, which kama resolves against the declaring manifest; a machine path stays absolute. And a
dependency whose manifest does not parse stops the build, naming that file: silently skipping it would
silently drop whatever it was contributing. (`kama query` and `kama lsp` stay lenient — an editor sits
above trees you do not own.)

⚠️ Settings are read from the materialized `.kama/deps` view, which is what `kama pkg install` builds
and what the import path already uses. A stale view is a stale build in exactly the same way it is a
stale import; re-run `kama pkg install`.

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

## Runtime linkage

A separate axis from the output kind: **how the language's own runtime is linked into whatever you
produce.** The rule is *link non-system runtime statically, system components dynamically* — so a
binary depends only on things the target OS already has.

**Static is the default, and you opt *in* to a DLL:**

```sh
kama build app.kama                        # runtime linked statically
kama build app.kama --dynamic-runtime      # opt in to the runtime DLL
```

or per-project, since linkage is a toolchain property like `cc` and `sysroot`:

```json
"select": { "TARGET": { "WINDOWS": { "runtime": "dynamic" } } }
```

`--dynamic-runtime` wins over the manifest, the same way `--target` beats a declared `"default": true`.

**In practice this only bites on Windows.** On Linux and macOS libc *is* the system — glibc and
libSystem ship with the OS and pthreads live inside libc — so there is no non-system runtime to make a
choice about, and the flag is an accepted no-op there (one build script, no branching). Going further
would be actively wrong: static glibc breaks `dlopen`/NSS, and Apple does not support a static
libSystem at all.

On Windows it matters, because mingw-w64's winpthread ships with your *compiler*, not with Windows.
A program using `isolate` / `parfor` / `channel` used to bind `libwinpthread-1.dll` out of the msys2
tree and die at process start with `STATUS_DLL_NOT_FOUND` (`0xC0000135`) on any machine without it —
including the machine that built it, unless an msys2 shell launched it.

**The C++ runtime follows the same rule**, and it reaches a program through a C++/Objective-C++
`csources` entry. mingw's `libstdc++-6.dll` and `libgcc_s_seh-1.dll` ship with your *compiler*, so a
Windows C++ build used to die the same way (measured `0.9.383`: `tests/csources_cxx.d` exited
`0xC0000135` from a plain PowerShell on the machine that built it). A Windows link that holds C++ now
ends with `-static-libgcc -Wl,-Bstatic`, so the driver's own runtime tail binds statically. ⚠️ Note
what does NOT work, because it is the obvious thing to try: `-static-libstdc++` alone makes libstdc++
static, and mingw's static libstdc++ then needs winpthread — which the C++ driver appends *after* every
argument you pass, so the binary swaps one msys2 DLL for another. The flags are trailing, so libraries
you named are already bound and keep their linkage.

This is the same stance Go, Rust and Zig take: static runtime on Windows, dynamic libc elsewhere.
Libraries **you** name (`--link`, `--webgpu`) are untouched and remain your choice to ship or link —
kama makes the decision only about its own runtime.

Check what you actually produced:

```sh
objdump -p app.exe | grep 'DLL Name'    # want only KERNEL32 + api-ms-win-crt-*
```

## Subsystem

Another Windows-only axis, and the same shape as runtime linkage: **what kind of application the PE
declares itself to be.** Windows gives a *console-subsystem* program a console window whether it wants
one or not — so a GUI program built the default way opens two windows, the console Windows created for
it and then the one it actually asked for.

**Console is the default, and you opt *in* to windows:**

```sh
kama build game.kama --target WINDOWS                        # console subsystem (the default)
kama build game.kama --target WINDOWS --subsystem windows    # no console window
```

…or per-project, since the subsystem is a permanent property of the artifact:

```json
"select": { "TARGET": { "WINDOWS": { "subsystem": "windows" } } }
```

`--subsystem` wins over the manifest, the same way `--dynamic-runtime` does.

**`console` is the default deliberately.** It keeps every console tool, the CI legs and `kama` itself
behaving exactly as they always have, and a GUI app is the thing that knows it is a GUI app. The
alternative default breaks `print` for everyone to fix a stray window for a few.

**`print` still works from a terminal.** The cost that normally comes with `-mwindows` is that
`print`/`eprintln` go nowhere, because a GUI-subsystem process is given no console. kama undoes the half
that matters: at startup a `windows`-subsystem binary calls `AttachConsole(ATTACH_PARENT_PROCESS)` and,
if it was launched *from* a terminal, rebinds its standard descriptors onto that console. So the same
binary is silent when double-clicked from Explorer — correct for a GUI app — and prints normally when run
from PowerShell. Launched from Explorer there is no parent console, the attach fails, and output is
discarded.

**Everywhere else it is an accepted no-op.** No other object format has a subsystem field, so one
cross-platform build script can carry the flag unconditionally — the same stance `--dynamic-runtime`
takes.

Check what you actually produced:

```sh
file app.exe        # PE32+ executable (GUI) … , for MS Windows
```

## Where the artifacts land

Inside a **project** (a directory with a `kama.json`), everything a build generates goes under one root:

```
out/<triple>/<debug|release>/
```

```sh
kama build kama.json                             # out/aarch64-macos-none/debug/myapp
kama build kama.json --release                   # out/aarch64-macos-none/release/myapp
kama build kama.json --target WASM               # out/wasm32-emscripten-none/debug/myapp.html
```

Scoped by **both** axes deliberately. They vary independently, and a collision between them is silent —
you would run yesterday's binary with no diagnostic to tell you so. Cross-compiling for three targets
leaves three trees that never overwrite each other, and `.gitignore` needs one line: `out/`.

The root is the manifest's `"out"` key, defaulting to `out`. An explicit `-o` overrides it entirely.

A **loose `.kama` file with no manifest** is unaffected — `kama build hello.kama` still writes `./hello`
beside the source. `out/` is a project's concept, and one file is not a project.

## Platform notes

- **macOS → anything**: fine with zig or a sysroot.
- **anything → macOS**: needs the macOS SDK, which Apple's licence keeps on Apple hardware. Practical
  answer: build macOS artifacts on a Mac (or a Mac CI runner).
- **→ Windows**: the runtime links statically, so the `.exe` is distributable as-is — see
  [Runtime linkage](#runtime-linkage). `WINDOWS` targets the **mingw-w64** ABI (`-gnu`), which zig covers. An MSVC-ABI build
  (`x86_64-windows-msvc`) needs the MSVC headers and libraries, so build it on Windows.
- **→ wasm**: uses Emscripten (`emcc`), not zig — it emits an `.html` + `.js` + `.wasm` harness, and
  `$EMCC` or `--cc` overrides which `emcc`. Builds pass **`-msimd128`** (both debug and release, so the
  two tiers share one instruction set), which is what makes `std::math` vectorize here as it does on
  native — without it a `.wasm` holds no vector instruction at all. That sets the **runtime baseline**:
  wasm SIMD needs **node ≥ 16** or any current browser. It is the one place kama does not target the
  bare generic baseline, and it is deliberate: every engine that can run a 2026 `.wasm` has it.
- **→ bare metal**: for the host arch, nothing extra. For a real board, name its triple and give it a
  `cc` (`arm-none-eabi-gcc`, or `zig cc`). See [mcu.md](mcu.md) for the full firmware flow.

## Troubleshooting

**"cannot build for X — the default C compiler has no libc for it"**
kama will not guess a toolchain that cannot work. Pick a route from *Getting a cross toolchain* above,
or `kama transpile` and build the C elsewhere.

**On Windows the link fails with `ld: cannot find …\???\<name>-xxxxxx.o`, and every path I passed is ASCII.**
Your `%TEMP%` contains non-ASCII characters — which is what a Windows account name like `Björn` or
`日本語` gives you. It is the toolchain, not kama: on a single-invocation build clang compiles to a
temporary object under `%TEMP%` and hands *that* to GNU `ld`, which is a narrow program and cannot open
a non-ASCII path in either direction. Nothing on the command line has to be non-ASCII for this to fire.
Three ways out, any one of which is enough: add `-fuse-ld=lld` (LLVM's linker reads UTF-16 and has
neither limit), point `TEMP`/`TMP` at an ASCII directory, or let kama's per-translation-unit path do
the link — `-j 2` or higher never asks clang for a temporary, so only a one-TU build (or `-j 1`) is
exposed. The same narrowness in `ld`/`ar` is why kama hands them 8.3 aliases for its own paths; see
[platforms/windows.md](platforms/windows.md) for the measurements.

**My Windows `.exe` dies immediately with `0xC0000135` / "the code execution cannot proceed".**
That is `STATUS_DLL_NOT_FOUND` at process start — a DLL the binary imports is not on the machine.
`objdump -p app.exe | grep 'DLL Name'` names it. If it is `libwinpthread-1.dll`, you built with
`--dynamic-runtime` or a `"runtime": "dynamic"` target; drop it and the runtime links in statically.
If it is a library **you** named (`--link`, `--webgpu`), ship that DLL beside the `.exe` — kama does
not decide linkage for your dependencies. See [Runtime linkage](#runtime-linkage).

**I want to tune for a specific CPU.**
kama passes no `-march`/`-mcpu`/`-mtune`, so builds target the architecture's generic baseline — which
is what makes them portable. To override, put it in the target's `cflags`:
`"RPI": { "triple": "aarch64-linux-gnu", "cflags": ["-mcpu=cortex-a72"] }`. There is deliberately no
`--march=native` shorthand yet; it is a recorded follow-on (ROADMAP_DETAIL §10).

**A `@compileFor` gate isn't firing.**
Gate on the **derived** facts, not on how you spelled the build. `--target EMBEDDED` and
`--target xtensa-none-elf` both give you `OS_NONE`; neither gives you a flag called `EMBEDDED`, because
a built-in target name describes the invocation rather than the target. The flags a target contributes
are `ARCH_<arch>`, `OS_<os>`, `ABI_<abi>`, and `HOSTED` when the target has an OS.

**Two values of one group.**
`--select CONSOLE=PS5 --select CONSOLE=XBOX` is an error: a `select` group is single-select by
definition. Use `--define` / `flags` for things that combine.
