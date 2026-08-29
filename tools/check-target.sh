#!/bin/sh
# check-target.sh — build-configuration guard for the TARGET axis: the toolchain flags a build hands to
# the C compiler must come from the SELECTED TARGET, never from the machine the kama binary was built on.
#
# Until the build-configuration campaign, these were `#ifdef __APPLE__` / `#ifdef _WIN32` evaluated when
# the COMPILER itself was compiled. That is correct only while host == target, and it was the one hard
# blocker to cross-compilation — most sharply with `-lws2_32`, which a Windows build produced on Linux
# silently omitted, so every std::net program would fail to link.
#
# We prove the keying without needing a cross toolchain installed: `--cc "echo …"` substitutes the C
# compiler for `echo`, so the fully-assembled command line is printed instead of run.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
FIXTURE="$ROOT/tests/arith.kama"

if [ ! -x "$KAMA" ]; then echo "check-target: $KAMA not built" >&2; exit 1; fi
if [ ! -f "$FIXTURE" ]; then echo "check-target: missing $FIXTURE" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ⚠️ A "cross" triple has to be derived from THIS host, never hardcoded. Two assertions below used to
# hardcode `x86_64-linux-gnu` and `x86_64-windows-gnu`, and each of those IS the host triple on one of the
# CI runners (hostTarget() in kama.driver.cpp spells the Windows/MSYS2 host `x86_64-windows-gnu`). kama then
# correctly omits `-target` for a same-host build — and the assertion failed for being wrong, not for the
# compiler being wrong.
#
# ⚠️⚠️ …and the arch has to come from KAMA, not from `uname -m`, which answers a different question.
# This used to flip uname's answer, on the reasoning that a differing arch is foreign on any host. It is
# not: on an ARM64 Windows box running the x86_64 msys2 environment, `bash` is itself an emulated x86_64
# binary reporting on ITSELF, so `uname -m` says x86_64 while an ARM64-built kama.exe targets aarch64.
# Flipping then nominated aarch64 — the host — as the "cross" target, kama rightly omitted `-target`, and
# §8 failed for being wrong. (See docs/platforms/windows.md § "Two msys2 environments".)
#
# So probe instead of infer: kama adds `-target` exactly when the arch differs from its own, so the arch
# it stays SILENT about is the host's. The first one it names is genuinely foreign, whatever built kama.
CROSS_ARCH=
for _a in aarch64 x86_64 riscv64; do
    if "$KAMA" build --cc "echo zig cc" "$FIXTURE" --target "$_a-windows-gnu" -o "$tmp/archprobe" 2>/dev/null \
       | grep -qF -- "-target $_a-windows-gnu"; then CROSS_ARCH=$_a; break; fi
done
[ -n "$CROSS_ARCH" ] || { echo "check-target: could not find an arch foreign to this kama" >&2; exit 1; }
CROSS_WINDOWS="$CROSS_ARCH-windows-gnu"
# The Linux triple needs its OWN probe. The loop above establishes foreignness against `-windows-gnu`,
# where a DIFFERENT OS makes any arch foreign — including the host's. Reusing that arch for `-linux-gnu`
# then asks whether `aarch64-linux-gnu` is a cross build, which on an aarch64 LINUX host it is not: kama
# correctly passes no -target, and the assertion below read that as a failure. Invisible on x86_64 CI and
# on macOS (both genuinely cross to aarch64-linux-gnu); it fires on an ARM Linux host, which is what this
# repo's own dev container is.
CROSS_LINUX=
for _a in aarch64 x86_64 riscv64; do
    if "$KAMA" build --cc "echo zig cc" "$FIXTURE" --target "$_a-linux-gnu" -o "$tmp/archprobe2" 2>/dev/null \
       | grep -qF -- "-target $_a-linux-gnu"; then CROSS_LINUX="$_a-linux-gnu"; break; fi
done
[ -n "$CROSS_LINUX" ] || { echo "check-target: could not find a linux arch foreign to this kama" >&2; exit 1; }

# The named target used in §6 to prove the cross-toolchain rules has to be genuinely FOREIGN to this
# host. WINDOWS is not foreign when the host is Windows: kama builds it natively and correctly, so the
# "no cross toolchain, therefore refuse" branch was asserting a refusal that must never happen there.
# Every other case in this file stubs the compiler out and only inspects the command line, so the host
# does not enter into them.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CROSS_OS=LINUX;   CROSS_FILE_MAGIC="ELF" ;;
    *)                    CROSS_OS=WINDOWS; CROSS_FILE_MAGIC="MS Windows" ;;
esac

# The assembled cc command line for a given target, with the compiler stubbed out. The optional second
# argument picks a different input: most assertions here hold for any program, but the pay-for-what-you-use
# link flags (`-lpthread`) only appear when the program actually pulls in the seam that needs them.
ccline() {
    "$KAMA" build --release --cc "echo" "${2:-$FIXTURE}" --target "$1" -o "$tmp/out" 2>/dev/null
}

# Run a stubbed build and echo its stdout, KEEPING stderr for a failure report. A bare `2>/dev/null` here
# turns "the build errored" and "the build succeeded but printed the wrong thing" into the same message,
# which is how the Windows `.dll` failure arrived with no way to tell which it was.
tryline() {   # tryline <label> <args...>
    _lbl=$1; shift
    "$KAMA" "$@" >"$tmp/$_lbl.out" 2>"$tmp/$_lbl.err" || true
    cat "$tmp/$_lbl.out"
}
why() {   # why <label> — print whatever the stubbed build said, for a failing assertion
    if [ -s "$tmp/$1.err" ]; then
        echo "  the build reported:" >&2
        sed 's/^/    /' "$tmp/$1.err" >&2
    fi
    if [ -s "$tmp/$1.out" ]; then
        echo "  its command line was:" >&2
        sed 's/^/    /' "$tmp/$1.out" >&2
    else
        echo "  it produced NO command line at all (so it failed before the link step)." >&2
    fi
}

want() {   # want <target> <substring> <description> [fixture]
    if ! ccline "$1" "${4:-}" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 did not pass '$2' ($3)" >&2
        echo "  command line was:" >&2
        ccline "$1" "${4:-}" | sed 's/^/    /' >&2
        exit 1
    fi
}

reject() {   # reject <target> <substring> <description> [fixture]
    if ccline "$1" "${4:-}" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 passed '$2' but must not ($3)" >&2
        echo "  command line was:" >&2
        ccline "$1" "${4:-}" | sed 's/^/    /' >&2
        exit 1
    fi
}

# 1. WINSOCK — the flagship cross-compilation case. `-lws2_32` must follow the TARGET being Windows,
#    from any host, and must not appear for any other target.
want   WINDOWS -lws2_32 "std::net needs Winsock on Windows"
reject LINUX   -lws2_32 "POSIX sockets need no extra library"
reject MACOS   -lws2_32 "POSIX sockets need no extra library"

# 2. SECTION GC — ld64 and GNU ld/lld spell dead-code stripping differently, and ld64 treats `-s` as
#    obsolete (it warned on every release link before this was target-keyed).
want   MACOS   -Wl,-dead_strip   "ld64 spells section GC -dead_strip"
reject MACOS   -Wl,--gc-sections "that is the GNU-style spelling"
reject MACOS   " -s "            "ld64 warns that -s is obsolete"
want   LINUX   -Wl,--gc-sections "GNU ld/lld spell section GC --gc-sections"
want   WINDOWS -Wl,--gc-sections "a mingw target links with a GNU-style linker"

# 2b. RUNTIME LINKAGE — a Windows program must not depend on a runtime DLL it cannot ship.
#     mingw-w64 installs BOTH libpthread.a and libpthread.dll.a and the linker prefers the import library,
#     so a bare `-lpthread` bound libwinpthread-1.dll out of the msys2 tree. Measured before this was
#     written: tests/isolate_basic.kama built on Windows imported it and exited 0xC0000135
#     (STATUS_DLL_NOT_FOUND) from a plain PowerShell — on the very machine that built it.
#
#     Only ONE library is wrapped, not a blanket `-static`: the rule is "link non-system runtime
#     statically, system components dynamically", and libraries the USER names (--link, --webgpu) stay
#     their choice. A blanket -static would also re-bind -lglfw3 and break --webgpu, whose libwgpu_native.a
#     needs -lntdll/-luserenv/-lbcrypt that this tail never emits.
THREADED="$ROOT/tests/isolate_basic.kama"
if [ ! -f "$THREADED" ]; then echo "check-target: missing $THREADED" >&2; exit 1; fi

want   WINDOWS "-Wl,-Bstatic -lpthread -Wl,-Bdynamic" \
       "a Windows program must not need libwinpthread-1.dll to start" "$THREADED"
#     ...and the wrapper closes, or -lws2_32 and the target's own ldflags below it change meaning.
want   WINDOWS "-Wl,-Bdynamic" "the static wrapper must restore the linker default" "$THREADED"
#     Keyed on the TARGET: -static on Linux would statically link glibc, which is not the intent, and on
#     macOS pthreads live in libc so there is nothing to choose. This is the half that fails if someone
#     "fixes" it with a host #ifdef.
want   LINUX   " -lpthread " "Linux links pthreads dynamically, as it should" "$THREADED"
reject LINUX   "-Wl,-Bstatic" "static linkage on Linux would swallow glibc" "$THREADED"
reject MACOS   "-Wl,-Bstatic" "macOS pthreads live in libc — nothing to link statically" "$THREADED"
#     Pay-for-what-you-use survives: a program that never touches the isolate seam links no pthread at
#     all, so it gets no wrapper either.
reject WINDOWS "-Wl,-Bstatic" "a non-threaded program links no pthread to make static"

#     The opt-IN to the DLL, both spellings. `--shared` was already taken (it picks the OUTPUT kind), so
#     this is its own switch — and it has to be a real one, because the target's `ldflags` escape hatch
#     cannot cleanly UNDO a -Bstatic the driver already emitted.
dynline=$(tryline dynrt build --release --cc "echo" "$THREADED" --target WINDOWS --dynamic-runtime -o "$tmp/dyn")
if printf '%s' "$dynline" | grep -qF -- "-Wl,-Bstatic"; then
    echo "check-target: FAIL — --dynamic-runtime still linked the runtime statically" >&2
    why dynrt; exit 1
fi
if ! printf '%s' "$dynline" | grep -qF -- " -lpthread "; then
    echo "check-target: FAIL — --dynamic-runtime dropped -lpthread entirely" >&2
    why dynrt; exit 1
fi

#     The same choice per-project, as a target property in kama.json — it belongs beside cc/sysroot/cflags
#     rather than in a select group, because linkage is a toolchain fact and `@compileFor` has no business
#     branching on it. (A RUNTIME select group would also collide with OUTPUT=STATIC: the flag namespace
#     is flat.)
rt="$tmp/rt"; mkdir -p "$rt/src"
cat > "$rt/kama.json" <<'JSON'
{ "name": "rtdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "select": { "TARGET": { "WINDOWS": { "runtime": "dynamic" } } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cp "$THREADED" "$rt/src/app.kama"
rtline=$("$KAMA" build --release --cc "echo" "$rt/kama.json" --target WINDOWS -o "$rt/app" 2>/dev/null || true)
if printf '%s' "$rtline" | grep -qF -- "-Wl,-Bstatic"; then
    echo "check-target: FAIL — a target's \"runtime\": \"dynamic\" did not reach the link tail" >&2
    printf '%s\n' "$rtline" | sed 's/^/    /' >&2
    exit 1
fi
#     A typo must not read as "not dynamic" and silently hand back the default it was trying to change.
printf '%s\n' '{ "name": "rtdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } },
  "select": { "TARGET": { "WINDOWS": { "runtime": "shared" } } } }' > "$rt/kama.json"
if "$KAMA" build --release --cc "echo" "$rt/kama.json" --target WINDOWS -o "$rt/app" >/dev/null 2>"$rt/err"; then
    echo "check-target: FAIL — an unknown \"runtime\" value was accepted" >&2
    exit 1
fi
if ! grep -qF 'must be "static" or "dynamic"' "$rt/err"; then
    echo "check-target: FAIL — a bad \"runtime\" value failed, but not with the value diagnostic:" >&2
    sed 's/^/  /' "$rt/err" >&2
    exit 1
fi

# 2b. PE SUBSYSTEM — the same three-part shape as runtime linkage above (CLI flag, manifest key, validated
#     value), because it is the same kind of question: a permanent property of the artifact, chosen per
#     target. Console is the default and must stay byte-for-byte what it always was.
if ccline WINDOWS | grep -qF -- "--subsystem"; then
    echo "check-target: FAIL — a default Windows build asked for a subsystem; console must stay the default" >&2
    exit 1
fi
#     Opting in emits TWO flags into DIFFERENT phases: the linker gets --subsystem, and the C compiler gets
#     a -D so kama_args_init() reattaches the parent console. Assert both — a -D that silently landed in the
#     link tail instead of the compile flags would be lost by every per-TU `-c` job, and the symptom (a GUI
#     program that prints nothing) looks exactly like the feature never shipped.
guiline=$(tryline gui build --release --cc "echo" "$FIXTURE" --target WINDOWS --subsystem windows -o "$tmp/gui")
for flag in "-Wl,--subsystem,windows" "-DKAMA_SUBSYSTEM_WINDOWS=1"; do
    if ! printf '%s' "$guiline" | grep -qF -- "$flag"; then
        echo "check-target: FAIL — --subsystem windows did not emit $flag" >&2
        why gui; exit 1
    fi
done
#     …and everywhere else it is an accepted NO-OP, not an error, so one cross-platform build script can
#     carry the flag. No other object format has a subsystem field to set.
if tryline guilinux build --release --cc "echo" "$FIXTURE" --target LINUX --subsystem windows -o "$tmp/gl" \
   | grep -qF -- "--subsystem"; then
    echo "check-target: FAIL — --subsystem leaked into a non-Windows link" >&2
    why guilinux; exit 1
fi
#     The same choice per-project, as a target property in kama.json — beside `runtime`, for its reasons.
sub="$tmp/sub"; mkdir -p "$sub/src"
cat > "$sub/kama.json" <<'JSON'
{ "name": "subdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "select": { "TARGET": { "WINDOWS": { "subsystem": "windows" } } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cp "$FIXTURE" "$sub/src/app.kama"
subline=$("$KAMA" build --release --cc "echo" "$sub/kama.json" --target WINDOWS -o "$sub/app" 2>/dev/null || true)
if ! printf '%s' "$subline" | grep -qF -- "-Wl,--subsystem,windows"; then
    echo "check-target: FAIL — a target's \"subsystem\": \"windows\" did not reach the link tail" >&2
    printf '%s\n' "$subline" | sed 's/^/    /' >&2
    exit 1
fi
#     A typo must not read as "not windows" and silently hand back the console default being changed.
printf '%s\n' '{ "name": "subdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } },
  "select": { "TARGET": { "WINDOWS": { "subsystem": "gui" } } } }' > "$sub/kama.json"
if "$KAMA" build --release --cc "echo" "$sub/kama.json" --target WINDOWS -o "$sub/app" >/dev/null 2>"$sub/err"; then
    echo "check-target: FAIL — an unknown \"subsystem\" value was accepted" >&2
    exit 1
fi
if ! grep -qF 'must be "console" or "windows"' "$sub/err"; then
    echo "check-target: FAIL — a bad \"subsystem\" value failed, but not with the value diagnostic:" >&2
    sed 's/^/  /' "$sub/err" >&2
    exit 1
fi
#     …and so must a bad value on the CLI, which is a separate code path from the manifest reader.
if "$KAMA" build --release --cc "echo" "$FIXTURE" --target WINDOWS --subsystem gui -o "$tmp/bad" \
   >/dev/null 2>"$tmp/badsub.err"; then
    echo "check-target: FAIL — an unknown --subsystem value was accepted" >&2
    exit 1
fi
if ! grep -qF 'must be "console" or "windows"' "$tmp/badsub.err"; then
    echo "check-target: FAIL — a bad --subsystem value failed, but not with the value diagnostic:" >&2
    sed 's/^/  /' "$tmp/badsub.err" >&2
    exit 1
fi

# 3. SHARED-LIBRARY EXTENSION — the default output name follows the target's platform convention, so a
#    cross build does not produce a `.dylib` for Windows.
for spec in "WINDOWS .dll" "MACOS .dylib" "LINUX .so"; do
    set -- $spec
    # `tr -d '"'` because the driver always emits `-o "<path>"`, and whether the stub `--cc echo` strips
    # those quotes depends on the SHELL `system()` hands the command to: POSIX `sh` removes them, but a
    # natively-built Windows kama goes through `cmd.exe`, whose `echo` prints them verbatim. Without this
    # the anchored match sees `"…/arith.dll"` and fails on Windows only — reported as "did not default its
    # shared-library output to *.dll" even though the very next line of the report says it built arith.dll.
    out=$(tryline "shared$1" build --shared --cc "echo" "$FIXTURE" --target "$1" \
          | tr ' ' '\n' | tr -d '"' | grep -E "arith\\$2$" || true)
    if [ -z "$out" ]; then
        echo "check-target: FAIL — target $1 did not default its shared-library output to *$2" >&2
        why "shared$1"
        exit 1
    fi
done

# 4. BARE METAL is a TRIPLE PROPERTY, not a magic target name. Any `os=none` triple must get the
#    freestanding treatment — that is what lets a real board triple work without the compiler knowing it.
for t in EMBEDDED riscv32-none-elf thumbv7em-none-eabihf; do
    line=$("$KAMA" build --cc "echo" "$FIXTURE" --target "$t" -o "$tmp/o.o" 2>/dev/null || true)
    for flag in -ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED; do
        if ! printf '%s' "$line" | grep -qF -- "$flag"; then
            echo "check-target: FAIL — os=none target $t did not get $flag" >&2
            exit 1
        fi
    done
done

# 5. A HOSTED triple must NOT get the freestanding treatment — the complement of case 4, so a mistake
#    that turns freestanding on unconditionally cannot pass.
if ccline aarch64-linux-gnu | grep -qF -- "-nostdlib"; then
    echo "check-target: FAIL — a hosted triple was compiled freestanding" >&2
    exit 1
fi

# 6. NO TOOLCHAIN -> a clear refusal; A TOOLCHAIN -> it just works. Which of the two we assert depends
#    on whether this machine has a zig, since zig is picked up automatically for a cross build (it is the
#    one widely-available compiler that bundles every target's libc, so it is the only thing kama can
#    substitute unprompted and expect to succeed).
if command -v zig >/dev/null 2>&1; then
    if ! "$KAMA" build "$FIXTURE" --target $CROSS_OS -o "$tmp/auto.exe" >/dev/null 2>"$tmp/auto.err"; then
        echo "check-target: FAIL — a cross build did not pick up the zig on PATH:" >&2
        sed 's/^/  /' "$tmp/auto.err" >&2
        exit 1
    fi
    # ...and it must be a real binary FOR THAT TARGET, not a host one with a foreign name.
    if command -v file >/dev/null 2>&1; then
        if ! file "$tmp/auto.exe" | grep -qi "$CROSS_FILE_MAGIC"; then
            echo "check-target: FAIL — --target $CROSS_OS produced something that is not a $CROSS_FILE_MAGIC binary:" >&2
            file "$tmp/auto.exe" | sed 's/^/  /' >&2
            exit 1
        fi
        # …and while a real cross toolchain is in hand, prove the SUBSYSTEM end to end rather than only at
        # the command line: `file` reads the PE subsystem field directly, so a build each way is the whole
        # assertion — no execution, no Windows host. Only meaningful when the foreign target IS Windows
        # (on a Windows host CROSS_OS is LINUX, and ELF has no subsystem field).
        #
        # ⚠️ This must go through the ORDINARY build path, never `--cc`: an override suppresses the target
        # triple, and `kama: built <path>` is printed off the C compiler's exit status without stat-ing the
        # output — so a --cc that produced nothing still says "built". Assert on `file`, never on that.
        if [ "$CROSS_OS" = WINDOWS ]; then
            if ! file "$tmp/auto.exe" | grep -qi 'console'; then
                echo "check-target: FAIL — the default Windows binary is not console-subsystem:" >&2
                file "$tmp/auto.exe" | sed 's/^/  /' >&2
                exit 1
            fi
            if ! "$KAMA" build "$FIXTURE" --target WINDOWS --subsystem windows -o "$tmp/autogui.exe" \
                 >/dev/null 2>"$tmp/autogui.err"; then
                echo "check-target: FAIL — a --subsystem windows cross build did not link:" >&2
                sed 's/^/  /' "$tmp/autogui.err" >&2
                exit 1
            fi
            if ! file "$tmp/autogui.exe" | grep -qi 'GUI'; then
                echo "check-target: FAIL — --subsystem windows did not produce a GUI-subsystem PE:" >&2
                file "$tmp/autogui.exe" | sed 's/^/  /' >&2
                exit 1
            fi
        fi
    fi
else
    if "$KAMA" build "$FIXTURE" --target $CROSS_OS -o "$tmp/x" >/dev/null 2>"$tmp/cross.err"; then
        echo "check-target: FAIL — a cross build with no cross toolchain was accepted" >&2
        exit 1
    fi
    if ! grep -qF "has no libc for it" "$tmp/cross.err"; then
        echo "check-target: FAIL — a cross build failed, but not with the toolchain diagnostic:" >&2
        sed 's/^/  /' "$tmp/cross.err" >&2
        exit 1
    fi
fi

# 6b. …but "crossing" means the host compiler genuinely CANNOT do the job, not merely that the triple
#     string differs. Bare metal on the host's OWN arch is `-ffreestanding -nostdlib -c` stopping at an
#     object — something the host clang does perfectly well, and what has always made bare-metal builds
#     triple-agnostic. A foreign ARCH is a real cross build and must still refuse. Getting this wrong in
#     either direction is invisible until someone actually builds firmware, hence both halves.
if ! "$KAMA" build "$FIXTURE" --target EMBEDDED -o "$tmp/host.o" >/dev/null 2>"$tmp/emb.err"; then
    echo "check-target: FAIL — bare metal on the host's own arch was treated as a cross build:" >&2
    sed 's/^/  /' "$tmp/emb.err" >&2
    exit 1
fi
if "$KAMA" build "$FIXTURE" --target riscv32-none-elf -o "$tmp/rv.o" >/dev/null 2>"$tmp/rv.err"; then
    echo "check-target: FAIL — a foreign-arch bare-metal target built with the host compiler" >&2
    exit 1
fi

# 7. TRANSPILE IS ALWAYS ALLOWED — emitting C for someone else's toolchain needs no toolchain here, and
#    it is the escape hatch the refusal above points at, so it must actually work.
if ! "$KAMA" transpile --no-line "$FIXTURE" --target WINDOWS -o "$tmp/foreign.c" >/dev/null 2>&1; then
    echo "check-target: FAIL — transpile refused a foreign target (it needs no toolchain)" >&2
    exit 1
fi

# 8. `zig cc` TAKES ITS TARGET AS A FLAG. A cross gcc has the triple baked into its NAME and must not be
#    given -target; zig is one binary for every target and must be. Our triple is already Zig's 3-part
#    <arch>-<os>-<abi> form, so it passes straight through. Stubbed with echo — this asserts kama's
#    plumbing, which is kama's half; whether zig then emits a PE binary is zig's.
zigline=$(tryline zig build "$FIXTURE" --target "$CROSS_WINDOWS" --cc "echo zig cc" -o "$tmp/z")
if ! printf '%s' "$zigline" | grep -qF -- "-target $CROSS_WINDOWS"; then
    echo "check-target: FAIL — zig cc did not receive -target for a cross build ($CROSS_WINDOWS)" >&2
    why zig
    exit 1
fi
hostline=$(tryline zighost build "$FIXTURE" --cc "echo zig cc" -o "$tmp/z2")
if printf '%s' "$hostline" | grep -qF -- "-target "; then
    echo "check-target: FAIL — a same-host build passed -target (it should be left alone)" >&2
    why zighost
    exit 1
fi
# Plain clang is a multi-target driver too — it has always been able to cross, it just needs the
# target's headers/libs. So an EXISTING toolchain is a first-class path; zig is only the one that
# bundles the libc. Whereas a per-target binary (aarch64-linux-gnu-gcc) has its triple in its NAME and
# must NOT be handed -target, or it breaks.
clangline=$(tryline clang build "$FIXTURE" --target "$CROSS_LINUX" --cc "echo clang" -o "$tmp/c1")
if ! printf '%s' "$clangline" | grep -qF -- "-target $CROSS_LINUX"; then
    echo "check-target: FAIL — clang did not receive -target for a cross build ($CROSS_LINUX)" >&2
    why clang
    exit 1
fi
gccline=$(tryline gcc build "$FIXTURE" --target "$CROSS_LINUX" --cc "echo $CROSS_LINUX-gcc" -o "$tmp/c2")
if printf '%s' "$gccline" | grep -qF -- "-target "; then
    echo "check-target: FAIL — a per-target gcc was handed -target (its triple is in its name)" >&2
    why gcc
    exit 1
fi

# 9. TARGET SPECS — a target declared in kama.json carries its own toolchain, so a team shares one
#    checked-in cross setup instead of each developer remembering flags.
spec="$tmp/spec"
mkdir -p "$spec/src"
cat > "$spec/kama.json" <<'JSON'
{ "name": "crossdemo", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama",
  "select": { "TARGET": { "RPI": { "triple": "aarch64-linux-gnu", "cc": "echo RPICC:",
                                   "sysroot": "/opt/rpi-sysroot",
                                   "cflags": ["-mcpu=cortex-a72"], "ldflags": ["-Wl,--as-needed"] } } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cp "$FIXTURE" "$spec/src/app.kama"
specline=$("$KAMA" build "$spec/kama.json" --target RPI -o "$spec/app" 2>/dev/null || true)
for want in "RPICC:" "--sysroot=" "-mcpu=cortex-a72" "-Wl,--as-needed"; do
    if ! printf '%s' "$specline" | grep -qF -- "$want"; then
        echo "check-target: FAIL — a kama.json target spec did not contribute '$want'" >&2
        printf '%s\n' "$specline" | sed 's/^/    /' >&2
        exit 1
    fi
done
# and its derived flags come from the DECLARED triple, not the host
if ! "$KAMA" transpile --no-line "$spec/kama.json" --target RPI -o "$spec/app.c" >/dev/null 2>&1; then
    echo "check-target: FAIL — a declared cross target could not be transpiled" >&2
    exit 1
fi

# 9b. A DECLARED DEFAULT TARGET (LSP M6 A1.2). Every other select value could carry `"default": true`,
#     but `select.TARGET` parsed only triple/cc/ar/sysroot/cflags/ldflags — so a project that only ever
#     builds for one board had to retype `--target` forever, and (the reason this got fixed here) an
#     editor had no way to know which target to analyze for. Precedence must be:
#         built-in HOST  <  kama.json default  <  kama.local.json default  <  --target
dflt="$tmp/dflt"
mkdir -p "$dflt/src"
cat > "$dflt/kama.json" <<'JSON'
{ "name": "boardonly", "version": "0.1.0", "kind": "library",
  "select": { "TARGET": { "BOARD": { "triple": "riscv32-none-elf", "default": true } } },
  "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$dflt/src/gated.kama" <<'KAMA'
@compileFor(OS_NONE)  fn int32 bare() { return 1; }
@compileFor(!OS_NONE) fn int32 hosted() { return 0; }
KAMA
# The manifest sets the SCOPE (and with it the default target this section is about); the file stays
# what is being asked about. `query` is target-addressed, so the two compose.
symbols() { "$KAMA" query "$dflt/kama.json" "$dflt/src/gated.kama" --symbols 2>/dev/null; }
if ! symbols | grep -q 'function bare'; then
    echo "check-target: FAIL — a kama.json default target did not take effect (expected OS_NONE)" >&2
    symbols | sed 's/^/    /' >&2; exit 1
fi
if symbols | grep -q 'function hosted'; then
    echo "check-target: FAIL — the default target was declared bare-metal but HOSTED decls survived" >&2
    exit 1
fi
# an explicit --target still wins over the manifest default
if ! "$KAMA" query "$dflt/src/gated.kama" --symbols --target HOST 2>/dev/null | grep -q 'function hosted'; then
    echo "check-target: FAIL — --target did not override the manifest's default target" >&2
    exit 1
fi
# and kama.local.json overrides the manifest default (this is the LSP's configuration channel, so it
# has to work through exactly the same precedence the CLI uses — one mechanism, not two)
cat > "$dflt/kama.local.json" <<'JSON'
{ "select": { "TARGET": { "HOSTDEV": { "triple": "aarch64-macos-none", "default": true } } } }
JSON
if ! symbols | grep -q 'function hosted'; then
    echo "check-target: FAIL — kama.local.json did not override the manifest's default target" >&2
    symbols | sed 's/^/    /' >&2; exit 1
fi
rm -f "$dflt/kama.local.json"

# 10. OUTPUT AXIS — the artifact kind is its own single-select group rather than a `--shared` boolean plus
#     "bare metal implies object". STATIC is new capability (kama could not produce a `.a` at all), and
#     OBJECT on a HOSTED target proves object output is no longer welded to bare metal.
lib="$tmp/lib"
mkdir -p "$lib"
cp "$FIXTURE" "$lib/mathlib.kama"
"$KAMA" build "$lib/mathlib.kama" --select OUTPUT=STATIC >/dev/null 2>"$tmp/static.err" || {
    echo "check-target: FAIL — OUTPUT=STATIC did not build:" >&2; sed 's/^/  /' "$tmp/static.err" >&2; exit 1; }
if [ ! -f "$lib/libmathlib.a" ]; then
    echo "check-target: FAIL — OUTPUT=STATIC did not produce lib<name>.a" >&2
    exit 1
fi
# a real archive with a symbol table, not an empty or truncated file
if ! nm "$lib/libmathlib.a" 2>/dev/null | grep -q "kama_main"; then
    echo "check-target: FAIL — the static library holds no kama_main symbol" >&2
    exit 1
fi
"$KAMA" build "$lib/mathlib.kama" --select OUTPUT=OBJECT -o "$lib/m.o" >/dev/null 2>"$tmp/obj.err" || {
    echo "check-target: FAIL — OUTPUT=OBJECT on a hosted target did not build:" >&2
    sed 's/^/  /' "$tmp/obj.err" >&2; exit 1; }
if [ ! -s "$lib/m.o" ]; then
    echo "check-target: FAIL — OUTPUT=OBJECT produced no object on a hosted target" >&2
    exit 1
fi
# --shared remains sugar for OUTPUT=SHARED, defaulting its name to the target's convention
"$KAMA" build "$lib/mathlib.kama" --shared >/dev/null 2>&1
if [ ! -f "$lib/mathlib.dylib" ] && [ ! -f "$lib/mathlib.so" ] && [ ! -f "$lib/mathlib.dll" ]; then
    echo "check-target: FAIL — --shared produced no shared library" >&2
    exit 1
fi
# and an EXE is still an EXE
"$KAMA" build "$lib/mathlib.kama" -o "$lib/exe" >/dev/null 2>&1
if "$lib/exe"; then rc=0; else rc=$?; fi
if [ "$rc" != 42 ]; then
    echo "check-target: FAIL — the default EXE build returned $rc, expected 42" >&2
    exit 1
fi

# ---- the out/ layout ---------------------------------------------------------------------------------
# A project's build output collects under one root instead of scattering through the source tree, scoped
# by TRIPLE and BUILD TYPE because both vary independently and a collision between them is SILENT — you
# get yesterday's binary and no diagnostic. Belongs in this guard because the scoping IS the target axis.
od="$tmp/outdir"; mkdir -p "$od/src"
printf 'fn int32 main() { return 9; }\n' > "$od/src/app.kama"
printf '{ "name": "od", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "modules": { ".": { "visibility": "internal" } } }\n' > "$od/kama.json"

( cd "$od" && "$KAMA" build kama.json ) >/dev/null 2>"$tmp/od1.err" || {
    echo "check-target: FAIL — project build failed:" >&2; sed 's/^/  /' "$tmp/od1.err" >&2; exit 1; }
HOSTTRIPLE=$(ls "$od/out")
[ -x "$od/out/$HOSTTRIPLE/debug/app" ] || {
    echo "check-target: FAIL — no binary at out/$HOSTTRIPLE/debug/app; tree was:" >&2
    find "$od/out" -type f | sed 's/^/  /' >&2; exit 1; }

# The source tree must be untouched — no stray binary, no generated .c beside the source. This is the
# assertion that keeps a .gitignore three lines long instead of thirty.
stray=$(find "$od/src" -type f ! -name '*.kama' | head -5)
[ -z "$stray" ] || { echo "check-target: FAIL — build left artifacts in src/:" >&2
                     echo "$stray" | sed 's/^/  /' >&2; exit 1; }

# debug and release coexist rather than overwrite
( cd "$od" && "$KAMA" build kama.json --release ) >/dev/null 2>&1
[ -x "$od/out/$HOSTTRIPLE/release/app" ] && [ -x "$od/out/$HOSTTRIPLE/debug/app" ] || {
    echo "check-target: FAIL — a release build did not coexist with the debug one" >&2; exit 1; }

# the manifest's `out` key relocates the root
printf '{ "name": "od", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "out": "artifacts", "modules": { ".": { "visibility": "internal" } } }\n' > "$od/kama.json"
( cd "$od" && "$KAMA" build kama.json ) >/dev/null 2>&1
[ -x "$od/artifacts/$HOSTTRIPLE/debug/app" ] || {
    echo "check-target: FAIL — the manifest \"out\" key did not relocate the output root" >&2; exit 1; }

# -o still wins over both
( cd "$od" && "$KAMA" build kama.json -o chosen ) >/dev/null 2>&1
[ -x "$od/chosen" ] || { echo "check-target: FAIL — -o no longer wins over the out root" >&2; exit 1; }

# A LOOSE .kama with no manifest is NOT a project and keeps landing beside itself — `kama build hello.kama`
# -> ./hello is the documented first experience, and one file is not a project.
loose="$tmp/loose"; mkdir -p "$loose"
printf 'fn int32 main() { return 9; }\n' > "$loose/hello.kama"
( cd "$loose" && "$KAMA" build hello.kama ) >/dev/null 2>&1
[ -x "$loose/hello" ] && [ ! -d "$loose/out" ] || {
    echo "check-target: FAIL — a manifest-less build changed behavior (expected ./hello, no out/)" >&2; exit 1; }
# ...and still leaves no generated .c behind it
[ ! -f "$loose/hello.c" ] || { echo "check-target: FAIL — a manifest-less build left hello.c behind" >&2; exit 1; }

echo "check-target: PASS (link/compile flags follow the selected target, not the host: winsock, section GC,
  a Windows runtime linked statically so the .exe ships (with --dynamic-runtime / a target \"runtime\" key to opt out),
  shared-library extension, freestanding keyed on os=none rather than a target name; cross builds refuse
  without a toolchain, transpile always works, zig cc gets -target, kama.json target specs apply;
  a declared default target applies and loses to --target;
  OUTPUT selects exe/shared/static/object, incl. static archives and hosted object output;
  a project's artifacts collect under out/<triple>/<type>/ leaving src/ clean, \"out\" relocates it, -o wins,
  and a manifest-less build still lands beside its source)"
