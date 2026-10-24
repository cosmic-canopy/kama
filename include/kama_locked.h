// kama_locked.h — the cell behind std::concurrent's `Locked<T>` and `ReadWriteLocked<T>` (KR-226).
//
// One heap cell holds the handles' count, the OS lock and the payload, so a `Locked` handle is one pointer and its
// copies share the cell. The payload is reached only through a guard a `borrow` window holds, so it cannot be touched
// without the lock; the window may reach no wait and no other lock (the compiler's lock-window walk), and the guard's
// destructor releases on the thread that acquired, which `os_unfair_lock` requires.
//
// The lock, per OS — `Locked` takes the fastest exclusive lock there is, `ReadWriteLocked` the reader/writer twin:
//
//   | OS              | `Locked`                  | `ReadWriteLocked`                                         |
//   |-----------------|---------------------------|-----------------------------------------------------------|
//   | macOS           | `os_unfair_lock`, 4 B     | `pthread_rwlock_t`, 200 B                                 |
//   | Linux (glibc)   | `pthread_mutex_t` (futex) | `pthread_rwlock_t`, WRITER-preferring (see below)          |
//   | Windows         | `SRWLOCK`, 8 B            | `SRWLOCK` (exclusive and shared)                          |
//   | wasm, threaded  | `pthread_mutex_t`         | `pthread_rwlock_t` (emscripten: Atomics.wait)             |
//   | no threads      | a flag                    | a flag (unthreaded wasm, `--target embedded`)             |
//
// glibc's rwlock prefers READERS by default, so a steady read load starves a writer forever; its
// `PTHREAD_RWLOCK_PREFER_WRITER_NP` is accepted and ignored. `..._PREFER_WRITER_NONRECURSIVE_NP` is the one that
// works, and its one hazard — a thread re-taking a read lock while a writer waits deadlocks — cannot be written in
// kama, where a lock window takes no other lock. musl and emscripten have no knob; Windows' SRWLOCK and macOS's rwlock
// make no promise either way.
#ifndef KAMA_LOCKED_H
#define KAMA_LOCKED_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>          /* memcpy */
#include "kama_runtime.h"    /* kama_alloc, kama_free, kama_panic, kama_locks */

#if defined(KAMA_TARGET_EMBEDDED) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
  #define KAMA_LOCKED_THREADS 0
#else
  #define KAMA_LOCKED_THREADS 1
#endif

#if KAMA_LOCKED_THREADS && defined(_WIN32)
  // Not <windows.h>: headers are emitted in name order, and this one sorts before kama_os.h, which must include
  // <winsock2.h> first. The tag is the one <synchapi.h> defines, so a later include agrees (kama_runtime.h does the
  // same for CreateFileA).
  struct _RTL_SRWLOCK;
  typedef struct { void* ptr; } kama__srw;   /* SRWLOCK_INIT is all zero */
#elif KAMA_LOCKED_THREADS
  #include <pthread.h>
  #if defined(__APPLE__)
    #include <os/lock.h>
  #endif
  #if defined(__GLIBC__)
    // Declared here because kama compiles with `-std=c11 -D_DEFAULT_SOURCE`, and glibc declares it only under
    // `_GNU_SOURCE`; compatible with that declaration when a user's flags supply it.
    extern int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t* attr, int pref);
    #define KAMA__RWLOCK_PREFER_WRITER_NONRECURSIVE 2   /* PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP */
  #endif
#endif

// The header; the lock follows it and the payload follows that, at `off`.
typedef struct kama__locked {
    int32_t refs;   // handles sharing this cell (atomic)
    int32_t rw;     // 1: a reader/writer lock (`ReadWriteLocked`), 0: exclusive (`Locked`)
    size_t  off;    // the payload's offset from the cell
    size_t  bytes;  // the cell's size and alignment, which the sized free is handed back
    size_t  align;
} kama__locked;

#if !KAMA_LOCKED_THREADS
  typedef int32_t kama__xlock;
  typedef int32_t kama__rwlock;
#elif defined(_WIN32)
  typedef kama__srw kama__xlock;
  typedef kama__srw kama__rwlock;
#elif defined(__APPLE__)
  typedef os_unfair_lock   kama__xlock;
  typedef pthread_rwlock_t kama__rwlock;
#else
  typedef pthread_mutex_t  kama__xlock;
  typedef pthread_rwlock_t kama__rwlock;
#endif

#define KAMA__LOCK_AT(c) ((void*)((char*)(c) + ((sizeof(kama__locked) + 15) & ~(size_t)15)))

static inline size_t kama__locked_lock_size(int32_t rw) { return rw ? sizeof(kama__rwlock) : sizeof(kama__xlock); }

// A new cell holding a copy of `size` bytes at `src` (the caller forgets its own copy: the bytes move).
static inline void* kama_locked_new(const void* src, size_t size, size_t align, int32_t rw) {
    const size_t lockAt = (sizeof(kama__locked) + 15) & ~(size_t)15;
    if (align < 16) align = 16;
    const size_t off = (lockAt + kama__locked_lock_size(rw) + align - 1) & ~(align - 1);
    const size_t bytes = off + (size ? size : 1);
    kama__locked* c = (kama__locked*)kama_alloc(bytes, align);
    c->refs = 1; c->rw = rw; c->off = off; c->bytes = bytes; c->align = align;
    void* l = KAMA__LOCK_AT(c);
#if !KAMA_LOCKED_THREADS
    *(int32_t*)l = 0;
#elif defined(_WIN32)
    ((kama__srw*)l)->ptr = 0;
#else
    if (rw) {
        pthread_rwlockattr_t a;
        pthread_rwlockattr_init(&a);
  #if defined(__GLIBC__)
        pthread_rwlockattr_setkind_np(&a, KAMA__RWLOCK_PREFER_WRITER_NONRECURSIVE);
  #endif
        pthread_rwlock_init((pthread_rwlock_t*)l, &a);
        pthread_rwlockattr_destroy(&a);
    } else {
  #if defined(__APPLE__)
        *(os_unfair_lock*)l = OS_UNFAIR_LOCK_INIT;
  #else
        pthread_mutex_init((pthread_mutex_t*)l, NULL);
  #endif
    }
#endif
    if (size) memcpy((char*)c + off, src, size);
    return c;
}

static inline void* kama_locked_payload(void* cell) { kama__locked* c = (kama__locked*)cell; return (char*)c + c->off; }

static inline void kama_locked_retain(void* cell) { __atomic_add_fetch(&((kama__locked*)cell)->refs, 1, __ATOMIC_RELAXED); }

// Non-zero when this was the last handle: the caller drops the payload, then frees the cell.
static inline int32_t kama_locked_release(void* cell) {
    return __atomic_sub_fetch(&((kama__locked*)cell)->refs, 1, __ATOMIC_ACQ_REL) == 0;
}

static inline void kama_locked_free(void* cell) {
    kama__locked* c = (kama__locked*)cell;
#if KAMA_LOCKED_THREADS && !defined(_WIN32)
    void* l = KAMA__LOCK_AT(c);
    if (c->rw) pthread_rwlock_destroy((pthread_rwlock_t*)l);
  #if !defined(__APPLE__)
    else pthread_mutex_destroy((pthread_mutex_t*)l);
  #endif
#endif
    kama_free(c, c->bytes, c->align);
}

// Defence in depth behind the lock-window walk: a lock window reaches no other lock's acquisition, so a thread that
// holds one and asks for another got there through code the walk cannot read — a C callback into kama. Trapped, not
// left to deadlock.
static inline void kama__locked_enter(void) {
    static const char msg[] = "a lock is taken inside a lock window: a foreign callback re-entered kama while this "
                              "thread held a lock, which can deadlock (KR-226)";
    if (kama_locks.held > 0) kama_panic(kama_string_lit(msg, sizeof msg - 1));
}

static inline void kama__locked_x(void* l, int lock) {
#if !KAMA_LOCKED_THREADS
    *(int32_t*)l = lock;
#elif defined(_WIN32)
    if (lock) { extern void __stdcall AcquireSRWLockExclusive(struct _RTL_SRWLOCK*); AcquireSRWLockExclusive((struct _RTL_SRWLOCK*)l); }
    else      { extern void __stdcall ReleaseSRWLockExclusive(struct _RTL_SRWLOCK*); ReleaseSRWLockExclusive((struct _RTL_SRWLOCK*)l); }
#elif defined(__APPLE__)
    if (lock) os_unfair_lock_lock((os_unfair_lock*)l); else os_unfair_lock_unlock((os_unfair_lock*)l);
#else
    if (lock) pthread_mutex_lock((pthread_mutex_t*)l); else pthread_mutex_unlock((pthread_mutex_t*)l);
#endif
}

static inline void kama__locked_rw(void* l, int lock, int shared) {
#if !KAMA_LOCKED_THREADS
    *(int32_t*)l = lock;
#elif defined(_WIN32)
    if (lock && shared)  { extern void __stdcall AcquireSRWLockShared(struct _RTL_SRWLOCK*); AcquireSRWLockShared((struct _RTL_SRWLOCK*)l); }
    else if (lock)       { extern void __stdcall AcquireSRWLockExclusive(struct _RTL_SRWLOCK*); AcquireSRWLockExclusive((struct _RTL_SRWLOCK*)l); }
    else if (shared)     { extern void __stdcall ReleaseSRWLockShared(struct _RTL_SRWLOCK*); ReleaseSRWLockShared((struct _RTL_SRWLOCK*)l); }
    else                 { extern void __stdcall ReleaseSRWLockExclusive(struct _RTL_SRWLOCK*); ReleaseSRWLockExclusive((struct _RTL_SRWLOCK*)l); }
#else
    if (!lock)       pthread_rwlock_unlock((pthread_rwlock_t*)l);
    else if (shared) pthread_rwlock_rdlock((pthread_rwlock_t*)l);
    else             pthread_rwlock_wrlock((pthread_rwlock_t*)l);
#endif
}

// Several locks in one `borrow` (KR-226, the group the compiler brackets the mints with): each mint RECORDS its cell, and
// the group's end takes them all in ADDRESS order. Two `borrow`s that name `a, b` and `b, a` then take them the same
// way, so neither can hold one and wait for the other — the deadlock-free order C++'s `std::lock` gets by trying and
// backing off, here without a retry. A cell named twice (two handles to one lock) traps: it would be taken twice.
typedef struct kama_lockgroup { int32_t n; int32_t cap; void** cells; int8_t* shared; } kama_lockgroup;

static inline void kama_lockgroup_begin(kama_lockgroup* g) { g->n = 0; kama_locks.group = g; }

static inline int kama__lockgroup_record(void* cell, int shared) {
    kama_lockgroup* g = kama_locks.group;
    if (!g) return 0;
    if (g->n < g->cap) { g->cells[g->n] = cell; g->shared[g->n] = (int8_t)shared; ++g->n; }
    return 1;
}

static inline void kama_lockgroup_end(kama_lockgroup* g, int line) {
    kama_locks.group = 0;
    for (int32_t i = 1; i < g->n; ++i) {   // insertion sort: a handful of cells
        void* c = g->cells[i]; int8_t s = g->shared[i]; int32_t j = i - 1;
        while (j >= 0 && (uintptr_t)g->cells[j] > (uintptr_t)c) { g->cells[j + 1] = g->cells[j]; g->shared[j + 1] = g->shared[j]; --j; }
        g->cells[j + 1] = c; g->shared[j + 1] = s;
    }
    for (int32_t i = 1; i < g->n; ++i)
        if (g->cells[i] == g->cells[i - 1]) {
            static const char msg[] = "a lock is taken twice by one `borrow`: two of its handles name the same lock (KR-226)";
            (void)line;
            kama_panic(kama_string_lit(msg, sizeof msg - 1));
        }
    kama__locked_enter();
    for (int32_t i = 0; i < g->n; ++i) {
        kama__locked* c = (kama__locked*)g->cells[i];
        if (g->shared[i]) kama__locked_rw(KAMA__LOCK_AT(c), 1, 1);
        else if (c->rw)   kama__locked_rw(KAMA__LOCK_AT(c), 1, 0);
        else              kama__locked_x(KAMA__LOCK_AT(c), 1);
    }
    kama_locks.held += g->n;
}

static inline void kama_locked_acquire(void* cell) {
    kama__locked* c = (kama__locked*)cell;
    if (kama__lockgroup_record(cell, 0)) return;
    kama__locked_enter();
    if (c->rw) kama__locked_rw(KAMA__LOCK_AT(c), 1, 0); else kama__locked_x(KAMA__LOCK_AT(c), 1);
    ++kama_locks.held;
}

static inline void kama_locked_acquire_shared(void* cell) {
    if (kama__lockgroup_record(cell, 1)) return;
    kama__locked_enter();
    kama__locked_rw(KAMA__LOCK_AT(cell), 1, 1);
    ++kama_locks.held;
}

static inline void kama_locked_unlock(void* cell) {
    kama__locked* c = (kama__locked*)cell;
    --kama_locks.held;
    if (c->rw) kama__locked_rw(KAMA__LOCK_AT(c), 0, 0); else kama__locked_x(KAMA__LOCK_AT(c), 0);
}

static inline void kama_locked_unlock_shared(void* cell) {
    --kama_locks.held;
    kama__locked_rw(KAMA__LOCK_AT(cell), 0, 1);
}

#endif  // KAMA_LOCKED_H
