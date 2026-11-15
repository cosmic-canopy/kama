#include "fakewin.h"
#include <pthread.h>
#include <time.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int posted, withDeadline, forever_, posts;

void fakewin_wait(int64_t ns, bool forever) {
    pthread_mutex_lock(&m);
    if (forever) ++forever_; else ++withDeadline;
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    if (ns < 0) ns = 0;
    at.tv_sec += (time_t)(ns / 1000000000); at.tv_nsec += (long)(ns % 1000000000);
    if (at.tv_nsec >= 1000000000) { at.tv_sec += 1; at.tv_nsec -= 1000000000; }
    while (!posted) {
        if (forever) pthread_cond_wait(&c, &m);
        else if (pthread_cond_timedwait(&c, &m, &at) != 0) break;   /* the timeout: an event loop's wait returns */
    }
    posted = 0;
    pthread_mutex_unlock(&m);
}
void fakewin_post(void) { pthread_mutex_lock(&m); posted = 1; ++posts; pthread_cond_signal(&c); pthread_mutex_unlock(&m); }
int32_t fakewin_waits_with_deadline(void) { pthread_mutex_lock(&m); int v = withDeadline; pthread_mutex_unlock(&m); return v; }
int32_t fakewin_waits_forever(void) { pthread_mutex_lock(&m); int v = forever_; pthread_mutex_unlock(&m); return v; }
int32_t fakewin_posts(void) { pthread_mutex_lock(&m); int v = posts; pthread_mutex_unlock(&m); return v; }
