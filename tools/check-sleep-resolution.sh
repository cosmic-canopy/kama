#!/bin/sh
# check-sleep-resolution.sh — `std::time::sleep` honours the span it was ASKED for, not the platform's
# scheduler tick (KR-76).
#
# The contract in SPEC is "at least `d`", and Windows kept it while being useless: `Sleep()` rounds to the
# next scheduler tick, ~15.625 ms by default, so `sleep(1 ms)` and `sleep(8 ms)` BOTH cost one tick.
# Every program that paces itself with `sleep` — a frame loop's 8 ms nap, a poll loop's 10 ms wait — ran
# at half rate there and at the asked rate everywhere else. Measured before the fix on this repo's Windows
# box: 1 ms -> 13.4 ms, 8 ms -> 15.6 ms. Reported by the first consumer, whose frame loop found it.
#
# ⚠️ THE ASSERTION IS RELATIVE, AND THAT IS THE WHOLE DESIGN. An absolute ceiling ("8 ms must take under
# 14 ms") is a coin flip on a loaded box, and these guards run in PARALLEL with dozens of others, so load
# is the normal case rather than the exception. Under a tick, a 1 ms and an 8 ms sleep cost the SAME; with
# a real timer the 8 ms costs about 7 ms more. Scheduling delay adds to both means and cancels out of the
# difference, so the signal survives what an absolute bound cannot.
#
# check-legs: native
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-sleep-resolution: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/res.kama" <<'KAMA'
import { core::print, std::time::Duration, std::time::Instant, std::time::monotonicNow, std::time::sleep };

// The MEAN over many runs, in microseconds. One sample is scheduling noise; the tick is a property of
// the distribution, so it takes a distribution to see it.
fn int64 meanMicros(int64 ms, int32 runs) {
    Instant t0 = monotonicNow();
    int32 i = 0;
    while (i < runs) {
        sleep(d: Duration.fromMillis(ms: ms));
        i = i + 1;
    }
    return monotonicNow().since(earlier: t0).asMicros() / cast<int64>(runs);
}

fn int32 main() {
    int64 one   = meanMicros(ms: 1i64, runs: 60);
    int64 eight = meanMicros(ms: 8i64, runs: 60);
    print(s: "one ${one}\n");
    print(s: "eight ${eight}\n");
    return 0;
}
KAMA

"$KAMA" build --release "$tmp/res.kama" -o "$tmp/res" >/dev/null 2>"$tmp/build.err" || {
    echo "check-sleep-resolution: FAIL — the probe did not build" >&2
    sed 's/^/  /' "$tmp/build.err" >&2
    exit 1
}
"$tmp/res" > "$tmp/out" 2>/dev/null || {
    echo "check-sleep-resolution: FAIL — the probe did not run" >&2; exit 1; }

one=$(awk '/^one /   { print $2 }' "$tmp/out")
eight=$(awk '/^eight / { print $2 }' "$tmp/out")
[ -n "$one" ] && [ -n "$eight" ] || {
    echo "check-sleep-resolution: FAIL — the probe printed nothing usable:" >&2
    sed 's/^/  /' "$tmp/out" >&2; exit 1; }

# 1. The CONTRACT, which held even while the resolution did not: never less than the span asked for.
if [ "$eight" -lt 8000 ]; then
    echo "check-sleep-resolution: FAIL — sleep(8 ms) averaged ${eight} us, under the 8000 it promises." >&2
    echo "  SPEC says a sleep lasts AT LEAST its duration; returning early breaks every timeout built on it." >&2
    exit 1
fi

# 2. The RESOLUTION. Asking for 7 ms more must COST more. Under a scheduler tick the two are the same
#    number, so the difference collapses toward zero; the 4000 us floor is a wide margin either way
#    (measured 2215 us broken, 7082 us fixed).
diff=$((eight - one))
if [ "$diff" -lt 4000 ]; then
    echo "check-sleep-resolution: FAIL — sleep(1 ms) averaged ${one} us and sleep(8 ms) ${eight} us," >&2
    echo "  a difference of ${diff} us for 7 ms more of asked-for sleep. The two are landing on the same" >&2
    echo "  scheduler tick, so the sleep is quantized rather than honoured and any program pacing itself" >&2
    echo "  with it runs slow. On Windows this is the high-resolution timer arm in kama_sleep_ns" >&2
    echo "  (include/kama_time.h) having failed or been removed." >&2
    exit 1
fi

echo "check-sleep-resolution: PASS (sleep(1 ms) ${one} us, sleep(8 ms) ${eight} us — "\
"honoured, not quantized to the scheduler tick)"
