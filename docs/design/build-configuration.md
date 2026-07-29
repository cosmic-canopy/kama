# Build configuration — selection groups, target triples, cross-compilation

**Status: ✅ SHIPPED 2026-07-29** (`fdc9a75`, `d8e8950`, `69b95fd`, `45756a1`, `0c6cd7c` on dev). Design of record for the build-configuration campaign.
Supersedes the flag-source decisions in [conditional-compilation.md](conditional-compilation.md)
(open-Q4 in particular, which leaned "`--define` for v1, a dedicated axis later" — this is that
axis). Sequenced **before** [LSP M6](lsp-m6-kickoff.md), which needs a flag model to point the editor
at.

## Why

`@compileFor(FLAG)` shipped with a half-formed notion of where flags come from:

- **`--target native|wasm|embedded` is a *backend* axis** wearing the name Rust/Clang/Zig use for the
  *platform triple*. The platform axis does not exist. `--define WINDOWS` is the convention — but it
  is unvalidated, and nothing stops you defining `WINDOWS` and `LINUX` simultaneously.
- **`kama.json` can only declare user flags.** Built-in names are explicitly *rejected* inside
  `"flags"` (kama.driver.cpp:4179-4185), so a project that only ever builds for one target must
  retype `--target …` on every invocation — and tooling (the LSP) cannot know what the project builds.
- **Output kind is smeared** across a `--shared` boolean and `--target embedded` implying object-only.
  Static libraries (`.a`) do not exist at all.
- **The driver picks link flags from `#ifdef` on the host**, at kama-compile time — correct only
  because host has always equalled target. This is the one hard blocker to cross-compilation.

**Goal (user, 2026-07-29): configurable, expandable builds with sane defaults for the standard
targets — mac, Windows, Linux, web (wasm) — plus real cross-compilation.**

**Why now:** all 16 `@compileFor` uses in the tree are in five `tests/compilefor_*` fixtures; zero in
`lib/std`, `prelude`, `examples`, `bench`. Migration cost is ~zero today and permanent after 1.0.

## The primitive — one named selection group

A **selection group** is single-select: pick exactly one value, and its name joins the active flag
set. `@compileFor(XBOX)` therefore needs no language change — `compileForActive` is already
set-membership, and `pruneInactiveDecls` is untouched.

There is deliberately **no user-declared MULTI group**: `flags` already *is* the one multi-select bag,
and a second grouping mechanism would buy only presentation. Every group shares one manifest shape —
`{ "<VALUE>": { … } }`, the same as `flags` — rather than inventing a second spelling.

This is the MSBuild Configuration×Platform model and Gradle's flavor dimensions, generalized. Related
prior art: Cargo user-definable profiles (`[profile.x] inherits`) and features; Zig's typed build
options (`b.option(enum, …)`); CMake cache vars with a `STRINGS` property (which cmake-gui renders as
a dropdown).

### Built-in groups

| Group | Mode | Values | Drives |
|---|---|---|---|
| `TARGET` | single | `HOST` (default), `MACOS`, `WINDOWS`, `LINUX`, `WASM`, `EMBEDDED` — a catalog of triples | the C toolchain + every platform-derived flag |
| `BUILD_TYPE` | single | `DEBUG` (default), `RELEASE` | optimization, stripping, `debugAssert`, log-level baking |
| `OUTPUT` | single | `EXE` (default), `SHARED`, `STATIC`, `OBJECT` | link mode and artifact kind |
| `flags` | multi | user-declared | nothing but `@compileFor` |

All three are **user-extendible**. `flags` is the existing multi-select bag, unchanged.

### `NATIVE` and `EMBEDDED` stop being modes

Both were special cases standing in for facts a triple already carries. `NATIVE` is gone outright;
`EMBEDDED` survives only as a catalog NAME (an `os=none` shortcut), never as a gate:

- **`EMBEDDED` → `os=none`.** Freestanding (`-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED`), no
  entry point, weak panic hook, `OUTPUT` defaulting to `OBJECT` — all keyed on the derived `OS_NONE`
  flag, so *any* bare-metal triple gets it (`xtensa-none-elf`, `riscv32-none-elf`,
  `thumbv7em-none-eabihf`), not one blessed value.
- **`WASM`'s driver behavior** (emcc, `.html`+`.js`+`.wasm` harness, `-Oz`) keys on `ARCH_WASM32`
  (`wasm32-emscripten-none`). `WASM` becomes an ordinary catalog entry, not a mode.
- **`NATIVE` → `HOST`**, which resolves to a *real* triple. So `OS_MACOS` / `ARCH_AARCH64` exist for
  plain native builds — capability kama does not have today.

## Target triples

A `TARGET` value is a **name that declares a triple**. Selecting it inserts the name **plus derived
component flags**:

```
--target RPI          where RPI = aarch64-linux-gnu (declared by the project)
  → { RPI, ARCH_AARCH64, OS_LINUX, ABI_GNU, HOSTED }

--target EMBEDDED     a built-in shortcut for <host-arch>-none-none
  → { ARCH_AARCH64, OS_NONE, ABI_NONE }      note: no `EMBEDDED` flag — see below
```

**Form: Zig's 3-part `arch-os-abi`**, not GNU's 4-part `arch-vendor-os-abi`. `vendor` is vestigial
(`unknown`/`pc`) and drops out cleanly; clang accepts and normalizes both, so nothing is lost on the
way to `cc`.

| Part | Examples | Derived flag |
|---|---|---|
| arch | `x86_64`, `aarch64`, `riscv64`, `wasm32`, `thumbv7em`, `xtensa` | `ARCH_<UPPER>` |
| os | `linux`, `windows`, `macos`, `wasi`, `none` | `OS_<UPPER>` |
| abi | `gnu`, `musl`, `msvc`, `eabihf`, `emscripten` | `ABI_<UPPER>` |

A **built-in catalog name is NOT a flag.** `MACOS`/`EMBEDDED` are shortcuts for a triple family, so
gating on one would gate on how the build was *spelled*: `@compileFor(EMBEDDED)` would silently stop
applying the moment a real board triple (`xtensa-none-elf`) replaced the shortcut. `@compileFor(OS_NONE)`
is the fact, and holds for both. A **user-declared** target name *is* a fact about a target the project
defined, so it does become a flag — that is the `@compileFor(RPI)` granularity.

Plus exactly **one synthesized flag: `HOSTED`** (any `os != none`). "Do I have an OS and a libc" is
the most common gate in a systems language and `!OS_NONE` reads badly. Everything else is a literal
triple component.

Prefixes (`OS_`/`ARCH_`/`ABI_`) keep derived flags from colliding with user flag names and make their
origin obvious at the use site. This is Rust's `cfg(target_os)`/`cfg(target_arch)`/`cfg(target_env)`
expressed in kama's flat-boolean vocabulary, with **no new language mechanism**.

Three useful granularities fall out of one declaration: `@compileFor(OS_LINUX)` for POSIX-ish code,
`@compileFor(ARCH_AARCH64)` for NEON, `@compileFor(RPI)` for board-specific.

**Arch ambiguity:** a named shortcut like `MACOS` does not pin an arch. Rule: **named shortcuts
resolve arch to the host's; a precise build spells the triple** (`--target aarch64-macos-none`).
Names are conveniences, triples are exact.

## Manifest surface

```json
{
  "select": {
    "TARGET":     { "RPI":   { "triple": "aarch64-linux-gnu",
                               "cc": "aarch64-linux-gnu-gcc", "ar": "aarch64-linux-gnu-ar",
                               "sysroot": "/opt/rpi-sysroot", "cflags": [], "ldflags": [] },
                    "ESP32": { "triple": "xtensa-none-elf" } },
    "BUILD_TYPE": { "FAST":  { "inherits": "RELEASE" } },
    "CONSOLE":    { "XBOX": { "default": true }, "PS5": {}, "SWITCH": {} }
  },
  "flags": { "TELEMETRY": { "default": true }, "PROFILING": {} }
}
```

- **`BUILD_TYPE` inheritance carries flag membership *and* the base's driver behavior.** `FAST` puts
  `{FAST, RELEASE}` in the active set and inherits `-O3`/`-DNDEBUG`/`debugAssert`-stripping verbatim.
- **Mutual exclusion means one *selected* value per group.** Inherited ancestors and derived triple
  flags are implied members, not competing selections.
- **`TARGET` values carry toolchain settings; `BUILD_TYPE` values do not** (v1). A target genuinely
  *is* a toolchain; a build-type override would turn `kama.json` into a build-settings language.
  "RELEASE plus my flags" is already useful. Per-value build settings are a recorded follow-on.
- `kama.local.json` overrides **defaults only**, extending the existing `flags` deep-merge rule.

**Precedence: CLI > `kama.local.json` > `kama.json` > built-in default.**

### No manifest present

Bare builds stay zero-config — an existing guarantee (no manifest ⇒ permissive):

- Built-in groups and derived flags need no manifest.
- `--target` / `--release` / `--debug` / `--shared` remain CLI sugar for `--select`.
- `--define` stays permissive without a manifest, strict with one (unchanged).
- **Anonymous targets:** a `--target` value that is not a known name but *parses as a triple* is a
  one-off anonymous target — the `zig cc -target` gesture, no config file needed.

## Deliberately not doing

**Value comparisons** like `@compileFor(DRAW_LEVEL > 4)`. `@compileFor` is *tagging, not logic* by
design (conditional-compilation.md "Boolean logic — keep it tagging"), and kama already has the
better answer for genuine values: `comptime` constants and `comptime fn` bake compile-time values
with real arithmetic — Zig's `@import("build_options")` model. Ordered tags
(`DRAW_LEVEL_LOW|MED|HIGH`) cover the tagging case with nothing new.

## Cross-compilation

kama **emits ISO C11 and shells out**, so cross-compilation is "invoke the right C compiler", not
"write a backend". Two shipped paths already prove the pattern: wasm delegates to emcc (a real cross
toolchain) and bare-metal stops at `-c` for the board's linker. `--cc` already exists.
`kama_runtime.h` / `kama_os.h` platform branches are `_WIN32`-style macros **in C**, so the cross
compiler's own predefined macros select them correctly with no work.

**Tier 0 (works today):** `kama transpile` emits portable C for anyone's toolchain; `OUTPUT=OBJECT`
stops at an object.

**Your existing toolchain is a first-class path — zig is a convenience, not a requirement.** kama
recognizes two shapes of C compiler and treats them oppositely when crossing:

| Shape | Examples | Treatment |
|---|---|---|
| **multi-target driver** — one binary for every target, triple as a flag | `clang`, `zig cc` | gets `-target <triple>` |
| **per-target binary** — triple baked into the name | `aarch64-linux-gnu-gcc`, a vendor ARM gcc, an NDK wrapper | left alone |

clang has always been cross-capable; what it needs from you is the target's **headers and libraries**
(a sysroot). That is the *only* thing zig adds — it bundles musl / glibc stubs / mingw-w64 / wasi-libc,
so no sysroot hunt. If you already have a sysroot or a cross toolchain, use it and skip zig entirely.

**Tier 1 — target specs.** A `TARGET` value carries `cc` / `ar` / `sysroot` / `cflags` / `ldflags`,
declared once in `kama.json` and checked in so the team shares it. Rust's target-spec model. The
`triple` field feeds all three consumers: derived flags, the toolchain invocation, and the link-flag
lookups below.

**Tier 2 — bundled `zig cc` (opt-in).** `zig cc` is a drop-in clang that cross-compiles to
essentially every target with zero setup, because Zig ships musl / glibc stubs / mingw-w64 /
wasi-libc sources and headers. One binary makes `--target WINDOWS` work from a Mac. Wired as a
toolchain provider behind the same target-spec seam, installed through the existing `kama toolchain`
store. **Opt-in:** an unconfigured target errors with a "run `kama toolchain install-cc zig`" hint,
never a silent download.

**Policy while a target has no toolchain:** `check`, `query`, `lsp` and `transpile` accept any
selection (editor browsing, and emitting C for someone else's toolchain). `build`/`run` refuse with
an error naming both the missing toolchain and `transpile` as the escape hatch.

### The blocker: host `#ifdef` → target lookup

The driver picks compile/link flags from `#ifdef` on the **host at kama-compile time**: `-lws2_32` is
added only if the *kama binary itself* was built on Windows (kama.driver.cpp:4696);
`-Wl,-dead_strip` vs `--gc-sections` comes from `__APPLE__` (:4571); ~12 sites across the build path.
Each becomes a lookup on the selected target's derived flags — `OS_WINDOWS` ⇒ `-lws2_32`, `OS_MACOS`
⇒ `-Wl,-dead_strip`, else `-Wl,--gc-sections`. This is latent-bug removal independent of any cross
feature, and it is the prerequisite for all of Tier 1/2.

## `OUTPUT` and library artifacts

| Value | Behavior |
|---|---|
| `EXE` | today's default |
| `SHARED` | today's `--shared`: `-fPIC -shared -fvisibility=hidden`; `expose fn` → bare symbol + `KAMA_EXPORT` |
| `STATIC` | **new** — compile to objects, archive with `ar rcs` |
| `OBJECT` | **new as an explicit choice** — what `--target embedded` implies today, now sayable for any target |

`OUTPUT` defaults to `OBJECT` when `OS_NONE`, rather than being hardcoded to the retired `EMBEDDED`
value. The wasm restriction is lifted where it is real: a wasm `SHARED`/`STATIC` build is a
legitimate side-module / reactor output rather than the `.html`+`.js` harness. Combinations that
genuinely cannot work (e.g. `EXE` with `OS_NONE`) fail with a message naming both groups, not a
linker error.

## Verification

- Migrate the five `tests/compilefor_*` fixtures off `NATIVE`/`EMBEDDED` onto derived flags —
  `@compileFor(HOSTED)` / `@compileFor(ARCH_WASM32)`, which exercises the derived-flag path.
- Migrate `tools/check-embedded.sh` + `tests/embedded_blink` onto an `os=none` triple. **This is the
  proof that the derived-flag path reproduces the old behavior exactly.**
- Extend `tools/check-compilefor.sh`: a manifest-declared default taking effect, `--select`
  overriding it, a same-group conflict rejected, an unknown axis/value rejected, a bare triple
  accepted with no manifest, a `kama.local.json` default override.
- New fixtures: `OUTPUT=STATIC` (archive exists, symbol present via `nm`) and `OUTPUT=OBJECT` on a
  *hosted* target (proving object output is no longer welded to bare metal).
- Cross: transpile-grep that a foreign target selects the right decls; a Tier-1 fixture with a stub
  `cc` script asserting the spec's flags reach the command line; a real `zig cc` build if Tier 2
  lands (skipped when no zig is present).
- `tools/lspref.sh` before/after → emission byte-identical.
- **Run the full matrix at every checkpoint** — `tools/cdev test`, host `run_tests.sh`, `KAMA_SAN=1`,
  `KAMA_WASM=1`. This campaign is the only one that touches the build system, and a flag-model
  regression is invisible in a single leg.

## Follow-ons (recorded, not built)

- Per-value `BUILD_TYPE` settings (own opt-level / LTO / strip).
- Numeric build options surfaced as comptime constants — the non-tagging half of the value question.
- A full Rust-style target-spec JSON file, if named manifest entries prove too coarse.
