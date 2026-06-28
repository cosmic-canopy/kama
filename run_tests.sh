#!/bin/bash
# cstar end-to-end test harness.
#
# Each fixture is a .cstar file under tests/ with a matching .expect file whose
# single line is the expected process exit code. We transpile, compile, run, and
# compare the exit code.
#
# A multi-file fixture is a directory tests/<name>.d/ containing several .cstar
# files plus one .expect; all its .cstar are built together (the module system).
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

# Multi-file fixtures: tests/<name>.d/ with several .cstar built together.
for dir in "$TESTS_DIR"/*.d; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir" .d)"
    expect_file="$dir/expect"
    if [ ! -f "$expect_file" ]; then
        echo "SKIP $name (no expect)"
        continue
    fi
    expected="$(cat "$expect_file")"

    exe="$TMP/$name"
    if ! "$CSTAR" build "$dir"/*.cstar -o "$exe" >/dev/null 2>"$TMP/$name.err"; then
        echo "FAIL $name (build failed)"; cat "$TMP/$name.err"; fail=$((fail+1)); continue
    fi
    "$exe"; actual=$?

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name (multi-file, exit $actual)"; pass=$((pass+1))
    else
        echo "FAIL $name (got $actual, expected $expected)"; fail=$((fail+1))
    fi
done

# Negative fixtures: tests/xfail/<name>.cstar MUST FAIL to build (a clear compile-time
# rejection — this is how we guard "reject bad code" guarantees like const-correctness,
# access control, and use-after-move). Optional tests/xfail/<name>.msg holds a substring
# the compiler's error output must contain, so we assert the RIGHT error, not any failure.
for src in "$TESTS_DIR"/xfail/*.cstar; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .cstar)"
    err="$TMP/xf_$name.err"
    if "$CSTAR" build "$src" -o "$TMP/xf_$name" >/dev/null 2>"$err"; then
        echo "FAIL xfail/$name (compiled, but must be REJECTED)"; fail=$((fail+1)); continue
    fi
    msg_file="$TESTS_DIR/xfail/$name.msg"
    if [ -f "$msg_file" ] && ! grep -qF "$(cat "$msg_file")" "$err"; then
        echo "FAIL xfail/$name (rejected, but error missing \"$(cat "$msg_file")\")"; head -2 "$err"; fail=$((fail+1)); continue
    fi
    echo "PASS xfail/$name (rejected)"; pass=$((pass+1))
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
