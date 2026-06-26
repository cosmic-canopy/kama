#!/bin/bash
# cstar end-to-end test harness.
#
# Each fixture is a .cstar file under tests/ with a matching .expect file whose
# single line is the expected process exit code. We transpile, compile, run, and
# compare the exit code.
set -u

CSTAR="./cstar"
TESTS_DIR="tests"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

for src in "$TESTS_DIR"/*.cstar; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .cstar)"
    expect_file="$TESTS_DIR/$name.expect"
    if [ ! -f "$expect_file" ]; then
        echo "SKIP $name (no .expect)"
        continue
    fi
    expected="$(cat "$expect_file")"

    exe="$TMP/$name"
    if ! "$CSTAR" build "$src" -o "$exe" >/dev/null 2>"$TMP/$name.err"; then
        echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; fail=$((fail+1)); continue
    fi
    "$exe"; actual=$?

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (exit $actual)"; pass=$((pass+1))
    else
        echo "FAIL $name (got $actual, expected $expected)"; fail=$((fail+1))
    fi
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
