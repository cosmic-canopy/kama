// transmit.c — KR-3 step 8's brief (ROADMAP_DETAIL s6-windows-io), KR-229: `TcpStream.sendFile` on Windows, today's copy
// against `TransmitFile`, over loopback TCP. Windows only. Not the runtime.
//
// Modes, from the first argument:
//   copy <C> <size> <reps>    what sendFile does on Windows before step 8 (`sendFileCopy`, net.kama): 8 KiB read at an
//                             offset, then `send` it all, per chunk. C senders at once, each to its own reader.
//   tf <C> <size> <reps>      overlapped `TransmitFile` of the whole range (chunks below 2^31 - 2 bytes), the sender
//                             waiting on the OVERLAPPED's event as a thread would; a fiber would take the same event as a
//                             wait packet (complete.c).
//   both print: mode, C, size, reps, total MB/s, median and max ms per send, process CPU ms per GB sent.
//   cap <C>                   C senders start a 64 MiB TransmitFile each, at once, to readers that read nothing for
//                             300 ms; then prints the bytes waiting at each reader. A reader with nothing waiting is one
//                             whose sender has not started: Windows' client editions run two TransmitFiles at a time.
//   cancel                    a 64 MiB TransmitFile to a reader that never reads, cancelled after 100 ms with CancelIoEx:
//                             the error, the bytes it reports, and whether the socket can still send.
//   pos                       whether a TransmitFile at an offset given in its OVERLAPPED moves the file's position.
//   again                     a second TransmitFile of the same range on one socket, with the position left where the
//                             first moved it and with it set back to the offset first.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t path[MAX_PATH];
static uint64_t size;
static int reps;
static SOCKET lsock;
static uint16_t port;

static void die(const char* what) { fprintf(stderr, "transmit: %s failed (%lu, wsa %d)\n", what, GetLastError(), WSAGetLastError()); exit(2); }
static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double now_ms(void) { static double f; LARGE_INTEGER c; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = 1e3 / (double)q.QuadPart; } QueryPerformanceCounter(&c); return (double)c.QuadPart * f; }
static uint64_t cpu_ns(void) {
    FILETIME c, x, k, u; GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
}

static void make_file(uint64_t n) {
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%lskama-bench-transmit-%lu.bin", dir, GetCurrentProcessId());
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) die("create file");
    static uint8_t chunk[1 << 16];
    for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = (uint8_t)(i * 31 + 7);
    for (uint64_t w = 0; w < n; ) { DWORD k = (DWORD)(n - w < sizeof chunk ? n - w : sizeof chunk), d; WriteFile(h, chunk, k, &d, NULL); w += d; }
    CloseHandle(h);
    size = n;
}
static HANDLE open_file(void) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) die("open file");
    return h;
}

static void listen_any(void) {
    lsock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lsock, (struct sockaddr*)&a, sizeof a) || listen(lsock, 64)) die("listen");
    int len = sizeof a; getsockname(lsock, (struct sockaddr*)&a, &len); port = ntohs(a.sin_port);
}
// A connected pair: `out` sends (kama's sockets are all WSA_FLAG_OVERLAPPED), `in` reads.
static void pair(SOCKET* out, SOCKET* in) {
    *in = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (connect(*in, (struct sockaddr*)&a, sizeof a)) die("connect");
    *out = accept(lsock, NULL, NULL);
    if (*out == INVALID_SOCKET) die("accept");
}

typedef struct { SOCKET s; uint64_t want; uint64_t got; } reader_t;
static DWORD WINAPI reader(LPVOID arg) {
    reader_t* r = arg; char buf[1 << 16];
    while (r->got < r->want) { int n = recv(r->s, buf, sizeof buf, 0); if (n <= 0) break; r->got += (uint64_t)n; }
    return 0;
}

// Sends `len` bytes from `off` by overlapped TransmitFile, in chunks under its 2,147,483,646-byte limit, and waits.
// The file's position is set to the offset first: TransmitFile leaves it somewhere past the start (32 KiB in, for a whole
// 1 MiB file), and the next call from an offset the position is past fails with WSAEINVAL (`again` shows it). `pos` and
// `again`'s first half set `no_seek`, to see TransmitFile alone.
static int no_seek;
static BOOL transmit(SOCKET s, HANDLE f, uint64_t off, uint64_t len, HANDLE ev, DWORD* err) {
    while (len > 0) {
        LARGE_INTEGER at; at.QuadPart = (LONGLONG)off; if (!no_seek) SetFilePointerEx(f, at, NULL, FILE_BEGIN);
        DWORD chunk = (DWORD)(len > 0x7FFFF000ull ? 0x7FFFF000ull : len);
        OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32); ov.hEvent = ev;
        if (!TransmitFile(s, f, chunk, 0, &ov, NULL, 0) && WSAGetLastError() != ERROR_IO_PENDING) { *err = (DWORD)WSAGetLastError(); return FALSE; }
        DWORD sent, flags;
        if (!WSAGetOverlappedResult(s, &ov, &sent, TRUE, &flags)) { *err = (DWORD)WSAGetLastError(); return FALSE; }
        off += sent; len -= sent;
    }
    return TRUE;
}

typedef struct { SOCKET s; int tf; double* ms; } sender_t;
static DWORD WINAPI sender(LPVOID arg) {
    sender_t* w = arg;
    HANDLE f = open_file(), ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    char buf[8192];
    for (int i = 0; i < reps; i++) {
        double t = now_ms();
        if (w->tf) { DWORD err; if (!transmit(w->s, f, 0, size, ev, &err)) { SetLastError(err); die("TransmitFile"); } }
        else {
            for (uint64_t off = 0; off < size; ) {
                OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
                DWORD k = (DWORD)(size - off < sizeof buf ? size - off : sizeof buf), got;
                if (!ReadFile(f, buf, k, &got, &ov) || got == 0) die("ReadFile");
                for (DWORD sent = 0; sent < got; ) { int n = send(w->s, buf + sent, (int)(got - sent), 0); if (n <= 0) die("send"); sent += (DWORD)n; }
                off += got;
            }
        }
        w->ms[i] = now_ms() - t;
    }
    CloseHandle(f); CloseHandle(ev);
    return 0;
}

static void bulk(const char* mode, int c) {
    int tf = !strcmp(mode, "tf");
    SOCKET* out = calloc((size_t)c, sizeof *out); reader_t* rd = calloc((size_t)c, sizeof *rd); sender_t* sd = calloc((size_t)c, sizeof *sd);
    double* ms = calloc((size_t)c * (size_t)reps, sizeof *ms);
    HANDLE* th = calloc((size_t)c * 2, sizeof *th);
    for (int i = 0; i < c; i++) { pair(&out[i], &rd[i].s); rd[i].want = size * (uint64_t)reps; sd[i].s = out[i]; sd[i].tf = tf; sd[i].ms = ms + (size_t)i * (size_t)reps; }
    uint64_t c0 = cpu_ns(); double t0 = now_ms();
    for (int i = 0; i < c; i++) th[i] = CreateThread(NULL, 0, reader, &rd[i], 0, NULL);
    for (int i = 0; i < c; i++) th[c + i] = CreateThread(NULL, 0, sender, &sd[i], 0, NULL);
    WaitForMultipleObjects((DWORD)(2 * c), th, TRUE, INFINITE);
    double wall = now_ms() - t0; uint64_t c1 = cpu_ns();
    for (int i = 0; i < c; i++) if (rd[i].got != rd[i].want) die("short read");
    int n = c * reps; qsort(ms, (size_t)n, sizeof *ms, cmpd);
    double gb = (double)size * n / 1e9;
    printf("%s\t%d\t%llu\t%d\t%.0f\t%.2f\t%.2f\t%.0f\n", mode, c, (unsigned long long)size, reps, gb * 1e6 / wall,
           ms[n / 2], ms[n - 1], (double)(c1 - c0) / 1e6 / gb);
    for (int i = 0; i < c; i++) { closesocket(out[i]); closesocket(rd[i].s); }
}

typedef struct { SOCKET s; HANDLE f, ev; DWORD err; BOOL ok; } tf_job;
static DWORD WINAPI tf_thread(LPVOID arg) { tf_job* j = arg; j->ok = transmit(j->s, j->f, 0, size, j->ev, &j->err); return 0; }

int main(int argc, char** argv) {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    if (argc < 2) { fprintf(stderr, "usage: transmit copy|tf <C> <size> <reps> | cap <C> | cancel | pos\n"); return 2; }
    listen_any();
    const char* mode = argv[1];
    if (!strcmp(mode, "copy") || !strcmp(mode, "tf")) {
        if (argc < 5) return 2;
        reps = atoi(argv[4]); make_file(strtoull(argv[3], NULL, 10)); bulk(mode, atoi(argv[2]));
    } else if (!strcmp(mode, "cap")) {
        int c = argc > 2 ? atoi(argv[2]) : 4;
        make_file(64ull << 20);
        tf_job* j = calloc((size_t)c, sizeof *j); SOCKET* in = calloc((size_t)c, sizeof *in); HANDLE* th = calloc((size_t)c, sizeof *th);
        for (int i = 0; i < c; i++) {
            pair(&j[i].s, &in[i]);
            int small = 64 << 10; setsockopt(in[i], SOL_SOCKET, SO_RCVBUF, (char*)&small, sizeof small);
            j[i].f = open_file(); j[i].ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        }
        for (int i = 0; i < c; i++) th[i] = CreateThread(NULL, 0, tf_thread, &j[i], 0, NULL);
        Sleep(300);
        printf("cap\t%d\twaiting bytes per reader after 300 ms:", c);
        for (int i = 0; i < c; i++) { u_long q = 0; ioctlsocket(in[i], FIONREAD, &q); printf(" %lu", q); }
        printf("\n");
        reader_t* rd = calloc((size_t)c, sizeof *rd); HANDLE* rt = calloc((size_t)c, sizeof *rt);
        double t0 = now_ms();
        for (int i = 0; i < c; i++) { rd[i].s = in[i]; rd[i].want = size; rt[i] = CreateThread(NULL, 0, reader, &rd[i], 0, NULL); }
        WaitForMultipleObjects((DWORD)c, th, TRUE, INFINITE); WaitForMultipleObjects((DWORD)c, rt, TRUE, INFINITE);
        int ok = 0; for (int i = 0; i < c; i++) ok += j[i].ok && rd[i].got == size;
        printf("cap\t%d\tall done in %.0f ms once read, %d of %d whole\n", c, now_ms() - t0, ok, c);
    } else if (!strcmp(mode, "cancel")) {
        make_file(64ull << 20);
        SOCKET out, in; pair(&out, &in);
        HANDLE f = open_file(), ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = ev;
        if (!TransmitFile(out, f, (DWORD)size, 0, &ov, NULL, 0) && WSAGetLastError() != ERROR_IO_PENDING) die("TransmitFile");
        Sleep(100);
        BOOL c = CancelIoEx((HANDLE)out, &ov);
        DWORD sent = 0, flags = 0; BOOL ok = WSAGetOverlappedResult(out, &ov, &sent, TRUE, &flags);
        int err = ok ? 0 : WSAGetLastError();
        int n = send(out, "x", 1, 0);
        printf("cancel\tCancelIoEx %s; result %s, error %d, %lu bytes reported; a send after it %s (%d)\n",
               c ? "ok" : "failed", ok ? "ok" : "failed", err, sent, n == 1 ? "works" : "fails", n == 1 ? 0 : WSAGetLastError());
    } else if (!strcmp(mode, "again")) {
        // A second TransmitFile of the same range on one socket, as a server sending one file twice does: with the file's
        // position left where the first moved it, and with it set back to the offset first.
        make_file(1 << 20);
        for (int seek = 0; seek < 2; seek++) {
            SOCKET out, in; pair(&out, &in);
            HANDLE f = open_file(), ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            reader_t rd = { in, 3u << 20, 0 }; HANDLE rt = CreateThread(NULL, 0, reader, &rd, 0, NULL);
            printf("again\t%s:", seek ? "position set back first" : "position left");
            no_seek = !seek;
            for (int i = 0; i < 3; i++) {
                LARGE_INTEGER zero = {0}, at; SetFilePointerEx(f, zero, &at, FILE_CURRENT);
                DWORD err = 0; BOOL ok = transmit(out, f, 0, size, ev, &err);
                printf(" call %d, the position found at %lld, %s (%lu);", i + 1, at.QuadPart, ok ? "ok" : "fails", err);
            }
            closesocket(out); WaitForSingleObject(rt, 2000);
            printf(" %llu bytes read\n", (unsigned long long)rd.got);
            closesocket(in); CloseHandle(f); CloseHandle(ev);
        }
    } else if (!strcmp(mode, "pos")) {
        make_file(1 << 20);
        SOCKET out, in; pair(&out, &in);
        HANDLE f = open_file(), ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        LARGE_INTEGER zero = {0}, before, after; SetFilePointerEx(f, zero, &before, FILE_CURRENT);
        reader_t rd = { in, 1 << 19, 0 }; HANDLE rt = CreateThread(NULL, 0, reader, &rd, 0, NULL);
        no_seek = 1;
        DWORD err; if (!transmit(out, f, 1 << 19, 1 << 19, ev, &err)) { SetLastError(err); die("TransmitFile"); }
        WaitForSingleObject(rt, INFINITE);
        SetFilePointerEx(f, zero, &after, FILE_CURRENT);
        printf("pos\tposition %lld before, %lld after a 512 KiB TransmitFile from offset 512 KiB; %llu bytes read\n",
               before.QuadPart, after.QuadPart, (unsigned long long)rd.got);
    } else { fprintf(stderr, "transmit: unknown mode %s\n", mode); return 2; }
    DeleteFileW(path);
    return 0;
}
