#ifndef KAMA_CHANNEL_H
#define KAMA_CHANNEL_H

// The native channel seam — a typed, blocking pipe between isolates. ONE heap queue is co-owned by
// a Sender and a Receiver living on different threads; it is the first sanctioned cross-isolate
// shared object in the concurrency model (M3). All thread-safety lives HERE, in C: a single
// pthread_mutex serializes every state transition (send, recv, endpoint-drop), so no atomics are
// needed — the mutex also serializes liveness + free, so the second endpoint to drop frees the queue
// race-free. The kama side stays a thin `UnsafePtr`-handle library (Channel/Sender/Receiver), exactly like
// the Isolate handle wraps kama_isolate.h.
//
// A pay-for-what-you-use header (like kama_isolate.h) pulled in only by a program that
// `extern "kama_channel.h";`'s (std::concurrent's channel.kama) — NOT folded into kama_runtime.h,
// which must stay usable on the freestanding/MCU path where <pthread.h> does not exist.

#include <pthread.h>
#include <string.h>   /* memcpy */
#include <time.h>     /* clock_gettime, for the timed receive */
#include <errno.h>    /* ETIMEDOUT */
#include "kama_runtime.h"   /* kama_panic, kama_string_lit */

// `clock_gettime` and the clock ids are POSIX, and the strict `-std=c11` build may hide them: emscripten's <time.h>
// always does (every channel fixture failed to compile on the wasm leg at 0.9.475). kama_time.h's convention, and its
// id table: declare the one libc symbol, and name the ids (stable kernel ABI constants). Restated rather than reached
// by including kama_time.h, which on Windows is <windows.h> — and headers are included in name order, so it would land
// in front of a kama_os.h whose <winsock2.h> must come first.
#ifdef CLOCK_MONOTONIC
#  define KAMA__CHANNEL_MONO CLOCK_MONOTONIC
#  define KAMA__CHANNEL_WALL CLOCK_REALTIME
#else
extern int clock_gettime(int clk_id, struct timespec* ts);
#  if defined(__APPLE__)
#    define KAMA__CHANNEL_MONO 6
#  elif defined(__FreeBSD__)
#    define KAMA__CHANNEL_MONO 4
#  else
#    define KAMA__CHANNEL_MONO 1      /* glibc, musl (and so emscripten) */
#  endif
#  define KAMA__CHANNEL_WALL 0
#endif

// A bounded (ring-buffer) channel. `cap` is the buffer capacity in elements; `elemSize` the bitwise
// size of the moved value T. `count`/`head`/`tail` drive the ring.
//
// Endpoints are COUNTED, not flagged. They were two booleans, which silently assumed an invariant the
// API never enforced: `Channel.sender()`/`.receiver()` mint endpoints without limit, so a second one on
// either side made the first to drop believe it was last — it drained the ring and freed the struct out
// from under a peer still parked in pthread_cond_wait. A side is closed when its count reaches zero, so
// any number of Senders/Receivers is now correct rather than corrupt; this mirrors how a Shared<T>
// control block already works, with the count moving exactly where ownership does.
//
// ⚠️ A side is also OPEN BEFORE IT IS EVER CLAIMED, which is why `claimed` exists beside each count. A
// channel is born open on both sides (the flags started at 1), and programs depend on it: a worker that
// receives the Channel itself and mints its Sender inside the isolate races main's first recv(), which
// must BLOCK rather than see "zero senders, closed" and return None. The Channel holds no share of a SIDE —
// that would break the opposite case: channel_close leaves `ch` alive in main for the whole run while close
// must come from the producer's Sender drop. It holds a share of the MEMORY (`factoryLive`): an unclaimed side
// is open only while the factory that could claim it exists, and the queue is freed by whichever of the
// senders, the receivers and the factory lets go last (`kama__channel_unheld`).
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  notEmpty;   /* a blocked recv waits here; a send / last-sender-drop signals it */
    pthread_cond_t  notFull;    /* a blocked send waits here; a recv / last-receiver-drop signals it */
    unsigned char*  buf;        /* cap * elemSize bytes */
    size_t          elemSize;
    size_t          kama_cap;
    size_t          count;
    size_t          head;       /* next slot to read  */
    size_t          tail;       /* next slot to write */
    int             senders;         /* live Sender endpoints   */
    int             receivers;       /* live Receiver endpoints */
    int             senderClaimed;   /* has sender()   ever been called (see the "born open" note) */
    int             receiverClaimed; /* has receiver() ever been called */
    unsigned long   takenSeq;        /* rendezvous only: bumped each time a receiver takes the slot */
    int             monotonic;       /* `notEmpty`'s timed wait runs on CLOCK_MONOTONIC (see kama__channel_cond_init) */
    int             factoryLive;     /* the `Channel` factory still exists, so an unclaimed side may yet be claimed */
} kama_channel_t;

// "The sender side can still deliver" — a live sender exists, or none has been claimed yet while the factory that
// could still claim one exists ("born open"). Callers hold `mu`. The recv-side twin is what tells a blocked `send`
// whether anyone is left to receive. The factory half used to be missing: an unclaimed side stayed open forever, so a
// receiver waiting on a sender nobody could mint any more blocked forever.
static inline int kama_channel_send_open(kama_channel_t* ch) {
    return ch->senders > 0 || (!ch->senderClaimed && ch->factoryLive);
}
static inline int kama_channel_recv_open(kama_channel_t* ch) {
    return ch->receivers > 0 || (!ch->receiverClaimed && ch->factoryLive);
}

// The queue has three kinds of holder — senders, receivers, and the factory — and whichever lets go LAST frees it.
// Callers hold `mu`. It used to be "both sides closed", which leaked the queue of a channel whose factory was dropped
// with a side never claimed (safe code; LeakSanitizer on tests/net_stream_to_isolate.kama), and would have freed the
// queue under a live factory once the factory gets a destructor.
static inline int kama__channel_unheld(kama_channel_t* ch) {
    return ch->senders == 0 && ch->receivers == 0 && !ch->factoryLive;
}

// A timed receive (`Receiver.recvTimeout`) must not be stretched or cut short by the wall clock moving, so the
// condvar a receiver waits on is put on CLOCK_MONOTONIC where the platform lets a condvar choose its clock. Apple
// does not (no pthread_condattr_setclock); it has a RELATIVE timed wait instead, used below. Anywhere setclock is
// refused, `monotonic` stays 0 and the wait runs in wall-clock slices of at most KAMA__CHANNEL_SLICE_NS, each
// recomputed from the monotonic clock — so a wall-clock jump costs at most one slice, never the deadline.
static inline int kama__channel_cond_init(pthread_cond_t* c) {
#if defined(__APPLE__)
    pthread_cond_init(c, NULL);
    return 0;
#else
    pthread_condattr_t a;
    int mono = 0;
    if (pthread_condattr_init(&a) == 0) {
        mono = pthread_condattr_setclock(&a, KAMA__CHANNEL_MONO) == 0;
        pthread_cond_init(c, &a);
        pthread_condattr_destroy(&a);
    } else {
        pthread_cond_init(c, NULL);
    }
    return mono;
#endif
}
static inline int64_t kama__channel_now_ns(int clk) {
    struct timespec t;
    clock_gettime(clk, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}
#define KAMA__CHANNEL_SLICE_NS 50000000LL   /* 50 ms: the most a wall-clock jump can stretch a fallback wait */
// Wait on `notEmpty` until `deadline` (monotonic ns). Returns ETIMEDOUT, without waiting, once the deadline has
// passed — the monotonic clock is the only judge of that. Any wait returns 0: a wakeup, a spurious one, or a slice
// ending early are all "look again", and the caller re-checks its predicate and calls back in.
static inline int kama__channel_wait_until(kama_channel_t* ch, int64_t deadline) {
    int64_t left = deadline - kama__channel_now_ns(KAMA__CHANNEL_MONO);
    if (left <= 0) return ETIMEDOUT;
    struct timespec ts;
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(left / 1000000000LL); ts.tv_nsec = (long)(left % 1000000000LL);
    (void)pthread_cond_timedwait_relative_np(&ch->notEmpty, &ch->mu, &ts);
#else
    if (!ch->monotonic && left > KAMA__CHANNEL_SLICE_NS) left = KAMA__CHANNEL_SLICE_NS;
    int64_t at = ch->monotonic ? deadline : kama__channel_now_ns(KAMA__CHANNEL_WALL) + left;
    ts.tv_sec = (time_t)(at / 1000000000LL); ts.tv_nsec = (long)(at % 1000000000LL);
    (void)pthread_cond_timedwait(&ch->notEmpty, &ch->mu, &ts);
#endif
    return 0;
}

// Create a bounded channel over T (elemSize bytes) with `cap` buffered elements (cap >= 1 for M3.1).
// Returns an opaque handle held by kama as an `UnsafePtr`. Panics on allocation failure (spawn-like: not a
// recoverable condition in the M3 surface).
static inline void* kama_channel_new(size_t elemSize, size_t cap) {
    kama_channel_t* ch = (kama_channel_t*)kama_alloc(sizeof(kama_channel_t), _Alignof(kama_channel_t));
    if (!ch) kama_panic(kama_string_lit("channel alloc failed", 20));
    // The element's alignment is not passed across this seam, so the ring takes the fundamental one: enough for
    // every element short of an over-aligned (`@align(32)`+) type — which is also the most a byte ring can use,
    // since elements are memcpy'd in and out rather than read in place.
    ch->buf = (unsigned char*)kama_alloc((cap ? cap : 1) * elemSize, _Alignof(max_align_t));
    if (!ch->buf) kama_panic(kama_string_lit("channel alloc failed", 20));
    pthread_mutex_init(&ch->mu, NULL);
    ch->monotonic = kama__channel_cond_init(&ch->notEmpty);
    pthread_cond_init(&ch->notFull, NULL);
    ch->elemSize = elemSize;
    ch->kama_cap = cap;
    ch->count = ch->head = ch->tail = 0;
    ch->senders = ch->receivers = 0;
    ch->senderClaimed = ch->receiverClaimed = 0;   /* unclaimed == open; see the struct's note */
    ch->takenSeq = 0;
    ch->factoryLive = 1;
    return ch;
}

// Register one more endpoint on a side. Called by `Channel.sender()` / `.receiver()` BEFORE the handle
// is wrapped, so the count is up before the endpoint can be moved to another isolate and dropped there.
static inline void kama_channel_add_sender(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->senders++;
    ch->senderClaimed = 1;
    pthread_mutex_unlock(&ch->mu);
}

static inline void kama_channel_add_receiver(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->receivers++;
    ch->receiverClaimed = 1;
    pthread_mutex_unlock(&ch->mu);
}

// Free the queue + its buffer + sync primitives. Precondition: called by the queue's LAST holder
// (`kama__channel_unheld`), with the mutex UNLOCKED, so no other thread can reach it. Any items still
// buffered must already have been drained by the caller (kama-side, where element type T is known) —
// this frees the raw buffer without touching element contents.
static inline void kama_channel_free(kama_channel_t* ch) {
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->notEmpty);
    pthread_cond_destroy(&ch->notFull);
    kama_free(ch->buf, (ch->kama_cap ? ch->kama_cap : 1) * ch->elemSize, _Alignof(max_align_t));
    kama_free(ch, sizeof(kama_channel_t), _Alignof(kama_channel_t));
}

// Move one element (elemSize bytes) INTO the channel. Blocks while the ring is full and the receiver
// is still live. Returns 0 on success, -1 if the receiver has gone (the value is NOT enqueued — the
// caller still owns the bytes at `elem`). The bytes are memcpy-relocated: the receiver's recv produces
// the sole owning copy.
//
// Rendezvous (cap == 0): the single `buf[0]` slot is a synchronous hand-off. `send` places the value and
// then BLOCKS until a receiver takes it (`count` returns to 0) — so `send` completing means the value was
// actually received, not merely buffered. `count` doubles as the slot-full flag (0/1); head/tail unused.
static inline int kama_channel_send(void* h, const void* elem) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    if (ch->kama_cap == 0) {                                       /* rendezvous */
        while (ch->count != 0 && kama_channel_recv_open(ch)) /* wait for a free slot (prior hand-off done) */
            pthread_cond_wait(&ch->notFull, &ch->mu);
        if (!kama_channel_recv_open(ch)) { pthread_mutex_unlock(&ch->mu); return -1; }
        memcpy(ch->buf, elem, ch->elemSize);
        ch->count = 1;
        /* ⚠️ Wait for MY item to be taken, not merely for the slot to be free again. `count == 0` was the
           test, and with a second sender it is ambiguous: the receiver takes my item and signals, a peer
           wins the lock and places ITS item, and I wake to `count == 1` and conclude mine was NOT taken —
           disowning a value the receiver already owns, which is a double free. The sequence is
           unambiguous because only a take bumps it. */
        unsigned long mySeq = ch->takenSeq;
        pthread_cond_signal(&ch->notEmpty);                  /* offer it to a receiver */
        while (ch->takenSeq == mySeq && kama_channel_recv_open(ch))
            pthread_cond_wait(&ch->notFull, &ch->mu);
        int taken = (ch->takenSeq != mySeq);
        if (!taken) ch->count = 0;                           /* receiver died: DISOWN the stale copy in buf[0] —
                                                                the caller keeps ownership (SendResult::Undelivered),
                                                                so teardown-drain must not drop it too (double-free) */
        pthread_mutex_unlock(&ch->mu);
        return taken ? 0 : -1;                               /* receiver died before taking → not delivered */
    }
    while (ch->count == ch->kama_cap && kama_channel_recv_open(ch))
        pthread_cond_wait(&ch->notFull, &ch->mu);
    if (!kama_channel_recv_open(ch)) { pthread_mutex_unlock(&ch->mu); return -1; }
    memcpy(ch->buf + ch->tail * ch->elemSize, elem, ch->elemSize);
    ch->tail = (ch->tail + 1) % ch->kama_cap;
    ch->count++;
    pthread_cond_signal(&ch->notEmpty);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

// Take the element at the head into `out`, then unlock. Precondition: the mutex is held and `count > 0`. The one
// place an element leaves the ring for a receiver — shared by the blocking, the non-blocking and the timed receive.
static inline int kama__channel_take(kama_channel_t* ch, void* out) {
    if (ch->kama_cap == 0) {                                       /* rendezvous: take from the single slot */
        memcpy(out, ch->buf, ch->elemSize);
        ch->count = 0;
        ch->takenSeq++;                                      /* THIS is what tells the sender its item landed */
        /* broadcast, not signal: senders park on `notFull` under two different predicates here — "a slot
           is free" (before placing) and "my sequence moved" (after) — and a signal may wake the wrong
           one, which then re-waits while the sender that could proceed never runs. */
        pthread_cond_broadcast(&ch->notFull);
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    memcpy(out, ch->buf + ch->head * ch->elemSize, ch->elemSize);
    ch->head = (ch->head + 1) % ch->kama_cap;
    ch->count--;
    pthread_cond_signal(&ch->notFull);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

// Move one element OUT of the channel into `out` (elemSize bytes). Blocks while the ring is empty and
// the sender is still live. Returns 0 on success, -1 when the channel is closed AND drained (all
// senders dropped and no buffered elements remain) — the receiver's None.
static inline int kama_channel_recv(void* h, void* out) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    while (ch->count == 0 && kama_channel_send_open(ch))
        pthread_cond_wait(&ch->notEmpty, &ch->mu);
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return -1; }   /* drained + senders gone */
    return kama__channel_take(ch, out);
}

// The receive that does not wait (`Receiver.tryRecv`): 0 = an element was written to `out`, 1 = none is
// buffered but a sender is live (Empty), -1 = drained and every sender gone (Closed).
static inline int kama_channel_try_recv(void* h, void* out) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    if (ch->count == 0) { int open = kama_channel_send_open(ch); pthread_mutex_unlock(&ch->mu); return open ? 1 : -1; }
    return kama__channel_take(ch, out);
}

// The receive that waits at most `nanos` (`Receiver.recvTimeout`), with the same three answers as
// kama_channel_try_recv. A zero or negative span is a try. The deadline is monotonic; see kama__channel_cond_init.
static inline int kama_channel_recv_timeout(void* h, void* out, int64_t nanos) {
    kama_channel_t* ch = (kama_channel_t*)h;
    const int64_t now = kama__channel_now_ns(KAMA__CHANNEL_MONO);
    const int64_t deadline = nanos > INT64_MAX - now ? INT64_MAX : now + (nanos > 0 ? nanos : 0);
    pthread_mutex_lock(&ch->mu);
    while (ch->count == 0 && kama_channel_send_open(ch)) {
        if (kama__channel_wait_until(ch, deadline) == ETIMEDOUT) {   /* still empty, still open: not yet */
            pthread_mutex_unlock(&ch->mu);
            return 1;
        }
    }
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return -1; }
    return kama__channel_take(ch, out);
}

// Non-blocking pop of one buffered element into `out` (elemSize bytes). Returns 1 if an element was
// written, 0 if the ring is empty. Used ONLY by the last holder's teardown drain (below): at that point
// every holder is gone and the queue is quiescent, so the lock is a formality. The element is
// relocated out (memcpy) exactly like recv — the caller (kama, which knows T) then drops it.
static inline int kama_channel_try_pop(void* h, void* out) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return 0; }
    memcpy(out, ch->buf + ch->head * ch->elemSize, ch->elemSize);
    if (ch->kama_cap != 0) ch->head = (ch->head + 1) % ch->kama_cap;
    ch->count--;
    pthread_mutex_unlock(&ch->mu);
    return 1;
}

// Drop one sender endpoint. The sender SIDE closes only when the last one goes, and that is when any
// receiver parked in recv is woken (to re-check and return None once drained). Returns 1 if this was the
// last holder of the queue (`kama__channel_unheld`) — the caller must then drain any buffered items (kama-side,
// running each element's ~dtor) and call kama_channel_free. Returns 0 otherwise (someone else frees later).
//
// The decrement and the other holders' state are read under one lock hold, so no two holders can both
// observe "last": whichever lets go last sees the others already gone → exactly one frees.
static inline int kama_channel_close_sender(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->senders--;
    /* Broadcast — not signal: EVERY blocked receiver must observe the closure, or the ones not woken
       park forever on a channel that will never deliver again. That is the whole point of a worker pool. */
    if (ch->senders == 0) pthread_cond_broadcast(&ch->notEmpty);
    int last = kama__channel_unheld(ch);
    pthread_mutex_unlock(&ch->mu);
    return last;
}

// Drop the `Channel` factory. No side can be claimed after this, so an unclaimed side closes now: wake everyone
// parked on it. Returns 1 when no endpoint is left either — the factory was the last holder, and the caller drains
// any buffered items and frees the queue, exactly as a last endpoint does.
static inline int kama_channel_close_factory(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->factoryLive = 0;
    pthread_cond_broadcast(&ch->notEmpty);
    pthread_cond_broadcast(&ch->notFull);
    int last = kama__channel_unheld(ch);
    pthread_mutex_unlock(&ch->mu);
    return last;
}

// Drop one receiver endpoint. Symmetric: the receiver SIDE closes when the last one goes, waking any
// sender parked in send (so it re-checks and returns -1). Same drain+free contract.
static inline int kama_channel_close_receiver(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->receivers--;
    if (ch->receivers == 0) pthread_cond_broadcast(&ch->notFull);
    int last = kama__channel_unheld(ch);
    pthread_mutex_unlock(&ch->mu);
    return last;
}

#endif
