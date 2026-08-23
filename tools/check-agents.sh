#!/bin/sh
# check-agents.sh — the AI/agent surface guard.
#
# This deliverable's whole claim is "verified answers, not plausible ones", so it would be
# embarrassing for its own documentation to drift. Four things are asserted:
#
#   1. DISCOVERABILITY — every subcommand usage() advertises actually dispatches, and every `kama
#      query` mode docs/agents.md names actually runs. A doc that names a flag the binary dropped is
#      exactly the failure this campaign exists to prevent.
#   2. DRY — the binary's embedded AGENTS.md is byte-identical to agents/AGENTS.md, and every
#      per-tool file is a POINTER rather than a copy. The "content lives once" property is the design;
#      asserting it mechanically is what keeps it true when someone adds a tool in a hurry.
#   3. INSTALL — `kama agents install` produces the advertised tree, refuses to clobber, and never
#      half-writes on a bad argument.
#   4. THE CAVEAT — `kama check` still passes an expression type error. usage(), docs/agents.md and
#      AGENTS.md all say so; if the front end ever gains real type checking this fails, and the fix
#      is to DELETE the caveat from all three rather than weaken the assertion.
#
# Run standalone or from `./dev check` (which glob-enrolls every tools/check-*.sh).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-agents: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()   { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1" >&2; fail=1; }

# ---------------------------------------------------------------------------------------------------
echo "check-agents: discoverability"

# `kama` with no args prints usage on stderr and exits 2.
"$KAMA" > "$tmp/usage.txt" 2>&1 || true
for verb in transpile build run check query lsp seed agents pkg publish toolchain update; do
    if grep -q "kama $verb" "$tmp/usage.txt"; then ok "usage() advertises \`$verb\`"
    else bad "usage() never mentions \`$verb\`"; fi
done

# Every mode docs/agents.md documents must actually be accepted. The list is read FROM THE DOC, so a
# mode added to the page without being added to the compiler fails here.
FIX="$ROOT/tests/query/shapes.kama"
DOC="$ROOT/docs/agents.md"
[ -f "$DOC" ] || { echo "check-agents: missing $DOC" >&2; exit 1; }
modes=$(grep -oE '`?--(symbols|search|def|type|refs|complete|sighelp|coverage|diagnostics|project|json)' "$DOC" \
        | tr -d '`' | sort -u)
for m in $modes; do
    case "$m" in
        --search)   args="--search Point" ;;
        --def|--type|--refs) args="$m 6:11" ;;
        --complete) args="--complete 24:5" ;;
        --sighelp)  args="--sighelp 18:26" ;;
        --project|--json) continue ;;    # modifiers, exercised below
        *)          args="$m" ;;
    esac
    # shellcheck disable=SC2086
    if "$KAMA" query "$FIX" $args >/dev/null 2>&1; then ok "docs/agents.md: \`$m\` runs"
    else bad "docs/agents.md documents \`$m\`, but \`kama query $args\` failed"; fi
done
"$KAMA" query "$FIX" --symbols --json >"$tmp/j.txt" 2>/dev/null || true
grep -q '"schema":1' "$tmp/j.txt" && ok "--json carries a schema version" || bad "--json lost its schema"

# ---------------------------------------------------------------------------------------------------
echo "check-agents: DRY — one copy of the content, everything else a pointer"

"$KAMA" agents print > "$tmp/embedded.md" 2>/dev/null || true
if diff -q "$tmp/embedded.md" "$ROOT/agents/AGENTS.md" >/dev/null 2>&1; then
    ok "the embedded AGENTS.md is byte-identical to agents/AGENTS.md"
else
    bad "the binary's AGENTS.md differs from agents/AGENTS.md — rebuild, or reconcile:"
    diff -u "$ROOT/agents/AGENTS.md" "$tmp/embedded.md" | head -20 >&2
fi
"$KAMA" agents print --skill > "$tmp/skill.md" 2>/dev/null || true
diff -q "$tmp/skill.md" "$ROOT/agents/skill/SKILL.md" >/dev/null 2>&1 \
    && ok "the embedded SKILL.md is byte-identical to agents/skill/SKILL.md" \
    || bad "the binary's SKILL.md differs from agents/skill/SKILL.md"

# THE DRY INVARIANT. A per-tool file must POINT at AGENTS.md, never restate it. Five lines is a
# generous ceiling for "@AGENTS.md" or one sentence; anything longer is guidance text getting copied.
for stub in "$ROOT"/agents/stubs/*.md; do
    name=$(basename "$stub")
    body=$(sed '1d' "$stub")                       # line 1 is the `<!-- dest: … -->` declaration
    lines=$(printf '%s\n' "$body" | grep -c '' || true)
    if [ "$lines" -gt 5 ]; then
        bad "$name is $lines lines — a stub must POINT at AGENTS.md, not duplicate it"
    elif printf '%s' "$body" | grep -q 'AGENTS.md'; then
        ok "$name is a $lines-line pointer to AGENTS.md"
    else
        bad "$name never mentions AGENTS.md, so it is not a pointer"
    fi
    # And every stub must declare where it goes, or embed_agents.sh could not place it.
    head -1 "$stub" | grep -qE '^<!-- dest: .+ -->$' \
        || bad "$name line 1 must be '<!-- dest: <path> -->'"
done

# The always-on file is paid for on every agent turn, so it has a budget. 200 lines is the documented
# ceiling for an instruction file; this stays well inside it deliberately.
alines=$(grep -c '' "$ROOT/agents/AGENTS.md")
[ "$alines" -le 200 ] && ok "agents/AGENTS.md is $alines lines (<= 200, the always-on budget)" \
                      || bad "agents/AGENTS.md is $alines lines — too long for a file loaded every turn"

# ---------------------------------------------------------------------------------------------------
echo "check-agents: install"

# `agents install` names the project it writes into — the operand rule reaches every command that acts
# ON a project, not just the ones that build. So each fixture here is a real project.
mkproj() { mkdir -p "$1"; printf '{ "name": "%s", "version": "0.1.0", "kind": "library" }\n' "$2" > "$1/kama.json"; }
proj="$tmp/proj"; mkproj "$proj" agentsproj
"$KAMA" agents install "$proj/kama.json" --all-tools --skill >/dev/null 2>&1 \
    || bad "\`agents install --all-tools --skill\` failed"
[ -f "$proj/AGENTS.md" ] && ok "install writes AGENTS.md" || bad "install did not write AGENTS.md"
# The Claude pointer is a real import, which is the documented way to avoid duplicating the content.
if [ "$(cat "$proj/CLAUDE.md" 2>/dev/null)" = "@AGENTS.md" ]; then
    ok "CLAUDE.md is the one-line @AGENTS.md import"
else
    bad "CLAUDE.md should be exactly '@AGENTS.md', got: $(cat "$proj/CLAUDE.md" 2>/dev/null)"
fi
# Every declared destination must exist after --all-tools.
for stub in "$ROOT"/agents/stubs/*.md; do
    dest=$(sed -n '1s/^<!-- *dest: *\(.*[^ ]\) *-->$/\1/p' "$stub")
    [ -f "$proj/$dest" ] && ok "install wrote $dest" || bad "install skipped $dest"
done
[ -f "$proj/.claude/skills/kama/SKILL.md" ] && ok "install --skill writes the skill" \
                                            || bad "install --skill wrote no skill"

# Writing into somebody's repository must not clobber.
if "$KAMA" agents install "$proj/kama.json" >/dev/null 2>&1; then
    bad "a second install overwrote existing files without --force"
else
    ok "install refuses to overwrite without --force"
fi
"$KAMA" agents install "$proj/kama.json" --force >/dev/null 2>&1 && ok "--force overwrites" || bad "--force failed"

# A bad argument must write NOTHING — a typo used to leave a half-installed tree behind an exit 2.
fresh="$tmp/fresh"; mkproj "$fresh" freshproj
"$KAMA" agents install "$fresh/kama.json" --tool nosuchtool >/dev/null 2>&1 || true
[ -e "$fresh/AGENTS.md" ] && bad "an unknown --tool still wrote AGENTS.md" \
                          || ok "an unknown --tool writes nothing at all"

# Neither may a COLLISION write anything. The name check above has always been up front, but the file
# check used to happen per file AS IT WROTE: someone who already had a CLAUDE.md got a brand-new
# AGENTS.md dropped in their repo and then an exit 1 — a half-install with no bad argument in sight.
coll="$tmp/collide"; mkproj "$coll" collide
printf 'mine\n' > "$coll/CLAUDE.md"
"$KAMA" agents install "$coll/kama.json" --claude >/dev/null 2>&1 || true
[ -f "$coll/AGENTS.md" ] && bad "a colliding install still wrote AGENTS.md" \
                         || ok "a colliding install writes nothing at all"
[ "$(cat "$coll/CLAUDE.md")" = "mine" ] || bad "a refused install modified the existing file"

"$KAMA" agents list >/dev/null 2>&1 && ok "\`agents list\` runs" || bad "\`agents list\` failed"

# ---------------------------------------------------------------------------------------------------
echo "check-agents: the documented \`check\` boundary"

# usage(), docs/agents.md and agents/AGENTS.md all tell an agent exactly how far `kama check`'s type
# checking reaches. Pin BOTH ends of that boundary, so neither half can quietly become a lie.
#
# Both of these assertions used to read the other way, and BOTH have now fired as designed. The first
# asserted `check` did not type-check at all, and failed the day the kind rule landed. The second
# asserted a WIDTH mismatch was still missed, and failed the day strict numeric conversion landed. Each
# failure forced these docs to be corrected instead of rotting, which is the entire point of the pair —
# so when the next rule extends this boundary, correct the docs and then the assertion, in that order,
# and never by weakening it.
bad_kama="$tmp/typebad.kama"
printf 'fn int32 main() {\n    int32 x = "oops";\n    return 0;\n}\n' > "$bad_kama"
if "$KAMA" check "$bad_kama" >/dev/null 2>&1; then
    bad "\`check\` no longer catches a KIND mismatch — the docs promise it does"
else
    ok "\`check\` catches a kind mismatch (\`int32 x = \"oops\"\`)"
fi
# ...and the far end: WIDTH is checked too — kama has no implicit numeric conversion, so `int8 a = big`
# is an error and wants `cast<int8>(big)`.
width_kama="$tmp/typewidth.kama"
printf 'fn int32 main() {\n    int32 big = 300;\n    int8 a = big;\n    return 0;\n}\n' > "$width_kama"
if "$KAMA" check "$width_kama" >/dev/null 2>&1; then
    bad "\`check\` no longer catches a WIDTH mismatch — the docs promise no implicit numeric conversion"
else
    ok "\`check\` catches a width mismatch (\`int8 a = big\`)"
fi
# The OTHER end of the width boundary, and the one a blunt fix would break: a LITERAL is typed by its
# destination, so it is not a conversion and must still compile. Without this, "reject a width mismatch"
# is satisfiable by rejecting `int8 a = 100;` too, which would make the language unusable and this guard
# green. Both halves, same reasoning as the kind pair above.
lit_kama="$tmp/typelit.kama"
printf 'fn int32 main() {\n    int8 a = 100;\n    float32 f = 3;\n    int8 b = 2 + 3;\n    return 0;\n}\n' > "$lit_kama"
if "$KAMA" check "$lit_kama" >/dev/null 2>&1; then
    ok "\`check\` still accepts a literal typed by its destination (\`int8 a = 100\`)"
else
    bad "\`check\` now rejects a literal at its destination's type — the width rule must exempt
        contextually-typed literals; see docs/agents.md's table of what is NOT a conversion"
fi
# ...and `build` must still catch it, or the advice to use `build` is wrong too.
if "$KAMA" build "$bad_kama" -o "$tmp/typebad.out" >/dev/null 2>&1; then
    bad "\`build\` no longer catches the type error either — the advice in AGENTS.md is now wrong"
else
    ok "\`build\` catches it, so \"verify with kama build\" is sound advice"
fi
# Every doc that gives the advice must actually say `kama build`.
for f in "$ROOT/docs/agents.md" "$ROOT/agents/AGENTS.md"; do
    grep -q 'kama build' "$f" && ok "$(basename "$f") points at \`kama build\`" \
                              || bad "$(basename "$f") never mentions \`kama build\`"
done

if [ "$fail" != 0 ]; then echo "check-agents: FAILED" >&2; exit 1; fi
echo "check-agents: OK"
