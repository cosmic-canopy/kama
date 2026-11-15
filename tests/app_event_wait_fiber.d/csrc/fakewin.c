#include "fakewin.h"
#include <pthread.h>
#include <time.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int posted, queued, posts, inputs;

bool fakewin_wait(int64_t ns, bool forever) {
    pthread_mutex_lock(&m);
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
    posted = 0; queued = 0;
    pthread_mutex_unlock(&m);
    return delivered;
}
void fakewin_post(void) { pthread_mutex_lock(&m); posted = 1; ++posts; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
void fakewin_input(void) { pthread_mutex_lock(&m); ++queued; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
int32_t fakewin_posts(void) { pthread_mutex_lock(&m); int v = posts; pthread_mutex_unlock(&m); return v; }
int32_t fakewin_inputs(void) { pthread_mutex_lock(&m); int v = inputs; pthread_mutex_unlock(&m); return v; }
