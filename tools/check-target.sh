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

# The assembled cc command line for a given target, with the compiler stubbed out.
ccline() {
    "$KAMA" build --release --cc "echo" "$FIXTURE" --target "$1" -o "$tmp/out" 2>/dev/null
}

want() {   # want <target> <substring> <description>
    if ! ccline "$1" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 did not pass '$2' ($3)" >&2
        echo "  command line was:" >&2
        ccline "$1" | sed 's/^/    /' >&2
        exit 1
    fi
}

reject() {   # reject <target> <substring> <description>
    if ccline "$1" | grep -qF -- "$2"; then
        echo "check-target: FAIL — target $1 passed '$2' but must not ($3)" >&2
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

# 3. SHARED-LIBRARY EXTENSION — the default output name follows the target's platform convention, so a
#    cross build does not produce a `.dylib` for Windows.
for spec in "WINDOWS .dll" "MACOS .dylib" "LINUX .so"; do
    set -- $spec
    out=$("$KAMA" build --shared --cc "echo" "$FIXTURE" --target "$1" 2>/dev/null | tr ' ' '\n' | grep -E "arith\\$2$" || true)
    if [ -z "$out" ]; then
        echo "check-target: FAIL — target $1 did not default its shared-library output to *$2" >&2
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
    if ! "$KAMA" build "$FIXTURE" --target WINDOWS -o "$tmp/auto.exe" >/dev/null 2>"$tmp/auto.err"; then
        echo "check-target: FAIL — a cross build did not pick up the zig on PATH:" >&2
        sed 's/^/  /' "$tmp/auto.err" >&2
        exit 1
    fi
    # ...and it must be a real binary FOR THAT TARGET, not a host one with a foreign name.
    if command -v file >/dev/null 2>&1; then
        if ! file "$tmp/auto.exe" | grep -qi "MS Windows"; then
            echo "check-target: FAIL — --target WINDOWS produced something that is not a PE binary:" >&2
            file "$tmp/auto.exe" | sed 's/^/  /' >&2
            exit 1
        fi
    fi
else
    if "$KAMA" build "$FIXTURE" --target WINDOWS -o "$tmp/x" >/dev/null 2>"$tmp/cross.err"; then
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
zigline=$("$KAMA" build "$FIXTURE" --target x86_64-windows-gnu --cc "echo zig cc" -o "$tmp/z" 2>/dev/null || true)
if ! printf '%s' "$zigline" | grep -qF -- "-target x86_64-windows-gnu"; then
    echo "check-target: FAIL — zig cc did not receive -target for a cross build" >&2
    exit 1
fi
hostline=$("$KAMA" build "$FIXTURE" --cc "echo zig cc" -o "$tmp/z2" 2>/dev/null || true)
if printf '%s' "$hostline" | grep -qF -- "-target "; then
    echo "check-target: FAIL — a same-host build passed -target (it should be left alone)" >&2
    exit 1
fi
# Plain clang is a multi-target driver too — it has always been able to cross, it just needs the
# target's headers/libs. So an EXISTING toolchain is a first-class path; zig is only the one that
# bundles the libc. Whereas a per-target binary (aarch64-linux-gnu-gcc) has its triple in its NAME and
# must NOT be handed -target, or it breaks.
clangline=$("$KAMA" build "$FIXTURE" --target x86_64-linux-gnu --cc "echo clang" -o "$tmp/c1" 2>/dev/null || true)
if ! printf '%s' "$clangline" | grep -qF -- "-target x86_64-linux-gnu"; then
    echo "check-target: FAIL — clang did not receive -target for a cross build" >&2
    exit 1
fi
gccline=$("$KAMA" build "$FIXTURE" --target x86_64-linux-gnu --cc "echo x86_64-linux-gnu-gcc" -o "$tmp/c2" 2>/dev/null || true)
if printf '%s' "$gccline" | grep -qF -- "-target "; then
    echo "check-target: FAIL — a per-target gcc was handed -target (its triple is in its name)" >&2
    exit 1
fi

# 9. TARGET SPECS — a target declared in kama.json carries its own toolchain, so a team shares one
#    checked-in cross setup instead of each developer remembering flags.
spec="$tmp/spec"
mkdir -p "$spec"
cat > "$spec/kama.json" <<'JSON'
{ "name": "cross-demo", "version": "0.1.0",
  "select": { "TARGET": { "RPI": { "triple": "aarch64-linux-gnu", "cc": "echo RPICC:",
                                   "sysroot": "/opt/rpi-sysroot",
                                   "cflags": ["-mcpu=cortex-a72"], "ldflags": ["-Wl,--as-needed"] } } } }
JSON
cp "$FIXTURE" "$spec/app.kama"
specline=$("$KAMA" build "$spec/app.kama" --target RPI -o "$spec/app" 2>/dev/null || true)
for want in "RPICC:" "--sysroot=" "-mcpu=cortex-a72" "-Wl,--as-needed"; do
    if ! printf '%s' "$specline" | grep -qF -- "$want"; then
        echo "check-target: FAIL — a kama.json target spec did not contribute '$want'" >&2
        printf '%s\n' "$specline" | sed 's/^/    /' >&2
        exit 1
    fi
done
# and its derived flags come from the DECLARED triple, not the host
if ! "$KAMA" transpile --no-line "$spec/app.kama" --target RPI -o "$spec/app.c" >/dev/null 2>&1; then
    echo "check-target: FAIL — a declared cross target could not be transpiled" >&2
    exit 1
fi

# 9b. A DECLARED DEFAULT TARGET (LSP M6 A1.2). Every other select value could carry `"default": true`,
#     but `select.TARGET` parsed only triple/cc/ar/sysroot/cflags/ldflags — so a project that only ever
#     builds for one board had to retype `--target` forever, and (the reason this got fixed here) an
#     editor had no way to know which target to analyze for. Precedence must be:
#         built-in HOST  <  kama.json default  <  kama.local.json default  <  --target
dflt="$tmp/dflt"
mkdir -p "$dflt"
cat > "$dflt/kama.json" <<'JSON'
{ "name": "board-only", "version": "0.1.0",
  "select": { "TARGET": { "BOARD": { "triple": "riscv32-none-elf", "default": true } } } }
JSON
cat > "$dflt/gated.kama" <<'KAMA'
@compileFor(OS_NONE)  fn int32 bare() { return 1; }
@compileFor(!OS_NONE) fn int32 hosted() { return 0; }
KAMA
symbols() { "$KAMA" query "$dflt/gated.kama" --symbols 2>/dev/null; }
if ! symbols | grep -q 'function bare'; then
    echo "check-target: FAIL — a kama.json default target did not take effect (expected OS_NONE)" >&2
    symbols | sed 's/^/    /' >&2; exit 1
fi
if symbols | grep -q 'function hosted'; then
    echo "check-target: FAIL — the default target was declared bare-metal but HOSTED decls survived" >&2
    exit 1
fi
# an explicit --target still wins over the manifest default
if ! "$KAMA" query "$dflt/gated.kama" --symbols --target HOST 2>/dev/null | grep -q 'function hosted'; then
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

echo "check-target: PASS (link/compile flags follow the selected target, not the host: winsock, section GC,
  shared-library extension, freestanding keyed on os=none rather than a target name; cross builds refuse
  without a toolchain, transpile always works, zig cc gets -target, kama.json target specs apply;
  a declared default target applies and loses to --target;\n  OUTPUT selects exe/shared/static/object, incl. static archives and hosted object output)"
