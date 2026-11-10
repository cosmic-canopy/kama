// wait_own_tree.c — can a thread waiting inside a chunk hang on another caller's chunk? `pool.c`'s rules, one per POOL_HELP
// (KR-363, ROADMAP_DETAIL `s6-pool-recursion`). S hands out a fork whose chunk forks again, so a thread waits inside S's
// chunk, then writes a byte to a pipe; C hands out chunks that each read a byte from it. A thread waiting inside S's chunk
// that runs one of C's reads waits on S, which waits on it. Prints the rounds done, or HANG after 5 s: `own` (the rule
// kama ran until 0.9.904) hangs, every isolate mode runs all 2,000 rounds. tests/parfor_wait_own_tree is the same in kama.
//
//   clang -O2 -I bench/jobs bench/jobs/probes/wait_own_tree.c -o /tmp/w && POOL_HELP=isolate-workers /tmp/w
#define main pool_main
#include "pool.c"
#undef main
#include <signal.h>
#include <unistd.h>

static int fds[2];
enum { ROUNDS = 2000, READERS = 2 };

static void spin(void* c, int32_t i) { (void)c; (void)i; int64_t until = now_ns() + 20000; while (now_ns() < until) relax(); }
static void outer(void* c, int32_t i) { (void)i; pool_run(spin, c, 2); }
static void reader(void* c, int32_t i) { (void)c; (void)i; char b; if (read(fds[0], &b, 1) != 1) abort(); }

static atomic_int done_rounds;
static void* s_side(void* p) {
    (void)p;
    for (int r = 0; r < ROUNDS; ++r) {
        pool_run(outer, NULL, 2);
        for (int k = 0; k < READERS; ++k) if (write(fds[1], "x", 1) != 1) abort();
        atomic_store(&done_rounds, r + 1);
    }
    return NULL;
}
static void* c_side(void* p) {
    (void)p;
    for (int r = 0; r < ROUNDS; ++r) pool_run_parks(reader, NULL, READERS, 1);
    return NULL;
}
static void on_alarm(int sig) {
    (void)sig;
    char m[64];
    int n = snprintf(m, sizeof m, "HANG after %d rounds\n", atomic_load(&done_rounds));
    write(1, m, (size_t)n);
    _exit(3);
}

int main(void) {
    const char* help = getenv("POOL_HELP");
    if (help && !strcmp(help, "any")) help_kind = HELP_ANY;
    if (help && !strcmp(help, "nested")) help_kind = HELP_NESTED;
    if (help && !strcmp(help, "tree")) help_kind = HELP_OWN_TREE;
    if (help && !strcmp(help, "isolate")) help_kind = HELP_ISOLATE;
    if (help && !strcmp(help, "isolate-all")) help_kind = HELP_ISOLATE_ALL;
    if (help && !strcmp(help, "isolate-parks")) help_kind = HELP_ISOLATE_PARKS;
    if (help && !strcmp(help, "isolate-workers")) help_kind = HELP_ISOLATE_WORKERS;
    const char* ex = getenv("POOL_EXEMPT");
    exempt = help_kind == HELP_ISOLATE_PARKS || (ex && !strcmp(ex, "parks"));
    track = help_kind >= HELP_NESTED;
    if (pipe(fds)) abort();
    signal(SIGALRM, on_alarm);
    alarm(5);
    pool_start(4);
    pthread_t s, c;
    pthread_create(&s, NULL, s_side, NULL);
    pthread_create(&c, NULL, c_side, NULL);
    pthread_join(s, NULL);
    pthread_join(c, NULL);
    printf("%s: %d rounds, no hang\n", help ? help : "own", atomic_load(&done_rounds));
    return 0;
}
