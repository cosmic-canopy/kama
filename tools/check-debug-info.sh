#!/bin/sh
# check-debug-info.sh — a DEBUG build is debuggable: a breakpoint set on a .kama line resolves.
#
# That is the claim GOALS.md § *Production-grade build & debugging* makes, and it was false on macOS for
# every ordinary build. On ELF the linker copies DWARF out of each object into the executable; Mach-O
# instead leaves a DEBUG MAP pointing back at the `.o` files, and `dsymutil` is what walks it into a
# `.dSYM`. clang runs dsymutil when ONE invocation compiles and links, and never when it is handed
# objects — which is kama's `perTU` arm, taken whenever `-j` is above 1, i.e. by default on any
# multi-core host for any program with an `import`. The objects are then cleaned up and the map dangles:
#
#     error: debug map object file ".../app.o" containing debug info does not exist
#     (lldb) br set -f app.kama -l 12   ->   Breakpoint 1: no locations (pending).
#
# Nothing in the suite could see it, because `tools/run-checks.sh` exports KAMA_BUILD_JOBS=1 and that
# takes the OTHER arm. So this guard drives BOTH arms explicitly rather than trusting the default.
#
# The assertion is the breakpoint, not the artifact: a `.dSYM` that exists but carries no line table
# would pass a file-existence check and still leave a user unable to stop anywhere. Where there is no
# debugger to ask, the artifact check is the most that can be said, and the guard says only that.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-debug-info: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "check-debug-info: FAIL — $1" >&2; shift; [ $# -eq 0 ] || sed 's/^/  /' "$@" >&2; exit 1; }
ok()   { echo "  ok: $1"; }

# An `import` is what makes this a MULTI-TU program, which is what makes the link take objects.
cat > "$tmp/probe.kama" <<'KAMA'
import { std::collections::DynamicArray };
fn int32 work(int32 seed)
{
    DynamicArray<int32> xs = DynamicArray.empty();
    xs.add(item: seed);
    int32 total = 0;
    foreach (int32 v in xs) { total = total + v; }
    return total;
}
fn int32 main() { return work(seed: 7); }
KAMA
BPLINE=6        # `int32 total = 0;` — a statement, so it carries a #line of its own

case "$(uname -s 2>/dev/null || echo unknown)" in
    Darwin) host=macos ;;
    *)      host=other ;;
esac
have_lldb=no
command -v lldb >/dev/null 2>&1 && have_lldb=yes

# `one_build <label> <jobs>` — build both link arms and assert what each host can assert.
#   -j 1  single clang invocation: compiles and links together, so clang runs dsymutil itself
#   -j 2  per-TU: kama compiles objects and links them, so kama must run dsymutil
one_build() {
    label=$1; jobs=$2
    out="$tmp/app_$jobs"
    "$KAMA" build "$tmp/probe.kama" -j "$jobs" -o "$out" > "$tmp/out" 2>&1 \
        || fail "the $label build failed" "$tmp/out"
    # A build that merely WARNS has already lost on macOS — a dangling debug map warns rather than errors.
    if grep -qi 'warning' "$tmp/out"; then fail "the $label build warned" "$tmp/out"; fi

    if [ "$host" = macos ]; then
        [ -d "$out.dSYM" ] || fail "$label produced no $(basename "$out").dSYM — the Mach-O debug map was never consolidated, so lldb has nothing to read"
        ok "$label: a .dSYM is produced"
    fi

    if [ "$have_lldb" = yes ]; then
        lldb -b -o "br set -f probe.kama -l $BPLINE" "$out" > "$tmp/lldb" 2>&1 || true
        if ! grep -q "probe.kama:$BPLINE" "$tmp/lldb"; then
            fail "$label: a breakpoint on probe.kama:$BPLINE did not resolve to a location" "$tmp/lldb"
        fi
        ok "$label: br set -f probe.kama -l $BPLINE resolves"
    fi
}

one_build "single-invocation (-j 1)" 1
one_build "per-TU (-j 2)"            2

# RELEASE is the other half of the contract: `--release` turns `#line` and `-g` off on purpose, so it
# must NOT pay for a .dSYM. This also catches the opposite regression — running dsymutil unconditionally.
"$KAMA" build "$tmp/probe.kama" -j 2 --release -o "$tmp/rel" > "$tmp/out" 2>&1 \
    || fail "the release build failed" "$tmp/out"
if [ "$host" = macos ]; then
    [ -d "$tmp/rel.dSYM" ] && fail "a --release build produced a .dSYM; release is deliberately without debug info"
    ok "release: no .dSYM"
fi

if [ "$have_lldb" = no ]; then
    echo "check-debug-info: OK (artifacts only — no lldb on this host to set a breakpoint with)"
else
    echo "check-debug-info: OK (a .kama line breakpoint resolves in a debug build from both link arms)"
fi
