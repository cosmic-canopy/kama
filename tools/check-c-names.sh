#!/bin/sh
# check-c-names.sh — kama's three C registers are still FREE on this host (KR-67).
#
# Every name kama emits lives in one of three registers, and the whole scheme rests on one premise:
# nothing else in the translation unit spells them.
#
#     KAMA_…   macros
#     kama_…   what the compiler owns (runtime types, emitted members, temps, the prelude's scope)
#     k_…      what the user owns (fields, payloads, parameters, locals, bindings, contract slots)
#
# Why a guard and not a comment. The premise was MEASURED once — 0 of 4,708 macros on macOS and 0 of
# 21,748 on one Windows TU start with `k_` — and a measurement taken once is a fact about that day's
# SDK. The macro surface moves with every OS release, every toolchain bump, and every `extern "<vendor.h>"`
# a user writes. If a platform header ever defines `k_foo`, every kama program on that platform starts
# miscompiling in the way KR-67 existed to stop, and the failure would look like the bug, not like a
# broken assumption. So the premise is re-checked wherever the suite runs.
#
# ⚠️ This guard cannot see a USER's own FFI header — kama compiles headers it has never met. That
# residual is stated in SPEC § C names; what is checkable is the surface kama SHIPS and the system
# headers its own seam pulls in, and that is what runs here.
#
# ⚠️ THE GUARD PROVES ITS OWN INSTRUMENT, EVERY RUN (§3). A grep for a pattern that can never match sits
# green forever; this repo has shipped three such fixtures already. §3 feeds the same scanner a header
# that DOES define a hostile macro and requires it to be caught.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC_BIN=${CC:-cc}

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "check-c-names: NOTE — no C compiler ($CC_BIN); skipping"; exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The scanner: every macro visible after preprocessing $1, one name per line.
macros_of() { "$CC_BIN" -std=c11 -dM -E -I "$ROOT/include" "$1" 2>/dev/null | awk '{print $2}' | sed 's/(.*//'; }

fail=0
bad() { echo "check-c-names: FAIL — $1" >&2; fail=1; }

# --- §1. The FOREIGN surface: the system headers kama's own seam pulls in ------------------------------
# Preprocessed WITHOUT kama's headers, so every macro here belongs to the platform. None of them may
# spell a register kama emits into. (`kama_os.h` is the widest reach kama has — it is what drags
# <sys/stat.h>, <netinet/in.h>, <pthread.h> and friends into every program that imports `std::fs`.)
cat > "$tmp/system.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <time.h>
EOF
# ⚠️ A PREFIX match, not equality: `uname -s` on msys2 is `MINGW64_NT-10.0-26200-ARM64`, never the bare
# `MINGW64_NT` this used to compare against — so Windows took the POSIX arm, `<netinet/in.h>` was not
# there, the preprocess died and §1 scanned ZERO macros. The floor below caught it, which is what it is
# for; the guard shipped from a Mac and had never run here.
case "$(uname -s)" in
  MSYS*|MINGW*|CYGWIN*) ;;
  *)
    cat >> "$tmp/system.c" <<'EOF'
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <pthread.h>
EOF
    ;;
esac
echo 'int main(void){return 0;}' >> "$tmp/system.c"

macros_of "$tmp/system.c" > "$tmp/system.macros" || true
sys_n=$(wc -l < "$tmp/system.macros" | tr -d ' ')
if [ "$sys_n" -lt 100 ]; then
    bad "the system scan found only $sys_n macros — the scanner is not reading headers, so §1 proves nothing"
fi
if LC_ALL=C grep -E '^(k_|kama_|KAMA_)' "$tmp/system.macros" > "$tmp/clash" 2>/dev/null && [ -s "$tmp/clash" ]; then
    bad "a PLATFORM header defines a macro in a register kama emits into:"
    sed 's/^/    /' "$tmp/clash" >&2
    echo "    Every kama program on this host is now at risk of the KR-67 miscompile." >&2
else
    echo "  ok: $sys_n platform macros, none in \`k_\`/\`kama_\`/\`KAMA_\`"
fi

# --- §2. kama's OWN shipped headers define nothing in the USER's register ------------------------------
# `k_` is the user's half of the family, and kama must never put a macro there: a user field named `foo`
# becomes `k_foo`, so a kama-shipped `#define k_foo` would rewrite it exactly as `<windef.h>`'s `near`
# used to. (`kama_`/`KAMA_` are the compiler's own and are expected here.)
printf '#include "kama_runtime.h"\n#include "kama_os.h"\nint main(void){return 0;}\n' > "$tmp/shipped.c"
macros_of "$tmp/shipped.c" > "$tmp/shipped.macros" || true
ship_n=$(wc -l < "$tmp/shipped.macros" | tr -d ' ')
if [ "$ship_n" -lt "$sys_n" ]; then
    bad "the shipped-header scan ($ship_n) saw fewer macros than the system scan ($sys_n) — it did not read kama's headers"
fi
if LC_ALL=C grep -E '^k_' "$tmp/shipped.macros" > "$tmp/own" 2>/dev/null && [ -s "$tmp/own" ]; then
    bad "a kama-SHIPPED header defines a macro in the user's register \`k_\`:"
    sed 's/^/    /' "$tmp/own" >&2
else
    kama_n=$(LC_ALL=C grep -cE '^(kama_|KAMA_)' "$tmp/shipped.macros" || true)
    echo "  ok: $ship_n macros over the shipped set; $kama_n are kama's own, 0 in \`k_\`"
fi

# --- §3. The control: the scanner can actually catch one ----------------------------------------------
# Without this, §1 and §2 are indistinguishable from a grep that never matches.
printf '#define k_near 1\n#define kama_planted 2\nint main(void){return 0;}\n' > "$tmp/planted.c"
if macros_of "$tmp/planted.c" | LC_ALL=C grep -qE '^(k_|kama_)'; then
    echo "  ok: the control is caught (the scan can fail)"
else
    bad "the CONTROL was not caught — this guard cannot detect a clash and every PASS above is vacuous"
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-c-names: PASS (kama's \`k_\`/\`kama_\`/\`KAMA_\` registers are free on this host)"
