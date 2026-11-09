// pipes.c — KR-3 step 8's brief (ROADMAP_DETAIL s6-windows-io): a child's piped ends as overlapped named pipes (ruled
// 2026-10-30), and what a pipeline does with one, since the next child is handed the parent's overlapped end. Windows
// only. Not the runtime.
//
// Modes, from the first argument:
//   reopen       a named pipe pair as Rust's std makes it (the parent's end FILE_FLAG_OVERLAPPED, the child's not):
//                does `ReOpenFile` on either end succeed, and is the new handle synchronous (FileModeInformation)?
//                `DuplicateHandle` for comparison, which shares the original's file object and so its mode.
//   child <how> <n>   n runs each of a child that reads all of its stdin through the C library (`_read`, which is
//                `ReadFile` with no OVERLAPPED) and checks it, and of one that writes 8 MiB through `fwrite` to stdout,
//                checked by the parent. <how> is the handle the child is given:
//                  overlapped  the parent's overlapped end itself: a pipeline that hands it on unchanged
//                  reopened    a `ReOpenFile` of it with no FILE_FLAG_OVERLAPPED
//                  anon        the CRT's `_pipe`, as before step 8
//                Prints pass counts. A system tool too: `findstr /n "^"` reading the overlapped end as its stdin.
//   rate         256 MiB through each kind of pipe, written in 64 KiB blocks by one thread and read by another:
//                `_pipe` read blocking; the named pipe read overlapped, the reader waiting on its event; and a relay,
//                Rust's answer for a pipeline: a thread copying from the overlapped end into a fresh anonymous pipe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS_;
typedef struct { union { NTSTATUS_ Status; PVOID Pointer; }; ULONG_PTR Information; } IOSB_;
typedef NTSTATUS_ (NTAPI *QueryInfoFn)(HANDLE, IOSB_*, PVOID, ULONG, int);
static QueryInfoFn NtQueryInformationFile_;
#define FILE_MODE_INFORMATION_CLASS 16
#ifndef FILE_SYNCHRONOUS_IO_ALERT
#define FILE_SYNCHRONOUS_IO_ALERT 0x10
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#endif

static void die(const char* what) { fprintf(stderr, "pipes: %s failed (%lu)\n", what, GetLastError()); exit(2); }

static const char* mode_of(HANDLE h) {
    IOSB_ io; ULONG mode = 0;
    if (NtQueryInformationFile_(h, &io, &mode, sizeof mode, FILE_MODE_INFORMATION_CLASS) != 0) return "unknown";
    return (mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) ? "synchronous" : "overlapped";
}

static int seq;
// A pair: `*parent` overlapped, `*child` synchronous and inheritable. inbound: the parent reads (a child's stdout).
static void named_pair(int inbound, HANDLE* parent, HANDLE* child) {
    wchar_t name[96];
    swprintf(name, 96, L"\\\\.\\pipe\\kama-bench-pipes-%lu-%d", GetCurrentProcessId(), seq++);
    *parent = CreateNamedPipeW(name, (inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) die("CreateNamedPipeW");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    *child = CreateFileW(name, inbound ? GENERIC_WRITE : GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (*child == INVALID_HANDLE_VALUE) die("CreateFileW");
}

static void reopen(void) {
    HANDLE p, c;
    named_pair(1, &p, &c);
    printf("reopen\tparent end (server)\t%s\n", mode_of(p));
    printf("reopen\tchild end (client)\t%s\n", mode_of(c));
    DWORD shares[2] = { 0, FILE_SHARE_READ | FILE_SHARE_WRITE };
    for (int i = 0; i < 2; i++) {
        HANDLE r = ReOpenFile(p, GENERIC_READ, shares[i], 0);
        if (r == INVALID_HANDLE_VALUE) printf("reopen\tReOpenFile(server, share %lu)\tfails, error %lu\n", shares[i], GetLastError());
        else {
            DWORD n = 0; char b[4]; WriteFile(c, "abc", 3, &n, NULL);
            BOOL ok = ReadFile(r, b, 3, &n, NULL);
            printf("reopen\tReOpenFile(server, share %lu)\t%s; a synchronous read %s %lu bytes\n", shares[i], mode_of(r), ok ? "returns" : "fails,", n);
            CloseHandle(r);
        }
    }
    HANDLE r = ReOpenFile(c, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (r == INVALID_HANDLE_VALUE) printf("reopen\tReOpenFile(client)\tfails, error %lu\n", GetLastError());
    else { printf("reopen\tReOpenFile(client)\t%s\n", mode_of(r)); CloseHandle(r); }
    // Flipping the mode in place: FileModeInformation set to synchronous on the overlapped end.
    typedef NTSTATUS_ (NTAPI *SetInfoFn)(HANDLE, IOSB_*, PVOID, ULONG, int);
    SetInfoFn set = (SetInfoFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
    IOSB_ io; ULONG want = FILE_SYNCHRONOUS_IO_NONALERT;
    NTSTATUS_ st = set(p, &io, &want, sizeof want, FILE_MODE_INFORMATION_CLASS);
    printf("reopen\tNtSetInformationFile(server, FileModeInformation = synchronous)\tstatus 0x%lx, now %s\n", (unsigned long)st, mode_of(p));
    HANDLE d;
    if (DuplicateHandle(GetCurrentProcess(), p, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        printf("reopen\tDuplicateHandle(server)\t%s\n", mode_of(d)); CloseHandle(d);
    }
    // The outbound direction: the parent writes, the child reads (a child's stdin).
    HANDLE po, co; named_pair(0, &po, &co);
    r = ReOpenFile(po, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (r == INVALID_HANDLE_VALUE) printf("reopen\tReOpenFile(outbound server)\tfails, error %lu\n", GetLastError());
    else { printf("reopen\tReOpenFile(outbound server)\t%s\n", mode_of(r)); CloseHandle(r); }
}

// ---- children -----------------------------------------------------------------------------------------------------
#define CHILD_BYTES (8u << 20)
static uint8_t pat(uint32_t i) { return (uint8_t)(i * 2654435761u >> 24); }

static int child_read(void) {                              // reads stdin to its end through the CRT, checks the bytes
    _setmode(0, _O_BINARY);
    static uint8_t buf[1 << 15]; uint32_t at = 0; int bad = 0;
    for (;;) {
        int n = _read(0, buf, (unsigned)(1 + (at % 7) * 4000));  // uneven sizes, as a real reader's are
        if (n < 0) return 3;
        if (n == 0) break;
        for (int i = 0; i < n; i++) if (buf[i] != pat(at + (uint32_t)i)) bad = 1;
        at += (uint32_t)n;
    }
    return bad ? 1 : at == CHILD_BYTES ? 0 : 2;
}
static int child_write(void) {                             // writes the pattern to stdout through fwrite
    _setmode(1, _O_BINARY);
    static uint8_t buf[CHILD_BYTES];
    for (uint32_t i = 0; i < CHILD_BYTES; i++) buf[i] = pat(i);
    for (uint32_t at = 0; at < CHILD_BYTES; ) { size_t k = 1 + (at % 5) * 3000; if (k > CHILD_BYTES - at) k = CHILD_BYTES - at; if (fwrite(buf + at, 1, k, stdout) != k) return 3; at += (uint32_t)k; }
    return fflush(stdout) == 0 ? 0 : 3;
}

static HANDLE spawn(const wchar_t* cmdline, HANDLE in, HANDLE out) {
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in; si.hStdOutput = out; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t cmd[1024]; wcscpy(cmd, cmdline);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) die("CreateProcessW");
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static void inheritable(HANDLE h) { SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT); }
static HANDLE nul_out(void) { SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL); }

// The child-facing handle for `how`: the overlapped end itself, a synchronous reopen of it, or a `_pipe` end. `keep` is
// the other end, synchronous, for the parent to use; `give` is what the child gets.
static void make(const char* how, int child_reads, HANDLE* give, HANDLE* keep) {
    if (!strcmp(how, "anon")) {
        HANDLE r, w; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        if (!CreatePipe(&r, &w, &sa, 65536)) die("CreatePipe");
        *give = child_reads ? r : w; *keep = child_reads ? w : r;
        SetHandleInformation(*keep, HANDLE_FLAG_INHERIT, 0);
        return;
    }
    // A pipeline's middle: the parent's overlapped end is what the next child gets. The other end, synchronous, stays.
    HANDLE ov, sync; named_pair(child_reads, &ov, &sync);       // inbound: the overlapped end reads
    SetHandleInformation(sync, HANDLE_FLAG_INHERIT, 0);
    *keep = sync;
    if (!strcmp(how, "overlapped")) { inheritable(ov); *give = ov; return; }
    HANDLE r = ReOpenFile(ov, child_reads ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (r == INVALID_HANDLE_VALUE) die("ReOpenFile");
    CloseHandle(ov);
    inheritable(r); *give = r;
}

static int one_read(const wchar_t* self, const char* how) {
    HANDLE give, keep; make(how, 1, &give, &keep);
    wchar_t cmd[600]; swprintf(cmd, 600, L"\"%ls\" -child-read", self);
    HANDLE nul = nul_out();
    HANDLE pr = spawn(cmd, give, nul);
    CloseHandle(give); CloseHandle(nul);
    static uint8_t buf[CHILD_BYTES];
    for (uint32_t i = 0; i < CHILD_BYTES; i++) buf[i] = pat(i);
    for (uint32_t at = 0; at < CHILD_BYTES; ) { DWORD k = 1 + (at % 3) * 9000, n; if (k > CHILD_BYTES - at) k = CHILD_BYTES - at; if (!WriteFile(keep, buf + at, k, &n, NULL)) break; at += n; }
    CloseHandle(keep);
    WaitForSingleObject(pr, 30000);
    DWORD code = 99; GetExitCodeProcess(pr, &code); CloseHandle(pr);
    return (int)code;
}
static int one_write(const wchar_t* self, const char* how) {
    HANDLE give, keep; make(how, 0, &give, &keep);
    wchar_t cmd[600]; swprintf(cmd, 600, L"\"%ls\" -child-write", self);
    HANDLE pr = spawn(cmd, GetStdHandle(STD_INPUT_HANDLE), give);
    CloseHandle(give);
    static uint8_t buf[1 << 16]; uint32_t at = 0; int bad = 0;
    for (;;) { DWORD n; if (!ReadFile(keep, buf, sizeof buf, &n, NULL) || n == 0) break; for (DWORD i = 0; i < n; i++) if (buf[i] != pat(at + i)) bad = 1; at += n; }
    CloseHandle(keep);
    WaitForSingleObject(pr, 30000);
    DWORD code = 99; GetExitCodeProcess(pr, &code); CloseHandle(pr);
    return code != 0 ? (int)code : bad ? 1 : at == CHILD_BYTES ? 0 : 2;
}
static int one_findstr(const char* how) {
    HANDLE give, keep; make(how, 1, &give, &keep);
    HANDLE r, w; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; CreatePipe(&r, &w, &sa, 65536); SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    HANDLE pr = spawn(L"C:\\Windows\\System32\\findstr.exe /n \"^\"", give, w);
    CloseHandle(give); CloseHandle(w);
    enum { LINES = 2000 };                // input and output each fit one 64 KiB pipe buffer: written first, read after
    static char text[LINES * 12]; int len = 0;
    for (int i = 0; i < LINES; i++) len += sprintf(text + len, "line %05d\r\n", i);
    for (int at = 0; at < len; ) { DWORD k = (DWORD)(len - at < 4096 ? len - at : 4096), n; if (!WriteFile(keep, text + at, k, &n, NULL)) break; at += (int)n; }
    CloseHandle(keep);
    int lines = 0; char b[4096];
    for (;;) { DWORD n; if (!ReadFile(r, b, sizeof b, &n, NULL) || n == 0) break; for (DWORD i = 0; i < n; i++) lines += b[i] == '\n'; }
    CloseHandle(r);
    WaitForSingleObject(pr, 30000); CloseHandle(pr);
    return lines == LINES ? 0 : 1;
}

static void children(const char* how, int n) {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!strcmp(how, "reopened")) {
        HANDLE ov, sync; named_pair(1, &ov, &sync);
        HANDLE r = ReOpenFile(ov, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
        if (r == INVALID_HANDLE_VALUE) { printf("child\treopened\tnot run: ReOpenFile on a pipe end fails, error %lu\n", GetLastError()); return; }
        CloseHandle(r); CloseHandle(ov); CloseHandle(sync);
    }
    int rd = 0, wr = 0, fs = 0, codes[8] = {0};
    for (int i = 0; i < n; i++) {
        int a = one_read(self, how), b = one_write(self, how), c = one_findstr(how);
        rd += a == 0; wr += b == 0; fs += c == 0;
        if (a > 0 && a < 8) codes[a]++;
        if (b > 0 && b < 8) codes[b]++;
    }
    printf("child\t%s\tCRT reader %d/%d, CRT writer %d/%d, findstr %d/%d; failures: wrong bytes %d, short %d, error %d\n",
           how, rd, n, wr, n, fs, n, codes[1], codes[2], codes[3]);
}

// ---- rate ---------------------------------------------------------------------------------------------------------
#define RATE_BYTES (256ull << 20)
static HANDLE rate_w;
static DWORD WINAPI rate_writer(LPVOID arg) {
    (void)arg; static uint8_t b[1 << 16];
    for (uint64_t at = 0; at < RATE_BYTES; ) { DWORD n; if (!WriteFile(rate_w, b, sizeof b, &n, NULL)) break; at += n; }
    CloseHandle(rate_w);
    return 0;
}
static double now_ms(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1e3 / (double)f.QuadPart; }
static HANDLE relay_in, relay_out;
static DWORD WINAPI relay(LPVOID arg) {                   // overlapped reads from relay_in, blocking writes to relay_out
    (void)arg; static uint8_t b[1 << 16];
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (;;) {
        OVERLAPPED o; memset(&o, 0, sizeof o); o.hEvent = ev; DWORD n = 0, w;
        if (!ReadFile(relay_in, b, sizeof b, NULL, &o) && GetLastError() != ERROR_IO_PENDING) break;
        if (!GetOverlappedResult(relay_in, &o, &n, TRUE) || n == 0) break;
        if (!WriteFile(relay_out, b, n, &w, NULL)) break;
    }
    CloseHandle(relay_in); CloseHandle(relay_out); CloseHandle(ev);
    return 0;
}
static void rate(void) {
    static uint8_t b[1 << 16];
    for (int round = 0; round < 3; round++) {
        int fds[2]; if (_pipe(fds, 65536, _O_BINARY)) die("_pipe");
        rate_w = (HANDLE)_get_osfhandle(fds[1]);
        double t0 = now_ms(); HANDLE th = CreateThread(NULL, 0, rate_writer, NULL, 0, NULL);
        uint64_t got = 0; for (;;) { int n = _read(fds[0], b, sizeof b); if (n <= 0) break; got += (uint64_t)n; }
        WaitForSingleObject(th, INFINITE);
        printf("rate\t_pipe blocking\t%.0f MB/s\t%llu bytes\n", (double)got / 1e6 / ((now_ms() - t0) / 1e3), (unsigned long long)got);
        _close(fds[0]);
        HANDLE ov, sync; named_pair(1, &ov, &sync); rate_w = sync;
        HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        t0 = now_ms(); th = CreateThread(NULL, 0, rate_writer, NULL, 0, NULL);
        got = 0;
        for (;;) {
            OVERLAPPED o; memset(&o, 0, sizeof o); o.hEvent = ev; DWORD n = 0;
            if (!ReadFile(ov, b, sizeof b, NULL, &o) && GetLastError() != ERROR_IO_PENDING) break;
            if (!GetOverlappedResult(ov, &o, &n, TRUE) || n == 0) break;
            got += n;
        }
        WaitForSingleObject(th, INFINITE);
        printf("rate\tnamed overlapped\t%.0f MB/s\t%llu bytes\n", (double)got / 1e6 / ((now_ms() - t0) / 1e3), (unsigned long long)got);
        CloseHandle(ov); CloseHandle(ev);
        // A pipeline through a relay, as Rust's std does it: the writer's bytes reach an overlapped end the parent holds,
        // and a thread copies them into a fresh anonymous pipe the next reader is given.
        named_pair(1, &ov, &sync); rate_w = sync;
        HANDLE rr, rw; if (!CreatePipe(&rr, &rw, NULL, 65536)) die("CreatePipe");
        relay_in = ov; relay_out = rw;
        t0 = now_ms(); th = CreateThread(NULL, 0, rate_writer, NULL, 0, NULL);
        HANDLE rl = CreateThread(NULL, 0, relay, NULL, 0, NULL);
        got = 0; for (;;) { DWORD n; if (!ReadFile(rr, b, sizeof b, &n, NULL) || n == 0) break; got += n; }
        WaitForSingleObject(th, INFINITE); WaitForSingleObject(rl, INFINITE);
        printf("rate\trelay thread\t%.0f MB/s\t%llu bytes\n", (double)got / 1e6 / ((now_ms() - t0) / 1e3), (unsigned long long)got);
        CloseHandle(rr);
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "-child-read")) return child_read();
    if (argc >= 2 && !strcmp(argv[1], "-child-write")) return child_write();
    NtQueryInformationFile_ = (QueryInfoFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    if (argc < 2) { fprintf(stderr, "usage: pipes reopen | child overlapped|reopened|anon <n> | rate\n"); return 2; }
    if (!strcmp(argv[1], "reopen")) reopen();
    else if (!strcmp(argv[1], "child") && argc >= 4) children(argv[2], atoi(argv[3]));
    else if (!strcmp(argv[1], "rate")) rate();
    else { fprintf(stderr, "pipes: unknown mode\n"); return 2; }
    return 0;
}
