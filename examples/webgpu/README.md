# WebGPU triangle — in Kama

A WebGPU example written in **Kama**, compiled to WASM and run in the browser. It brings the whole
pipeline up (instance → adapter → device → queue → surface) and draws a triangle over an animated
background — the graphics "hello triangle", and the first step of the engine track
([docs/ENGINE_READINESS.md](../../docs/ENGINE_READINESS.md)).

It is **pure Kama over the WebGPU C API** (Emscripten's `emdawnwebgpu` port on the web, wgpu-native on the
desktop). Opaque handles are `UnsafePtr`, the C descriptor structs are `type extern value` (the header owns the real
layout — Kama assigns fields *by name*, so declaring only the fields you touch is safe), and the async adapter/device
handshake rides Kama `fnptr` callbacks with app state threaded through the WebGPU `userdata` pointer.

It is also an ordinary **kama project** ([`kama.json`](kama.json)): `"webgpu": true` says it uses WebGPU, and the
few lines of C that differ between a browser and a desktop window — its platform layer — are its own `csources`,
gated by fact. That is the shape to copy for a project of your own: nothing in the compiler knows about this
example.

## Build & run

```sh
# 1. build (inside the toolchain container) -> out/triangle.{html,js,wasm}
tools/cdev exec examples/webgpu/build.sh

# 2. serve over localhost (WebGPU needs a secure context; localhost counts) and open it
examples/webgpu/serve.sh                       # then browse http://localhost:8000/triangle.html
#   ... or: cd out && python3 -m http.server 8000
```

Open `http://localhost:8000/triangle.html` in a **WebGPU-capable browser** (Chrome/Edge 113+, or Safari
Technology Preview). You should see an orange triangle on a background whose blue channel pulses each frame.

The default Emscripten HTML shell already contains `<canvas id="canvas">`, which is the selector the surface
is created against.

## How it maps to WebGPU

| Piece | Kama |
|---|---|
| Opaque handles (`WGPUDevice`, `WGPUSurface`, …) | `UnsafePtr` |
| C structs (`WGPURenderPassDescriptor`, …) | `type extern value` with only the touched fields; header owns layout |
| `&descriptor` out/in pointers | `addr(of: x)` |
| WGSL source / selectors → `WGPUStringView` | `s.cstr()` + `s.length()` |
| async `RequestAdapter`/`RequestDevice` callbacks | Kama `fnptr`; state via the `userdata` `UnsafePtr` |
| the frame loop (browser-driven) | [`std::app`](../../lib/std/app/app.kama)'s `run(tick, state)` |

## Notes on binding a C API from kama

Binding a callback-based C API means handing a kama `fnptr` to a C callback field, which the C compiler
flags as an incompatible-function-pointer type (kama lowers enums to `int` and handles to `void*`). The
compiler demotes that to a warning at the `extern` boundary (the sanctioned unsafe seam), so it links —
you'll see the warning at build time.

> ⚠️ **This section used to list three limitations that no longer exist**, and they were load-bearing for
> how a reader structures a renderer — one project reported nearly building an asset-embedding step to
> work around the string one. All three are gone, and each has a fixture:
>
> - **Aggregate init for `type extern value` works** — `WGPUColor(r: 1.0, …)` sets fields rather than
>   silently zero-initing (`tests/extern_value_init.kama`, [SPEC.md](../../docs/SPEC.md) *extern types*).
>   The `T x = T(); x.field = …;` pattern below is legacy, not a workaround.
> - **Multi-line string literals work** — `triangleWgsl()` is now written as one, plus `stripIndent"…"`
>   for dedenting (`tests/tag_strip_indent.kama`).
> - **Module-level mutable globals work** — `static int32 g_frames;`. This example still threads state
>   through the WebGPU `userdata` pointer, which remains the better choice here (a module `static` is
>   per-isolate and takes value/`UnsafePtr`/`InlineArray`/`Simd` only), but it is a choice now rather
>   than a workaround.
>
> Kept as a note rather than deleted, because "the example says X is impossible" is exactly the kind of
> claim that costs someone a design cycle, and it did.

## Native (Mac / Linux / Windows)

The **same** `src/triangle.kama` also runs as a native desktop app. The web-vs-native differences — surface
creation, present, and the per-tick event pump — live in the project's platform layer,
[`csrc/gpu.h`](csrc/gpu.h): `static inline` bodies for the web, and [`csrc/gpu.c`](csrc/gpu.c) for a native
window, which macOS compiles as Objective-C through [`csrc/gpu_macos.m`](csrc/gpu_macos.m) (its surface is a
`CAMetalLayer`). So the Kama source is identical across targets. Native uses **wgpu-native** (the WebGPU
implementation) + **GLFW** (window/surface).

What each OS links is the manifest's, by fact: GLFW and the Cocoa, Metal and QuartzCore frameworks on macOS
(`ldflags` entries gated `OS_MACOS`, beside Homebrew's `-L/opt/homebrew/lib`), GLFW and X11 on Linux, GLFW and
GDI on Windows.

```sh
# 1. fetch the wgpu-native SDK (once) -> third_party/wgpu/ (gitignored; not vendored)
tools/fetch-webgpu.sh
# 2. install GLFW (once):  macOS: brew install glfw   |   Linux: apt install libglfw3-dev
# 3. build + run
examples/webgpu/build-native.sh                # -> out/triangle
out/triangle                                    # a 512×512 window: the same spinning triangle
```

`"webgpu": true` (or `--webgpu` on a loose file) is the platform API: on a native build it finds the fetched
wgpu-native SDK and links it (via an rpath), on the web it selects emscripten's WebGPU port. `$KAMA_WGPU_DIR`
overrides the SDK location. The window is the project's own business — GLFW here, linked by `kama.json`. wgpu-native is dual-licensed **MIT or Apache-2.0**, at your choice — the same
permissive terms as kama. It is fetched rather than vendored only because it is several MB; a program
that ships the library ships its license notice with it.

> The hardcoded WebGPU enum constants (formats, sTypes, load/store ops) target the upstream
> `webgpu.h` both backends track; if a native run renders wrong or aborts, reconcile those few
> constants against `third_party/wgpu/include/webgpu/webgpu.h`.

## Scope

Browser/WASM (`--target wasm`, the `emdawnwebgpu` port) **and** native desktop (above). The next steps are the
first-party packages `@kama/gpu` (a safe binding: one `resource` per handle, release on drop) and `@kama/window`
(windows and their events), after which this triangle moves onto them, and the engine spine
(buffers/bindings/pipelines beyond a hardcoded triangle) — see [docs/ENGINE_READINESS.md](../../docs/ENGINE_READINESS.md).
`tools/check-webgpu.sh` keeps the example checking for both targets on every run, and builds it where the SDK
and GLFW are installed.
