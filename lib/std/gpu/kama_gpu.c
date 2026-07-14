// Native half of the WebGPU platform seam (see kama_gpu.h). Creates a GLFW window, derives a
// wgpu-native surface from its OS handle, presents, and pumps events. The web half is `static
// inline` in the header; the driver compiles THIS file only for a native `--webgpu` build (as
// Objective-C on macOS, where the surface is a CAMetalLayer on the NSWindow's content view).
//
// One window per process is all the "first triangle" needs (it mirrors the single-surface app
// model), so the GLFWwindow is a file static — kama_gpu_pump reaches it without threading a handle
// back through Kama.

#ifndef __EMSCRIPTEN__

#include "kama_gpu.h"
#include <webgpu/wgpu.h>          // wgpu-native extensions (wgpuDevicePoll)
#include <stddef.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11   // v1: X11 (XWayland covers most Wayland desktops); native Wayland is a follow-up
#endif
#include <GLFW/glfw3native.h>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

static GLFWwindow* g_window = NULL;

// Build the platform-specific WGPUSurface from a GLFW window handle.
static WGPUSurface surface_from_window(WGPUInstance instance, GLFWwindow* window) {
#if defined(__APPLE__)
    NSWindow* ns_window = glfwGetCocoaWindow(window);
    [ns_window.contentView setWantsLayer:YES];
    CAMetalLayer* metal_layer = [CAMetalLayer layer];
    [ns_window.contentView setLayer:metal_layer];
    WGPUSurfaceSourceMetalLayer src;
    src.chain.next = NULL;
    src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    src.layer = metal_layer;
#elif defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND src;
    src.chain.next = NULL;
    src.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
    src.hinstance = GetModuleHandle(NULL);
    src.hwnd = glfwGetWin32Window(window);
#else
    WGPUSurfaceSourceXlibWindow src;
    src.chain.next = NULL;
    src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
    src.display = glfwGetX11Display();
    src.window = (uint64_t)glfwGetX11Window(window);
#endif
    WGPUSurfaceDescriptor sd;
    sd.nextInChain = (WGPUChainedStruct*)&src;   // the source struct's `chain` is its first member
    sd.label = (WGPUStringView){ NULL, 0 };
    return wgpuInstanceCreateSurface(instance, &sd);
}

WGPUSurface kama_gpu_surface(WGPUInstance instance, uint32_t width, uint32_t height, const char* title) {
    if (!glfwInit()) return NULL;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // WebGPU owns the swapchain, not GL
    g_window = glfwCreateWindow((int)width, (int)height, title ? title : "kama", NULL, NULL);
    if (!g_window) { glfwTerminate(); return NULL; }
    return surface_from_window(instance, g_window);
}

void kama_gpu_present(WGPUSurface surface) {
    wgpuSurfacePresent(surface);
}

int32_t kama_gpu_pump(WGPUDevice device) {
    glfwPollEvents();
    if (device) wgpuDevicePoll(device, 0, NULL);   // wgpu-native: run queued callbacks / reclaim resources
    return (g_window && !glfwWindowShouldClose(g_window)) ? 1 : 0;
}

#endif  // !__EMSCRIPTEN__
