#ifndef KAMA_FIBER_H
#define KAMA_FIBER_H

// The fiber seam (KR-3, docs/design/fibers.md decisions 2 and 14): a stack and a switch. A fiber is a second thread of
// control INSIDE one isolate — it never leaves the thread that made it, so it needs no `Sendable` bound — and it runs
// until it switches away. This header is the bottom of that: allocate a stack, switch to a fiber and back, free it.
// The scheduler, the park rule and `spawn fiber` are built on it.
//
// Pay for what you use, as kama_isolate.h: a program that `extern "kama_fiber.h";`s pulls it in (kama_isolate.h
// does, for the overflow handler below), and kama_runtime.h stays freestanding.
//
// THE SWITCH. Own asm on POSIX, about thirty instructions an architecture: it pushes only the callee-saved
// registers on the stack it leaves, stores that stack pointer, loads the other and pops. `swapcontext` costs 25–35
// times as much — a system call per switch, to set the signal mask — and is deprecated on macOS and absent from musl
// (the design's §6: 21 ns a round trip against 497–714 ns). Windows uses its own Fiber API, which keeps the TIB's
// stack fields, `__chkstk` probes and fiber-local storage right by construction. The asm defines FILE-LOCAL symbols:
// every unit that includes this header assembles its own copy, and global ones would collide at link. A switch saves
// no kama runtime state: at every switch the fiber leaving has no open access and no `@onPanic` region (decision 3
// forbids both across a park), so there is nothing per fiber beyond the registers.
//
// THE STACKS. An `mmap` reservation with one PROT_NONE guard page below it, committed only as it is touched: 256 KiB
// unless the caller states a size, ×3 under ASan, whose frames are larger. Finished fibers' stacks wait on a
// per-thread list for the next fiber of the same size.
//
// OVERFLOW. A guard-page hit says which stack overflowed and how to raise it — "fiber stack overflow (256 KiB;
// `spawn fiber(stack: …)` to raise it)" — where it was a bare segfault, and the same handler serves an isolate's
// stack (kama_isolate.h). POSIX runs it on a per-thread signal stack; Windows, a vectored handler with a stack
// guarantee, as Rust's std does there. It covers the guard and 64 KiB below it, where a frame larger than a page
// lands. The main thread is not covered: that would install a handler in every program, fibers or not.
//
// SANITIZERS are annotated here, in the first commit, because without them `./dev test san` and the TSan leg fail or
// lie: ASan is told each switch (`__sanitizer_start/finish_switch_fiber`, so it tracks the stack in use and destroys a
// finished fiber's fake stack), TSan is given a fiber of its own per kama fiber (`__tsan_create/switch_to/
// destroy_fiber`), and Valgrind has each stack registered.
//
// STATE is one per thread and one per process, declared here and defined once, in the program's entry unit
// (CEmitter::emitRuntimeSlotDefinitions, under this header's guard): a `static` here would give every unit its own.

#include "kama_runtime.h"

#if !defined(__EMSCRIPTEN__)   /* fibers on wasm are JSPI (decision 4), a later step; wasm stacks are emscripten's */

#if defined(_WIN32)
  #define KAMA_FIBER_WIN 1
#elif defined(__aarch64__) || defined(__x86_64__) || defined(__i386__) || (defined(__riscv) && __riscv_xlen == 64)
  #define KAMA_FIBER_ASM 1
#else
  #define KAMA_FIBER_NONE 1      /* no switch for this architecture: `spawn fiber` is refused at its site */
#endif

#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define KAMA__FIBER_ASAN 1
  #endif
  #if __has_feature(thread_sanitizer)
    #define KAMA__FIBER_TSAN 1
  #endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(KAMA__FIBER_ASAN)
  #define KAMA__FIBER_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__) && !defined(KAMA__FIBER_TSAN)
  #define KAMA__FIBER_TSAN 1
#endif
#if defined(KAMA__FIBER_ASAN)
void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
#endif
#if defined(KAMA__FIBER_TSAN)
void* __tsan_get_current_fiber(void);
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif
#if defined(__has_include)
  #if __has_include(<valgrind/valgrind.h>)
    #include <valgrind/valgrind.h>
    #define KAMA__FIBER_VALGRIND 1
  #endif
#endif

#include <string.h>
#include <stdlib.h>
#if !defined(KAMA_FIBER_WIN)
  #include <signal.h>
  #include <sched.h>
  #include <sys/mman.h>
  #include <unistd.h>
  #include <pthread.h>
#endif

// The default stack, and the one kama states when `spawn fiber` names none: decision 2.
#define KAMA_FIBER_STACK ((size_t)256 << 10)
// Idle stacks a thread keeps for reuse; a fiber freed beyond that is unmapped.
#define KAMA__FIBER_IDLE_MAX 64
// How far below a guard page a fault still counts as that stack's overflow: a frame larger than a page jumps it.
#define KAMA__STACK_SLACK ((size_t)64 << 10)

typedef struct kama_fiber {
    void* sp;                       // the saved stack pointer while switched out (asm)
    struct kama_fiber* resumer;     // the fiber that last switched to this one: where its entry's return goes
    struct kama_fiber* next;        // the idle list here; the scheduler's queues later
    void (*entry)(void*);
    void* arg;
    char* map;                      // the reservation: the guard page, then the stack (NULL for a thread's own)
    size_t maplen;
    char* lo;                       // the stack's lowest usable byte
    size_t size;                    // its usable bytes
    size_t stated;                  // the size asked for, which an overflow names
    int done;                       // the entry returned
    // The scheduler's (kama_sched.h); a fiber switched by hand leaves them alone.
    void* sched;                    // the scheduler it runs on
    struct kama_fiber* joiner;      // parked in a join on this one, woken when it ends
    struct kama_fiber* remoteNext;  // the scheduler's remote queue
    uint64_t deadline;              // a timed park's, while it is in the timer heap
    int32_t heapIx;                 // its slot in the timer heap, or -1
    int32_t refs;                   // its owner, and each remote wake in flight
    int32_t remoteQueued;           // a remote wake is in flight
    int8_t parked, timedOut, detached;
#if defined(KAMA__FIBER_ASAN)
    void* asanFake;
#endif
#if defined(KAMA__FIBER_TSAN)
    void* tsan;
#endif
#if defined(KAMA__FIBER_VALGRIND)
    unsigned vgId;
#endif
#if defined(KAMA_FIBER_WIN)
    void* os;                       // the OS fiber
#endif
} kama_fiber;

// One per thread.
typedef struct kama_fiber_thread {
    kama_fiber root;                // the thread's own context, as the fiber a new fiber's first switch leaves
    int rootReady;
    kama_fiber* cur;                // running now
    kama_fiber* from;               // the one that switched to it
    kama_fiber* idle;               // finished fibers whose stacks wait for reuse
    int idleN;
    void* alt;                      // the signal stack this header made (or 1: the thread had one)
    int guarded;                    // the overflow handler covers this thread
    char* guardLo;                  // an isolate's own guard, [guardLo, guardHi), and the size it was given
    char* guardHi;
    size_t stated;
    void* sched;                    // this isolate's fiber scheduler (kama_sched.h), once it has one
} kama_fiber_thread;
// One per process.
typedef struct kama_fiber_process {
    int state;                      // the handler: 0 not installed, 1 installing, 2 installed
#if !defined(KAMA_FIBER_WIN)
    struct sigaction oldSegv, oldBus;
    pthread_key_t key;              // its destructor frees a thread's signal stack and idle fibers
#endif
} kama_fiber_process;
extern KAMA_ISOLATE_LOCAL kama_fiber_thread kama__fibers;
extern kama_fiber_process kama__fiberProc;

#if !defined(KAMA_FIBER_NONE)
static inline void kama__fiber_main(kama_fiber* self);
#endif

// ---- the switch -------------------------------------------------------------------------------------------------
#if defined(KAMA_FIBER_ASM)
// kama__fiber_swap(save, load): push the callee-saved registers on this stack, store sp in *save, make `load` the
// stack, pop the same registers from it and return — into whatever the other side was doing when it swapped away,
// or, the first time, into kama__fiber_boot, which calls kama__fiber_main(fiber) with the frame pointer (and link
// register) zeroed so a backtrace ends there.
//
// HIDDEN, because the symbols are file-local: taking the address of an ordinary extern function in position-
// independent code goes through the GOT, and a GOT entry against a local symbol keeps no offset — GCC's PIE
// resolved `&kama__fiber_boot` to the start of the unit's text, which is `kama__fiber_swap`, so a new fiber
// "returned" into the switch and spun. A hidden declaration is addressed PC-relative, offset and all.
__attribute__((visibility("hidden"))) void kama__fiber_swap(void** save, void* load);
__attribute__((visibility("hidden"))) void kama__fiber_boot(void);
#if defined(__APPLE__)
  #define KAMA__FSYM(x) "_" #x
#else
  #define KAMA__FSYM(x) #x
#endif

#if defined(__aarch64__)
// AAPCS64: x19–x28, x29 (fp), x30 (lr) and d8–d15 — 160 bytes. x18 is the platform register, left alone (Apple
// reserves it). The first frame holds the fiber in x19 and kama__fiber_main in x20, with x30 = kama__fiber_boot.
// `hint #34` is `bti c`, a no-op where BTI is off; a `ret` into the boot stub is not BTI-checked. LR is saved and
// restored as the `bl` left it, never signed here, so PAC-signed callers are unaffected.
__asm__(".text\n.p2align 2\n" KAMA__FSYM(kama__fiber_swap) ":\n"
        "hint #34\n"
        "sub sp, sp, #160\n"
        "stp x19, x20, [sp, #0]\n stp x21, x22, [sp, #16]\n stp x23, x24, [sp, #32]\n stp x25, x26, [sp, #48]\n"
        "stp x27, x28, [sp, #64]\n stp x29, x30, [sp, #80]\n stp d8, d9, [sp, #96]\n stp d10, d11, [sp, #112]\n"
        "stp d12, d13, [sp, #128]\n stp d14, d15, [sp, #144]\n"
        "mov x2, sp\n str x2, [x0]\n mov sp, x1\n"
        "ldp x19, x20, [sp, #0]\n ldp x21, x22, [sp, #16]\n ldp x23, x24, [sp, #32]\n ldp x25, x26, [sp, #48]\n"
        "ldp x27, x28, [sp, #64]\n ldp x29, x30, [sp, #80]\n ldp d8, d9, [sp, #96]\n ldp d10, d11, [sp, #112]\n"
        "ldp d12, d13, [sp, #128]\n ldp d14, d15, [sp, #144]\n"
        "add sp, sp, #160\n ret\n"
        ".p2align 2\n" KAMA__FSYM(kama__fiber_boot) ":\n"
        "hint #34\n"
        "mov x0, x19\n mov x29, #0\n mov x30, #0\n blr x20\n brk #0\n");
static inline void* kama__fiber_frame(char* top, kama_fiber* f) {
    uint64_t* fr = (uint64_t*)top - 20;
    for (int i = 0; i < 20; i++) fr[i] = 0;
    fr[0] = (uint64_t)(uintptr_t)f;                                  // x19
    fr[1] = (uint64_t)(uintptr_t)&kama__fiber_main;                  // x20
    fr[11] = (uint64_t)(uintptr_t)&kama__fiber_boot;                 // x30 (x29 = 0)
    return fr;
}
#elif defined(__x86_64__)
// SysV x86-64: rbx, rbp, r12–r15, and the control words — MXCSR's control bits and the x87 control word are
// callee-saved too. 56 bytes plus the return address. The first frame holds the fiber in rbx and kama__fiber_main in
// r12, returning into the boot stub with the stack 16-byte aligned for its call. `endbr64` is a no-op without IBT.
__asm__(".text\n.p2align 4\n" KAMA__FSYM(kama__fiber_swap) ":\n"
#if defined(__CET__)
        "endbr64\n"
#endif
        "pushq %rbp\n pushq %rbx\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
        "subq $8, %rsp\n stmxcsr (%rsp)\n fnstcw 4(%rsp)\n"
        "movq %rsp, (%rdi)\n movq %rsi, %rsp\n"
        "ldmxcsr (%rsp)\n fldcw 4(%rsp)\n addq $8, %rsp\n"
        "popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbx\n popq %rbp\n"
        "ret\n"
        ".p2align 4\n" KAMA__FSYM(kama__fiber_boot) ":\n"
#if defined(__CET__)
        "endbr64\n"
#endif
        "movq %rbx, %rdi\n xorl %ebp, %ebp\n callq *%r12\n ud2\n");
// CET shadow stacks. A unit built with `-fcf-protection=full` is marked shadow-stack compatible, and where the kernel
// and C library turn them on for the process, a `ret` onto a stack the switch made faults: a correct switch gives each
// fiber a shadow stack of its own and restores it with a token, which this one does not yet do (KR-238). So it refuses,
// naming why, rather than crash at the first switch. `rdsspq` reads the shadow stack pointer, and is a no-op that
// leaves zero where shadow stacks are off.
static inline void kama__fiber_cet(void) {
#if defined(__CET__) && (__CET__ & 2)
    unsigned long long ssp = 0;
    __asm__ volatile("rdsspq %0" : "+r"(ssp));
    if (ssp) kama_panic(kama_string_lit("fiber: this process runs with CET shadow stacks, which kama's fiber switch does not support yet", 95));
#endif
}
static inline void* kama__fiber_frame(char* top, kama_fiber* f) {
    kama__fiber_cet();
    uint64_t* fr = (uint64_t*)top - 8;                               // ctl, r15, r14, r13, r12, rbx, rbp, ret
    fr[0] = (uint64_t)0x1F80 | ((uint64_t)0x037F << 32);             // MXCSR and the x87 CW at their defaults
    fr[1] = fr[2] = fr[3] = 0;                                       // r15 r14 r13
    fr[4] = (uint64_t)(uintptr_t)&kama__fiber_main;                  // r12
    fr[5] = (uint64_t)(uintptr_t)f;                                  // rbx
    fr[6] = 0;                                                       // rbp
    fr[7] = (uint64_t)(uintptr_t)&kama__fiber_boot;                  // the return address
    return fr;
}
#elif defined(__i386__)
// SysV i386: ebx, esi, edi, ebp, and the control words (MXCSR only where SSE exists); the arguments are on the
// stack. The first frame holds the fiber in ebx and kama__fiber_main in esi; the boot stub keeps the 16-byte
// alignment the ABI wants at a call.
__asm__(".text\n.p2align 4\n" KAMA__FSYM(kama__fiber_swap) ":\n"
#if defined(__CET__)
        "endbr32\n"
#endif
        "movl 4(%esp), %eax\n movl 8(%esp), %ecx\n"
        "pushl %ebp\n pushl %ebx\n pushl %esi\n pushl %edi\n"
        "subl $8, %esp\n"
#if defined(__SSE__)
        "stmxcsr (%esp)\n"
#endif
        "fnstcw 4(%esp)\n"
        "movl %esp, (%eax)\n movl %ecx, %esp\n"
#if defined(__SSE__)
        "ldmxcsr (%esp)\n"
#endif
        "fldcw 4(%esp)\n addl $8, %esp\n"
        "popl %edi\n popl %esi\n popl %ebx\n popl %ebp\n"
        "ret\n"
        ".p2align 4\n" KAMA__FSYM(kama__fiber_boot) ":\n"
        "subl $12, %esp\n pushl %ebx\n xorl %ebp, %ebp\n call *%esi\n ud2\n");
static inline void* kama__fiber_frame(char* top, kama_fiber* f) {
    uint32_t* fr = (uint32_t*)top - 7;                               // mxcsr, cw, edi, esi, ebx, ebp, ret
    fr[0] = 0x1F80; fr[1] = 0x037F;
    fr[2] = 0;                                                       // edi
    fr[3] = (uint32_t)(uintptr_t)&kama__fiber_main;                  // esi
    fr[4] = (uint32_t)(uintptr_t)f;                                  // ebx
    fr[5] = 0;                                                       // ebp
    fr[6] = (uint32_t)(uintptr_t)&kama__fiber_boot;                  // the return address
    return fr;
}
#elif defined(__riscv)
// RV64 (LP64D where the D extension is present): ra, s0–s11 and fs0–fs11 — 208 bytes. The first frame holds the
// fiber in s1 and kama__fiber_main in s2, with ra = kama__fiber_boot; s0, the frame pointer, starts at zero.
#if defined(__riscv_flen) && __riscv_flen >= 64
  #define KAMA__RV_FS(op) \
    op " fs0, 104(sp)\n " op " fs1, 112(sp)\n " op " fs2, 120(sp)\n " op " fs3, 128(sp)\n " op " fs4, 136(sp)\n " \
    op " fs5, 144(sp)\n " op " fs6, 152(sp)\n " op " fs7, 160(sp)\n " op " fs8, 168(sp)\n " op " fs9, 176(sp)\n " \
    op " fs10, 184(sp)\n " op " fs11, 192(sp)\n"
#else
  #define KAMA__RV_FS(op) ""
#endif
#define KAMA__RV_S(op) \
    op " ra, 0(sp)\n " op " s0, 8(sp)\n " op " s1, 16(sp)\n " op " s2, 24(sp)\n " op " s3, 32(sp)\n " \
    op " s4, 40(sp)\n " op " s5, 48(sp)\n " op " s6, 56(sp)\n " op " s7, 64(sp)\n " op " s8, 72(sp)\n " \
    op " s9, 80(sp)\n " op " s10, 88(sp)\n " op " s11, 96(sp)\n"
__asm__(".text\n.p2align 2\n" KAMA__FSYM(kama__fiber_swap) ":\n"
        "addi sp, sp, -208\n"
        KAMA__RV_S("sd") KAMA__RV_FS("fsd")
        "sd sp, 0(a0)\n mv sp, a1\n"
        KAMA__RV_S("ld") KAMA__RV_FS("fld")
        "addi sp, sp, 208\n ret\n"
        ".p2align 2\n" KAMA__FSYM(kama__fiber_boot) ":\n"
        "mv a0, s1\n li s0, 0\n jalr s2\n ebreak\n");
static inline void* kama__fiber_frame(char* top, kama_fiber* f) {
    uint64_t* fr = (uint64_t*)top - 26;
    for (int i = 0; i < 26; i++) fr[i] = 0;
    fr[0] = (uint64_t)(uintptr_t)&kama__fiber_boot;                  // ra
    fr[2] = (uint64_t)(uintptr_t)f;                                  // s1
    fr[3] = (uint64_t)(uintptr_t)&kama__fiber_main;                  // s2
    return fr;
}
#endif
#endif /* KAMA_FIBER_ASM */

// ---- the overflow message ---------------------------------------------------------------------------------------
#if defined(KAMA_FIBER_WIN)
// Declared rather than included: headers are emitted in name order, and <windows.h> belongs to kama_os.h (as in
// kama_locked.h). Each matches its <windows.h> declaration, so a unit that sees both compiles.
struct _EXCEPTION_POINTERS;
struct _OVERLAPPED;
__declspec(dllimport) void* __stdcall ConvertThreadToFiberEx(void* param, unsigned long flags);
#if defined(_WIN64)
typedef unsigned long long kama__win_size;       /* SIZE_T, which is ULONG_PTR: not size_t's spelling on 32-bit */
#else
typedef unsigned long kama__win_size;
#endif
__declspec(dllimport) void* __stdcall CreateFiberEx(kama__win_size commit, kama__win_size reserve, unsigned long flags,
                                                    void (__stdcall *start)(void*), void* param);
__declspec(dllimport) void __stdcall SwitchToFiber(void* fiber);
__declspec(dllimport) void __stdcall DeleteFiber(void* fiber);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
__declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(unsigned long first,
                                                                  long (__stdcall *handler)(struct _EXCEPTION_POINTERS*));
__declspec(dllimport) int __stdcall SetThreadStackGuarantee(unsigned long* size);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned long which);
__declspec(dllimport) int __stdcall WriteFile(void* file, const void* buf, unsigned long n, unsigned long* written,
                                              struct _OVERLAPPED* overlapped);
#define KAMA__FIBER_FLOAT_SWITCH 0x1ul           /* FIBER_FLAG_FLOAT_SWITCH: x86 saves the FPU state only when asked */
#define KAMA__ERROR_ALREADY_FIBER 1280ul
static inline void kama__stack_say(const char* s) {
    unsigned long n = 0, w = 0; while (s[n]) n++;
    WriteFile(GetStdHandle((unsigned long)-12), s, n, &w, NULL);   /* STD_ERROR_HANDLE */
}
#else
static inline void kama__stack_say(const char* s) { size_t n = 0; while (s[n]) n++; (void)!write(2, s, n); }
#endif
// "256 KiB", "8 MiB": a size in the largest unit it divides.
static inline void kama__stack_saySize(size_t n) {
    const char* unit = " B";
    if (n >= ((size_t)1 << 20) && n % ((size_t)1 << 20) == 0) { n >>= 20; unit = " MiB"; }
    else if (n >= 1024 && n % 1024 == 0) { n >>= 10; unit = " KiB"; }
    char buf[24]; int i = 23; buf[i] = 0;
    do { buf[--i] = (char)('0' + n % 10); n /= 10; } while (n && i > 0);
    kama__stack_say(buf + i); kama__stack_say(unit);
}
static inline void kama__stack_sayFiber(size_t stated) {
    kama__stack_say("kama: fiber stack overflow ("); kama__stack_saySize(stated);
    kama__stack_say("; `spawn fiber(stack: …)` to raise it)\n");
}
static inline void kama__stack_sayIsolate(size_t stated) {
    kama__stack_say("kama: isolate stack overflow ("); kama__stack_saySize(stated);
    kama__stack_say("; `spawn(stack: …)` to raise it)\n");
}

// ---- the overflow handler ---------------------------------------------------------------------------------------
#if defined(KAMA_FIBER_WIN)
// A vectored handler sees EXCEPTION_STACK_OVERFLOW first, says which stack it was and lets the search continue, so
// the process still ends with STATUS_STACK_OVERFLOW. It runs on what the stack guarantee leaves, set per thread and
// per fiber: the overflowed stack has no room of its own.
static long __stdcall kama__stack_onException(struct _EXCEPTION_POINTERS* ep) {
    unsigned long code = **(unsigned long**)ep;   /* ep->ExceptionRecord->ExceptionCode */
    if (code != 0xC00000FDul) return 0;           /* EXCEPTION_CONTINUE_SEARCH */
    kama_fiber* f = kama__fibers.cur;
    if (f && f->os && f != &kama__fibers.root) kama__stack_sayFiber(f->stated);
    else if (kama__fibers.stated) kama__stack_sayIsolate(kama__fibers.stated);
    return 0;
}
static inline void kama__stack_guarantee(void) { unsigned long n = 32u << 10; SetThreadStackGuarantee(&n); }
static inline void kama__stack_install(void) {
    int s = 0;
    if (__atomic_compare_exchange_n(&kama__fiberProc.state, &s, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        AddVectoredExceptionHandler(1, kama__stack_onException);
}
static inline void kama__stack_thread(void) {
    if (kama__fibers.guarded) return;
    kama__fibers.guarded = 1;
    kama__stack_install();
    kama__stack_guarantee();
}
#else
// POSIX: a guard page is hit on a stack with no room left, so the handler runs on a signal stack of its own, one per
// thread, made the first time the thread makes a fiber or starts as an isolate. Any other fault is handed back to
// whatever handler was there before — a sanitizer's report or the default crash, unchanged. Everything it calls is
// async-signal-safe.
static void kama__stack_onFault(int sig, siginfo_t* si, void* uc) {
    char* a = (char*)si->si_addr;
    kama_fiber* f = kama__fibers.cur;
    if (f && f->map && a >= f->map - KAMA__STACK_SLACK && a < f->lo) { kama__stack_sayFiber(f->stated); abort(); }
    if (kama__fibers.guardHi && a >= kama__fibers.guardLo && a < kama__fibers.guardHi) {
        kama__stack_sayIsolate(kama__fibers.stated); abort();
    }
    (void)uc;
    sigaction(sig, sig == SIGSEGV ? &kama__fiberProc.oldSegv : &kama__fiberProc.oldBus, NULL);   // it repeats there
}
static void kama__stack_threadDone(void* alt);
static inline size_t kama__stack_altSize(void) { size_t n = (size_t)64 << 10; return (size_t)SIGSTKSZ > n ? (size_t)SIGSTKSZ : n; }
// A forked child: macOS does not carry the thread's signal stack across `fork` (Linux does), and a guard-page hit with
// none left the kernel no room to deliver the fault, so it ended the child with SIGILL and said nothing. Register
// this header's stack again; one the thread had from elsewhere is looked at afresh on next use.
static void kama__stack_afterFork(void) {
    if (kama__fibers.alt && kama__fibers.alt != (void*)1) {
        stack_t ss; ss.ss_sp = kama__fibers.alt; ss.ss_size = kama__stack_altSize(); ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    } else {
        kama__fibers.guarded = 0;
        kama__fibers.alt = NULL;
    }
}
// Once per process: the handler, and the key whose destructor frees a thread's signal stack and idle stacks.
static inline void kama__stack_install(void) {
    int s = 0;
    if (__atomic_compare_exchange_n(&kama__fiberProc.state, &s, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pthread_key_create(&kama__fiberProc.key, kama__stack_threadDone);
        pthread_atfork(NULL, NULL, kama__stack_afterFork);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = kama__stack_onFault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &kama__fiberProc.oldSegv);
        sigaction(SIGBUS, &sa, &kama__fiberProc.oldBus);
        __atomic_store_n(&kama__fiberProc.state, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&kama__fiberProc.state, __ATOMIC_ACQUIRE) != 2) sched_yield();
}
// Once per thread: a signal stack, unless the thread has one (a sanitizer's, or the program's own).
static inline void kama__stack_thread(void) {
    if (kama__fibers.guarded) return;
    kama__fibers.guarded = 1;
    kama__stack_install();
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) kama__fibers.alt = (void*)1;
    else {
        size_t n = kama__stack_altSize();
        void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        stack_t ss; ss.ss_sp = p; ss.ss_size = n; ss.ss_flags = 0;
        if (p != MAP_FAILED && sigaltstack(&ss, NULL) == 0) kama__fibers.alt = p;
        else { if (p != MAP_FAILED) munmap(p, n); kama__fibers.alt = (void*)1; }
    }
    pthread_setspecific(kama__fiberProc.key, kama__fibers.alt);   // non-NULL, so the destructor runs
}
#endif

// An isolate's thread, as it starts (kama_isolate.h): the handler covers its stack, which was given `stated` bytes.
#if defined(__linux__)
extern int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);   /* glibc declares it only under _GNU_SOURCE */
#endif
static inline void kama__stack_isolate(size_t stated) {
    kama__stack_thread();
    kama__fibers.stated = stated;
#if !defined(KAMA_FIBER_WIN)
    char* lo = NULL;
    size_t guard = (size_t)sysconf(_SC_PAGESIZE);
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    lo = (char*)pthread_get_stackaddr_np(self) - pthread_get_stacksize_np(self);
#elif defined(__linux__)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void* addr = NULL; size_t size = 0, g = 0;
        if (pthread_attr_getstack(&a, &addr, &size) == 0) lo = (char*)addr;
        if (pthread_attr_getguardsize(&a, &g) == 0 && g) guard = g;
        pthread_attr_destroy(&a);
    }
#endif
    if (lo) { kama__fibers.guardHi = lo; kama__fibers.guardLo = lo - guard - KAMA__STACK_SLACK; }
#endif
}

// ---- fibers -----------------------------------------------------------------------------------------------------
#if !defined(KAMA_FIBER_NONE)
static inline void kama__fiber_unmap(kama_fiber* f) {
#if defined(KAMA__FIBER_VALGRIND)
    VALGRIND_STACK_DEREGISTER(f->vgId);
#endif
#if defined(KAMA__FIBER_TSAN)
    if (f->tsan) __tsan_destroy_fiber(f->tsan);
#endif
#if defined(KAMA_FIBER_WIN)
    if (f->os) DeleteFiber(f->os);
#else
    if (f->map) munmap(f->map, f->maplen);
#endif
    kama_free(f, sizeof *f, _Alignof(kama_fiber));
}
#endif

#if !defined(KAMA_FIBER_WIN)
static void kama__stack_threadDone(void* alt) {
#if !defined(KAMA_FIBER_NONE)
    while (kama__fibers.idle) { kama_fiber* f = kama__fibers.idle; kama__fibers.idle = f->next; kama__fiber_unmap(f); }
    kama__fibers.idleN = 0;
#endif
    if (alt && alt != (void*)1) {
        stack_t ss; ss.ss_sp = NULL; ss.ss_size = 0; ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        munmap(alt, kama__stack_altSize());
    }
}
#endif

#if !defined(KAMA_FIBER_NONE)
#if defined(KAMA_FIBER_WIN)
static void __stdcall kama__fiber_winmain(void* param) { kama__stack_guarantee(); kama__fiber_main((kama_fiber*)param); }
// The thread's current fiber when something else already made it one: NtCurrentTeb()->NtTib.FiberData, read the
// way <winnt.h>'s GetCurrentFiber does (it needs <windows.h>, and mingw's __readgsqword trips -Warray-bounds).
static inline void* kama__fiber_currentOs(void) {
    void* f;
#if defined(__x86_64__)
    __asm__("movq %%gs:0x20, %0" : "=r"(f));
#elif defined(__aarch64__)
    char* teb; __asm__("mov %0, x18" : "=r"(teb)); f = *(void**)(teb + 0x20);
#else
    __asm__("movl %%fs:0x10, %0" : "=r"(f));
#endif
    return f;
}
#endif

// The thread's own context, as the fiber a new fiber's first switch leaves.
static inline kama_fiber* kama_fiber_root(void) {
    kama_fiber_thread* t = &kama__fibers;
    if (!t->rootReady) {
        t->rootReady = 1;
        memset(&t->root, 0, sizeof t->root);
#if defined(KAMA__FIBER_TSAN)
        t->root.tsan = __tsan_get_current_fiber();
#endif
#if defined(KAMA_FIBER_WIN)
        t->root.os = ConvertThreadToFiberEx(NULL, KAMA__FIBER_FLOAT_SWITCH);
        if (!t->root.os && GetLastError() == KAMA__ERROR_ALREADY_FIBER) t->root.os = kama__fiber_currentOs();
        if (!t->root.os) kama_panic(kama_string_lit("fiber: the thread could not become a fiber", 43));
#endif
        t->cur = &t->root;
    }
    return &t->root;
}
// The fiber running on this thread now.
static inline kama_fiber* kama_fiber_current(void) { return kama__fibers.cur ? kama__fibers.cur : kama_fiber_root(); }

// A fiber that will run `entry(arg)` on a stack of `stack` bytes (0: the default), from the first switch to it. It runs
// on the thread that makes it, and only there. Panics when the stack cannot be had: as `spawn`, not recoverable.
static inline kama_fiber* kama_fiber_new(void (*entry)(void*), void* arg, size_t stack) {
    kama_fiber_root();
    kama__stack_thread();
    size_t stated = stack ? stack : KAMA_FIBER_STACK;
    size_t want = stated;
#if defined(KAMA__FIBER_ASAN)
    want *= 3;
#endif
    kama_fiber* f = NULL;
#if !defined(KAMA_FIBER_WIN)
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (want + page - 1) & ~(page - 1);
    for (kama_fiber** p = &kama__fibers.idle; *p; p = &(*p)->next)          // a stack this size, if one is idle
        if ((*p)->size == size) { f = *p; *p = f->next; kama__fibers.idleN--; break; }
#if defined(KAMA__FIBER_ASAN)
    if (f) __asan_unpoison_memory_region(f->lo, f->size);                  // its last fiber's final frame never returned
#endif
    if (!f) {
        f = (kama_fiber*)kama_alloc(sizeof *f, _Alignof(kama_fiber));
        if (!f) kama_panic(kama_string_lit("fiber: out of memory", 20));
        memset(f, 0, sizeof *f);
        int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
        flags |= MAP_NORESERVE;
#endif
#if defined(MAP_STACK)
        flags |= MAP_STACK;
#endif
        char* base = (char*)mmap(NULL, size + page, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (base == (char*)MAP_FAILED || mprotect(base, page, PROT_NONE) != 0) {
            if (base != (char*)MAP_FAILED) munmap(base, size + page);
            kama_free(f, sizeof *f, _Alignof(kama_fiber));
            kama_panic(kama_string_lit("fiber: the stack could not be reserved", 38));
        }
        f->map = base; f->maplen = size + page; f->lo = base + page; f->size = size;
#if defined(KAMA__FIBER_VALGRIND)
        f->vgId = VALGRIND_STACK_REGISTER(f->lo, f->lo + size);
#endif
#if defined(KAMA__FIBER_TSAN)
        f->tsan = __tsan_create_fiber(0);
#endif
    }
    f->sp = kama__fiber_frame(f->lo + f->size, f);
#else
    f = (kama_fiber*)kama_alloc(sizeof *f, _Alignof(kama_fiber));
    if (!f) kama_panic(kama_string_lit("fiber: out of memory", 20));
    memset(f, 0, sizeof *f);
    f->size = want;
    f->os = CreateFiberEx(0, (kama__win_size)want, KAMA__FIBER_FLOAT_SWITCH, kama__fiber_winmain, f);
    if (!f->os) {
        kama_free(f, sizeof *f, _Alignof(kama_fiber));
        kama_panic(kama_string_lit("fiber: the stack could not be reserved", 38));
    }
#endif
    f->entry = entry; f->arg = arg; f->stated = stated; f->done = 0; f->resumer = NULL; f->next = NULL;
    f->sched = NULL; f->joiner = NULL; f->remoteNext = NULL; f->heapIx = -1; f->refs = 0;   // a reused one's are stale
    return f;
}

// Leave the fiber running now, `from`, for `to`; returns when something switches back to `from`.
static inline void kama_fiber_switch(kama_fiber* from, kama_fiber* to) {
    kama_fiber_thread* t = &kama__fibers;
    to->resumer = from;
    t->from = from;
    t->cur = to;
#if defined(KAMA_FIBER_WIN)
    SwitchToFiber(to->os);
#else
#if defined(KAMA__FIBER_ASAN)
    __sanitizer_start_switch_fiber(&from->asanFake, to->lo, to->size);
#endif
#if defined(KAMA__FIBER_TSAN)
    __tsan_switch_to_fiber(to->tsan, 0);
#endif
    kama__fiber_swap(&from->sp, to->sp);
#if defined(KAMA__FIBER_ASAN)
    {   // back on `from`; the fiber that switched here learns its own bounds (the thread's, for a root)
        const void* lo = NULL; size_t n = 0;
        __sanitizer_finish_switch_fiber(from->asanFake, &lo, &n);
        kama_fiber* prev = kama__fibers.from;
        if (prev && !prev->map) { prev->lo = (char*)lo; prev->size = n; }
    }
#endif
#endif
    kama__fibers.cur = from;
}

// The first switch into a fiber arrives here, on its own stack, and its entry's return leaves from here.
static inline void kama__fiber_main(kama_fiber* self) {
#if defined(KAMA__FIBER_ASAN)
    {
        const void* lo = NULL; size_t n = 0;
        __sanitizer_finish_switch_fiber(NULL, &lo, &n);
        kama_fiber* prev = kama__fibers.from;
        if (prev && !prev->map) { prev->lo = (char*)lo; prev->size = n; }
    }
#endif
    kama__fibers.cur = self;
    self->entry(self->arg);
    self->done = 1;
    kama_fiber* to = self->resumer;
    kama__fibers.from = self;
    kama__fibers.cur = to;
#if defined(KAMA_FIBER_WIN)
    SwitchToFiber(to->os);
#else
#if defined(KAMA__FIBER_ASAN)
    __sanitizer_start_switch_fiber(NULL, to->lo, to->size);   // NULL: this fiber's fake stack is destroyed
#endif
#if defined(KAMA__FIBER_TSAN)
    __tsan_switch_to_fiber(to->tsan, 0);
#endif
    kama__fiber_swap(&self->sp, to->sp);
#endif
    abort();   /* a finished fiber is never switched to again */
}

// Free a fiber that is not running: finished, or never started. Its stack waits on this thread's idle list for the
// next fiber of the same size, up to KAMA__FIBER_IDLE_MAX of them.
static inline void kama_fiber_free(kama_fiber* f) {
#if !defined(KAMA_FIBER_WIN)
    if (kama__fibers.idleN < KAMA__FIBER_IDLE_MAX) {
        f->next = kama__fibers.idle; kama__fibers.idle = f; kama__fibers.idleN++;
        return;
    }
#endif
    kama__fiber_unmap(f);
}
#endif /* !KAMA_FIBER_NONE */

#endif /* !__EMSCRIPTEN__ */
#endif /* KAMA_FIBER_H */
