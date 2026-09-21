#!/bin/sh
# check-doc-snippets.sh — every ```kama block in a published doc must pass `kama check`.
#
# The defect this guards. `docs/tour.md` promised "every snippet is real code that compiles today", and
# the home page that every snippet is "taken from a compiled fixture". Neither was checked. By `0.9.417`
# eight of the tour's snippets no longer compiled — the constructor rule moved to `this.f = …`, contract
# methods became `const fn`, `parallel_for` grew a mandatory `workers:` — and SPEC carried dozens more, so
# a reader copying the reference hit a diagnostic the reference itself had stopped matching. The only
# positive-snippet instrument was `tests/idioms_kama_way.kama`, a hand copy of ONE page's examples; a
# hand copy guards the copy, not the page.
#
# So this guard reads the page. Its doc set is DERIVED: every `src:` in tools/site/pages.mjs (what the
# website publishes) plus the pages a reader or an agent meets outside it. A new page on the site is
# guarded with no list to update.
#
# THE ONE OPT-OUT IS IN THE DOC, AND IT IS VISIBLE TO THE READER'S EDITOR. A block that is deliberately
# partial — a signature excerpt, a statement shown out of its function — is fenced ```kama fragment.
# The site (`tools/site/render.mjs`) and GitHub both read only the first word of the info string, so it
# renders exactly like ```kama; nothing about it is hidden. Prefer making a block whole (add the missing
# declaration or a `fn int32 main()`) over marking it: a fragment is a snippet nothing checks.
# Blocks meant to be REJECTED are not this guard's business — check-doc-claims.sh pins those to a
# `tests/xfail/` fixture, and they belong in a fragment fence here.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The site's pages, derived, plus the three read outside it: the repo front page, the migration page
# (linked from the tour, not in the sidebar), and the AGENTS.md the binary installs into every project.
SITE=$(sed -n "s/.*src: *'\([^']*\.md\)'.*/\1/p" "$ROOT/tools/site/pages.mjs")
DOCS="$SITE README.md docs/coming-from-other-languages.md agents/AGENTS.md"

nsite=$(printf '%s\n' $SITE | wc -l | tr -d ' ')
if [ "$nsite" -lt 8 ]; then
    echo "check-doc-snippets: FAIL — read only $nsite page(s) out of tools/site/pages.mjs; the manifest's shape drifted." >&2
    exit 1
fi

# ---- extract: one file per ```kama block, named <n>.kama, with a map line "<n> <doc>:<fence line>" --------
# A fence may be indented (inside a list item); its body is de-indented by the fence's own indent.
n=0
for rel in $DOCS; do
    [ -f "$ROOT/$rel" ] || { echo "check-doc-snippets: FAIL — $rel is missing." >&2; exit 1; }
    awk -v rel="$rel" -v dir="$tmp" -v start="$n" '
        BEGIN { k = start }
        !inblk && /^[[:space:]]*```/ {
            line = $0; ind = match(line, /```/) - 1
            info = substr(line, ind + 4); sub(/[[:space:]]+$/, "", info)
            inblk = 1; want = (info == "kama")
            if (info == "kama" || info ~ /^kama[[:space:]]+fragment$/) total++
            if (want) { k++; f = dir "/" k ".kama"; printf "" > f; print k " " rel ":" NR >> (dir "/map") }
            next
        }
        inblk && /^[[:space:]]*```[[:space:]]*$/ { if (want) close(f); inblk = 0; want = 0; next }
        inblk && want { l = $0; if (substr(l, 1, ind) ~ /^[[:space:]]*$/) l = substr(l, ind + 1); print l > f }
        END { print k > (dir "/count"); print total + 0 >> (dir "/fences") }
    ' "$ROOT/$rel"
    n=$(cat "$tmp/count")
done

# A guard that reads nothing passes everything. Floor the FENCES, fragments included — a floor on the
# checked blocks alone would fail every time a block is honestly reclassified as a fragment.
fences=$(awk '{ t += $1 } END { print t + 0 }' "$tmp/fences")
if [ "$fences" -lt 150 ] || [ "$n" -lt 75 ]; then
    echo "check-doc-snippets: FAIL — found only $fences kama fences ($n checked); the docs carry far more." >&2
    exit 1
fi

# ---- check each -------------------------------------------------------------------------------------------
fail=0
while read -r k where; do
    if ! "$KAMA" check "$tmp/$k.kama" > "$tmp/$k.out" 2>&1; then
        [ "$fail" -eq 0 ] && echo "check-doc-snippets: FAIL — these \`\`\`kama blocks do not pass \`kama check\`" \
            "(make the block whole, or fence it \`\`\`kama fragment if it is deliberately partial):" >&2
        fail=$((fail + 1))
        echo "  $where" >&2
        grep -v '^kama: ' "$tmp/$k.out" | sed "s|$tmp/$k.kama|    block|" | head -3 >&2
    fi
done < "$tmp/map"

if [ "$fail" -ne 0 ]; then
    echo "check-doc-snippets: $fail of $n blocks failed." >&2
    exit 1
fi
echo "check-doc-snippets: OK ($n blocks checked, $((fences - n)) fragments)"
