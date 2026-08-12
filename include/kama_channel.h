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
#include "kama_runtime.h"   /* kama_panic, kama_string_lit */

// A bounded (ring-buffer) channel. `cap` is the buffer capacity in elements; `elemSize` the bitwise
// size of the moved value T. `count`/`head`/`tail` drive the ring. `senderLive`/`receiverLive` are
// the endpoint-liveness flags (both start 1); the last endpoint to drop frees the whole struct.
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  notEmpty;   /* a blocked recv waits here; a send / sender-drop signals it */
    pthread_cond_t  notFull;    /* a blocked send waits here; a recv / receiver-drop signals it */
    unsigned char*  buf;        /* cap * elemSize bytes */
    size_t          elemSize;
    size_t          cap;
    size_t          count;
    size_t          head;       /* next slot to read  */
    size_t          tail;       /* next slot to write */
    int             senderLive;
    int             receiverLive;
} kama_channel_t;

// Create a bounded channel over T (elemSize bytes) with `cap` buffered elements (cap >= 1 for M3.1).
// Returns an opaque handle held by kama as an `UnsafePtr`. Panics on allocation failure (spawn-like: not a
// recoverable condition in the M3 surface).
static inline void* kama_channel_new(size_t elemSize, size_t cap) {
    kama_channel_t* ch = (kama_channel_t*)malloc(sizeof(kama_channel_t));
    if (!ch) kama_panic(kama_string_lit("channel alloc failed", 20));
    ch->buf = (unsigned char*)malloc((cap ? cap : 1) * elemSize);
    if (!ch->buf) kama_panic(kama_string_lit("channel alloc failed", 20));
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->notEmpty, NULL);
    pthread_cond_init(&ch->notFull, NULL);
    ch->elemSize = elemSize;
    ch->cap = cap;
    ch->count = ch->head = ch->tail = 0;
    ch->senderLive = 1;
    ch->receiverLive = 1;
    return ch;
}

// Free the queue + its buffer + sync primitives. Precondition: called by the LAST endpoint to close
// (both liveness flags down), with the mutex UNLOCKED, so no other thread can reach it. Any items still
// buffered must already have been drained by the caller (kama-side, where element type T is known) —
// this frees the raw buffer without touching element contents.
static inline void kama_channel_free(kama_channel_t* ch) {
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->notEmpty);
    pthread_cond_destroy(&ch->notFull);
    free(ch->buf);
    free(ch);
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
    if (ch->cap == 0) {                                       /* rendezvous */
        while (ch->count != 0 && ch->receiverLive)           /* wait for a free slot (prior hand-off done) */
            pthread_cond_wait(&ch->notFull, &ch->mu);
        if (!ch->receiverLive) { pthread_mutex_unlock(&ch->mu); return -1; }
        memcpy(ch->buf, elem, ch->elemSize);
        ch->count = 1;
        pthread_cond_signal(&ch->notEmpty);                  /* offer it to a receiver */
        while (ch->count != 0 && ch->receiverLive)           /* block until the receiver takes it */
            pthread_cond_wait(&ch->notFull, &ch->mu);
        int taken = (ch->count == 0);
        if (!taken) ch->count = 0;                           /* receiver died: DISOWN the stale copy in buf[0] —
                                                                the caller keeps ownership (SendResult::Undelivered),
                                                                so teardown-drain must not drop it too (double-free) */
        pthread_mutex_unlock(&ch->mu);
        return taken ? 0 : -1;                               /* receiver died before taking → not delivered */
    }
    while (ch->count == ch->cap && ch->receiverLive)
        pthread_cond_wait(&ch->notFull, &ch->mu);
    if (!ch->receiverLive) { pthread_mutex_unlock(&ch->mu); return -1; }
    memcpy(ch->buf + ch->tail * ch->elemSize, elem, ch->elemSize);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->notEmpty);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

// Move one element OUT of the channel into `out` (elemSize bytes). Blocks while the ring is empty and
// the sender is still live. Returns 0 on success, -1 when the channel is closed AND drained (all
// senders dropped and no buffered elements remain) — the receiver's None.
static inline int kama_channel_recv(void* h, void* out) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    while (ch->count == 0 && ch->senderLive)
        pthread_cond_wait(&ch->notEmpty, &ch->mu);
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return -1; }   /* drained + sender gone */
    if (ch->cap == 0) {                                       /* rendezvous: take from the single slot */
        memcpy(out, ch->buf, ch->elemSize);
        ch->count = 0;
        pthread_cond_signal(&ch->notFull);                   /* release the blocked sender (hand-off done) */
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    memcpy(out, ch->buf + ch->head * ch->elemSize, ch->elemSize);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->notFull);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

// Non-blocking pop of one buffered element into `out` (elemSize bytes). Returns 1 if an element was
// written, 0 if the ring is empty. Used ONLY by the last-endpoint teardown drain (below): at that point
// both endpoints are closing and the queue is quiescent, so the lock is a formality. The element is
// relocated out (memcpy) exactly like recv — the caller (kama, which knows T) then drops it.
static inline int kama_channel_try_pop(void* h, void* out) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return 0; }
    memcpy(out, ch->buf + ch->head * ch->elemSize, ch->elemSize);
    if (ch->cap != 0) ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_mutex_unlock(&ch->mu);
    return 1;
}

// Close the sender endpoint: mark the sender side closed and wake any receiver parked in recv (so it
// re-checks liveness and returns None once drained). Returns 1 if this was the LAST endpoint (the
// receiver is already gone) — the caller must then drain any buffered items (kama-side, running each
// element's ~dtor) and call kama_channel_free. Returns 0 otherwise (the receiver frees later). The
// liveness flag is set and the other side is read under one lock hold, so the two endpoints can never
// both observe "last": whichever closes second sees the other's flag already down → exactly one frees.
static inline int kama_channel_close_sender(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->senderLive = 0;
    pthread_cond_broadcast(&ch->notEmpty);
    int last = !ch->receiverLive;
    pthread_mutex_unlock(&ch->mu);
    return last;
}

// Close the receiver endpoint: mark the receiver side closed and wake any sender parked in send (so it
// re-checks liveness and returns -1). Returns 1 if this was the LAST endpoint (same drain+free contract
// as kama_channel_close_sender), 0 otherwise.
static inline int kama_channel_close_receiver(void* h) {
    kama_channel_t* ch = (kama_channel_t*)h;
    pthread_mutex_lock(&ch->mu);
    ch->receiverLive = 0;
    pthread_cond_broadcast(&ch->notFull);
    int last = !ch->senderLive;
    pthread_mutex_unlock(&ch->mu);
    return last;
}

#endif
