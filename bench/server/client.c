// client.c — the one load client every server arm of KR-243 is measured through (docs/benchmarks/servers.md).
//
// Usage: client <mode> <port> <conns> <threads> <warmup_s> <seconds>      one tab-separated line on stdout
//        client -h                                                        prints the header that line follows
//   mode  echo  64-byte ping-pongs: write 64 bytes, read the same 64 back, repeat — a server parks once per request
//         http  HTTP/1.1 keep-alive `GET /` (no pipelining); the reply must be `200` with the body `Hello, World!`
//         skew  echo, except that SKEW_HOT connections (default 2) are hot: each of their requests asks the server for
//               SKEW_ROUNDS rounds (default 1500) of a mix over its 64 bytes, a CPU burst. A request's first four bytes
//               are its rounds (little-endian), and the reply carries the mix in bytes 4-7, so no compiler can drop
//               the work; every other byte must come back as sent. Latency is the light connections' only, the tail
//               a burst puts on the connections that share its core; requests/s counts both.
//         idle  open <conns> connections, make one echo round trip on each so the server has accepted it, print
//               READY on stderr, hold them for <seconds>, and report how many were held
//
// SERVER_PID (Linux): the server's process. The client then reports the CPU it and its descendants (node's cluster
// workers) spent inside the measured window (user + system: not its start, the connects, or the warm-up).
// SERVER_CORES=<n> (Linux): the server is pinned to CPUs 0..n-1. The client then reports how busy those CPUs were over
// the window, from /proc/stat: that includes the kernel's softirq time, which no process is charged for, so it, and not
// the process's CPU, says whether the server was the limit.
//
// Every reply is VERIFIED — the echoed bytes, the HTTP status and body — and a mismatch, an unexpected close or a
// connect failure counts as an error. Any error makes the exit status 2 after the line is printed, so a server that
// answers wrongly can never post a number (the fairness gate, as the CPU suite's checksums are).
//
// Connections are spread over <threads> threads, each running its own epoll (Linux) or kqueue (macOS) loop. A
// connection's latency is from the first byte of its request to the last byte of its reply, and only replies that
// complete inside the measured window count. Over 20,000 connections, the source address cycles through 127.0.0.1-8 on
// Linux, so one destination port does not run out of ephemeral ports.
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

enum { MODE_ECHO, MODE_HTTP, MODE_IDLE, MODE_SKEW };
enum { PAYLOAD = 64, BUF = 2048 };
static const char HTTP_REQ[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
static const char HTTP_BODY[] = "Hello, World!";

static int g_mode, g_port;
static uint32_t g_hot, g_rounds;   // skew: how many connections are hot, and the rounds each of their requests asks for
static atomic_int g_phase;   // 0 warm-up, 1 measuring, 2 stop

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// A log-linear latency histogram: 16 sub-buckets per power of two, about 6% wide, in nanoseconds.
enum { SUB = 16, EXPS = 64 };
typedef struct { uint64_t n[EXPS * SUB]; } hist;
static void hist_add(hist* h, uint64_t v) {
    if (v < SUB) { h->n[v]++; return; }
    int e = 63 - __builtin_clzll(v);                 // v in [2^e, 2^(e+1))
    int s = (int)((v >> (e - 4)) & (SUB - 1));      // the next four bits
    h->n[(e - 3) * SUB + s]++;
}
static uint64_t hist_value(int i) {                 // a bucket's lower bound
    if (i < SUB) return (uint64_t)i;
    int e = i / SUB + 3, s = i % SUB;
    return ((uint64_t)(SUB + s)) << (e - 4);
}
static uint64_t hist_pct(const hist* h, uint64_t total, double p) {
    if (!total) return 0;
    uint64_t want = (uint64_t)(p * (double)total), seen = 0;
    if (want >= total) want = total - 1;
    for (int i = 0; i < EXPS * SUB; ++i) { seen += h->n[i]; if (seen > want) return hist_value(i); }
    return 0;
}

typedef struct {
    int fd, id;
    uint32_t seq;
    size_t out, in;          // bytes of the request written, bytes of the reply read
    uint64_t t0;             // when the request's first byte went out
    char buf[BUF];
    int done;                // closed: an error, or held to the end (idle)
} conn;

typedef struct {
    int first, count;        // this thread's connections: [first, first + count)
    conn* c;
    hist h;
    uint64_t reqs, timed, errors, maxlat, held;   // timed: the requests in the latency histogram
    pthread_t th;
} worker;

static int g_conns;
static conn* g_all;

static void fill_payload(conn* c, char* p) {
    for (int i = 0; i < PAYLOAD; ++i) p[i] = (char)((c->id * 31 + c->seq * 7 + i) & 0xff);
}
static size_t request_len(void) { return g_mode == MODE_HTTP ? sizeof HTTP_REQ - 1 : PAYLOAD; }
static uint32_t request_rounds(const conn* c) { return g_mode == MODE_SKEW && (uint32_t)c->id < g_hot ? g_rounds : 0; }
static void request_bytes(conn* c, char* out) {
    if (g_mode == MODE_HTTP) { memcpy(out, HTTP_REQ, sizeof HTTP_REQ - 1); return; }
    fill_payload(c, out);
    if (g_mode != MODE_SKEW) return;
    uint32_t r = request_rounds(c);
    for (int i = 0; i < 4; ++i) out[i] = (char)(r >> (8 * i));
}

// 1: a whole, correct reply is in buf[0, in); 0: not yet; -1: a wrong one.
static int reply_complete(conn* c) {
    if (g_mode != MODE_HTTP) {
        if (c->in < PAYLOAD) return 0;
        if (c->in > PAYLOAD) return -1;
        char want[PAYLOAD]; request_bytes(c, want);
        if (g_mode == MODE_SKEW)   // bytes 4-7 are the server's mix, unchecked: the client would pay the burst too
            return memcmp(want, c->buf, 4) == 0 && memcmp(want + 8, c->buf + 8, PAYLOAD - 8) == 0 ? 1 : -1;
        return memcmp(want, c->buf, PAYLOAD) == 0 ? 1 : -1;
    }
    c->buf[c->in < BUF ? c->in : BUF - 1] = 0;
    char* end = strstr(c->buf, "\r\n\r\n");
    if (!end) return c->in >= BUF - 1 ? -1 : 0;
    if (strncmp(c->buf, "HTTP/1.1 200", 12) != 0) return -1;
    long len = -1;
    for (char* p = c->buf; p && p < end; p = strstr(p, "\r\n")) {
        while (*p == '\r' || *p == '\n') ++p;
        if (strncasecmp(p, "content-length:", 15) == 0) { len = strtol(p + 15, NULL, 10); break; }
    }
    if (len != (long)(sizeof HTTP_BODY - 1)) return -1;
    size_t head = (size_t)(end + 4 - c->buf);
    if (c->in < head + (size_t)len) return 0;
    if (c->in > head + (size_t)len) return -1;   // no pipelining: nothing may follow the body
    return memcmp(end + 4, HTTP_BODY, (size_t)len) == 0 ? 1 : -1;
}

static int send_request(conn* c) {
    char req[BUF]; request_bytes(c, req);
    size_t len = request_len();
    while (c->out < len) {
        ssize_t n = write(c->fd, req + c->out, len - c->out);
        if (n > 0) { c->out += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1;
    }
    return 0;
}

static int g_why;   // CLIENT_WHY=1: say on stderr why each failed connection failed
static void fail_why(worker* w, conn* c, const char* why) {
    if (g_why) fprintf(stderr, "client: connection %d: %s (errno %d, %zu bytes in)\n", c->id, why, errno, c->in);
    w->errors++; c->done = 1; close(c->fd);
}
#define fail(w, c) fail_why((w), (c), __func__)

static void start_request(worker* w, conn* c) {
    c->out = 0; c->in = 0; c->t0 = now_ns();
    if (send_request(c) != 0) fail(w, c);
}

// Readable: read what is there; on a whole reply, count it and send the next request.
static void on_readable(worker* w, conn* c) {
    for (;;) {
        if (c->in >= BUF - 1) { fail(w, c); return; }
        ssize_t n = read(c->fd, c->buf + c->in, BUF - 1 - c->in);
        if (n > 0) { c->in += (size_t)n; continue; }
        if (n == 0) { fail_why(w, c, "closed by the server"); return; }   // a keep-alive connection ended
        if (errno == EINTR) continue;
        if (errno == EAGAIN) break;
        fail(w, c); return;
    }
    int r = reply_complete(c);
    if (r < 0) { fail_why(w, c, "a wrong reply"); return; }
    if (r == 0) return;
    uint64_t lat = now_ns() - c->t0;
    if (g_mode == MODE_IDLE) { w->held++; return; }        // one round trip proves the connection is served
    if (atomic_load(&g_phase) == 1) {
        w->reqs++;
        if (request_rounds(c) == 0) {
            hist_add(&w->h, lat); w->timed++;
            if (lat > w->maxlat) w->maxlat = lat;
        }
    }
    c->seq++;
    if (atomic_load(&g_phase) < 2) start_request(w, c);
}

static int connect_one(int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef __linux__
    if (g_conns > 20000) {   // spread the source address so one port pair space is not exhausted
        struct sockaddr_in src; memset(&src, 0, sizeof src);
        src.sin_family = AF_INET; src.sin_addr.s_addr = htonl(0x7f000001u + (uint32_t)(id % 8));
        if (bind(fd, (struct sockaddr*)&src, sizeof src) != 0) { close(fd); return -1; }
    }
#else
    (void)id;
#endif
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(0x7f000001u);
    for (int tries = 0; connect(fd, (struct sockaddr*)&a, sizeof a) != 0; ++tries) {
        if (errno == EINTR) continue;
        if ((errno == ECONNREFUSED || errno == EAGAIN) && tries < 50) { usleep(20000); continue; }
        close(fd); return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void* run(void* arg) {
    worker* w = arg;
#ifdef __linux__
    int q = epoll_create1(0);
#else
    int q = kqueue();
#endif
    for (int i = 0; i < w->count; ++i) {
        conn* c = &w->c[i];
        if (c->done) continue;
#ifdef __linux__
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
        epoll_ctl(q, EPOLL_CTL_ADD, c->fd, &ev);
#else
        struct kevent ev; EV_SET(&ev, c->fd, EVFILT_READ, EV_ADD, 0, 0, c);
        kevent(q, &ev, 1, NULL, 0, NULL);
#endif
        start_request(w, c);
    }
    enum { MAXEV = 256 };
    while (atomic_load(&g_phase) < 2) {
#ifdef __linux__
        struct epoll_event evs[MAXEV];
        int n = epoll_wait(q, evs, MAXEV, 50);
        for (int i = 0; i < n; ++i) { conn* c = evs[i].data.ptr; if (!c->done) on_readable(w, c); }
#else
        struct kevent evs[MAXEV];
        struct timespec to = { 0, 50 * 1000000 };
        int n = kevent(q, NULL, 0, evs, MAXEV, &to);
        for (int i = 0; i < n; ++i) { conn* c = evs[i].udata; if (!c->done) on_readable(w, c); }
#endif
    }
    close(q);
    return NULL;
}

// The server's processes: SERVER_PID and its descendants, found when the measured window opens (Linux).
static long g_pids[256];
static int g_npids;
static long parent_of(long pid) {
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(line, 1, sizeof line - 1, f); fclose(f); line[n] = 0;
    char* q = strrchr(line, ')');   // the command name may hold spaces; the fields follow its closing paren
    long ppid = -1;
    if (!q || sscanf(q + 2, "%*c %ld", &ppid) != 1) return -1;
    return ppid;
}
static void find_server(void) {
    g_npids = 0;
    const char* root = getenv("SERVER_PID");
    if (!root) return;
    g_pids[g_npids++] = atol(root);
    for (int grew = 1; grew && g_npids < 256; ) {   // until a pass over /proc adds no one
        grew = 0;
        DIR* d = opendir("/proc");
        if (!d) return;
        for (struct dirent* e; (e = readdir(d)) != NULL && g_npids < 256; ) {
            char* end; long pid = strtol(e->d_name, &end, 10);
            if (*end || pid <= 0) continue;
            int known = 0;
            for (int i = 0; i < g_npids; ++i) known |= g_pids[i] == pid;
            if (known) continue;
            long pp = parent_of(pid);
            for (int i = 0; i < g_npids; ++i) if (g_pids[i] == pp) { g_pids[g_npids++] = pid; grew = 1; break; }
        }
        closedir(d);
    }
}
// The CPU the server's processes have spent so far, in seconds (0 off Linux, or without SERVER_PID).
static double server_cpu(void) {
    double s = 0;
    long hz = sysconf(_SC_CLK_TCK);
    for (int i = 0; i < g_npids; ++i) {
        char path[64], line[1024];
        snprintf(path, sizeof path, "/proc/%ld/stat", g_pids[i]);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(line, 1, sizeof line - 1, f); fclose(f); line[n] = 0;
        char* q = strrchr(line, ')');
        unsigned long long ut = 0, st = 0;
        if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
            s += (double)(ut + st) / (double)hz;
    }
    return s;
}

// Busy and total ticks so far on CPUs 0..n-1 (Linux; 0 elsewhere or without SERVER_CORES).
static void cores_ticks(unsigned long long* busy, unsigned long long* total) {
    *busy = *total = 0;
#ifdef __linux__
    int n = getenv("SERVER_CORES") ? atoi(getenv("SERVER_CORES")) : 0;
    FILE* f = n > 0 ? fopen("/proc/stat", "r") : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int cpu; unsigned long long v[8] = {0};
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 9 || cpu >= n) continue;
        unsigned long long t = 0;
        for (int i = 0; i < 8; ++i) t += v[i];
        *total += t; *busy += t - v[3] - v[4];   // all but idle and iowait
    }
    fclose(f);
#endif
}

static void raise_fd_limit(void) {
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) == 0) {
        r.rlim_cur = r.rlim_max;
#ifdef __APPLE__
        if (r.rlim_cur > 1048576) r.rlim_cur = 1048576;
#endif
        setrlimit(RLIMIT_NOFILE, &r);
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        puts("mode\tconns\tthreads\tseconds\trequests\trps\tp50_us\tp99_us\tp999_us\tmax_us\terrors\tsrv_cpu_s\tsrv_busy");
        return 0;
    }
    if (argc != 7) { fprintf(stderr, "usage: client <echo|http|skew|idle> <port> <conns> <threads> <warmup_s> <seconds>\n"); return 64; }
    g_mode = strcmp(argv[1], "http") == 0 ? MODE_HTTP : strcmp(argv[1], "idle") == 0 ? MODE_IDLE
           : strcmp(argv[1], "skew") == 0 ? MODE_SKEW : MODE_ECHO;
    g_hot = (uint32_t)atoi(getenv("SKEW_HOT") ? getenv("SKEW_HOT") : "2");
    g_rounds = (uint32_t)atoi(getenv("SKEW_ROUNDS") ? getenv("SKEW_ROUNDS") : "1500");
    if (g_mode == MODE_ECHO && strcmp(argv[1], "echo") != 0) { fprintf(stderr, "client: unknown mode '%s'\n", argv[1]); return 64; }
    g_port = atoi(argv[2]); g_conns = atoi(argv[3]);
    int threads = atoi(argv[4]);
    double warm = atof(argv[5]), secs = atof(argv[6]);
    if (g_conns < 1 || threads < 1) { fprintf(stderr, "client: conns and threads must be positive\n"); return 64; }
    if (threads > g_conns) threads = g_conns;
    raise_fd_limit();
    g_why = getenv("CLIENT_WHY") != NULL;

    g_all = calloc((size_t)g_conns, sizeof(conn));
    uint64_t connectErrors = 0;
    for (int i = 0; i < g_conns; ++i) {
        g_all[i].id = i;
        g_all[i].fd = connect_one(i);
        if (g_all[i].fd < 0) {
            g_all[i].done = 1; connectErrors++;
            if (getenv("CLIENT_WHY")) fprintf(stderr, "client: connection %d: connect failed (errno %d)\n", i, errno);
        }
    }
    worker* ws = calloc((size_t)threads, sizeof(worker));
    for (int t = 0, at = 0; t < threads; ++t) {
        int n = g_conns / threads + (t < g_conns % threads);
        ws[t].first = at; ws[t].count = n; ws[t].c = &g_all[at]; at += n;
    }
    atomic_store(&g_phase, g_mode == MODE_IDLE ? 1 : 0);
    for (int t = 0; t < threads; ++t) pthread_create(&ws[t].th, NULL, run, &ws[t]);

    uint64_t t0 = 0, t1 = 0;
    double cpu0 = 0, cpu1 = 0;
    unsigned long long busy0 = 0, busy1 = 0, total0 = 0, total1 = 0;
    if (g_mode == MODE_IDLE) {
        // Ready once every live connection has made its round trip (or ten seconds have passed).
        uint64_t deadline = now_ns() + 10ull * 1000000000ull;
        for (;;) {
            uint64_t held = 0, errs = 0;
            for (int t = 0; t < threads; ++t) { held += ws[t].held; errs += ws[t].errors; }
            if (held + errs + connectErrors >= (uint64_t)g_conns || now_ns() > deadline) break;
            usleep(10000);
        }
        fprintf(stderr, "READY\n"); fflush(stderr);
        usleep((useconds_t)(secs * 1e6));
    } else {
        usleep((useconds_t)(warm * 1e6));
        find_server(); cpu0 = server_cpu(); cores_ticks(&busy0, &total0); t0 = now_ns(); atomic_store(&g_phase, 1);
        usleep((useconds_t)(secs * 1e6));
        t1 = now_ns(); cpu1 = server_cpu(); cores_ticks(&busy1, &total1);
    }
    atomic_store(&g_phase, 2);
    for (int t = 0; t < threads; ++t) pthread_join(ws[t].th, NULL);

    hist h; memset(&h, 0, sizeof h);
    uint64_t reqs = 0, timed = 0, errors = connectErrors, maxlat = 0, held = 0;
    for (int t = 0; t < threads; ++t) {
        for (int i = 0; i < EXPS * SUB; ++i) h.n[i] += ws[t].h.n[i];
        reqs += ws[t].reqs; timed += ws[t].timed; errors += ws[t].errors; held += ws[t].held;
        if (ws[t].maxlat > maxlat) maxlat = ws[t].maxlat;
    }
    if (g_mode == MODE_IDLE) {
        printf("idle\t%d\t%d\t%.1f\t%llu\t0\t0\t0\t0\t0\t%llu\t0\t0\n", g_conns, threads, secs,
               (unsigned long long)held, (unsigned long long)errors);
    } else {
        double el = (double)(t1 - t0) / 1e9;
        printf("%s\t%d\t%d\t%.2f\t%llu\t%.0f\t%.1f\t%.1f\t%.1f\t%.1f\t%llu\t%.2f\t%.3f\n", argv[1], g_conns, threads, el,
               (unsigned long long)reqs, (double)reqs / el,
               hist_pct(&h, timed, 0.50) / 1e3, hist_pct(&h, timed, 0.99) / 1e3, hist_pct(&h, timed, 0.999) / 1e3,
               maxlat / 1e3, (unsigned long long)errors, cpu1 - cpu0,
               total1 > total0 ? (double)(busy1 - busy0) / (double)(total1 - total0) : 0.0);
    }
    for (int i = 0; i < g_conns; ++i) if (!g_all[i].done) close(g_all[i].fd);
    return errors ? 2 : 0;
}
