#!/usr/bin/env python3
"""run.py — build every arm of KR-3's pool benchmark and run them INTERLEAVED, one line per run into <out.tsv>.

Usage: bench/jobs/run.py <out.tsv> [rounds]     then: bench/jobs/summarize.py <out.tsv>
       SHAPES="tight frame phase-100us phase-1ms nested"   narrows the shapes; phase-* is `frame` carrying that much work at k=1.
       WORKERS="1 2 4 10 16"         narrows the chunks a hand-out makes. A pool has the core count's threads whatever k is, as a real
                                     one is sized for the machine rather than for one hand-out, so k above that is more chunks than
                                     threads; `parallel_for` starts k threads, which is what it costs today.
       SLOTS="kama pool-sleep ..."   narrows the arms. `kama` runs twice, the second as `kama#2`: its gap to itself is the noise
                                     floor. An arm whose toolchain is missing is left out with a line saying so.

Each round runs every arm on every config as a fresh process, in an order rotated by the round, so drift lands on every arm
alike. The child's user + system CPU comes from the OS, whatever language it is in: `extra` in the table is CPU beyond the one
core the frame's caller spins on, which is what a pool burns to stay fast.

Runs natively on macOS (kama, the C pool, Rayon), on Windows under msys2 (every arm, with UCRT64's rust and go; the CPU there is
the child's GetProcessTimes), and in the bench image for OpenMP and Go too (`bench/run jobs`).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import resource
except ImportError:  # Windows: no rusage, so a child's CPU comes from its process handle instead (`child_cpu`)
    resource = None
    import ctypes
    from ctypes import wintypes


def child_cpu(p):
    """A finished child's user + kernel CPU, from the handle Popen still holds, which keeps its times readable (Windows)."""
    t = [wintypes.FILETIME() for _ in range(4)]   # creation, exit, kernel, user
    if not ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(int(p._handle)), *[ctypes.byref(x) for x in t]):
        return ""
    return f"{sum(x.dwHighDateTime << 32 | x.dwLowDateTime for x in t[2:]) / 1e7:.4f}"   # 100 ns ticks

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(here))
out = sys.argv[1]
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 7
tmp = tempfile.mkdtemp()
kama = subprocess.check_output(["sh", "-c", f'ROOT="{root}"; . "$ROOT/tools/kama-bin.sh"; printf %s "$KAMA"'], text=True)
ncpu = os.cpu_count() or 1
exe = ".exe" if sys.platform == "win32" else ""   # cargo writes it, and go and the launch need it; cc and kama add their own

# arm -> (binary, extra environment)
arms = {}
subprocess.check_call([kama, "build", f"{here}/jobs.kama", "-o", f"{tmp}/kama", "--release"], stdout=subprocess.DEVNULL)
arms["kama"] = ("kama", {})

libs = ["-lpthread"] + (["-lsynchronization"] if sys.platform in ("win32", "msys", "cygwin") else [])
subprocess.check_call(["cc", "-O3", "-DNDEBUG", f"{here}/pool.c", "-o", f"{tmp}/pool"] + libs)
for name, env in {
    "pool-sleep": {"POOL_SPIN_NS": "0"},
    "pool-50us": {"POOL_SPIN_NS": "50000"},
    "pool-2ms": {"POOL_SPIN_NS": "2000000"},
    "pool-any": {"POOL_SPIN_NS": "50000", "POOL_HELP": "any"},
    "pool-chain": {"POOL_SPIN_NS": "50000", "POOL_WAKE": "chain"},
    "pool-all": {"POOL_SPIN_NS": "50000", "POOL_WAKE": "all"},
    "pool-one": {"POOL_SPIN_NS": "50000", "POOL_WAKE": "one"},
}.items():
    arms[name] = ("pool", env)

if shutil.which("cargo"):
    target = f"{tmp}/cargo"
    env = dict(os.environ, CARGO_TARGET_DIR=target)
    if subprocess.call(["cargo", "build", "--release", "--offline", "-q"], cwd=f"{here}/rust", env=env, stderr=subprocess.DEVNULL):
        subprocess.check_call(["cargo", "build", "--release", "-q"], cwd=f"{here}/rust", env=env)
    shutil.copy(f"{target}/release/jobs{exe}", f"{tmp}/rayon{exe}")
    for how in ("outside", "inside", "scope"):
        arms[f"rayon-{how}"] = ("rayon", {"BENCH_RAYON": how})
else:
    print("run.py: no Rayon arm (no cargo here)", file=sys.stderr)

if subprocess.call(["sh", "-c", f"echo 'int main(void){{return 0;}}' | gcc -x c -fopenmp - -o {tmp}/probe 2>/dev/null"]) == 0:
    subprocess.check_call(["gcc", "-O3", "-fopenmp", f"{here}/omp.c", "-o", f"{tmp}/omp"])
    arms["omp"] = ("omp", {})
else:
    print("run.py: no OpenMP arm (no gcc -fopenmp here)", file=sys.stderr)

if shutil.which("go"):
    subprocess.check_call(["go", "build", "-o", f"{tmp}/go{exe}", "."], cwd=f"{here}/go")
    arms["go"] = ("go", {})
else:
    print("run.py: no Go arm (no go here)", file=sys.stderr)

# The nested shape needs a form that forks inside a fork, which only these have. kama's would start a thread per fork.
nestable = {"pool-sleep", "pool-50us", "pool-2ms", "pool-any", "rayon-outside"}
default = ["kama", "kama#2", "pool-sleep", "pool-50us", "pool-2ms", "rayon-outside", "rayon-inside", "rayon-scope", "omp", "go"]
slots = []
for s in os.environ.get("SLOTS", " ".join(default)).split():
    if s.split("#")[0] in arms:
        slots.append(s)
    elif "SLOTS" in os.environ:
        print(f"run.py: no {s} here", file=sys.stderr)

shapes = os.environ.get("SHAPES", "tight frame phase-100us phase-1ms nested").split()
workers = [int(w) for w in os.environ.get("WORKERS", "1 2 4 10 16").split()]
# Rounds per element for a phase of that size at k=1 on the M4's performance cores: `ROUNDS_100US` and `ROUNDS_1MS` recalibrate
# for another machine. 1,024 elements, as the tight shape has.
phase_rounds = {"phase-100us": int(os.environ.get("ROUNDS_100US", "70")), "phase-1ms": int(os.environ.get("ROUNDS_1MS", "750"))}
configs = []
for shape in shapes:
    if shape == "nested":
        configs.append(("nested", ncpu, 7, 36, 0, "nested"))   # 7 runs of fib(36), forking down to fib(10)
        continue
    for w in workers:
        if shape in phase_rounds:
            configs.append(("frame", w, 720, 1024, phase_rounds[shape], shape))
        else:
            configs.append((shape, w, 2000 if shape == "tight" else 720, 1024, 0, shape))

def run(slot, config):
    shape, w, calls, elems, work, label = config
    arm = slot.split("#")[0]
    binary, extra = arms[arm]
    env = dict(os.environ, BENCH_SHAPE=shape, BENCH_WORKERS=str(w), BENCH_POOL=str(ncpu), BENCH_CALLS=str(calls), BENCH_ELEMS=str(elems), BENCH_ROUNDS=str(work))
    env.update(extra)
    before = resource.getrusage(resource.RUSAGE_CHILDREN) if resource else None
    t0 = time.perf_counter()
    p = subprocess.Popen([f"{tmp}/{binary}"], env=env, stdout=subprocess.PIPE, text=True)
    row = p.communicate()[0].strip()
    wall = time.perf_counter() - t0
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode, p.args)
    cpu = ""
    if resource:
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu = f"{(after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime):.4f}"
    elif sys.platform == "win32":
        cpu = child_cpu(p)
    return f"{label}\t{row.split(chr(9), 1)[1]}\t{wall:.4f}\t{cpu}"

with open(out, "w") as f:
    f.write("round\tslot\tshape\tworkers\tcalls\telems\ttotal_ns\tp50_ns\tp99_ns\tcheck\twall_s\tcpu_s\n")
    for r in range(1, rounds + 1):
        for config in configs:
            k = (r - 1) % len(slots)
            for slot in slots[k:] + slots[:k]:
                if config[5] == "nested" and slot.split("#")[0] not in nestable:
                    continue
                f.write(f"{r}\t{slot}\t{run(slot, config)}\n")
                f.flush()
        print(f"round {r}/{rounds} done", file=sys.stderr)
shutil.rmtree(tmp)
