# WebGPU triangle — in Kama

A WebGPU example written in **Kama**, compiled to WASM and run in the browser. It brings the whole
pipeline up (instance → adapter → device → queue → surface) and draws a triangle over an animated
background — the graphics "hello triangle", and the first step of the engine track
([docs/ENGINE_READINESS.md](../../docs/ENGINE_READINESS.md)).

It is **pure Kama over the WebGPU C API** (Emscripten's `emdawnwebgpu` port): no C glue. Opaque handles
are `UnsafePtr`, the C descriptor structs are `type extern value` (the header owns the real layout — Kama assigns
fields *by name*, so declaring only the fields you touch is safe), and the async adapter/device handshake
rides Kama `fnptr` callbacks with app state threaded through the WebGPU `userdata` pointer.

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

## Notes / honest limitations surfaced by this example

Two Kama ergonomic gaps showed up (both have clean workarounds here; tracked in
[docs/ROADMAP.md](../../docs/ROADMAP.md)):

1. **No aggregate initializer for `type extern value`.** A field-wise call like `WGPUColor(r: 1.0, …)`
   silently zero-inits instead of setting fields, so the code uses `T x = T(); x.field = …;`. Verbose for
   WebGPU's many-field descriptors.
2. **No mutable globals yet.** App state lives on the heap (`calloc`) and is passed through the WebGPU
   `userdata` pointer into the callbacks rather than a module-level variable.

Also: binding a callback-based C API means handing a Kama `fnptr` to a C callback field, which the C
compiler flags as an incompatible-function-pointer type (Kama lowers enums to `int` and handles to `void*`).
The compiler now demotes that to a warning at the `extern` boundary (the sanctioned unsafe seam), so it
links — you'll see the warning at build time.

## Native (Mac / Linux / Windows)

The **same** `triangle.kama` also runs as a native desktop app. The web-vs-native differences —
surface creation, present, and the per-tick event pump — live behind the **`std::gpu` seam**
([`lib/std/gpu/kama_gpu.h`](../../lib/std/gpu/kama_gpu.h)), so the Kama source is identical across
targets. Native uses **wgpu-native** (the WebGPU implementation) + **GLFW** (window/surface).

```sh
# 1. fetch the wgpu-native SDK (once) -> third_party/wgpu/ (gitignored; not vendored)
tools/fetch-webgpu.sh
# 2. install GLFW (once):  macOS: brew install glfw   |   Linux: apt install libglfw3-dev
# 3. build + run
examples/webgpu/build-native.sh                # -> out/triangle
out/triangle                                    # a 512×512 window: the same spinning triangle
```

`--webgpu` on a native build links the fetched wgpu-native lib (via an rpath) + GLFW; `$KAMA_WGPU_DIR`
overrides the SDK location. wgpu-native is MPL-2.0 (file-level copyleft) — we link the **unmodified
prebuilt**, so kama stays MIT.

> The hardcoded WebGPU enum constants (formats, sTypes, load/store ops) target the upstream
> `webgpu.h` both backends track; if a native run renders wrong or aborts, reconcile those few
> constants against `third_party/wgpu/include/webgpu/webgpu.h`.

## Scope

Browser/WASM (`--target wasm --webgpu`, the `emdawnwebgpu` port) **and** native desktop (above). The
next steps are an idiomatic safe `gpu::` wrapper and the engine spine (buffers/bindings/pipelines
beyond a hardcoded triangle) — see [docs/ENGINE_READINESS.md](../../docs/ENGINE_READINESS.md).
