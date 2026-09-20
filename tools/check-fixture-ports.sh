#!/bin/sh
# check-fixture-ports.sh — no two fixtures bind the same TCP port.
#
# The suite fans fixtures out across NCPU, so two that share a port RUN AT THE SAME TIME. On Windows a
# second bind to a live port can succeed, and the connection then lands in whichever listener's backlog
# the OS picks — so the loser's accept() never returns and the fixture fails having done nothing wrong.
#
# It is worth a guard because of how it PRESENTS: `net_resolve` and `net_nonblocking_accept` both bound
# 47661, and the symptom was `net_nonblocking_accept` failing about one run in six with its
# spin-budget-exhausted sentinel. That reads as a timing flake, and it was diagnosed as one twice — once
# by making the budget bigger, once by replacing the spin with a sleep. Neither helped, because no amount
# of waiting recovers a connection that went to another socket. A shared port is a static fact about the
# corpus, so it is checkable statically, and a static check cannot be fooled by how the failure looks.
#
# Deterministic and instant: no build, no network, no compiler. Just the corpus.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# `cast<uint16>(47661)` is how every net fixture spells a port. Anything else that binds one should be
# spelled that way too, so that it is visible here.
grep -rn 'cast<uint16>(4[0-9][0-9][0-9][0-9])' "$ROOT"/tests/*.kama 2>/dev/null \
    | sed 's#.*/tests/##; s/:.*cast<uint16>(\([0-9]*\)).*/ \1/' \
    | awk '{ print $2, $1 }' | sort -n > "$tmp/ports" || true

if [ ! -s "$tmp/ports" ]; then
    echo "check-fixture-ports: FAIL — found no fixture ports at all; the scan stopped matching," >&2
    echo "  so this guard proves nothing. Check the cast<uint16>(...) spelling in tests/*.kama." >&2
    exit 1
fi

dups=$(awk '{ print $1 }' "$tmp/ports" | uniq -d)
if [ -n "$dups" ]; then
    echo "check-fixture-ports: FAIL — these ports are bound by more than one fixture, and the suite runs" >&2
    echo "  fixtures in PARALLEL, so they collide:" >&2
    for p in $dups; do
        echo "    port $p:" >&2
        grep "^$p " "$tmp/ports" | sed 's/^/      /' >&2
    done
    echo "  Give each fixture its own port. The failure this causes looks like a timing flake in" >&2
    echo "  whichever fixture loses the race, which is why it is caught here instead." >&2
    exit 1
fi

n=$(wc -l < "$tmp/ports" | tr -d ' ')
echo "check-fixture-ports: PASS ($n fixture ports, all distinct)"
