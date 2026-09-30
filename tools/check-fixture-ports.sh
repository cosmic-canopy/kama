#!/bin/sh
# check-fixture-ports.sh — no fixture binds a fixed port.
#
# The suite fans fixtures out across NCPU, so two that share a port RUN AT THE SAME TIME. On Windows a
# second bind to a live port can succeed, and the connection then lands in whichever listener's backlog
# the OS picks — so the loser's accept() never returns and the fixture fails having done nothing wrong.
#
# It is worth a guard because of how it PRESENTS: `net_resolve` and `net_nonblocking_accept` both bound
# 47661, and the symptom was `net_nonblocking_accept` failing about one run in six with its
# spin-budget-exhausted sentinel. That reads as a timing flake, and it was diagnosed as one twice — once
# by making the budget bigger, once by replacing the spin with a sleep. Neither helped, because no amount
# of waiting recovers a connection that went to another socket.
#
# This guard used to keep every fixture's port DISTINCT, because there was no way to learn an ephemeral
# port: `TcpListener` had no `localAddr()`. Since KR-105 it has one, so every fixture binds port 0 and
# reads back the port the OS picked, and a collision is impossible rather than merely avoided. The rule is
# therefore stronger and simpler: a fixture that binds a socket spells no fixed port at all. It also used
# to match only `cast<uint16>(47661)`, so `47671ui16` and a bare `port: 47811` slipped past it; all three
# spellings are refused now. (The web fixtures connect to servers `run_tests.sh` starts on fixed ports —
# those belong to the harness, and a fixture that binds nothing is not scanned.)
#
# Deterministic and instant: no build, no network, no compiler. Just the corpus.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Every spelling a port has had in this corpus: `cast<uint16>(N)`, `Nui16`, and a bare `port: N`.
PORT_RE='cast<uint16>\([1-9][0-9]{3,4}\)|\b[1-9][0-9]{3,4}ui16\b|port: *[1-9][0-9]{3,4}\b'
BINDS_RE='(TcpListener|UdpSocket)\.bind(To)?\('

# Self-check: the pattern must still see each historical spelling, and must not see port 0 — a guard whose
# pattern drifted blind would pass every corpus.
seen=$(printf '%s\n' 'cast<uint16>(47661)' '47671ui16' 'bind(host: h, port: 47811)' 'port: 0ui16' \
       | grep -cE "$PORT_RE" || true)
if [ "$seen" != 3 ]; then
    echo "check-fixture-ports: FAIL — the port pattern matched $seen of the 3 known spellings (and must" >&2
    echo "  not match port 0); it has drifted, so it would pass a fixture that pins a port." >&2
    exit 1
fi

find "$ROOT/tests" -name '*.kama' > "$tmp/files"
: > "$tmp/binders"
while IFS= read -r f; do
    grep -qE "$BINDS_RE" "$f" && printf '%s\n' "$f" >> "$tmp/binders"
done < "$tmp/files"
if [ ! -s "$tmp/binders" ]; then
    echo "check-fixture-ports: FAIL — found no fixture that binds a socket at all; the scan stopped" >&2
    echo "  matching, so this guard proves nothing. Check the bind spelling in tests/." >&2
    exit 1
fi

: > "$tmp/pinned"
while IFS= read -r f; do
    grep -nHE "$PORT_RE" "$f" | sed "s#^$ROOT/##" >> "$tmp/pinned" || true
done < "$tmp/binders"
if [ -s "$tmp/pinned" ]; then
    echo "check-fixture-ports: FAIL — these fixtures bind a socket and spell a fixed port. The suite runs" >&2
    echo "  fixtures in PARALLEL, so a fixed port can collide with another run's:" >&2
    sed 's/^/    /' "$tmp/pinned" >&2
    echo "  Bind port 0 and read the port back: \`listener.localAddr()\` (a port nothing is bound to any" >&2
    echo "  more: bind 0, read it, let the listener drop — see tests/net_refused.kama)." >&2
    exit 1
fi

n=$(wc -l < "$tmp/binders" | tr -d ' ')
echo "check-fixture-ports: PASS ($n fixtures bind sockets, every one on port 0)"
