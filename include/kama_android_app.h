#ifndef KAMA_ANDROID_APP_H
#define KAMA_ANDROID_APP_H

// An Android app's entry (KR-283). Included last by the entry translation unit of an app built for Android
// (`"kind": "app"`, so the driver says `-DKAMA_ANDROID_APP`), after `kama_app_main`, which runs the program's `main`.
//
// NativeActivity, the platform's activity for an app with no Java, loads the app's library and calls
// `ANativeActivity_onCreate`. The NDK's native_app_glue, compiled into every app, answers it by starting a thread
// that calls `android_main`, which is this. It runs `main` there, once per process:
//   * stdout and stderr have no reader in an app the launcher starts, so both become a pipe, whose other end a thread
//     of ours copies to logcat a line at a time, at INFO under the tag `kama` (gomobile and Godot do the same, as
//     `GoLog` and `godot`). Every write reaches it: kama's `print` and a `csources` file's `printf` alike. A panic's
//     message is handed to bionic too (`android_set_abort_message`, in kama__fail), so the crash report carries it
//     even when the process ends before the thread has copied it.
//   * `args()` holds the process's own name, which for an app is its id: /proc/self/cmdline, as zygote wrote it.
//   * the app's own directories are its environment, and `main` starts in its files directory (kama__android_dirs).
//   * when `main` returns, the activity is finished, its last events are answered, and the process exits with `main`'s
//     code. Android would otherwise keep the process cached and start `android_main` again in it on the next launch
//     (measured on the emulator, 2026-11-07), where a second `main` would find the first one's globals.
//
//   * the activity's lifecycle reaches the program through std::app: `run` pumps the glue's looper between ticks
//     (include/kama_app.h), and its accessors read the glue's app from here. Until `main` calls `run`, nothing answers
//     the activity, and Android's UI thread waits: its start, then its pause and its input, which Android reports as
//     not responding after about five seconds. So work before `run` is short, and a `main` that never calls it is a
//     program that runs to completion.
//
// The body is the entry unit's alone, under the driver's define: anywhere else, a host compile of every runtime header
// beside every other included (tools/check-header-order.sh), it is empty, since the NDK's headers are not there.
#if defined(KAMA_ANDROID_APP)
#include <android/log.h>
#include <android_native_app_glue.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

// std::app's (include/kama_app.h declares them): one object each, defined here because this is the entry unit.
struct android_app* kama__android_app;
pthread_t kama__android_thread;
bool (*kama__android_input)(void*);
bool kama__app_awake;

static int kama__app_log_fd = -1;

#if defined(KAMA_APP_RUNNER)
static int kama__runner_asked;                 // started with the extra: a runner exists for this program (below)
#endif
// The glue's input hook: std::app::android's handler, given each event; one that is not handled goes back to Android
// (Back then finishes the activity). Under a runner, its sentinel is the glue's own: `KEYCODE_UNKNOWN` from the virtual
// keyboard, which the driver injects synchronously behind an injection that only queues (`keycombination`), so that
// its return means the app has handled everything before it. The program never sees it.
static int32_t kama__android_on_input(struct android_app* app, AInputEvent* event) {
    (void)app;
#if defined(KAMA_APP_RUNNER)
    if (kama__runner_asked && AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY
        && AKeyEvent_getKeyCode(event) == AKEYCODE_UNKNOWN && AInputEvent_getDeviceId(event) == -1) return 1;
#endif
    return kama__android_input && kama__android_input(event) ? 1 : 0;
}

// The pipe's reader: each line to logcat as it completes, a line longer than the buffer in pieces, and a last line
// with no newline when the pipe closes.
static void* kama__app_log_pump(void* unused) {
    (void)unused;
    char buf[1024];
    size_t have = 0;
    for (;;) {
        const ssize_t n = read(kama__app_log_fd, buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        have += (size_t)n;
        size_t start = 0;
        for (size_t k = 0; k < have; ++k)
            if (buf[k] == '\n') { buf[k] = 0; __android_log_write(ANDROID_LOG_INFO, "kama", buf + start); start = k + 1; }
        if (start == 0 && have == sizeof buf - 1) { buf[have] = 0; __android_log_write(ANDROID_LOG_INFO, "kama", buf); have = 0; }
        else { memmove(buf, buf + start, have - start); have -= start; }
    }
    if (have) { buf[have] = 0; __android_log_write(ANDROID_LOG_INFO, "kama", buf); }
    return NULL;
}

// An app's directories, which the platform has and a process zygote starts is not told (KR-266): HOME is unset, so a
// home directory would be bionic's account entry, `/data`, which the app cannot write; TMPDIR is set only from Android
// 13; and the process starts in `/`. As .NET for Android's host does, HOME becomes the app's files directory (the NDK's
// `internalDataPath`, `getFilesDir()`), TMPDIR and XDG_CACHE_HOME its cache directory (`getCacheDir()`, which Android
// clears under storage pressure), each only where it is unset, so a value set apart from the platform wins; Android
// 13's TMPDIR names the same directory. And `main` starts in the files directory, so a relative path means the app's
// own storage, as it means the launch directory on a desktop. The NDK names no cache directory, so the activity is
// asked through JNI, as gomobile and SDL3 ask it; this is the glue's thread, which the VM knows once it attaches. A
// failure leaves the cache variables unset, and std then answers what it answers in a shell.
static void kama__android_dirs(ANativeActivity* activity) {
    if (activity->internalDataPath && *activity->internalDataPath) {
        setenv("HOME", activity->internalDataPath, 0);
        (void)chdir(activity->internalDataPath);
    }
    JavaVM* vm = activity->vm;
    JNIEnv* env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID getCacheDir = cls ? (*env)->GetMethodID(env, cls, "getCacheDir", "()Ljava/io/File;") : NULL;
    jobject dir = getCacheDir ? (*env)->CallObjectMethod(env, activity->clazz, getCacheDir) : NULL;
    jclass fileCls = dir && !(*env)->ExceptionCheck(env) ? (*env)->GetObjectClass(env, dir) : NULL;
    jmethodID abs = fileCls ? (*env)->GetMethodID(env, fileCls, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
    jstring path = abs ? (jstring)(*env)->CallObjectMethod(env, dir, abs) : NULL;
    if (path && !(*env)->ExceptionCheck(env)) {
        const char* cache = (*env)->GetStringUTFChars(env, path, NULL);
        if (cache) {
            setenv("TMPDIR", cache, 0);
            setenv("XDG_CACHE_HOME", cache, 0);
            (*env)->ReleaseStringUTFChars(env, path, cache);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (path) (*env)->DeleteLocalRef(env, path);
    if (fileCls) (*env)->DeleteLocalRef(env, fileCls);
    if (dir) (*env)->DeleteLocalRef(env, dir);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*vm)->DetachCurrentThread(vm);
}

#if defined(KAMA_APP_RUNNER)
// std::app::runner's device half (KR-397): the bodies of kama_app_runner.h in an app, compiled only when the program
// reaches std::app::runner (the emitter says KAMA_APP_RUNNER). `kama run` starts the app with the intent extra
// `kama.runner`, a name, and the app listens on an abstract UNIX socket of that name, which needs no permission; the
// driver reaches it with `adb forward tcp:0 localabstract:<name>` (Chrome DevTools' route) and makes each request's
// steps with the device's `cmd input`. Started any other way the app has no extra, and `begin` answers -1: NoRunner.
//
// The wire, a line each way: the app says `kama-runner 1` once connected, then sends each request as
// {"id": h, "x": .., "y": .., "steps": [..]}, the steps as kama_app_runner.js writes them and x, y the window's origin
// on the screen; the driver answers `<h> ok` once `cmd input` has returned for the last step (it waits for the app to
// handle each event, measured), or `<h> refused <why>`. Each answer wakes std::app's loop. Requests sent before the
// runner connects wait in `unsent`; if the connection ends, every request still pending is refused with the reason.
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "kama_app_runner.h"

typedef struct { int32_t state; int dropped; char* reason; char* steps; size_t len, room; } kama__runner_req;
static pthread_mutex_t kama__runner_mu = PTHREAD_MUTEX_INITIALIZER;
static kama__runner_req* kama__runner_reqs;
static int32_t kama__runner_count, kama__runner_room;
static int kama__runner_listening = -1, kama__runner_fd = -1;
static const char* kama__runner_gone;          // why every request is refused from now on, or NULL
static char* kama__runner_unsent; static size_t kama__runner_unsent_len, kama__runner_unsent_room;

static void kama__runner_put(char** buf, size_t* len, size_t* room, const char* s, size_t n) {
    if (*len + n + 1 > *room) {
        size_t r = *room ? *room : 256;
        while (*len + n + 1 > r) r *= 2;
        char* grown = (char*)(*buf ? kama_realloc(*buf, *room, r, 1) : kama_alloc(r, 1));
        if (!grown) { static const char msg[] = "std::app::runner: out of memory for a request"; kama_panic(kama_string_lit(msg, sizeof msg - 1)); }
        *buf = grown; *room = r;
    }
    memcpy(*buf + *len, s, n); *len += n; (*buf)[*len] = 0;
}
// A buffer's release, by the room it was grown to (the funnel takes each block's layout back, KR-48).
static void kama__runner_drop_buf(char** buf, size_t* len, size_t* room) {
    if (*buf) kama_free(*buf, *room, 1);
    *buf = NULL; *len = *room = 0;
}
// A refusal's reason, kept for the program to read: strlen + 1 bytes, which is how it is released.
static char* kama__runner_copy(const char* s) {
    const size_t n = strlen(s) + 1;
    char* c = (char*)kama_alloc(n, 1);
    if (!c) { static const char msg[] = "std::app::runner: out of memory for a request"; kama_panic(kama_string_lit(msg, sizeof msg - 1)); }
    memcpy(c, s, n);
    return c;
}
static void kama__runner_drop_reason(kama__runner_req* r) {
    if (r->reason) kama_free(r->reason, strlen(r->reason) + 1, 1);
    r->reason = NULL;
}
static void kama__runner_puts(kama__runner_req* r, const char* s) { kama__runner_put(&r->steps, &r->len, &r->room, s, strlen(s)); }
static void kama__runner_put_string(kama__runner_req* r, const char* s) {   // a JSON string; UTF-8 passes as it is
    kama__runner_puts(r, "\"");
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; e[2] = 0; }
        else if (*p < 0x20) snprintf(e, sizeof e, "\\u%04x", *p);
        else { e[0] = (char)*p; e[1] = 0; }
        kama__runner_puts(r, e);
    }
    kama__runner_puts(r, "\"");
}
static int kama__runner_write(int fd, const char* s, size_t n) {   // MSG_NOSIGNAL: a runner gone is an answer, not SIGPIPE
    while (n) {
        const ssize_t k = send(fd, s, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        s += k; n -= (size_t)k;
    }
    return 1;
}
static void kama__runner_wake(void) {
#if defined(KAMA_APP_H)
    (void)kama__app_wake(kama_app_waker());   // std::app's loop hears each answer, an app on demand too
#endif
}
// Under the lock. An answer for a request dropped, already answered, or never made is ignored.
static void kama__runner_settle(long h, int32_t state, const char* reason) {
    if (h < 0 || h >= kama__runner_count) return;
    kama__runner_req* r = &kama__runner_reqs[h];
    if (r->dropped || r->state != 0) return;
    r->state = state;
    if (reason) r->reason = kama__runner_copy(reason);
}
// The connection is over, or never came: every request still pending is refused with `why`, and so is every later one.
static void kama__runner_end(const char* why) {
    pthread_mutex_lock(&kama__runner_mu);
    kama__runner_gone = why;
    kama__runner_fd = -1;
    for (int32_t h = 0; h < kama__runner_count; ++h) kama__runner_settle(h, 2, why);
    pthread_mutex_unlock(&kama__runner_mu);
    kama__runner_wake();
}

// The one connection: the driver's. Its answers, a line at a time, until it closes.
static void* kama__runner_serve(void* unused) {
    (void)unused;
    int c;
    do c = accept4(kama__runner_listening, NULL, NULL, SOCK_CLOEXEC); while (c < 0 && errno == EINTR);
    close(kama__runner_listening);   // one runner: a second connection is refused
    if (c < 0) { kama__runner_end("the runner could not connect to the app"); return NULL; }
    pthread_mutex_lock(&kama__runner_mu);
    int ok = kama__runner_write(c, "kama-runner 1\n", 14) &&
             (!kama__runner_unsent_len || kama__runner_write(c, kama__runner_unsent, kama__runner_unsent_len));
    kama__runner_drop_buf(&kama__runner_unsent, &kama__runner_unsent_len, &kama__runner_unsent_room);
    if (ok) kama__runner_fd = c;
    pthread_mutex_unlock(&kama__runner_mu);
    char buf[4096];
    size_t have = 0;
    while (ok) {
        const ssize_t n = read(c, buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        have += (size_t)n;
        size_t start = 0;
        int heard = 0;
        pthread_mutex_lock(&kama__runner_mu);
        for (size_t k = 0; k < have; ++k) {
            if (buf[k] != '\n' && !(start == 0 && k == sizeof buf - 2)) continue;   // a line, or a full buffer
            buf[k] = 0;
            char* end;
            const long h = strtol(buf + start, &end, 10);
            if (end != buf + start && *end == ' ') {
                if (!strcmp(end + 1, "ok")) { kama__runner_settle(h, 1, NULL); heard = 1; }
                else if (!strncmp(end + 1, "refused ", 8)) { kama__runner_settle(h, 2, end + 9); heard = 1; }
            }
            start = k + 1;
        }
        pthread_mutex_unlock(&kama__runner_mu);
        memmove(buf, buf + start, have - start); have -= start;
        if (heard) kama__runner_wake();
    }
    kama__runner_end("the runner went away before it answered");   // first: no send writes to `c` once it is closed
    close(c);
    return NULL;
}

// Read the extra and, when there is one, listen; before `main`, so the driver can connect while the program starts.
static void kama__runner_start(ANativeActivity* activity) {
    char name[100] = { 0 };
    JavaVM* vm = activity->vm;
    JNIEnv* env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID getIntent = cls ? (*env)->GetMethodID(env, cls, "getIntent", "()Landroid/content/Intent;") : NULL;
    jobject intent = getIntent ? (*env)->CallObjectMethod(env, activity->clazz, getIntent) : NULL;
    jclass icls = intent && !(*env)->ExceptionCheck(env) ? (*env)->GetObjectClass(env, intent) : NULL;
    jmethodID get = icls ? (*env)->GetMethodID(env, icls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;") : NULL;
    jstring key = get ? (*env)->NewStringUTF(env, "kama.runner") : NULL;
    jstring value = key ? (jstring)(*env)->CallObjectMethod(env, intent, get, key) : NULL;
    if (value && !(*env)->ExceptionCheck(env)) {
        const char* s = (*env)->GetStringUTFChars(env, value, NULL);
        if (s) { snprintf(name, sizeof name, "%s", s); (*env)->ReleaseStringUTFChars(env, value, s); }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (value) (*env)->DeleteLocalRef(env, value);
    if (key) (*env)->DeleteLocalRef(env, key);
    if (icls) (*env)->DeleteLocalRef(env, icls);
    if (intent) (*env)->DeleteLocalRef(env, intent);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*vm)->DetachCurrentThread(vm);
    if (!*name) return;
    kama__runner_asked = 1;
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    const size_t n = strlen(name);
    memcpy(a.sun_path + 1, name, n);   // sun_path[0] is 0: the abstract namespace, which needs no file and no permission
    pthread_t t;
    if (s < 0 || bind(s, (struct sockaddr*)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) != 0
        || listen(s, 1) != 0) {
        fprintf(stderr, "kama: the app cannot listen for its runner on @%s: %s\n", name, strerror(errno));
        if (s >= 0) close(s);
        kama__runner_gone = "the app could not listen for its runner";
        return;
    }
    kama__runner_listening = s;
    if (pthread_create(&t, NULL, kama__runner_serve, NULL) != 0) {
        close(s);
        kama__runner_gone = "the app could not start the thread that hears its runner";
        return;
    }
    pthread_detach(t);
}

// The window's origin on the screen, which `cmd input` places in: a request's positions are the window's pixels, as
// its events report them. NativeActivity's window fills the display at 0,0 (measured on API 34), but a window in
// split screen or freeform does not. Asked through JNI on the loop's thread; 0,0 if any step fails.
static void kama__runner_origin(int* x, int* y) {
    *x = *y = 0;
    ANativeActivity* act = kama__android_app ? kama__android_app->activity : NULL;
    if (!act) return;
    JavaVM* vm = act->vm;
    JNIEnv* env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, act->clazz);
    jmethodID getWindow = cls ? (*env)->GetMethodID(env, cls, "getWindow", "()Landroid/view/Window;") : NULL;
    jobject window = getWindow ? (*env)->CallObjectMethod(env, act->clazz, getWindow) : NULL;
    jclass wcls = window && !(*env)->ExceptionCheck(env) ? (*env)->GetObjectClass(env, window) : NULL;
    jmethodID peek = wcls ? (*env)->GetMethodID(env, wcls, "peekDecorView", "()Landroid/view/View;") : NULL;
    jobject decor = peek ? (*env)->CallObjectMethod(env, window, peek) : NULL;
    jclass vcls = decor && !(*env)->ExceptionCheck(env) ? (*env)->GetObjectClass(env, decor) : NULL;
    jmethodID locate = vcls ? (*env)->GetMethodID(env, vcls, "getLocationOnScreen", "([I)V") : NULL;
    jintArray at = locate ? (*env)->NewIntArray(env, 2) : NULL;
    if (at) {
        (*env)->CallVoidMethod(env, decor, locate, at);
        jint xy[2] = { 0, 0 };
        if (!(*env)->ExceptionCheck(env)) { (*env)->GetIntArrayRegion(env, at, 0, 2, xy); *x = xy[0]; *y = xy[1]; }
        (*env)->DeleteLocalRef(env, at);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (vcls) (*env)->DeleteLocalRef(env, vcls);
    if (decor) (*env)->DeleteLocalRef(env, decor);
    if (wcls) (*env)->DeleteLocalRef(env, wcls);
    if (window) (*env)->DeleteLocalRef(env, window);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*vm)->DetachCurrentThread(vm);
}

int32_t kama_app_runner_begin(void) {
    if (!kama__runner_asked) return -1;
    pthread_mutex_lock(&kama__runner_mu);
    if (kama__runner_count == kama__runner_room) {
        const int32_t room = kama__runner_room ? kama__runner_room * 2 : 8;
        kama__runner_req* grown = (kama__runner_req*)(kama__runner_reqs
            ? kama_realloc(kama__runner_reqs, (size_t)kama__runner_room * sizeof *grown, (size_t)room * sizeof *grown, _Alignof(kama__runner_req))
            : kama_alloc((size_t)room * sizeof *grown, _Alignof(kama__runner_req)));
        if (!grown) { static const char msg[] = "std::app::runner: out of memory for a request"; kama_panic(kama_string_lit(msg, sizeof msg - 1)); }
        kama__runner_reqs = grown; kama__runner_room = room;
    }
    const int32_t h = kama__runner_count++;
    memset(&kama__runner_reqs[h], 0, sizeof kama__runner_reqs[h]);
    pthread_mutex_unlock(&kama__runner_mu);
    return h;
}

// kind: 0 move, 1 press, 2 release, 3 key down, 4 key up, 5 text, 6 wheel, 7 pause (x is its milliseconds), as the .js.
void kama_app_runner_step(int32_t h, int32_t kind, int32_t pointer, int32_t id, int32_t button,
                          double x, double y, double dx, double dy, const char* text) {
    static const char* const pointers[] = { "mouse", "pen", "touch" };
    const char* p = pointer >= 0 && pointer < 3 ? pointers[pointer] : "mouse";
    char s[256];
    pthread_mutex_lock(&kama__runner_mu);
    kama__runner_req* r = h >= 0 && h < kama__runner_count ? &kama__runner_reqs[h] : NULL;
    if (r && !r->dropped && r->state == 0) {
        if (r->len) kama__runner_puts(r, ",");
        if (kind == 0) snprintf(s, sizeof s, "{\"k\":\"move\",\"p\":\"%s\",\"id\":%d,\"x\":%.17g,\"y\":%.17g}", p, id, x, y);
        else if (kind == 1 || kind == 2) snprintf(s, sizeof s, "{\"k\":\"%s\",\"p\":\"%s\",\"id\":%d,\"b\":%d}", kind == 1 ? "down" : "up", p, id, button);
        else if (kind == 6) snprintf(s, sizeof s, "{\"k\":\"wheel\",\"x\":%.17g,\"y\":%.17g,\"dx\":%.17g,\"dy\":%.17g}", x, y, dx, dy);
        else if (kind == 7) snprintf(s, sizeof s, "{\"k\":\"pause\",\"ms\":%.17g}", x);
        else snprintf(s, sizeof s, "{\"k\":\"%s\",\"%s\":", kind == 3 ? "keydown" : kind == 4 ? "keyup" : "text", kind == 5 ? "text" : "code");
        kama__runner_puts(r, s);
        if (kind >= 3 && kind <= 5) { kama__runner_put_string(r, text ? text : ""); kama__runner_puts(r, "}"); }
    }
    pthread_mutex_unlock(&kama__runner_mu);
}

void kama_app_runner_send(int32_t h) {
    int ox, oy;
    kama__runner_origin(&ox, &oy);
    pthread_mutex_lock(&kama__runner_mu);
    kama__runner_req* r = h >= 0 && h < kama__runner_count ? &kama__runner_reqs[h] : NULL;
    if (r && !r->dropped && r->state == 0) {
        char head[96];
        const int n = snprintf(head, sizeof head, "{\"id\":%d,\"x\":%d,\"y\":%d,\"steps\":[", h, ox, oy);
        char* line = NULL; size_t len = 0, room = 0;
        kama__runner_put(&line, &len, &room, head, (size_t)n);
        if (r->len) kama__runner_put(&line, &len, &room, r->steps, r->len);
        kama__runner_put(&line, &len, &room, "]}\n", 3);
        kama__runner_drop_buf(&r->steps, &r->len, &r->room);
        if (kama__runner_gone) kama__runner_settle(h, 2, kama__runner_gone);
        else if (kama__runner_fd >= 0) { if (!kama__runner_write(kama__runner_fd, line, len)) kama__runner_settle(h, 2, "the runner went away before it answered"); }
        else kama__runner_put(&kama__runner_unsent, &kama__runner_unsent_len, &kama__runner_unsent_room, line, len);
        kama__runner_drop_buf(&line, &len, &room);
    }
    pthread_mutex_unlock(&kama__runner_mu);
}

int32_t kama_app_runner_state(int32_t h) {
    pthread_mutex_lock(&kama__runner_mu);
    const int32_t s = h >= 0 && h < kama__runner_count ? kama__runner_reqs[h].state : 2;
    pthread_mutex_unlock(&kama__runner_mu);
    return s;
}

int32_t kama_app_runner_reason(int32_t h, uint8_t* buf, size_t n) {
    pthread_mutex_lock(&kama__runner_mu);
    const char* why = h >= 0 && h < kama__runner_count && kama__runner_reqs[h].reason ? kama__runner_reqs[h].reason : "";
    const size_t len = strlen(why);
    memcpy(buf, why, len < n ? len : n);
    pthread_mutex_unlock(&kama__runner_mu);
    return (int32_t)len;
}

void kama_app_runner_drop(int32_t h) {
    pthread_mutex_lock(&kama__runner_mu);
    if (h >= 0 && h < kama__runner_count) {
        kama__runner_req* r = &kama__runner_reqs[h];
        r->dropped = 1;
        kama__runner_drop_reason(r);
        kama__runner_drop_buf(&r->steps, &r->len, &r->room);
    }
    pthread_mutex_unlock(&kama__runner_mu);
}
#endif  // KAMA_APP_RUNNER

void android_main(struct android_app* app) {
    pthread_t pump;
    int fds[2], piped = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a csources file's printf, a line at a time */
    if (pipe(fds) == 0) {   /* the reader first: a pipe nobody reads would block the program once it filled */
        kama__app_log_fd = fds[0];
        if (pthread_create(&pump, NULL, kama__app_log_pump, NULL) == 0) {
            piped = 1;
            dup2(fds[1], 1);
            dup2(fds[1], 2);
        } else {
            close(fds[0]);
        }
        close(fds[1]);
    }
    static char name[256];
    static char* argv[2] = { name, NULL };
    const int cf = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (cf >= 0) {
        const ssize_t n = read(cf, name, sizeof name - 1);
        if (n > 0) name[n] = 0;
        close(cf);
    }
    kama_args_init(1, argv);
    kama__android_dirs(app->activity);
#if defined(KAMA_APP_RUNNER)
    kama__runner_start(app->activity);
#endif
    kama__android_app = app;
    kama__android_thread = pthread_self();
    app->onInputEvent = kama__android_on_input;
    const int rc = kama_app_main();
    app->onInputEvent = NULL;   /* the program is over: its handler's state may be gone */
    fflush(NULL);
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        struct android_poll_source* src = NULL;
        int events = 0;
        if (ALooper_pollOnce(-1, NULL, &events, (void**)&src) == ALOOPER_POLL_ERROR) break;
        if (src) src->process(app, src);
    }
    if (piped) { close(1); close(2); pthread_join(pump, NULL); }   /* the reader drains the pipe, then sees it close */
    exit(rc);
}

#endif  // KAMA_ANDROID_APP
#endif
