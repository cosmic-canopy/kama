// complete.c — KR-3 step 8's brief (ROADMAP_DETAIL s6-windows-io): where an overlapped operation's completion should
// land when each scheduler has its own completion port and a handle can move between schedulers. Windows only. Not the
// runtime.
//
// Two threads stand for two isolates, each with its own port. They play ping-pong over one overlapped named pipe, one
// byte each way, each thread reading and writing its own end. The arms, from the first argument:
//   assoc       each end bound to its owner's port (CreateIoCompletionPort): Go's and libuv's shape, one port per process
//               there. A handle joins one port for life, so an end handed to another isolate keeps completing on the
//               first one's port.
//   assoc-skip  the same, with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: an operation that finishes at once posts no packet.
//   event       no end is bound to any port. Each operation carries an event in its OVERLAPPED; one that pends gets a
//               wait completion packet on that event, into the CURRENT owner's port. Nothing is bound, so a hand-off
//               costs nothing.
//   router      every end bound to one process-wide port; a router thread takes each completion and posts it to the
//               port of the scheduler that owns the operation, named in its record. Survives a hand-off too.
//   blocking    two anonymous pipes read and written blocking: a thread's pipe today (kama_pipe is the CRT's `_pipe`).
//   thread      the overlapped named pipe on threads with no scheduler: each operation carries its event and a thread
//               waits on that event, no port at all. What a thread's `Command` pipe becomes once the pipe is named.
//
// Usage: complete <arm> <round trips>    prints: arm, round trips, ns per round trip, CPU ns per round trip (process,
// user + kernel). Run the arms interleaved.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS_;
typedef NTSTATUS_ (NTAPI *CreateWaitPacketFn)(PHANDLE, ACCESS_MASK, PVOID);
typedef NTSTATUS_ (NTAPI *AssociateWaitPacketFn)(HANDLE, HANDLE, HANDLE, PVOID, PVOID, NTSTATUS_, ULONG_PTR, PBOOLEAN);
static CreateWaitPacketFn NtCreateWaitCompletionPacket_;
static AssociateWaitPacketFn NtAssociateWaitCompletionPacket_;

enum { ASSOC, ASSOC_SKIP, EVENT, ROUTER, BLOCKING, THREAD };
static int arm;
static int trips;
static HANDLE global_port;

typedef struct {
    OVERLAPPED ov;                 // first: a completion's lpOverlapped is the record
    HANDLE owner_port;             // where the router sends it
} op_t;

typedef struct {
    HANDLE end;                    // the overlapped end (or, blocking: the read end)
    HANDLE wend;                   // blocking only: the write end
    HANDLE port, event, packet;
    int ping;
} side_t;

static void die(const char* what) { fprintf(stderr, "complete: %s failed (%lu)\n", what, GetLastError()); exit(2); }

static void wait_for(side_t* s, op_t* op) {        // take packets off the owner's port until this operation's arrives
    OVERLAPPED_ENTRY e[4]; ULONG got;
    for (;;) {
        if (!GetQueuedCompletionStatusEx(s->port, e, 4, &got, INFINITE, FALSE)) die("GetQueuedCompletionStatusEx");
        for (ULONG i = 0; i < got; i++) if (e[i].lpOverlapped == &op->ov || e[i].lpCompletionKey == 3) return;
    }
}

static void io(side_t* s, int write) {
    uint8_t b = 1;
    if (arm == BLOCKING) {
        DWORD n; BOOL ok = write ? WriteFile(s->wend, &b, 1, &n, NULL) : ReadFile(s->end, &b, 1, &n, NULL);
        if (!ok || n != 1) die("blocking io");
        return;
    }
    op_t op; memset(&op, 0, sizeof op);
    op.owner_port = s->port;
    if (arm == EVENT || arm == THREAD) op.ov.hEvent = s->event;
    BOOL ok = write ? WriteFile(s->end, &b, 1, NULL, &op.ov) : ReadFile(s->end, &b, 1, NULL, &op.ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) die(write ? "WriteFile" : "ReadFile");
    if (arm == ASSOC || arm == ROUTER) { wait_for(s, &op); return; }      // a packet comes either way
    if (ok) return;                                                      // finished at once: no packet (skip, event)
    if (arm == THREAD) {                                                 // no port: the thread waits on the event
        DWORD n;
        if (!GetOverlappedResult(s->end, &op.ov, &n, TRUE) || n != 1) die("GetOverlappedResult");
        return;
    }
    if (arm == EVENT) {
        BOOLEAN already;
        if (NtAssociateWaitCompletionPacket_(s->packet, s->port, s->event, (PVOID)3, &op.ov, 0, 0, &already) != 0) die("associate");
    }
    wait_for(s, &op);
    DWORD n;
    if (!GetOverlappedResult(s->end, &op.ov, &n, FALSE) || n != 1) die("GetOverlappedResult");
}

static DWORD WINAPI play(LPVOID arg) {
    side_t* s = arg;
    for (int i = 0; i < trips; i++) {
        if (s->ping) { io(s, 1); io(s, 0); } else { io(s, 0); io(s, 1); }
    }
    return 0;
}

static DWORD WINAPI router(LPVOID arg) {
    (void)arg;
    OVERLAPPED_ENTRY e[16]; ULONG got;
    for (;;) {
        if (!GetQueuedCompletionStatusEx(global_port, e, 16, &got, INFINITE, FALSE)) return 0;
        for (ULONG i = 0; i < got; i++) {
            if (e[i].lpCompletionKey == 99) return 0;
            op_t* op = (op_t*)e[i].lpOverlapped;
            PostQueuedCompletionStatus(op->owner_port, e[i].dwNumberOfBytesTransferred, 1, &op->ov);
        }
    }
}

static uint64_t cpu_ns(void) {
    FILETIME c, x, k, u; GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: complete assoc|assoc-skip|event|router|blocking|thread <round trips>\n"); return 2; }
    const char* a = argv[1];
    arm = !strcmp(a, "assoc") ? ASSOC : !strcmp(a, "assoc-skip") ? ASSOC_SKIP : !strcmp(a, "event") ? EVENT
        : !strcmp(a, "router") ? ROUTER : !strcmp(a, "blocking") ? BLOCKING : !strcmp(a, "thread") ? THREAD : -1;
    if (arm < 0) { fprintf(stderr, "complete: unknown arm %s\n", a); return 2; }
    trips = atoi(argv[2]);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtCreateWaitCompletionPacket_ = (CreateWaitPacketFn)(void*)GetProcAddress(nt, "NtCreateWaitCompletionPacket");
    NtAssociateWaitCompletionPacket_ = (AssociateWaitPacketFn)(void*)GetProcAddress(nt, "NtAssociateWaitCompletionPacket");

    side_t s[2]; memset(s, 0, sizeof s);
    s[0].ping = 1;
    if (arm == BLOCKING) {
        HANDLE r1, w1, r2, w2;
        if (!CreatePipe(&r1, &w1, NULL, 4096) || !CreatePipe(&r2, &w2, NULL, 4096)) die("CreatePipe");
        s[0].wend = w1; s[1].end = r1; s[1].wend = w2; s[0].end = r2;
    } else {
        wchar_t name[96];
        swprintf(name, 96, L"\\\\.\\pipe\\kama-bench-complete-%lu-%lu", GetCurrentProcessId(), GetTickCount());
        s[0].end = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, NULL);
        if (s[0].end == INVALID_HANDLE_VALUE) die("CreateNamedPipeW");
        s[1].end = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (s[1].end == INVALID_HANDLE_VALUE) die("CreateFileW");
        if (arm == ROUTER) global_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        for (int i = 0; i < 2; i++) {
            s[i].port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            s[i].event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (NtCreateWaitCompletionPacket_(&s[i].packet, GENERIC_ALL, NULL) != 0) die("NtCreateWaitCompletionPacket");
            if (arm == ASSOC || arm == ASSOC_SKIP) { if (!CreateIoCompletionPort(s[i].end, s[i].port, 1, 0)) die("bind"); }
            if (arm == ROUTER) { if (!CreateIoCompletionPort(s[i].end, global_port, 1, 0)) die("bind global"); }
            if (arm == ASSOC_SKIP && !SetFileCompletionNotificationModes(s[i].end, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) die("skip");
        }
    }
    HANDLE rt = arm == ROUTER ? CreateThread(NULL, 0, router, NULL, 0, NULL) : NULL;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    uint64_t c0 = cpu_ns();
    QueryPerformanceCounter(&t0);
    HANDLE th[2];
    for (int i = 0; i < 2; i++) th[i] = CreateThread(NULL, 0, play, &s[i], 0, NULL);
    WaitForMultipleObjects(2, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    uint64_t c1 = cpu_ns();
    if (rt) { PostQueuedCompletionStatus(global_port, 0, 99, NULL); WaitForSingleObject(rt, INFINITE); }
    double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / trips;
    printf("%s\t%d\t%.0f\t%.0f\n", a, trips, ns, (double)(c1 - c0) / trips);
    return 0;
}
