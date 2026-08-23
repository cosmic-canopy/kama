#!/bin/sh
# check-diag-file.sh — a diagnostic must name the file that OWNS the declaration it is about.
#
# The defect this guards. `unsupported()` formats every message with `diagFile()`, and `diagFile()` used to
# prefer `_sourcePath` — "the module currently being WRITTEN" — over `_collectingUnitPath`, the unit whose
# declarations a collect pass is walking right now. In a single-file `kama check app.kama`, `_sourcePath`
# is set from the start, so it always won: every diagnostic about an IMPORTED module's declaration was
# reported against `app.kama`, at that declaration's line number.
#
# That is the worst shape a diagnostic can take. It is not vague and it is not empty — it is confidently
# WRONG, pointing a reader (and an editor's go-to-definition, which places by `Diagnostic.file`) at an
# innocent line of a file that has nothing wrong with it. Line right, file wrong. It cost a wrong diagnosis
# during the unsafe-seam campaign, where a rule broken by a PRELUDE declaration was reported against
# whichever fixture happened to pull it in, and it is why that campaign's corpus sweeps had to be driven
# from source analysis rather than from the compiler's own output.
#
# Guarded here rather than as a fixture because the failing operation needs a MULTI-FILE module and an
# assertion about the FILE NAME in the diagnostic — the xfail harness compares a message substring against
# stderr and has no shape for either. Same reasoning as check-self-import.sh.
#
# Five assertions, because there are several ways to be wrong and a fix can trade one for another. The
# first three are about the COLLECT pass, the last two about the EMIT walk underneath it — where the same
# defect survived the original fix, because `_collectingUnitPath` is empty by the time a body is walked:
#   1. A broken declaration in an IMPORTED module names that module's file, not the consumer's.
#   2. A broken declaration in the file being checked still names ITS OWN file — the case that always
#      worked, and the one an over-eager fix would break by letting the collect path win everywhere.
#   3. One mistake in a generic body is one diagnostic, not one per instantiation.
#   4. A mistake in an imported module's function BODY names that module.
#   5. A mistake in an imported GENERIC TEMPLATE's body names the template's file — that body is emitted
#      from the header pass, before any module's path is current, so it has its own record to read.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

if [ ! -x "$KAMA" ]; then echo "check-diag-file: $KAMA not built" >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- 1. the imported-module case (the bug) -----------------------------------------------------------
#
# ⚠️ EVERY SOURCE IS NAMED on the command line here and in cases 4 and 5 below, which is what §2i asks of
# a build with no manifest: the operands are the compilation, and a loose build does not go looking for
# `lib/thing/` on disk. The module is still a genuine IMPORT — a different file, a different module, which
# is the whole subject of this guard — it is simply handed over rather than found.
mkdir -p "$tmp/lib/thing"
cat > "$tmp/lib/thing/broken.kama" <<'EOF'
namespace lib::thing;
export { Leaky };
// This signature names a raw pointer and carries no `unsafe`, so it is rejected. The declaration lives
// HERE, on line 6 of THIS file — which is what the diagnostic has to say.
type value Leaky {
    public fn int32 peek(UnsafePtr<int32> p) { return 0; }
}
EOF
cat > "$tmp/app.kama" <<'EOF'
import lib::thing::{Leaky};
fn int32 main() { Leaky l = Leaky(); return 0; }
EOF

out=$("$KAMA" check "$tmp/app.kama" "$tmp/lib/thing/broken.kama" 2>&1 || true)

if ! printf '%s' "$out" | grep -q 'broken\.kama:6'; then
    echo "check-diag-file: FAIL — a diagnostic about an IMPORTED declaration does not name its own file."
    echo "  expected a mention of broken.kama:6; got:"
    printf '%s\n' "$out" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out" | grep -q 'app\.kama:6'; then
    echo "check-diag-file: FAIL — the declaration in broken.kama is reported against app.kama."
    echo "  line right, file wrong — the exact shape this guard exists for:"
    printf '%s\n' "$out" | sed 's/^/    /' | head -6
    exit 1
fi

# ---- 2. the same-file case (must not regress) ---------------------------------------------------------
cat > "$tmp/solo.kama" <<'EOF'
type value Leaky {
    public fn int32 peek(UnsafePtr<int32> p) { return 0; }
}
fn int32 main() { return 0; }
EOF

out2=$("$KAMA" check "$tmp/solo.kama" 2>&1 || true)
if ! printf '%s' "$out2" | grep -q 'solo\.kama:2'; then
    echo "check-diag-file: FAIL — a diagnostic about a declaration in the file being CHECKED lost its file."
    printf '%s\n' "$out2" | sed 's/^/    /' | head -6
    exit 1
fi

# ---- 3. one mistake, one diagnostic --------------------------------------------------------------------
# A generic type's member body is emitted ONCE PER INSTANTIATION, and `unsupported()` reported on every
# pass — so a single bad line inside `Box<T>` became "FAILED (2 errors)" as soon as a program used both
# `Box<int32>` and `Box<bool>`, with both errors pointing at the same line of the same file. An error
# count that tracks instantiations rather than mistakes teaches a reader to distrust the count.
#
# Here rather than as an xfail for the same reason as the two above: the assertion is about HOW MANY
# diagnostics appear, and the xfail harness only greps stderr for one substring.
cat > "$tmp/generic_dup.kama" <<'EOF'
type value Box<T> {
    public T item;
    public ctor make(T item) { this.item = item; }
    public fn int32 bad() { int32 x = "not an int"; return 0; }
}
fn int32 main() {
    Box<int32> a = Box.make(item: 1);
    Box<bool>  b = Box.make(item: true);
    return a.bad() + b.bad();
}
EOF

out3=$("$KAMA" check "$tmp/generic_dup.kama" 2>&1 || true)
n=$(printf '%s\n' "$out3" | grep -c 'error: a local is declared' || true)
if [ "$n" != 1 ]; then
    echo "check-diag-file: FAIL — one bad initializer in a generic body reported $n times (expected 1)."
    echo "  a diagnostic must count mistakes, not instantiations:"
    printf '%s\n' "$out3" | sed 's/^/    /' | head -6
    exit 1
fi

# ---- 4. the imported-module BODY case (the emit path, one layer under case 1) --------------------------
# Case 1 covers a DECLARATION, caught by the collect pass, where `_collectingUnitPath` names the unit being
# walked. A rule that fires inside a function BODY runs later, in the emit walk, where that variable is
# restored to empty and `diagFile()` fell through to `_sourcePath` — which `analyze()` had set to the ENTRY
# file for every unit at once. So the same "line right, file wrong" shape survived case 1 in the half of
# the compiler where most rules actually live. Measured on the corpus at 0.9.23: 1,174 of 1,848
# `--strict-numeric` rows named a line past the end of the file they named.
mkdir -p "$tmp/lib/body"
cat > "$tmp/lib/body/deep.kama" <<'EOF'
namespace lib::body;
export { Deep };
type value Deep {
    public ctor make() { }
    // The bad initializer is on line 7 of THIS file, inside a BODY — nothing about it is visible to the
    // collect pass, so only the emit walk can report it, and only this file owns it.
    public fn int32 oops() { int32 x = "not an int"; return 0; }
}
EOF
cat > "$tmp/consumer.kama" <<'EOF'
import lib::body::{Deep};
fn int32 main() { Deep d = Deep.make(); return d.oops(); }
EOF

out4=$("$KAMA" check "$tmp/consumer.kama" "$tmp/lib/body/deep.kama" 2>&1 || true)

if ! printf '%s' "$out4" | grep -q 'deep\.kama:7'; then
    echo "check-diag-file: FAIL — a diagnostic about an imported module's BODY does not name its own file."
    echo "  expected a mention of deep.kama:7; got:"
    printf '%s\n' "$out4" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out4" | grep -q 'consumer\.kama:'; then
    echo "check-diag-file: FAIL — a body diagnostic in deep.kama is reported against consumer.kama."
    echo "  line right, file wrong, one layer under case 1 — the emit path:"
    printf '%s\n' "$out4" | sed 's/^/    /' | head -6
    exit 1
fi

# ---- 5. the imported GENERIC TEMPLATE's body (emitted from the header pass) -----------------------------
# A generic instance's body is emitted BEFORE the per-module loop runs, so there is no "module currently
# being written" to fall back to at all — `_sourcePath` there is whatever the emitter was constructed with.
# The template's own file is recorded for this (`ClassInfo::declFile` for a type, `_genericDeclFile` for a
# function) and `diagFile()` reads it through `_emitDeclFile`. Without that, every user program that
# instantiates `Shared<T>` gets the prelude's line numbers stamped with the user's filename.
mkdir -p "$tmp/lib/tmpl"
cat > "$tmp/lib/tmpl/holder.kama" <<'EOF'
namespace lib::tmpl;
export { Holder };
type value Holder<T> {
    public T item;
    public ctor make(T item) { this.item = item; }
    // line 7, in a TEMPLATE body — re-walked once per instantiation, from the header pass
    public fn int32 oops() { int32 x = "not an int"; return 0; }
}
EOF
cat > "$tmp/instantiator.kama" <<'EOF'
import lib::tmpl::{Holder};
fn int32 main() { Holder<int32> h = Holder.make(item: 1); return h.oops(); }
EOF

out5=$("$KAMA" check "$tmp/instantiator.kama" "$tmp/lib/tmpl/holder.kama" 2>&1 || true)

if ! printf '%s' "$out5" | grep -q 'holder\.kama:7'; then
    echo "check-diag-file: FAIL — a diagnostic in an imported GENERIC body does not name the template's file."
    echo "  expected a mention of holder.kama:7; got:"
    printf '%s\n' "$out5" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out5" | grep -q 'instantiator\.kama:'; then
    echo "check-diag-file: FAIL — a template's mistake is reported against the file that instantiated it."
    printf '%s\n' "$out5" | sed 's/^/    /' | head -6
    exit 1
fi

echo "check-diag-file: PASS (a diagnostic names the file that owns the declaration, imported or local,"
echo "                       from the collect pass, a body, or a generic template's body; and one mistake"
echo "                       in a generic body is reported once, not once per instantiation)"
