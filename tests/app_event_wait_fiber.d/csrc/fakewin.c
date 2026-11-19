#include "fakewin.h"
#include <pthread.h>
#include <time.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static pthread_cond_t entered = PTHREAD_COND_INITIALIZER;   /* a wait began, or the timer fiber ran */
static int posted, queued, posts, inputs;
static int inside, entries, timerAt = -1;

bool fakewin_wait(int64_t ns, bool forever) {
    pthread_mutex_lock(&m);
    inside = 1; ++entries; pthread_cond_broadcast(&entered);
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    if (ns < 0) ns = 0;   /* a deadline that has passed polls: what is queued is delivered, and the wait returns */
    at.tv_sec += (time_t)(ns / 1000000000); at.tv_nsec += (long)(ns % 1000000000);
    if (at.tv_nsec >= 1000000000) { at.tv_sec += 1; at.tv_nsec -= 1000000000; }
    while (!posted && !queued) {
        if (forever) pthread_cond_wait(&c, &m);
        else if (pthread_cond_timedwait(&c, &m, &at) != 0) break;
    }
    const bool delivered = queued != 0;
    inputs += queued;
    posted = 0; queued = 0; inside = 0;
    pthread_mutex_unlock(&m);
    return delivered;
}
void fakewin_post(void) { pthread_mutex_lock(&m); posted = 1; ++posts; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
void fakewin_input(void) { pthread_mutex_lock(&m); ++queued; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
int32_t fakewin_posts(void) { pthread_mutex_lock(&m); int v = posts; pthread_mutex_unlock(&m); return v; }
int32_t fakewin_inputs(void) { pthread_mutex_lock(&m); int v = inputs; pthread_mutex_unlock(&m); return v; }

/* The fixture's clock is the loop's waits, not a sleep: a throttled process's timers are coalesced (a 60 ms sleep took
   240 ms under `taskpolicy -b`). Each blocks 5 s at most, so a broken wake fails a check instead of hanging. */
static void fiveSeconds(struct timespec* at) { clock_gettime(CLOCK_REALTIME, at); at->tv_sec += 5; }
int32_t fakewin_await(int32_t after) {
    struct timespec at; fiveSeconds(&at);
    pthread_mutex_lock(&m);
    while (!(inside && entries > after) && pthread_cond_timedwait(&entered, &m, &at) == 0) { }
    int v = entries; pthread_mutex_unlock(&m); return v;
}
void fakewin_timer(void) { pthread_mutex_lock(&m); timerAt = entries; pthread_cond_broadcast(&entered); pthread_mutex_unlock(&m); }
int32_t fakewin_timer_waits(void) {
    struct timespec at; fiveSeconds(&at);
    pthread_mutex_lock(&m);
    while (timerAt < 0 && pthread_cond_timedwait(&entered, &m, &at) == 0) { }
    int v = timerAt; pthread_mutex_unlock(&m); return v;
}
