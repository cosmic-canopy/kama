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

// The glue's input hook: std::app::android's handler, given each event; one that is not handled goes back to Android
// (Back then finishes the activity).
static int32_t kama__android_on_input(struct android_app* app, AInputEvent* event) {
    (void)app;
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
