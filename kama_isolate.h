#ifndef KAMA_ISOLATE_H
#define KAMA_ISOLATE_H

// The native isolate seam — spawn a top-level fn on a fresh OS thread with a MOVED-in argument
// bundle, and join it. Shared-nothing by construction: the entry takes ownership of the bundle, so
// no memory is aliased across the thread boundary (the whole M2 concurrency model in one header).
//
// A dedicated pay-for-what-you-use header (like kama_os.h / kama_gpu.h), pulled in only by a program
// that `extern "kama_isolate.h";`'s — NOT folded into kama_runtime.h, which must stay usable on the
// freestanding/MCU path where <pthread.h> does not exist. (And pthread_t is not void* on glibc, so
// the block-scoped-extern trick kama_runtime.h uses elsewhere would be unsafe here.)

#include <pthread.h>
#include "kama_runtime.h"   /* kama_panic, kama_string_lit */

typedef pthread_t kama_isolate_t;

// Spawn `entry` on a new thread with `arg` (the moved bundle pointer). Panics on failure — spawn is
// not a recoverable condition in the M2 surface (no channel to report back over yet).
static inline kama_isolate_t kama_isolate_spawn(void* (*entry)(void*), void* arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, entry, arg) != 0)
        kama_panic(kama_string_lit("isolate spawn failed", 20));
    return t;
}

// Block until the isolate finishes. Its result is dropped inside the trampoline, so there is nothing
// to hand back (M2 has no channels); the join is purely the RAII/structured-lifetime barrier.
static inline void kama_isolate_join(kama_isolate_t h) { pthread_join(h, NULL); }

// Heap-BOXED handle, for the RAII `Isolate` type (`Isolate h = isolate worker(...);`). kama holds the box
// as an opaque `Ptr` (void*) — portable, since a bare kama_isolate_t is not always pointer-sized. spawn_boxed
// mallocs the box and spawns; join_boxed joins the thread and frees the box. The handle owns the join, so
// dropping it (scope exit) joins — a forgotten join can't orphan the thread.
static inline void* kama_isolate_spawn_boxed(void* (*entry)(void*), void* arg) {
    kama_isolate_t* box = (kama_isolate_t*)malloc(sizeof(kama_isolate_t));
    if (!box) kama_panic(kama_string_lit("isolate spawn failed", 20));
    *box = kama_isolate_spawn(entry, arg);
    return box;
}
static inline void kama_isolate_join_boxed(void* h) {
    kama_isolate_t* box = (kama_isolate_t*)h;
    kama_isolate_join(*box);
    free(box);
}

#endif
