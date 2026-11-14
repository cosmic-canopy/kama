// rdcw.c — KR-330's brief (ROADMAP_DETAIL s5-file-watch): what ReadDirectoryChangesExW reports, checked rather than
// assumed, for the decisions a file watcher on Windows rests on. Windows only. Not the runtime.
//
// Every scenario runs in a fresh directory under <base> and prints one TSV row per event it saw:
//   scenario  batch  action  name  kind
// `batch` counts the completions, so two rows with one batch arrived in one buffer (a rename's two halves, say);
// `kind` is d or f from the extended information's attributes. A scenario that checks a call rather than events
// prints `scenario  -  result  detail  -`.
//   create write write3 rename delete mkdir rmdir      the plain cases, one watch on the directory
//   save-move save-replacefile save-posix              an editor's save: write a temporary, then replace the file
//   sub-create move-across mkdir-tree                  a recursive watch (bWatchSubtree)
//   move-out move-in hardlink                          what leaves, enters, or changes the directory from outside it
//   before-first between                               which changes a watch reports: before its first read, and
//                                                      between two reads
//   overflow-<n>                                       n files made while no read is outstanding (64 KiB buffer)
//   root-delete root-rename parent-rename              the watched directory itself, and what its handle pins
//   latency-event latency-packet                       a change to a waiting thread, and through a wait packet into a
//                                                      port as step 8's C3 completes an operation (median ns of 200)
//
// Usage: rdcw <base-dir>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define BUF (64 * 1024)
#define FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | \
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |       \
                FILE_NOTIFY_CHANGE_SECURITY)

static wchar_t base[MAX_PATH];
static int seq;

typedef struct {
    HANDLE dir, ev;
    OVERLAPPED ov;
    BOOL recursive, pending;   // pending: a read is outstanding, so the kernel owns the OVERLAPPED
    int batch;
    DWORD* buf;   // DWORD-aligned, as the call requires
} Watch;

static void path(wchar_t* out, const wchar_t* dir, const wchar_t* name) { swprintf(out, MAX_PATH, L"%ls\\%ls", dir, name); }

static void touch(const wchar_t* p, const char* body)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "touch %ls: %lu\n", p, GetLastError()); exit(1); }
    if (body) WriteFile(h, body, (DWORD)strlen(body), &n, NULL);
    CloseHandle(h);
}

static void append(const wchar_t* p, const char* body)
{
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "append %ls: %lu\n", p, GetLastError()); exit(1); }
    WriteFile(h, body, (DWORD)strlen(body), &n, NULL);
    CloseHandle(h);
}

// A fresh directory for one scenario: <base>\<n>-<name>, with `root` inside it (the watched one) and `out` beside
// it (outside every watch).
static void fresh(const char* name, wchar_t* top, wchar_t* root, wchar_t* out)
{
    swprintf(top, MAX_PATH, L"%ls\\%02d-%hs", base, ++seq, name);
    CreateDirectoryW(top, NULL);
    path(root, top, L"root");
    path(out, top, L"out");
    CreateDirectoryW(root, NULL);
    CreateDirectoryW(out, NULL);
}

static void row(const char* scenario, int batch, const char* action, const wchar_t* name, int nameLen, char kind)
{
    printf("%s\t%d\t%s\t%.*ls\t%c\n", scenario, batch, action, nameLen, name, kind);
    fflush(stdout);
}

static void note(const char* scenario, const char* result, const char* detail)
{
    printf("%s\t-\t%s\t%s\t-\n", scenario, result, detail);
    fflush(stdout);
}

static int open_watch(Watch* w, const wchar_t* dir, BOOL recursive)
{
    w->dir = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (w->dir == INVALID_HANDLE_VALUE) return -1;
    w->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->recursive = recursive;
    w->batch = 0;
    w->pending = FALSE;
    w->buf = (DWORD*)malloc(BUF);
    return 0;
}

static BOOL arm(Watch* w)
{
    memset(&w->ov, 0, sizeof w->ov);
    w->ov.hEvent = w->ev;
    ResetEvent(w->ev);
    w->pending = ReadDirectoryChangesExW(w->dir, w->buf, BUF, w->recursive, FILTER, NULL, &w->ov, NULL,
                                         ReadDirectoryNotifyExtendedInformation);
    return w->pending;
}

static const char* actionName(DWORD a)
{
    switch (a) {
        case FILE_ACTION_ADDED: return "ADDED";
        case FILE_ACTION_REMOVED: return "REMOVED";
        case FILE_ACTION_MODIFIED: return "MODIFIED";
        case FILE_ACTION_RENAMED_OLD_NAME: return "RENAMED_OLD";
        case FILE_ACTION_RENAMED_NEW_NAME: return "RENAMED_NEW";
        default: return "?";
    }
}

// Wait up to `ms` for the outstanding read; print what it brought. 1 = a batch came (the read is NOT re-armed),
// 0 = nothing in time (still outstanding), -1 = the read failed (its error printed). `count`, when given, receives
// the number of entries instead of printing them.
static int take(Watch* w, const char* scenario, DWORD ms, int* count)
{
    DWORD got = 0;
    if (!w->pending || WaitForSingleObject(w->ev, ms) != WAIT_OBJECT_0) return 0;
    w->pending = FALSE;
    w->batch++;
    if (!GetOverlappedResult(w->dir, &w->ov, &got, FALSE)) {
        char d[64];
        DWORD e = GetLastError();
        snprintf(d, sizeof d, "error %lu%s", e, e == ERROR_NOTIFY_ENUM_DIR ? " (ERROR_NOTIFY_ENUM_DIR)" : "");
        note(scenario, "read-failed", d);
        return -1;
    }
    if (got == 0) { note(scenario, "zero-bytes", "the buffer overflowed: every entry discarded"); return 1; }
    int n = 0;
    for (BYTE* p = (BYTE*)w->buf;;) {
        FILE_NOTIFY_EXTENDED_INFORMATION* e = (FILE_NOTIFY_EXTENDED_INFORMATION*)p;
        if (count) n++;
        else row(scenario, w->batch, actionName(e->Action), e->FileName, (int)(e->FileNameLength / sizeof(WCHAR)),
                 (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : 'f');
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    if (count) *count = n;
    return 1;
}

// Everything that arrives until `quiet` ms pass with nothing, re-arming after each batch.
static void drain(Watch* w, const char* scenario, DWORD quiet)
{
    for (;;) {
        int r = take(w, scenario, quiet, NULL);
        if (r <= 0) return;
        if (!arm(w)) { char d[32]; snprintf(d, sizeof d, "error %lu", GetLastError()); note(scenario, "rearm-failed", d); return; }
    }
}

static void close_watch(Watch* w)
{
    if (w->pending) {   // the kernel owns the OVERLAPPED and the buffer until the cancelled read completes
        DWORD got;
        CancelIoEx(w->dir, &w->ov);
        GetOverlappedResult(w->dir, &w->ov, &got, TRUE);
    }
    CloseHandle(w->dir);
    CloseHandle(w->ev);
    free(w->buf);
}

// One watch on `root` (recursive or not), armed; `act` runs; everything it caused is printed.
typedef void (*Act)(const wchar_t* root, const wchar_t* out);
static void scenario(const char* name, BOOL recursive, Act setup, Act act)
{
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    if (setup) setup(root, out);
    if (open_watch(&w, root, recursive) != 0 || !arm(&w)) { note(name, "watch-failed", ""); return; }
    act(root, out);
    drain(&w, name, 300);
    close_watch(&w);
}

static wchar_t A[MAX_PATH], B[MAX_PATH], C[MAX_PATH];

static void mk_f(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"f.txt"); touch(A, "one\n"); }
static void act_create(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"new.txt"); touch(A, "hello\n"); }
static void act_write(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"f.txt"); append(A, "two\n"); }
static void act_write3(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"f.txt");
    HANDLE h = CreateFileW(A, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n;
    for (int i = 0; i < 3; ++i) { WriteFile(h, "x\n", 2, &n, NULL); FlushFileBuffers(h); Sleep(20); }
    CloseHandle(h);
}
static void act_rename(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"f.txt"); path(B, r, L"g.txt"); MoveFileW(A, B); }
static void act_delete(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"f.txt"); DeleteFileW(A); }
static void act_mkdir(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"d"); CreateDirectoryW(A, NULL); }
static void mk_d(const wchar_t* r, const wchar_t* o) { act_mkdir(r, o); }
static void act_rmdir(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"d"); RemoveDirectoryW(A); }

static void act_save_move(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"f.txt.tmp"); path(B, r, L"f.txt");
    touch(A, "saved\n");
    MoveFileExW(A, B, MOVEFILE_REPLACE_EXISTING);
}
static void act_save_replacefile(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"f.txt.tmp"); path(B, r, L"f.txt");
    touch(A, "saved\n");
    if (!ReplaceFileW(B, A, NULL, 0, NULL, NULL)) fprintf(stderr, "ReplaceFileW: %lu\n", GetLastError());
}
static void act_save_posix(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"f.txt.tmp"); path(B, r, L"f.txt");
    touch(A, "saved\n");
    HANDLE h = CreateFileW(A, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    size_t nameBytes = wcslen(B) * sizeof(WCHAR);
    FILE_RENAME_INFO* ri = (FILE_RENAME_INFO*)calloc(1, sizeof(FILE_RENAME_INFO) + nameBytes);
    ri->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS | FILE_RENAME_FLAG_REPLACE_IF_EXISTS;
    ri->FileNameLength = (DWORD)nameBytes;
    memcpy(ri->FileName, B, nameBytes);
    if (!SetFileInformationByHandle(h, FileRenameInfoEx, ri, (DWORD)(sizeof(FILE_RENAME_INFO) + nameBytes)))
        fprintf(stderr, "FileRenameInfoEx: %lu\n", GetLastError());
    free(ri);
    CloseHandle(h);
}

static void mk_subs(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"s1"); CreateDirectoryW(A, NULL);
    path(B, r, L"s2"); CreateDirectoryW(B, NULL);
    path(C, A, L"f.txt"); touch(C, "one\n");
}
static void act_sub_create(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"s1\\new.txt"); touch(A, "x\n"); }
static void act_move_across(const wchar_t* r, const wchar_t* o) { (void)o; path(A, r, L"s1\\f.txt"); path(B, r, L"s2\\f.txt"); MoveFileW(A, B); }
static void act_mkdir_tree(const wchar_t* r, const wchar_t* o)
{
    (void)o;
    path(A, r, L"a"); CreateDirectoryW(A, NULL);
    path(A, r, L"a\\b"); CreateDirectoryW(A, NULL);
    path(A, r, L"a\\b\\f.txt"); touch(A, "x\n");
}
static void act_move_out(const wchar_t* r, const wchar_t* o) { path(A, r, L"f.txt"); path(B, o, L"f.txt"); MoveFileW(A, B); }
static void mk_out_f(const wchar_t* r, const wchar_t* o) { (void)r; path(A, o, L"g.txt"); touch(A, "x\n"); }
static void act_move_in(const wchar_t* r, const wchar_t* o) { path(A, o, L"g.txt"); path(B, r, L"g.txt"); MoveFileW(A, B); }
static void mk_link(const wchar_t* r, const wchar_t* o)
{
    path(A, o, L"l.txt"); touch(A, "x\n");
    path(B, r, L"f.txt");
    if (!CreateHardLinkW(B, A, NULL)) fprintf(stderr, "CreateHardLinkW: %lu\n", GetLastError());
}
static void act_hardlink(const wchar_t* r, const wchar_t* o) { (void)r; path(A, o, L"l.txt"); append(A, "through the other name\n"); }

// A change made after the handle is open but before its first read; then one after the read is issued.
static void before_first(void)
{
    const char* name = "before-first";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    path(A, root, L"made-before-first-read.txt"); touch(A, "x\n");
    arm(&w);
    path(A, root, L"made-after-first-read.txt"); touch(A, "x\n");
    drain(&w, name, 300);
    close_watch(&w);
}

// A batch taken and NOT re-armed; two changes while no read is outstanding; then the next read.
static void between(void)
{
    const char* name = "between";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    arm(&w);
    path(A, root, L"first.txt"); touch(A, "x\n");
    take(&w, name, 1000, NULL);
    Sleep(50);
    path(A, root, L"while-no-read-1.txt"); touch(A, "x\n");
    path(A, root, L"while-no-read-2.txt"); touch(A, "x\n");
    Sleep(50);
    arm(&w);
    drain(&w, name, 300);
    close_watch(&w);
}

// n files made while no read is outstanding, after a first read sized the kernel's buffer.
static void overflow(int n)
{
    char name[32];
    snprintf(name, sizeof name, "overflow-%d", n);
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    arm(&w);
    path(A, root, L"first.txt"); touch(A, "x\n");
    take(&w, name, 1000, NULL);
    for (int i = 0; i < n; ++i) { wchar_t f[32]; swprintf(f, 32, L"f%05d.txt", i); path(A, root, f); touch(A, NULL); }
    arm(&w);
    int total = 0, batches = 0;
    for (;;) {
        int c = 0;
        int r = take(&w, name, 300, &c);
        if (r <= 0) break;
        total += c; batches++;
        if (!arm(&w)) break;
    }
    char d[96];
    snprintf(d, sizeof d, "%d entries in %d batches for %d files (x3 events each: ADDED, MODIFIED...)", total, batches, n);
    note(name, "delivered", d);
    close_watch(&w);
}

static void root_delete(void)
{
    const char* name = "root-delete";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    arm(&w);
    char d[64];
    BOOL ok = RemoveDirectoryW(root);
    snprintf(d, sizeof d, "RemoveDirectory: %s %lu", ok ? "ok" : "failed", ok ? 0 : GetLastError());
    note(name, "call", d);
    drain(&w, name, 300);
    // the next read on the same handle
    if (!arm(&w)) { snprintf(d, sizeof d, "error %lu", GetLastError()); note(name, "rearm-failed", d); }
    else drain(&w, name, 300);
    snprintf(d, sizeof d, "%s", GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES ? "gone" : "still there");
    note(name, "after", d);
    close_watch(&w);
    snprintf(d, sizeof d, "%s", GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES ? "gone" : "still there");
    note(name, "after-close", d);
}

static void root_rename(void)
{
    const char* name = "root-rename";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    arm(&w);
    path(B, top, L"root-renamed");
    char d[64];
    BOOL ok = MoveFileW(root, B);
    snprintf(d, sizeof d, "MoveFile(root): %s %lu", ok ? "ok" : "failed", ok ? 0 : GetLastError());
    note(name, "call", d);
    drain(&w, name, 300);
    path(A, B, L"after-rename.txt"); touch(A, "x\n");
    note(name, "call", "a file made in the renamed directory");
    drain(&w, name, 300);
    close_watch(&w);
}

// The watched directory's PARENT renamed and removed while the watch is open.
static void parent_rename(void)
{
    const char* name = "parent-rename";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH], par[MAX_PATH], child[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    path(par, top, L"parent"); CreateDirectoryW(par, NULL);
    path(child, par, L"child"); CreateDirectoryW(child, NULL);
    open_watch(&w, child, FALSE);
    arm(&w);
    path(B, top, L"parent-renamed");
    char d[80];
    BOOL ok = MoveFileW(par, B);
    snprintf(d, sizeof d, "MoveFile(parent) while child is watched: %s %lu", ok ? "ok" : "failed", ok ? 0 : GetLastError());
    note(name, "call", d);
    if (ok) MoveFileW(B, par);
    // the same with a recursive watch on the parent instead: is its own parent pinned?
    close_watch(&w);
    ok = MoveFileW(par, B);
    snprintf(d, sizeof d, "MoveFile(parent) after the watch closed: %s %lu", ok ? "ok" : "failed", ok ? 0 : GetLastError());
    note(name, "call", d);
}

typedef LONG NTSTATUS_;
typedef NTSTATUS_ (NTAPI *CreateWaitPacketFn)(PHANDLE, ACCESS_MASK, PVOID);
typedef NTSTATUS_ (NTAPI *AssociateWaitPacketFn)(HANDLE, HANDLE, HANDLE, PVOID, PVOID, NTSTATUS_, ULONG_PTR, PBOOLEAN);

static double freq;
static uint64_t now_ns(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * freq); }
static int cmp64(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }

typedef struct { const wchar_t* root; volatile LONG go; volatile uint64_t stamp; int i; } Toucher;
static DWORD WINAPI toucher(void* arg)
{
    Toucher* t = (Toucher*)arg;
    while (!InterlockedCompareExchange(&t->go, 0, 0)) YieldProcessor();
    wchar_t f[32], p[MAX_PATH];
    swprintf(f, 32, L"l%05d.txt", t->i);
    path(p, t->root, f);
    t->stamp = now_ns();
    touch(p, NULL);
    return 0;
}

// The time from a file's creation (stamped just before the call) to the waiting side waking, both ways.
static void latency(int packet)
{
    const char* name = packet ? "latency-packet" : "latency-event";
    wchar_t top[MAX_PATH], root[MAX_PATH], out[MAX_PATH];
    Watch w;
    fresh(name, top, root, out);
    open_watch(&w, root, FALSE);
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1), pkt = NULL;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    CreateWaitPacketFn create = (CreateWaitPacketFn)(void*)GetProcAddress(nt, "NtCreateWaitCompletionPacket");
    AssociateWaitPacketFn assoc = (AssociateWaitPacketFn)(void*)GetProcAddress(nt, "NtAssociateWaitCompletionPacket");
    if (packet && (!create || !assoc || create(&pkt, GENERIC_ALL, NULL) != 0)) { note(name, "no-wait-packets", ""); return; }
    enum { N = 200 };
    uint64_t d[N];
    for (int i = 0; i < N; ++i) {
        Toucher t = { root, 0, 0, i };
        arm(&w);
        if (packet) assoc(pkt, port, w.ev, (PVOID)3, (PVOID)&w, 0, 0, NULL);
        HANDLE th = CreateThread(NULL, 0, toucher, &t, 0, NULL);
        InterlockedExchange(&t.go, 1);
        if (packet) {
            OVERLAPPED_ENTRY e; ULONG n;
            GetQueuedCompletionStatusEx(port, &e, 1, &n, INFINITE, FALSE);
        } else WaitForSingleObject(w.ev, INFINITE);
        d[i] = now_ns() - t.stamp;
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        DWORD got; GetOverlappedResult(w.dir, &w.ov, &got, FALSE);
        w.pending = FALSE;
        Sleep(1);
        // a create completes the read with ADDED, and the close may queue MODIFIED: drop what is left
        while (arm(&w) && WaitForSingleObject(w.ev, 5) == WAIT_OBJECT_0) w.pending = FALSE;
        if (w.pending) { CancelIoEx(w.dir, &w.ov); GetOverlappedResult(w.dir, &w.ov, &got, TRUE); w.pending = FALSE; }
    }
    qsort(d, N, sizeof d[0], cmp64);
    char s[64];
    snprintf(s, sizeof s, "median %llu ns, p90 %llu ns", (unsigned long long)d[N / 2], (unsigned long long)d[N * 9 / 10]);
    note(name, "wake", s);
    if (pkt) CloseHandle(pkt);
    CloseHandle(port);
    close_watch(&w);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: rdcw <base-dir>\n"); return 2; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = 1e9 / (double)f.QuadPart;
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, base, MAX_PATH);
    CreateDirectoryW(base, NULL);
    scenario("create", FALSE, NULL, act_create);
    scenario("write", FALSE, mk_f, act_write);
    scenario("write3", FALSE, mk_f, act_write3);
    scenario("rename", FALSE, mk_f, act_rename);
    scenario("delete", FALSE, mk_f, act_delete);
    scenario("mkdir", FALSE, NULL, act_mkdir);
    scenario("rmdir", FALSE, mk_d, act_rmdir);
    scenario("save-move", FALSE, mk_f, act_save_move);
    scenario("save-replacefile", FALSE, mk_f, act_save_replacefile);
    scenario("save-posix", FALSE, mk_f, act_save_posix);
    scenario("sub-create", TRUE, mk_subs, act_sub_create);
    scenario("sub-create-flat", FALSE, mk_subs, act_sub_create);
    scenario("move-across", TRUE, mk_subs, act_move_across);
    scenario("mkdir-tree", TRUE, NULL, act_mkdir_tree);
    scenario("move-out", FALSE, mk_f, act_move_out);
    scenario("move-in", FALSE, mk_out_f, act_move_in);
    scenario("hardlink", FALSE, mk_link, act_hardlink);
    before_first();
    between();
    overflow(300); overflow(500); overflow(600); overflow(1000);
    root_delete();
    root_rename();
    parent_rename();
    latency(0);
    latency(1);
    return 0;
}
