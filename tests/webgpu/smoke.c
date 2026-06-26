// WebGPU toolchain smoke test (hand-written C, not cstar — yet).
//
// Proves the containerized Emscripten toolchain can compile and link a program
// against the WebGPU API (the emdawnwebgpu port). This de-risks the project's
// core goal: a portable WebGPU engine compiled to WASM.
//
// It is plain C because cstar the language cannot yet express opaque handle
// types (WGPUInstance) or pointers; cstar-level WebGPU bindings arrive once
// pointer + extern-declaration support lands (see the plan's post-v1 work).
//
// Build: emcc --use-port=emdawnwebgpu smoke.c -o smoke.html
// Linking successfully is the test. Actually creating a device needs a browser
// with WebGPU, so we only exercise instance creation defensively.

#include <stdio.h>
#include <webgpu/webgpu.h>

int main(void)
{
    WGPUInstance instance = wgpuCreateInstance(NULL);
    if (instance) {
        printf("cstar webgpu smoke: WGPUInstance created\n");
        wgpuInstanceRelease(instance);
    } else {
        printf("cstar webgpu smoke: no instance (expected outside a browser)\n");
    }
    return 0;
}
