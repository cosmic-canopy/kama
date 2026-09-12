#!/bin/sh
# check-doc-claims.sh — a doc sentence saying the compiler REJECTS something must name the fixture that
# proves it, and that fixture must exist.
#
# The defect this guards. `docs/SPEC.md` said `foreach (char c in s)` "is a type error — the byte/codepoint
# distinction is enforced". It was not enforced: the loop walked UTF-8 bytes and bound each to a `char`,
# yielding mojibake. It survived for months because all 17 `foreach`-over-a-string sites in the tree used
# one of the two CORRECT spellings, so the wrong one was never typed and nothing ever checked the claim.
#
# That is the house rule from AGENTS.md aimed at the docs: a prose claim that something is rejected is
# unguarded without a `tests/xfail/` fixture. `tests/idioms_kama_way.kama` compiles the docs' POSITIVE
# examples, which is why the tour cannot rot — but you cannot put a REJECTED snippet in a fixture that must
# compile, so the negative half had no instrument at all until this one.
#
# It is a LINK check, in two halves, and the second is the one that matters:
#
#   A. every `<!-- xfail: name -->` / `<!-- test: name -->` marker names a fixture that exists. Catches a
#      rename or deletion out from under a claim.
#   B. every line that READS like a claim carries a marker. Half A alone says nothing about a claim nobody
#      ever marked, which is precisely today's failure mode — a new sentence, no fixture, suite still green.
#
# ⚠️ WHY THE PATTERN IS NARROW, and why that means NO OPT-OUT MARKER EXISTS. The nine phrases below can only
# describe a diagnostic, so a fixture can always be written and a false positive never arises. The tempting
# widening — `cannot be` / `may not` / `must not` — matches 55 more lines in these docs of which ~13 are
# prose no fixture can prove ("a module `static` ... cannot be observed", "where kama cannot be certain",
# "a test that cannot be written"). Requiring markers there means ~13 `n/a` waivers, and a waiver is a
# five-second escape hatch indistinguishable at review time from a real exemption. `check-doc-spelling.sh`
# records this repo paying that price once already: its first design needed ~110 opt-out markers and would
# have caught none of the 47 real defects. So the modal claims are marked BY HAND where a fixture exists
# (Half A still checks those), and are deliberately not pattern-required here.
#
# ⚠️ EMPHASIS IS STRIPPED BEFORE MATCHING. This is load-bearing, not tidiness: `is **rejected**` and
# `is a **compile error**` are how the docs actually write half of these, and a raw-text pattern misses
# them. Measured when this guard was written: 11 of 95 claims — including SPEC's own sendability
# paragraph — were visible only after stripping `*` and backticks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
note() { echo "check-doc-claims: FAIL — $1" >&2; fail=1; }

# The NORMATIVE docs — the ones that describe what the language IS. `ROADMAP_DETAIL.md`, `packages.md`,
# `agents.md` and `GOALS.md` are deliberately absent: they are narrative, and their "is an error" sentences
# are historical rationale about a decision, not a rule a fixture can pin.
#
# ⚠️ `agents/AGENTS.md` is here because it is normative IN EFFECT, and the omission cost a package a
# release cycle. It is not a doc about kama — it is the file `kama agents install` writes into every new
# project from inside the binary, and an agent reading it has been told to trust it. It carried three
# claims the compiler had stopped honouring up to thirty versions ago ("there is no const raw pointer",
# reported by the first external package at 0.9.233), and nothing could have caught them: the marker
# discipline that keeps SPEC honest simply did not reach it. Generated guidance that reinstalls itself
# needs MORE of an instrument than a doc a reader consults once, not less.
#
# `agents/PACKAGE.md` stays out on the guard's own rule. Its two claim lines are about the publish and
# manifest TOOLING ("a re-publish of the same version is refused"), which no `tests/xfail/` fixture can
# express — `check-manifest.sh` and `check-packages.sh` are where those live. Admitting it would need
# waivers, and this guard's header records why waivers are refused.
DOCS="docs/SPEC.md docs/KEYWORDS.md docs/TYPE_MODEL.md docs/coming-from-other-languages.md docs/tour.md agents/AGENTS.md"

# Every spelling of "the compiler rejects this". The last two arrived late and are worth their own note:
# the strongest promise in the whole no-heap section — "make every emitter-visible heap allocation a
# **compile error**" — matched NONE of the others, because it says "MAKE … a compile error" rather than
# "IS a compile error". So the one claim in SPEC that a whole roadmap row existed to make true was the one
# claim this guard could not see, and it went unpinned long enough to become false. A claim pattern that
# only recognises one grammatical voice is a claim pattern with a blind spot.
CLAIM='is an error|is a compile error|is a type error|is rejected|does not compile|is refused|is forbidden|is not allowed|is a hard error|a compile error|may not call'

for rel in $DOCS; do
    [ -f "$ROOT/$rel" ] || {
        echo "check-doc-claims: FAIL — $rel is missing; a guard that cannot read its inputs passes everything." >&2
        exit 1
    }
done

# ---- the claim lines ----------------------------------------------------------------------------------
# Fenced blocks are dropped: a claim inside a code comment is a sample's aside, not a specification
# sentence, and there is nowhere sensible to hang a marker on it.
for rel in $DOCS; do
    awk -v rel="$rel" -v pat="$CLAIM" '
        /^[[:space:]]*```/ { inblk = !inblk; next }
        inblk { next }
        {
            bare = $0; gsub(/[*`]/, "", bare)          # see EMPHASIS above
            if (bare ~ pat) print rel ":" NR ":" $0
        }
    ' "$ROOT/$rel"
done > "$tmp/claims"

# ---- self-checks: a guard that reads nothing passes everything -----------------------------------------
if [ ! -s "$tmp/claims" ]; then
    echo "check-doc-claims: FAIL — matched ZERO claims across $DOCS." >&2
    echo "  The pattern or the doc set has drifted; a guard matching nothing cannot fail." >&2
    exit 1
fi
if ! grep -q '^docs/SPEC.md:' "$tmp/claims"; then
    echo "check-doc-claims: FAIL — no claim matched in docs/SPEC.md, which carries the large majority." >&2
    exit 1
fi
nfix=$(find "$ROOT/tests/xfail" -maxdepth 1 \( -name '*.kama' -o -name '*.d' \) | wc -l | tr -d ' ')
if [ "$nfix" -lt 100 ]; then
    echo "check-doc-claims: FAIL — found only $nfix fixtures in tests/xfail; expected hundreds." >&2
    echo "  Every marker would resolve to nothing, so Half A could not fail." >&2
    exit 1
fi

# ---- Half B: a claim line must carry a marker ----------------------------------------------------------
unmarked=$(grep -v '<!--' "$tmp/claims" || true)
if [ -n "$unmarked" ]; then
    n=$(printf '%s\n' "$unmarked" | wc -l | tr -d ' ')
    note "$n doc claim(s) name no fixture:"
    printf '%s\n' "$unmarked" | cut -c1-120 | sed 's/^/    /' >&2
fi

# ---- Half A: every marker must resolve ------------------------------------------------------------------
# One marker may name several fixtures (`<!-- xfail: a, b -->`); a trailing `*` is a glob, matching the
# spelling SPEC already used in prose for the `generic_uninst_*` family.
for rel in $DOCS; do
    grep -on '<!-- *\(xfail\|test\): *[^>]*-->' "$ROOT/$rel" 2>/dev/null \
        | sed "s|^|$rel:|" || true
done > "$tmp/markers"

# ⚠️ SPLIT WITH PARAMETER EXPANSION, NOT SUBPROCESSES. Each marker used to be taken apart by four
# `printf | cut` / `printf | sed` pipelines plus a `printf | tr` per name — about ten processes per
# marker across 334 markers (93 of which name several fixtures), for pure string splitting the shell
# does itself for free. On Linux that is a handful of seconds and nobody noticed. On msys2 `fork` is
# emulated and a spawn costs orders of magnitude more, which made this the SLOWEST GUARD IN THE SUITE
# — 495s of a 533s phase — while compiling absolutely nothing. Since no guard is `check-heavy`, that
# one number WAS the phase: twelve cores sat idle underneath it. Keep this loop process-free.
# (ROADMAP row 16.)
saved_ifs=$IFS
while IFS= read -r m; do
    [ -z "$m" ] && continue
    # `$m` is `<rel>:<lineno>:<!-- kind: a, b -->` — $rel comes from DOCS above and never has a colon.
    rel_m="${m%%:*}"                         # docs/SPEC.md
    rest="${m#*:}"                           # 123:<!-- xfail: a, b -->
    loc="$rel_m:${rest%%:*}"                 # docs/SPEC.md:123
    body="${rest#*:}"                        # <!-- xfail: a, b -->

    kind="${body#*<!--}"                     # " xfail: a, b -->"
    kind="${kind%%:*}"                       # " xfail"
    while :; do case $kind in ' '*) kind="${kind# }" ;; *) break ;; esac; done

    names="${body#*:}"                       # " a, b -->"
    names="${names%%-->*}"                   # " a, b "
    # Comma AND space in IFS, so `a, b` splits the same way `tr ',' ' '` + default splitting did.
    # Left unquoted deliberately: that preserves the previous pathname-expansion behaviour exactly
    # (no marker uses a glob today — see the trailing-`*` note above, which nothing exercises).
    IFS=', '
    for n in $names; do
        IFS=$saved_ifs
        [ -z "$n" ] && continue
        if [ "$kind" = xfail ]; then dir="$ROOT/tests/xfail"; else dir="$ROOT/tests"; fi
        # Each candidate tested on its own: `ls a b c` exits non-zero when ANY one is absent, which would
        # report every marker in the tree as broken (it did, the first time this guard ran).
        if   [ -f "$dir/$n.kama" ]; then :
        elif [ -d "$dir/$n.d" ];    then :
        elif [ "$kind" = test ] && [ -f "$ROOT/tests/trap/$n.kama" ]; then :
        else
            note "$loc names \`$n\`, which is not a fixture (looked for $n.kama / $n.d under ${dir#"$ROOT"/})"
        fi
    done
    IFS=$saved_ifs
done < "$tmp/markers"
IFS=$saved_ifs

[ "$fail" = 0 ] || {
    echo "" >&2
    echo "  A sentence saying the compiler rejects something needs a fixture proving it, linked as" >&2
    echo "  \`<!-- xfail: name -->\` at the END of the claim's own line (inline — a marker alone on a" >&2
    echo "  line splits the paragraph in CommonMark). Use \`<!-- test: name -->\` when a PASSING fixture" >&2
    echo "  proves it instead, as for a runtime rejection (\`tests/parse_errors.kama\`)." >&2
    echo "  There is deliberately no waiver marker: if the sentence is not a claim, reword it." >&2
    exit 1
}

nclaims=$(wc -l < "$tmp/claims" | tr -d ' ')
nmark=$(wc -l < "$tmp/markers" | tr -d ' ')
echo "check-doc-claims: OK ($nclaims claims, $nmark markers, all resolving)"
