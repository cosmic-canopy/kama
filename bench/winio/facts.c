// facts.c — KR-3 step 8's brief (ROADMAP_DETAIL s6-windows-io): what a wait completion packet can deliver into a
// scheduler's completion port, checked rather than assumed, and what the small calls on a park cost. Windows only. Not
// the runtime.
//
// Each check prints one TSV row: check, result, detail.
//   port-*     Is a completion port a waitable object? `WaitForSingleObject` on a port, and a wait packet on port B
//              delivered into port A, before and after a packet is posted to B, and whether B's packet is still there
//              (a user `Poller` waited on from a fiber needs exactly that: told, not drained). Measured 2026-11-09 on
//              Windows 11 26200: the first works and leaves the packet queued; the association is refused with
//              STATUS_INVALID_PARAMETER_3, so the later port-packet rows do not run.
//   proc-*     A child's exit as a packet: a wait packet on the process handle, the child exiting later and already
//              exited.
//   event-*    An event set by another thread as a packet: what an overlapped operation's `hEvent` gives a scheduler
//              whose port the handle is not bound to.
//   *-ns       Latency of a delivery (median of N, another thread posts or sets and stamps the time) and the cost of the
//              calls an AFD park makes today and a packet arm would make.
//
// Usage: facts            (it runs itself as the child for the proc checks)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS_;
typedef NTSTATUS_ (NTAPI *CreateWaitPacketFn)(PHANDLE, ACCESS_MASK, PVOID);
typedef NTSTATUS_ (NTAPI *AssociateWaitPacketFn)(HANDLE, HANDLE, HANDLE, PVOID, PVOID, NTSTATUS_, ULONG_PTR, PBOOLEAN);
typedef NTSTATUS_ (NTAPI *CancelWaitPacketFn)(HANDLE, BOOLEAN);
static CreateWaitPacketFn NtCreateWaitCompletionPacket_;
static AssociateWaitPacketFn NtAssociateWaitCompletionPacket_;
static CancelWaitPacketFn NtCancelWaitCompletionPacket_;

static double freq;
static uint64_t now_ns(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * freq); }
static int cmp64(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }
static void row(const char* check, const char* result, const char* detail) { printf("%s\t%s\t%s\n", check, result, detail); fflush(stdout); }

static HANDLE new_packet(void) {
    HANDLE p; if (NtCreateWaitCompletionPacket_(&p, GENERIC_ALL, NULL) != 0) { fprintf(stderr, "facts: packet\n"); exit(2); }
    return p;
}
static NTSTATUS_ associate(HANDLE packet, HANDLE port, HANDLE target, uintptr_t key, BOOLEAN* already) {
    return NtAssociateWaitCompletionPacket_(packet, port, target, (PVOID)key, NULL, 0, 0, already);
}
// One packet off `port` within `ms`; answers its key, or 0 when nothing came.
static uintptr_t take(HANDLE port, DWORD ms) {
    OVERLAPPED_ENTRY e; ULONG got = 0;
    if (!GetQueuedCompletionStatusEx(port, &e, 1, &got, ms, FALSE) || got == 0) return 0;
    return e.lpCompletionKey;
}

// ---- latency: another thread signals, the main thread takes the packet ----------------------------------------------
enum { SIG_POST, SIG_PORT_RELAY, SIG_EVENT_PACKET };
static volatile LONG go;
static volatile uint64_t stamp;
static HANDLE lat_port, lat_inner, lat_event, lat_drained;
static int lat_kind;
// SIG_PORT_RELAY: since a port cannot be a wait packet's target, a thread waits on it (`WaitForSingleObject`, which
// leaves the packet queued) and posts to the scheduler's port; the woken side drains the inner port itself. This is the
// shape step 7's pool would give `Poller.wait` on a fiber, minus the pool's own hand-out.
static DWORD WINAPI relay(LPVOID arg) {
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        WaitForSingleObject(lat_inner, INFINITE);
        PostQueuedCompletionStatus(lat_port, 0, 8, NULL);
        WaitForSingleObject(lat_drained, INFINITE);
    }
    return 0;
}
static DWORD WINAPI signaller(LPVOID arg) {
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        while (InterlockedCompareExchange(&go, 0, 1) != 1) YieldProcessor();
        Sleep(0);
        stamp = now_ns();
        if (lat_kind == SIG_POST) PostQueuedCompletionStatus(lat_port, 0, 7, NULL);
        else if (lat_kind == SIG_PORT_RELAY) PostQueuedCompletionStatus(lat_inner, 0, 9, NULL);
        else SetEvent(lat_event);
    }
    return 0;
}
static void latency(const char* name, int kind, int n) {
    lat_kind = kind;
    lat_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    lat_inner = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    lat_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    lat_drained = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE packet = new_packet();
    uint64_t* s = calloc((size_t)n, sizeof *s);
    HANDLE th = CreateThread(NULL, 0, signaller, (LPVOID)(intptr_t)n, 0, NULL);
    HANDLE rt = kind == SIG_PORT_RELAY ? CreateThread(NULL, 0, relay, (LPVOID)(intptr_t)n, 0, NULL) : NULL;
    for (int i = 0; i < n; i++) {
        BOOLEAN already = 0;
        if (kind == SIG_EVENT_PACKET) associate(packet, lat_port, lat_event, 8, &already);
        InterlockedExchange(&go, 1);
        uintptr_t k = take(lat_port, 5000);
        uint64_t t = now_ns();
        s[i] = t - stamp;
        if (k == 0) { row(name, "FAIL", "no packet in 5 s"); return; }
        if (kind == SIG_PORT_RELAY) {
            if (take(lat_inner, 0) != 9) { row(name, "FAIL", "inner packet gone"); return; }
            SetEvent(lat_drained);
        }
    }
    WaitForSingleObject(th, INFINITE);
    if (rt) WaitForSingleObject(rt, INFINITE);
    qsort(s, (size_t)n, sizeof *s, cmp64);
    char d[96]; snprintf(d, sizeof d, "median %llu ns, p90 %llu ns, n %d", (unsigned long long)s[n / 2], (unsigned long long)s[n * 9 / 10], n);
    row(name, "ok", d);
}

// ---- cost per call ------------------------------------------------------------------------------------------------
static void cost(const char* name, uint64_t spent, int n) { char d[64]; snprintf(d, sizeof d, "%.0f ns per call, n %d", (double)spent / n, n); row(name, "ok", d); }

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "child") == 0) { Sleep((DWORD)atoi(argv[2])); return atoi(argv[3]); }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = 1e9 / (double)f.QuadPart;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtCreateWaitCompletionPacket_ = (CreateWaitPacketFn)(void*)GetProcAddress(nt, "NtCreateWaitCompletionPacket");
    NtAssociateWaitCompletionPacket_ = (AssociateWaitPacketFn)(void*)GetProcAddress(nt, "NtAssociateWaitCompletionPacket");
    NtCancelWaitCompletionPacket_ = (CancelWaitPacketFn)(void*)GetProcAddress(nt, "NtCancelWaitCompletionPacket");
    if (!NtCreateWaitCompletionPacket_ || !NtAssociateWaitCompletionPacket_ || !NtCancelWaitCompletionPacket_) { row("ntdll", "FAIL", "lookup"); return 2; }
    row("ntdll", "ok", "NtCreate/Associate/CancelWaitCompletionPacket resolved");

    // ---- a port as a waitable object ----
    HANDLE A = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE B = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    DWORD w = WaitForSingleObject(B, 0);
    row("port-wfso-empty", w == WAIT_TIMEOUT ? "unsignaled" : w == WAIT_OBJECT_0 ? "SIGNALED" : "error", "WaitForSingleObject on an empty port");
    PostQueuedCompletionStatus(B, 0, 5, NULL);
    w = WaitForSingleObject(B, 0);
    row("port-wfso-queued", w == WAIT_OBJECT_0 ? "signaled" : w == WAIT_TIMEOUT ? "UNSIGNALED" : "error", "WaitForSingleObject on a port holding one packet");
    uintptr_t k = take(B, 0);
    row("port-wfso-kept", k == 5 ? "kept" : "CONSUMED", "the packet is still on the port after the wait");
    HANDLE packet = new_packet();
    BOOLEAN already = 0;
    NTSTATUS_ st = associate(packet, A, B, 8, &already);
    char d[128]; snprintf(d, sizeof d, "status 0x%lx, already %d, empty B", (unsigned long)st, already);
    row("port-packet-associate", st == 0 ? "ok" : "REFUSED", d);
    if (st != 0) goto proc;                              // 0xc00000f1 is STATUS_INVALID_PARAMETER_3: a port is no target
    k = take(A, 50);
    row("port-packet-quiet", k == 0 ? "quiet" : "FIRED", "nothing on A while B is empty");
    PostQueuedCompletionStatus(B, 0, 5, NULL);
    k = take(A, 1000);
    row("port-packet-fires", k == 8 ? "fired" : "SILENT", "a post to B puts the packet on A");
    k = take(B, 0);
    row("port-packet-kept", k == 5 ? "kept" : "CONSUMED", "B's packet is still there to drain");
    PostQueuedCompletionStatus(B, 0, 5, NULL);
    st = associate(packet, A, B, 8, &already);
    k = take(A, 1000);
    snprintf(d, sizeof d, "already %d", already);
    row("port-packet-already", k == 8 ? "fired" : "SILENT", d);
    take(B, 0);
    // A port waited on twice by one packet: does a second post while the first is undrained fire again?
    st = associate(packet, A, B, 8, &already);
    PostQueuedCompletionStatus(B, 0, 5, NULL);
    PostQueuedCompletionStatus(B, 0, 6, NULL);
    k = take(A, 1000);
    uintptr_t k2 = take(A, 50);
    row("port-packet-once", k == 8 && k2 == 0 ? "once" : "TWICE", "two posts to B, one association: one packet on A");
    take(B, 0); take(B, 0);

proc:
    // ---- a child's exit ----
    ;
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64];
    for (int late = 0; late < 2; late++) {
        swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" child %d 7", self, late ? 0 : 50);
        STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { row("proc", "FAIL", "CreateProcessW"); return 2; }
        CloseHandle(pi.hThread);
        if (late) WaitForSingleObject(pi.hProcess, INFINITE);       // exited before the association
        uint64_t t0 = now_ns();
        st = associate(packet, A, pi.hProcess, 11, &already);
        k = take(A, 5000);
        uint64_t spent = now_ns() - t0;
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        snprintf(d, sizeof d, "status 0x%lx, already %d, exit code %lu, %.1f ms after association", (unsigned long)st, already, code, (double)spent / 1e6);
        row(late ? "proc-exit-already" : "proc-exit-later", k == 11 && code == 7 ? "fired" : "FAIL", d);
        CloseHandle(pi.hProcess);
    }

    // ---- an event set elsewhere ----
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    st = associate(packet, A, ev, 12, &already);
    SetEvent(ev);
    k = take(A, 1000);
    w = WaitForSingleObject(ev, 0);
    row("event-packet", k == 12 ? "fired" : "SILENT", w == WAIT_TIMEOUT ? "an auto-reset event is reset by the delivery" : "the event stays set after delivery");

    // ---- latency of each delivery ----
    latency("post-ns", SIG_POST, 2000);
    latency("port-relay-ns", SIG_PORT_RELAY, 2000);
    latency("event-packet-ns", SIG_EVENT_PACKET, 2000);

    // ---- cost per call ----
    const int N = 200000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) { associate(packet, A, ev, 12, &already); NtCancelWaitCompletionPacket_(packet, TRUE); }
    cost("associate+cancel-ns", now_ns() - t0, N);
    t0 = now_ns();
    for (int i = 0; i < N; i++) { HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL); CloseHandle(e); }
    cost("event-create+close-ns", now_ns() - t0, N);
    t0 = now_ns();
    for (int i = 0; i < N; i++) { void* p = malloc(160); *(volatile char*)p = 1; free(p); }
    cost("malloc+free-ns", now_ns() - t0, N);
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKET base; DWORD bytes;
    t0 = now_ns();
    for (int i = 0; i < N; i++) WSAIoctl(s, SIO_BASE_HANDLE, NULL, 0, &base, sizeof base, &bytes, NULL, NULL);
    cost("sio-base-handle-ns", now_ns() - t0, N);
    snprintf(d, sizeof d, "base %s the socket", base == s ? "is" : "differs from");
    row("sio-base-handle", "ok", d);
    return 0;
}
