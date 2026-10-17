#ifndef GPU_H
#define GPU_H

// This project's WebGPU platform layer — the ONE place the web-vs-native surface/present/loop split lives, the
// same way kama's std::app is the one place the frame-loop split lives. A program's WebGPU code (the
// pipeline, render pass, draw) is identical across targets; only three things differ, so only three
// things live here:
//   * surface creation — the web binds an HTML <canvas> by selector; native binds a real OS window
//     (a GLFW window + its Metal layer / X11 / Wayland / HWND handle).
//   * present — the browser auto-presents each requestAnimationFrame tick; native must present.
//   * the per-tick pump — native drains window events + polls the device and reports window-close;
//     the web is host-driven and never needs it.
//
// The kama side `extern "gpu.h";`s it (kama.json puts csrc/ on the include path). On the web the bodies are
// `static inline` here (no separate translation unit); on native they live in gpu.c, which kama.json lists in
// `csources` — compiled as C on Linux and Windows, and as Objective-C on macOS through gpu_macos.m, where the
// surface is a CAMetalLayer. That is the shape any project's own platform layer takes: its C beside its kama,
// gated by fact in its manifest, and nothing in the compiler that knows about it.

#include <stdint.h>
#include <webgpu/webgpu.h>

// The seam's three functions (contract shared by both targets):
//
//   WGPUSurface gpu_surface(instance, width, height, title)
//     Create the render surface. Native ALSO creates and owns the window (width/height/title
//     honored); on the web those are ignored — the surface is the page's <canvas id="canvas"> and
//     the HTML shell owns its size. Returns the WGPUSurface (NULL on failure).
//
//   void gpu_present(surface)
//     Present the last-submitted frame. Native calls wgpuSurfacePresent; on the web a no-op
//     (emdawnwebgpu aborts on an explicit present — the browser presents the canvas each rAF tick).
//
//   int32_t gpu_pump(device)
//     Advance one tick's platform work and report whether to keep running. Native pumps the window
//     event queue + polls the device and returns 0 once the window is asked to close; on the web a
//     no-op that always returns 1 (the host drives the loop). Fits std::app's `while(tick())`.
//
//   void gpu_sleep_ms(int32_t ms)
//     Throttle a frame the loop is NOT going to present. ⚠️ This is a platform split, not a
//     convenience: a native frame loop's only throttle is vsync, and vsync applies only to a
//     PRESENTED frame — so every early return from a tick must sleep or the loop spins a core and,
//     if it also reconfigures the swapchain, allocates without bound. (That is exactly what happened
//     to the first project built on this example: 15.6 GB resident and a kernel-watchdog panic,
//     twice, just from covering the window.) On the web it MUST be a no-op: the browser owns the
//     loop through requestAnimationFrame, already throttles a hidden page, and blocking the main
//     thread is never the right answer there.
//
// Web bodies are `static inline` here (no separate TU); native bodies are declared here and defined
// in gpu.c.

#ifdef __EMSCRIPTEN__
// ---- web: self-contained, no separate TU (kama.json gates gpu.c out of a wasm build) ----

static inline WGPUSurface gpu_surface(WGPUInstance instance, uint32_t width, uint32_t height,
                                           const char* title) {
    (void)width; (void)height; (void)title;
    // Bind the page's <canvas id="canvas"> (the default emscripten HTML shell provides it), the same
    // chained selector struct the pure-Kama example used before this seam existed.
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc;
    canvasDesc.chain.next = NULL;
    canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasDesc.selector.data = "#canvas";
    canvasDesc.selector.length = 7;
    WGPUSurfaceDescriptor sd;
    sd.nextInChain = &canvasDesc.chain;
    sd.label = (WGPUStringView){ NULL, 0 };
    return wgpuInstanceCreateSurface(instance, &sd);
}

static inline void gpu_present(WGPUSurface surface) { (void)surface; }

static inline int32_t gpu_pump(WGPUDevice device) { (void)device; return 1; }

// A no-op by design, not by omission — see the contract above. The browser throttles rAF for a
// hidden page, so a skipped frame costs nothing here; blocking the main thread would cost plenty.
static inline void gpu_sleep_ms(int32_t ms) { (void)ms; }

#else
// ---- native: bodies in gpu.c (kama.json's `csources`) ----

WGPUSurface gpu_surface(WGPUInstance instance, uint32_t width, uint32_t height, const char* title);
void        gpu_present(WGPUSurface surface);
int32_t     gpu_pump(WGPUDevice device);
void        gpu_sleep_ms(int32_t ms);

#endif  // __EMSCRIPTEN__
#endif  // GPU_H
