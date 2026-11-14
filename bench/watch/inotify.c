// inotify.c — KR-330's brief (ROADMAP_DETAIL s5-file-watch): what inotify reports, checked rather than assumed, beside
// rdcw.c's answers for Windows. Linux only. Not the runtime.
//
// Every scenario runs in a fresh directory under <base> and prints one TSV row per event it read:
//   scenario  batch  mask  name  cookie  watch
// `batch` counts the read() calls that returned events; `cookie` is 0 or a small number standing for the cookie (two rows
// with the same number are one move's halves); `watch` names which watch reported it (root, s1, s2, a, ...). A scenario
// that checks a call rather than events prints `scenario  -  result  detail  -  -`.
//   create write write3 rename delete mkdir rmdir      the plain cases, one watch on the directory
//   save-move                                          an editor's save: write a temporary, rename it over the file
//   sub-create move-across mkdir-race                  below the watched directory: inotify watches one directory, so
//                                                      a tree is a watch per directory, added as directories appear
//   move-out move-in hardlink                          what leaves, enters, or changes the directory from outside it
//   overflow-<n>                                       n events queued with nothing read (max_queued_events)
//   root-delete root-rename parent-rename              the watched directory itself, and whether its watch pins it
//   limits fanotify                                    the inotify sysctls; unprivileged fanotify with an inode mark
//   drvfs                                              (with a second argument) a directory on another file system,
//                                                      written by this process and by a Windows process (WSL's /mnt/c)
//
// Usage: inotify <base-dir> [<dir-on-drvfs> <its-windows-path>]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char base[PATH_MAX];
static int seq;

typedef struct { int fd; int wd[8]; const char* wname[8]; int nw; int batch; uint32_t cookies[64]; int ncookies; } W;

static void note(const char* scenario, const char* result, const char* detail)
{
    printf("%s\t-\t%s\t%s\t-\t-\n", scenario, result, detail);
    fflush(stdout);
}

static void touch(const char* p, const char* body)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "touch %s: %s\n", p, strerror(errno)); exit(1); }
    if (body && write(fd, body, strlen(body)) < 0) perror("write");
    close(fd);
}

static void append(const char* p, const char* body)
{
    int fd = open(p, O_WRONLY | O_APPEND);
    if (fd < 0) { fprintf(stderr, "append %s: %s\n", p, strerror(errno)); exit(1); }
    if (write(fd, body, strlen(body)) < 0) perror("write");
    close(fd);
}

static void fresh(const char* name, char* top, char* root, char* out)
{
    snprintf(top, PATH_MAX, "%s/%02d-%s", base, ++seq, name);
    mkdir(top, 0755);
    snprintf(root, PATH_MAX, "%s/root", top);
    snprintf(out, PATH_MAX, "%s/out", top);
    mkdir(root, 0755);
    mkdir(out, 0755);
}

#define MASK (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | \
              IN_DELETE_SELF | IN_MOVE_SELF)

static void w_init(W* w) { memset(w, 0, sizeof *w); w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); }
static int w_add(W* w, const char* dir, const char* label)
{
    int wd = inotify_add_watch(w->fd, dir, MASK | IN_ONLYDIR);
    if (wd < 0) return -1;
    w->wd[w->nw] = wd; w->wname[w->nw] = label; w->nw++;
    return wd;
}
static const char* w_label(W* w, int wd)
{
    for (int i = 0; i < w->nw; ++i) if (w->wd[i] == wd) return w->wname[i];
    return wd == -1 ? "(queue)" : "?";
}
static int w_cookie(W* w, uint32_t c)
{
    if (!c) return 0;
    for (int i = 0; i < w->ncookies; ++i) if (w->cookies[i] == c) return i + 1;
    w->cookies[w->ncookies] = c;
    return ++w->ncookies;
}

static void maskText(uint32_t m, char* out, size_t n)
{
    static const struct { uint32_t bit; const char* name; } bits[] = {
        { IN_CREATE, "CREATE" }, { IN_DELETE, "DELETE" }, { IN_MODIFY, "MODIFY" }, { IN_CLOSE_WRITE, "CLOSE_WRITE" },
        { IN_MOVED_FROM, "MOVED_FROM" }, { IN_MOVED_TO, "MOVED_TO" }, { IN_ATTRIB, "ATTRIB" },
        { IN_DELETE_SELF, "DELETE_SELF" }, { IN_MOVE_SELF, "MOVE_SELF" }, { IN_IGNORED, "IGNORED" },
        { IN_Q_OVERFLOW, "Q_OVERFLOW" }, { IN_UNMOUNT, "UNMOUNT" }, { IN_ISDIR, "ISDIR" } };
    out[0] = 0;
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; ++i)
        if (m & bits[i].bit) { if (out[0]) strncat(out, "|", n - strlen(out) - 1); strncat(out, bits[i].name, n - strlen(out) - 1); }
}

// Read everything until `quiet` ms pass with nothing; print each event, or only count them when `count` is given.
static void drain(W* w, const char* scenario, int quiet, long* count)
{
    char buf[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        struct pollfd p = { w->fd, POLLIN, 0 };
        if (poll(&p, 1, quiet) <= 0) return;
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n <= 0) return;
        w->batch++;
        for (char* q = buf; q < buf + n;) {
            struct inotify_event* e = (struct inotify_event*)q;
            char m[128];
            maskText(e->mask, m, sizeof m);
            if (count && !(e->mask & IN_Q_OVERFLOW)) (*count)++;
            else {
                printf("%s\t%d\t%s\t%s\t%d\t%s\n", scenario, w->batch, m, e->len ? e->name : "", w_cookie(w, e->cookie),
                       w_label(w, e->wd));
                fflush(stdout);
            }
            q += sizeof *e + e->len;
        }
    }
}

typedef void (*Act)(const char* root, const char* out);
static char A[PATH_MAX], B[PATH_MAX], C[PATH_MAX];

static void scenario(const char* name, Act setup, Act act)
{
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    if (setup) setup(root, out);
    w_init(&w);
    w_add(&w, root, "root");
    act(root, out);
    drain(&w, name, 200, NULL);
    close(w.fd);
}

static void mk_f(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/f.txt", r); touch(A, "one\n"); }
static void act_create(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/new.txt", r); touch(A, "hello\n"); }
static void act_write(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/f.txt", r); append(A, "two\n"); }
static void act_write3(const char* r, const char* o)
{
    (void)o;
    snprintf(A, PATH_MAX, "%s/f.txt", r);
    int fd = open(A, O_WRONLY | O_APPEND);
    for (int i = 0; i < 3; ++i) { if (write(fd, "x\n", 2) < 0) perror("write"); fsync(fd); usleep(20000); }
    close(fd);
}
static void act_rename(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/f.txt", r); snprintf(B, PATH_MAX, "%s/g.txt", r); rename(A, B); }
static void act_delete(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/f.txt", r); unlink(A); }
static void act_mkdir(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/d", r); mkdir(A, 0755); }
static void act_rmdir(const char* r, const char* o) { (void)o; snprintf(A, PATH_MAX, "%s/d", r); rmdir(A); }
static void act_save_move(const char* r, const char* o)
{
    (void)o;
    snprintf(A, PATH_MAX, "%s/f.txt.tmp", r); snprintf(B, PATH_MAX, "%s/f.txt", r);
    touch(A, "saved\n");
    rename(A, B);
}
static void act_move_out(const char* r, const char* o) { snprintf(A, PATH_MAX, "%s/f.txt", r); snprintf(B, PATH_MAX, "%s/f.txt", o); rename(A, B); }
static void mk_out_f(const char* r, const char* o) { (void)r; snprintf(A, PATH_MAX, "%s/g.txt", o); touch(A, "x\n"); }
static void act_move_in(const char* r, const char* o) { snprintf(A, PATH_MAX, "%s/g.txt", o); snprintf(B, PATH_MAX, "%s/g.txt", r); rename(A, B); }
static void mk_link(const char* r, const char* o)
{
    snprintf(A, PATH_MAX, "%s/l.txt", o); touch(A, "x\n");
    snprintf(B, PATH_MAX, "%s/f.txt", r);
    if (link(A, B) != 0) perror("link");
}
static void act_hardlink(const char* r, const char* o) { (void)r; snprintf(A, PATH_MAX, "%s/l.txt", o); append(A, "through the other name\n"); }

// Below the watched directory: one watch on root sees a subdirectory's own entry change, not what happens inside it.
static void sub_create(void)
{
    const char* name = "sub-create";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    snprintf(A, PATH_MAX, "%s/s1", root); mkdir(A, 0755);
    w_init(&w);
    w_add(&w, root, "root");
    snprintf(B, PATH_MAX, "%s/s1/new.txt", root); touch(B, "x\n");
    drain(&w, name, 200, NULL);
    close(w.fd);
}

// A move between two watched directories: are the halves paired, by cookie, across watches?
static void move_across(void)
{
    const char* name = "move-across";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    snprintf(A, PATH_MAX, "%s/s1", root); mkdir(A, 0755);
    snprintf(B, PATH_MAX, "%s/s2", root); mkdir(B, 0755);
    snprintf(C, PATH_MAX, "%s/s1/f.txt", root); touch(C, "x\n");
    w_init(&w);
    w_add(&w, root, "root"); w_add(&w, A, "s1"); w_add(&w, B, "s2");
    snprintf(B, PATH_MAX, "%s/s2/f.txt", root);
    rename(C, B);
    drain(&w, name, 200, NULL);
    close(w.fd);
}

// The race a recursive watch must close: a tree made faster than its watches are added. The watcher sees `a` created,
// adds a watch on it, and the events for a/b and a/b/f.txt are already past; walking `a` once its watch is in place
// finds what was missed.
static void mkdir_race(void)
{
    const char* name = "mkdir-race";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    w_init(&w);
    w_add(&w, root, "root");
    snprintf(A, PATH_MAX, "%s/a", root); mkdir(A, 0755);
    snprintf(A, PATH_MAX, "%s/a/b", root); mkdir(A, 0755);
    snprintf(A, PATH_MAX, "%s/a/b/f.txt", root); touch(A, "x\n");
    drain(&w, name, 200, NULL);
    snprintf(A, PATH_MAX, "%s/a", root);
    w_add(&w, A, "a");
    snprintf(B, PATH_MAX, "%s/a/b/g.txt", root); touch(B, "x\n");   // after a's watch: still inside b, unwatched
    drain(&w, name, 200, NULL);
    struct stat st;
    snprintf(C, PATH_MAX, "%s/a/b", root);
    note(name, "walk", stat(C, &st) == 0 ? "a/b exists: only a walk after adding a's watch finds it and its files" : "a/b missing");
    close(w.fd);
}

static void overflow(long n)
{
    char name[32];
    snprintf(name, sizeof name, "overflow-%ld", n);
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    w_init(&w);
    w_add(&w, root, "root");
    snprintf(A, PATH_MAX, "%s/f.txt", root); touch(A, NULL);
    int fd = open(A, O_WRONLY);
    for (long i = 0; i < n; ++i) { if (write(fd, "x", 1) < 0) perror("write"); fchmod(fd, (i & 1) ? 0644 : 0600); }
    close(fd);
    long count = 0;
    drain(&w, name, 200, &count);
    char d[96];
    snprintf(d, sizeof d, "%ld events read for %ld write+chmod pairs (MODIFY and ATTRIB alternate, so none coalesce)", count, n);
    note(name, "delivered", d);
    close(w.fd);
}

static void root_delete(void)
{
    const char* name = "root-delete";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    w_init(&w);
    w_add(&w, root, "root");
    note(name, "call", rmdir(root) == 0 ? "rmdir: ok" : strerror(errno));
    drain(&w, name, 200, NULL);
    close(w.fd);
}

static void root_rename(void)
{
    const char* name = "root-rename";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    w_init(&w);
    w_add(&w, root, "root");
    snprintf(B, PATH_MAX, "%s/root-renamed", top);
    note(name, "call", rename(root, B) == 0 ? "rename(root): ok" : strerror(errno));
    snprintf(A, PATH_MAX, "%s/after-rename.txt", B); touch(A, "x\n");
    note(name, "call", "a file made in the renamed directory");
    drain(&w, name, 200, NULL);
    close(w.fd);
}

static void parent_rename(void)
{
    const char* name = "parent-rename";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX], par[PATH_MAX], child[PATH_MAX];
    W w;
    fresh(name, top, root, out);
    snprintf(par, PATH_MAX, "%s/parent", top); mkdir(par, 0755);
    snprintf(child, PATH_MAX, "%s/child", par); mkdir(child, 0755);
    w_init(&w);
    w_add(&w, child, "child");
    snprintf(B, PATH_MAX, "%s/parent-renamed", top);
    note(name, "call", rename(par, B) == 0 ? "rename(parent) while child is watched: ok" : strerror(errno));
    drain(&w, name, 200, NULL);
    close(w.fd);
}

static void limits(void)
{
    const char* f[] = { "/proc/sys/fs/inotify/max_user_watches", "/proc/sys/fs/inotify/max_queued_events",
                        "/proc/sys/fs/inotify/max_user_instances", "/proc/sys/fs/fanotify/max_user_marks",
                        "/proc/sys/fs/fanotify/max_user_groups", "/proc/sys/fs/fanotify/max_queued_events" };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; ++i) {
        char v[64] = "?";
        FILE* fp = fopen(f[i], "r");
        if (fp) { if (fgets(v, sizeof v, fp)) v[strcspn(v, "\n")] = 0; fclose(fp); }
        note("limits", f[i] + 10, v);
    }
}

// Unprivileged fanotify, as Zig's build watcher uses it: FID reporting with directory names, an inode mark on a
// directory with events on its children. Does it work, and does a child directory's content reach the mark?
static void fan(void)
{
    const char* name = "fanotify";
    char top[PATH_MAX], root[PATH_MAX], out[PATH_MAX], d[160];
    fresh(name, top, root, out);
    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_NONBLOCK, O_RDONLY);
    if (fd < 0) { snprintf(d, sizeof d, "fanotify_init(DFID_NAME) as uid %d: %s", (int)getuid(), strerror(errno)); note(name, "init", d); return; }
    snprintf(d, sizeof d, "fanotify_init(DFID_NAME) as uid %d: ok", (int)getuid());
    note(name, "init", d);
    uint64_t ev = FAN_CREATE | FAN_DELETE | FAN_MODIFY | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_EVENT_ON_CHILD | FAN_ONDIR;
    if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_ONLYDIR, ev, AT_FDCWD, root) != 0) {
        snprintf(d, sizeof d, "inode mark: %s", strerror(errno)); note(name, "mark", d); close(fd); return;
    }
    note(name, "mark", "inode mark on the directory: ok");
    int m = fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CREATE, AT_FDCWD, root);
    snprintf(d, sizeof d, "filesystem mark (a whole tree): %s", m == 0 ? "ok" : strerror(errno));
    note(name, "mark", d);
    snprintf(A, PATH_MAX, "%s/s1", root); mkdir(A, 0755);
    snprintf(B, PATH_MAX, "%s/s1/inside.txt", root); touch(B, "x\n");
    snprintf(C, PATH_MAX, "%s/top.txt", root); touch(C, "x\n");
    usleep(100000);
    char buf[8192];
    int events = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (struct fanotify_event_metadata* e = (struct fanotify_event_metadata*)buf; FAN_EVENT_OK(e, n); e = FAN_EVENT_NEXT(e, n)) {
            struct fanotify_event_info_fid* fid = (struct fanotify_event_info_fid*)(e + 1);
            const char* nm = "";
            if (fid->hdr.info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                struct file_handle* fh = (struct file_handle*)fid->handle;
                nm = (const char*)(fh->f_handle + fh->handle_bytes);
            }
            char line[200];
            snprintf(line, sizeof line, "mask 0x%llx name %s", (unsigned long long)e->mask, nm);
            note(name, "event", line);
            events++;
        }
    snprintf(d, sizeof d, "%d events for s1/, s1/inside.txt, top.txt", events);
    note(name, "events", d);
    close(fd);
}

// A directory on WSL's drvfs (/mnt/c): written by this process, then by a Windows process through interop.
static void drvfs(const char* dir, const char* winDir)
{
    const char* name = "drvfs";
    W w;
    w_init(&w);
    if (w_add(&w, dir, "drvfs") < 0) { char d[96]; snprintf(d, sizeof d, "add_watch: %s", strerror(errno)); note(name, "watch", d); return; }
    note(name, "watch", "add_watch on the drvfs directory: ok");
    snprintf(A, PATH_MAX, "%s/from-linux.txt", dir); touch(A, "x\n");
    drain(&w, name, 300, NULL);
    note(name, "call", "a file written by cmd.exe (Windows) in the same directory");
    pid_t p = fork();
    if (p == 0) {
        char cmd[PATH_MAX + 64];
        // No quotes (the path has no spaces; WSL's interop re-quotes an argument that has them), and a Windows working
        // directory, since cmd.exe refuses the UNC path of a Linux one.
        snprintf(cmd, sizeof cmd, "echo x>%s\\from-windows.txt", winDir);
        if (chdir("/mnt/c") != 0) _exit(126);
        execl("/mnt/c/Windows/System32/cmd.exe", "cmd.exe", "/c", cmd, (char*)NULL);
        _exit(127);
    }
    int st; waitpid(p, &st, 0);
    usleep(500000);
    drain(&w, name, 1000, NULL);
    struct stat s;
    snprintf(B, PATH_MAX, "%s/from-windows.txt", dir);
    note(name, "after", stat(B, &s) == 0 ? "from-windows.txt exists" : "from-windows.txt missing (cmd failed)");
    close(w.fd);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: inotify <base-dir> [<dir-on-drvfs> <its-windows-path>]\n"); return 2; }
    snprintf(base, sizeof base, "%s", argv[1]);
    mkdir(base, 0755);
    scenario("create", NULL, act_create);
    scenario("write", mk_f, act_write);
    scenario("write3", mk_f, act_write3);
    scenario("rename", mk_f, act_rename);
    scenario("delete", mk_f, act_delete);
    scenario("mkdir", NULL, act_mkdir);
    scenario("rmdir", act_mkdir, act_rmdir);
    scenario("save-move", mk_f, act_save_move);
    sub_create();
    move_across();
    mkdir_race();
    scenario("move-out", mk_f, act_move_out);
    scenario("move-in", mk_out_f, act_move_in);
    scenario("hardlink", mk_link, act_hardlink);
    overflow(1000); overflow(8000); overflow(20000);
    root_delete();
    root_rename();
    parent_rename();
    limits();
    fan();
    if (argc >= 4) drvfs(argv[2], argv[3]);
    return 0;
}
