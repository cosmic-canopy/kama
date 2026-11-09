// timer.c — KR-3 step 8's brief (ROADMAP_DETAIL s6-windows-io): how a scheduler waiting on a completion port can sleep
// for less than the ~15.6 ms system tick. Windows only. Not the runtime.
//
// The arms, from the first argument:
//   tick     what a fiber's sleep is before step 8: `GetQueuedCompletionStatusEx` with the time left rounded up to whole
//            milliseconds, waited again until the deadline has passed (kama_poller_wait, kama_os.h).
//   packet   Go's 2024 answer (cf52e70): one high-resolution waitable timer per scheduler, made once, set to the deadline
//            and tied to the port by a wait completion packet (NtAssociateWaitCompletionPacket), then an INFINITE wait on
//            the port, which returns when the timer's packet arrives.
//   percall  a thread's sleep before step 8: a high-resolution waitable timer created, set, waited on and closed per call
//            (kama__sleep_os, kama_time.h).
//   reuse    the same, with one timer made once: what the per-call create costs a thread.
//   rearm    no sleep: what moving the packet arm's deadline costs (cancel the packet and the timer, set it, associate it
//            again), the work a scheduler does when a new earliest deadline arrives. Prints ns per re-arm.
//
// Usage: timer <arm> <asked us> <n>    prints: arm, asked us, n, min, p50, p90, p99, max (us); a min below the asked
// time is an early wake. For rearm: arm, n, ns per re-arm.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef LONG NTSTATUS_;
typedef NTSTATUS_ (NTAPI *CreateWaitPacketFn)(PHANDLE, ACCESS_MASK, PVOID);
typedef NTSTATUS_ (NTAPI *AssociateWaitPacketFn)(HANDLE, HANDLE, HANDLE, PVOID, PVOID, NTSTATUS_, ULONG_PTR, PBOOLEAN);
typedef NTSTATUS_ (NTAPI *CancelWaitPacketFn)(HANDLE, BOOLEAN);
static CreateWaitPacketFn NtCreateWaitCompletionPacket_;
static AssociateWaitPacketFn NtAssociateWaitCompletionPacket_;
static CancelWaitPacketFn NtCancelWaitCompletionPacket_;

static double freq;
static uint64_t now_ns(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * freq);
}
static int cmp64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y;
}
static void die(const char* what) { fprintf(stderr, "timer: %s failed (%lu)\n", what, GetLastError()); exit(2); }

#define TIMER_KEY 2

static HANDLE port, timer, packet;

static void arm_packet(uint64_t ns) {
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);        // relative, in 100 ns units, never early
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) die("SetWaitableTimer");
    BOOLEAN already = 0;
    NTSTATUS_ st = NtAssociateWaitCompletionPacket_(packet, port, timer, (PVOID)(uintptr_t)TIMER_KEY, NULL, 0, 0, &already);
    if (st != 0) { fprintf(stderr, "timer: associate 0x%lx\n", (unsigned long)st); exit(2); }
}

static void sleep_tick(uint64_t ns) {                       // kama_poller_wait's loop, with nothing else on the port
    uint64_t by = now_ns() + ns;
    OVERLAPPED_ENTRY e[8]; ULONG got;
    for (;;) {
        uint64_t t = now_ns();
        if (t >= by) return;
        DWORD wait = (DWORD)((by - t + 999999) / 1000000);
        GetQueuedCompletionStatusEx(port, e, 8, &got, wait, FALSE);
    }
}

static void sleep_packet(uint64_t ns) {
    arm_packet(ns);
    OVERLAPPED_ENTRY e[8]; ULONG got;
    for (;;) {
        if (!GetQueuedCompletionStatusEx(port, e, 8, &got, INFINITE, FALSE)) die("GetQueuedCompletionStatusEx");
        for (ULONG i = 0; i < got; i++) if (e[i].lpCompletionKey == TIMER_KEY) return;
    }
}

static void sleep_thread(uint64_t ns, int reuse) {
    HANDLE t = reuse ? timer : CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) die("CreateWaitableTimerExW");
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);
    SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(t, INFINITE);
    if (!reuse) CloseHandle(t);
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: timer tick|packet|percall|reuse|rearm <asked us> <n>\n"); return 2; }
    const char* arm = argv[1];
    uint64_t asked = strtoull(argv[2], NULL, 10) * 1000;
    int n = atoi(argv[3]);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = 1e9 / (double)f.QuadPart;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtCreateWaitCompletionPacket_ = (CreateWaitPacketFn)(void*)GetProcAddress(nt, "NtCreateWaitCompletionPacket");
    NtAssociateWaitCompletionPacket_ = (AssociateWaitPacketFn)(void*)GetProcAddress(nt, "NtAssociateWaitCompletionPacket");
    NtCancelWaitCompletionPacket_ = (CancelWaitPacketFn)(void*)GetProcAddress(nt, "NtCancelWaitCompletionPacket");
    if (!NtCreateWaitCompletionPacket_ || !NtAssociateWaitCompletionPacket_ || !NtCancelWaitCompletionPacket_) die("ntdll lookup");
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) die("CreateIoCompletionPort");
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) die("CreateWaitableTimerExW");
    if (NtCreateWaitCompletionPacket_(&packet, GENERIC_ALL, NULL) != 0) die("NtCreateWaitCompletionPacket");

    if (strcmp(arm, "rearm") == 0) {
        // A deadline an hour out, moved n times: cancel the pending packet, cancel the timer, set and associate again.
        arm_packet(3600ull * 1000000000ull);
        uint64_t t0 = now_ns();
        for (int i = 0; i < n; i++) {
            NtCancelWaitCompletionPacket_(packet, TRUE);
            CancelWaitableTimer(timer);
            arm_packet(3600ull * 1000000000ull - (uint64_t)i * 1000);
        }
        uint64_t spent = now_ns() - t0;
        printf("rearm\t%d\t%.0f\n", n, (double)spent / n);
        return 0;
    }

    uint64_t* s = calloc((size_t)n, sizeof *s);
    for (int i = 0; i < n; i++) {
        uint64_t t = now_ns();
        if (strcmp(arm, "tick") == 0) sleep_tick(asked);
        else if (strcmp(arm, "packet") == 0) sleep_packet(asked);
        else if (strcmp(arm, "percall") == 0) sleep_thread(asked, 0);
        else if (strcmp(arm, "reuse") == 0) sleep_thread(asked, 1);
        else { fprintf(stderr, "timer: unknown arm %s\n", arm); return 2; }
        s[i] = now_ns() - t;
    }
    qsort(s, (size_t)n, sizeof *s, cmp64);
    printf("%s\t%llu\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\n", arm, (unsigned long long)(asked / 1000), n,
           (unsigned long long)(s[0] / 1000), (unsigned long long)(s[n / 2] / 1000), (unsigned long long)(s[n * 9 / 10] / 1000),
           (unsigned long long)(s[n * 99 / 100] / 1000), (unsigned long long)(s[n - 1] / 1000));
    return 0;
}
