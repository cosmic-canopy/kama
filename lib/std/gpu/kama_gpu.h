#ifndef KAMA_GPU_H
#define KAMA_GPU_H

// The WebGPU platform seam — the ONE place the web-vs-native surface/present/loop split lives, the
// same way kama_app.h is the one place the frame-loop split lives. A program's WebGPU code (the
// pipeline, render pass, draw) is identical across targets; only three things differ, so only three
// things live here:
//   * surface creation — the web binds an HTML <canvas> by selector; native binds a real OS window
//     (a GLFW window + its Metal layer / X11 / Wayland / HWND handle).
//   * present — the browser auto-presents each requestAnimationFrame tick; native must present.
//   * the per-tick pump — native drains window events + polls the device and reports window-close;
//     the web is host-driven and never needs it.
//
// Pulled in only by a program that `extern "kama_gpu.h";`'s. On the web the bodies are `static
// inline` here (no separate translation unit, like kama_app.h); on native they live in kama_gpu.c
// (the macOS surface needs Objective-C), which the driver compiles + links under native `--webgpu`.

#include <stdint.h>
#include <webgpu/webgpu.h>

// The seam's three functions (contract shared by both targets):
//
//   WGPUSurface kama_gpu_surface(instance, width, height, title)
//     Create the render surface. Native ALSO creates and owns the window (width/height/title
//     honored); on the web those are ignored — the surface is the page's <canvas id="canvas"> and
//     the HTML shell owns its size. Returns the WGPUSurface (NULL on failure).
//
//   void kama_gpu_present(surface)
//     Present the last-submitted frame. Native calls wgpuSurfacePresent; on the web a no-op
//     (emdawnwebgpu aborts on an explicit present — the browser presents the canvas each rAF tick).
//
//   int32_t kama_gpu_pump(device)
//     Advance one tick's platform work and report whether to keep running. Native pumps the window
//     event queue + polls the device and returns 0 once the window is asked to close; on the web a
//     no-op that always returns 1 (the host drives the loop). Fits std::app's `while(tick())`.
//
// Web bodies are `static inline` here (no separate TU); native bodies are declared here and defined
// in kama_gpu.c.

#ifdef __EMSCRIPTEN__
// ---- web: self-contained, no separate TU (the driver never compiles kama_gpu.c for wasm) ----

static inline WGPUSurface kama_gpu_surface(WGPUInstance instance, uint32_t width, uint32_t height,
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

static inline void kama_gpu_present(WGPUSurface surface) { (void)surface; }

static inline int32_t kama_gpu_pump(WGPUDevice device) { (void)device; return 1; }

#else
// ---- native: bodies in kama_gpu.c (compiled + linked by the driver under native --webgpu) ----

WGPUSurface kama_gpu_surface(WGPUInstance instance, uint32_t width, uint32_t height, const char* title);
void        kama_gpu_present(WGPUSurface surface);
int32_t     kama_gpu_pump(WGPUDevice device);

#endif  // __EMSCRIPTEN__
#endif  // KAMA_GPU_H
