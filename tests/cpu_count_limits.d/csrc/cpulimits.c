// The C half of tests/cpu_count_limits.d: kama_isolate.h's cgroup walk over a tree built here and the affinity mask in a child
// (Linux), or this process's own mask and job object (Windows).
#include "kama_isolate.h"
#include "cpulimits.h"
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/wait.h>
static char dirBuf[4096];
static void put(const char* rel, const char* text) {
    char path[4200];
    snprintf(path, sizeof path, "%s%s", dirBuf, rel);
    for (char* p = path + strlen(dirBuf) + 1; *p; ++p) if (*p == '/') { *p = 0; mkdir(path, 0700); *p = '/'; }
    FILE* f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}
static long ask(const char* self) {
    char p[4200];
    put("/self", self);
    snprintf(p, sizeof p, "%s/self", dirBuf);
    return kama__cgroupCpus(p, dirBuf);
}
static void fresh(const char* tag) {
    char base[4096];
    const char* t = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/kcpu%d_%s_XXXXXX", t && *t ? t : "/tmp", (int)getpid(), tag);
    strcpy(dirBuf, mkdtemp(base) ? base : "/nonexistent");
}
int cpu_limits_selftest(void) {
    fresh("v2");
    put("/a/b/cpu.max", "max 100000\n");
    if (ask("0::/a/b\n") != 0) return 1;                // v2, no quota
    put("/a/b/cpu.max", "150000 100000\n");
    if (ask("0::/a/b\n") != 2) return 2;                // 1.5 CPUs rounds up
    put("/a/cpu.max", "100000 100000\n");
    if (ask("0::/a/b\n") != 1) return 3;                // a parent's quota binds, and it is tighter
    put("/a/cpu.max", "800000 100000\n");
    if (ask("0::/a/b\n") != 2) return 4;                // the child's is tighter
    fresh("root");
    put("/cpu.max", "200000 100000\n");
    if (ask("0::/\n") != 2) return 5;                   // a container: its cgroup is the mount's root
    if (ask("0::/host/path/nothere\n") != 2) return 6;  // a host path the mount does not hold: the root answers
    put("/cpu.max", "50000 100000\n");
    if (ask("0::/\n") != 1) return 7;                   // under one CPU is still one
    fresh("v1");
    put("/cpu,cpuacct/cpu.cfs_quota_us", "300000\n");
    put("/cpu,cpuacct/cpu.cfs_period_us", "100000\n");
    if (ask("4:cpu,cpuacct:/docker/abc\n") != 3) return 8;          // v1, the usual mount name, from a host path
    if (ask("9:memory:/x\n4:cpu,cpuacct:/\n") != 3) return 9;       // past a line that is not the cpu controller
    if (ask("9:cpuset:/x\n") != 0) return 10;                       // `cpuset` is not `cpu`
    put("/cpu,cpuacct/cpu.cfs_quota_us", "-1\n");
    if (ask("4:cpu,cpuacct:/\n") != 0) return 11;                   // v1, no quota
    fresh("v1b");
    put("/cpu/cpu.cfs_quota_us", "200000\n");
    put("/cpu/cpu.cfs_period_us", "100000\n");
    if (ask("4:cpu:/\n") != 2) return 12;                           // v1, mounted under `cpu`
    if (ask("4:cpuacct,cpu:/\n") != 2) return 13;                   // the other order
    if (ask("4:net_cls,cpu,cpuacct:/\n") != 2) return 14;           // in the middle
    if (kama__cgroupCpus("/nonexistent/self", dirBuf) != 0) return 15;

    // An affinity mask, in a child so this process keeps its CPUs. A mask of the first allowed CPU must answer 1.
    pid_t pid = fork();
    if (pid == 0) {
        unsigned long mask[128];
        long bytes = syscall(__NR_sched_getaffinity, 0, sizeof mask, mask);
        if (bytes <= 0) _exit(1);
        long i = 0;
        while (i < bytes / (long)sizeof(unsigned long) && mask[i] == 0) ++i;
        unsigned long lowest = mask[i] & (~mask[i] + 1);
        memset(mask, 0, sizeof mask);
        mask[i] = lowest;
        if (syscall(__NR_sched_setaffinity, 0, (size_t)bytes, mask) != 0) _exit(2);
        _exit(kama__usableCpus() == 1 ? 0 : 3);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 16;
    return 0;
}
#elif defined(_WIN32)
// Windows: this process's own affinity mask, then a job object it joins, whose CPU rate it sets and clears. After
// kama_isolate.h on purpose: its hand-written declarations must agree with <windows.h>'s.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int capped(HANDLE job, DWORD flags, DWORD rate) {
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rc = {0};
    rc.ControlFlags = flags;
    rc.CpuRate = rate;   /* the union's first member; MIN_MAX_RATE reads it as MinRate (low) and MaxRate (high) */
    if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation, &rc, sizeof rc)) return -1;
    return kama__usableCpus();
}
int cpu_limits_selftest(void) {
    DWORD_PTR mask = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys) || !mask) return 21;
    const int all = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (kama__usableCpus() != __builtin_popcountll(mask)) return 22;            // no limit: the mask
    const DWORD_PTR lowest = mask & (~mask + 1);
    if (!SetProcessAffinityMask(GetCurrentProcess(), lowest)) return 23;
    if (kama__usableCpus() != 1) return 24;                                      // a mask of one CPU
    const DWORD_PTR two = lowest | ((mask & ~lowest) & (~(mask & ~lowest) + 1));
    if (two != lowest) {
        if (!SetProcessAffinityMask(GetCurrentProcess(), two)) return 25;
        if (kama__usableCpus() != 2) return 26;                                  // a mask of two
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) return 27;
    if (all < 2) return 0;                                                       // a cap below one CPU is still one

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job || !AssignProcessToJobObject(job, GetCurrentProcess())) return 28;
    const DWORD enable = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE;
    const DWORD oneAndHalf = (DWORD)(15000 / all), one = (DWORD)(10000 / all);   // shares of every processor
    if (capped(job, enable | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP, oneAndHalf) != 2) return 29;   // 1.5 rounds up
    if (capped(job, enable | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP, one) != 1) return 30;
    if (capped(job, enable | JOB_OBJECT_CPU_RATE_CONTROL_MIN_MAX_RATE, (one << 16) | 1) != 1) return 31;  // MaxRate (MinRate 0 is refused)
    if (capped(job, enable | JOB_OBJECT_CPU_RATE_CONTROL_WEIGHT_BASED, 5) != __builtin_popcountll(mask)) return 32;  // a weight caps nothing
    if (capped(job, 0, 0) != __builtin_popcountll(mask)) return 33;              // cleared
    return 0;
}
#else
int cpu_limits_selftest(void) { return 0; }
#endif
