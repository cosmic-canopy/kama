#!/bin/sh
# check-ctor-names.sh — no public constructor in std or the prelude takes a base name SPEC's naming rule refuses.
#
# SPEC *Construction* (KR-367): a ctor's base name says how the value is made, from a short vocabulary — `make`, the
# memberwise `of`, `empty` and `with<Setting>`, `from<Input>`, `parse(text:)`, an OS verb, a noun naming the value, a
# contract's own name. std is the example every program copies, and the triage that set the rule found `create`,
# `of<Input>` and `fromStr` beside names that meant the same thing. The nouns are open-ended, so this cannot prove a
# name right; it refuses the names the rule rules out: `new`, `create`, `init`, `of<Input>` and `fromStr…`.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# A ctor may spell its result first (`ctor Result<T, E> create(`), so the name is the last word before `(`.
bad=$(grep -rnE '^[[:space:]]*public[[:space:]]+([a-z]+[[:space:]]+)*ctor[[:space:]]+([^(]*[[:space:]])?(new|create|init|of[A-Z][A-Za-z0-9_]*|fromStr[A-Za-z0-9_]*)[[:space:]]*\(' \
      "$ROOT/lib/std" "$ROOT/prelude" --include='*.kama' || true)
if [ -n "$bad" ]; then
    echo "check-ctor-names: FAIL — a public constructor takes a name SPEC's Construction rule refuses:" >&2
    echo "$bad" | sed "s#^$ROOT/##" >&2
    exit 1
fi
echo "check-ctor-names: OK (no public ctor in std or the prelude is named new, create, init, of<Input> or fromStr)"
