#!/bin/sh
# check-shell-posix.sh — every script that CLAIMS `#!/bin/sh` must actually parse as POSIX sh.
#
# WHY. `sh` is bash on macOS and dash on Debian/Ubuntu, so a bashism in a `#!/bin/sh` script runs green
# on the host and dies on Linux — which is CI, and the container legs. `./dev test linux` exists to catch
# exactly that, and the `dev` header names it as the trap that once put a bashism in a guard. But that leg
# is a container round trip nobody pays per commit, and a guard's own failure is easy to read as a real
# finding, so the class survives for a long time when it does appear.
#
# It did. `tools/check-binding-widen.sh` carried this line from 2026-08-18 until the commit above:
#
#     echo "... the fixture's narrowing `cast<int32>(s)` lost its runtime check;" >&2
#
# The backticks are PROSE — the same markdown quoting every diagnostic in this tree uses — but inside a
# double-quoted string they are command substitution, so the shell tries to run `cast<int32>(s)` and the
# `(` is a hard syntax error. dash refuses to parse the FILE; the guard was not failing on Linux, it was
# never running, and it reported as one failed guard for eleven days. Every other guard escapes the
# backtick (`\``) in that position; this one forgot, and nothing could tell.
#
# WHAT THIS CATCHES, precisely: parse errors, which is the class above. It is `sh -n`, so it does NOT
# catch a bashism that PARSES under dash and misbehaves at runtime (`local`, `echo -e`, `${x,,}`, a
# `[ a == b ]`). Those want the Linux leg. Do not read a pass here as "POSIX-clean" — read it as "dash can
# at least parse it", which is the specific failure that hid an entire guard.
#
# The enrolment rule is the file's OWN shebang: a script declaring `#!/bin/bash` is exempt by declaration
# (run_tests.sh is bash on purpose — arrays, and it must stay bash-3.2-compatible for macOS). So nothing
# here is a list, and a new script joins or exempts itself by the line it already has to write.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# A strict POSIX shell, which `sh` is NOT on macOS. dash ships on Debian (as /bin/sh) and is present on
# this repo's macOS hosts via homebrew; if neither, say so and SKIP. A guard that cannot reach its tool
# must not report a pass — that is how nine guards once blamed the repository for a missing `diff`.
POSIX_SH=""
for c in dash /bin/dash /opt/homebrew/bin/dash /usr/local/bin/dash; do
    if command -v "$c" >/dev/null 2>&1; then POSIX_SH="$c"; break; fi
done
if [ -z "$POSIX_SH" ]; then
    echo "SKIP check-shell-posix (no dash found; \`sh\` is bash here and would parse bashisms happily)"
    echo "  install it with \`brew install dash\` — Debian/Ubuntu, including the container, already has it"
    exit 0
fi

checked=0
bad=0
for f in dev run_tests.sh tools/*.sh tools/cdev tools/kama-bin.sh; do
    [ -f "$f" ] || continue
    # The shebang is the enrolment. `#!/bin/sh` opts in; anything else opts out by declaration.
    case "$(head -1 "$f")" in
        '#!/bin/sh'|'#!/usr/bin/env sh') ;;
        *) continue ;;
    esac
    checked=$((checked + 1))
    if ! err=$("$POSIX_SH" -n "$f" 2>&1); then
        bad=$((bad + 1))
        echo "check-shell-posix: FAIL — $f declares #!/bin/sh but does not parse as POSIX sh:" >&2
        printf '  %s\n' "$err" >&2
        # The overwhelmingly common cause, and the one that is invisible on a host where sh is bash.
        if printf '%s' "$err" | grep -q 'Syntax error'; then
            echo "  Most often an UNESCAPED BACKTICK used as prose inside a double-quoted string:" >&2
            echo "      echo \"... \`foo(x)\` ...\"   ->   echo \"... \\\`foo(x)\\\` ...\"" >&2
            echo "  Backticks are command substitution there, so the quoted text is run as a command." >&2
        fi
    fi
done

if [ "$bad" -ne 0 ]; then
    echo "check-shell-posix: $bad of $checked #!/bin/sh script(s) do not parse under $POSIX_SH" >&2
    exit 1
fi
echo "PASS shell-posix ($checked #!/bin/sh scripts parse under $POSIX_SH; bashisms that still PARSE need the Linux leg)"
