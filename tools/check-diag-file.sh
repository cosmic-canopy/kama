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
export { Leaky };
// This signature names a raw pointer and carries no `unsafe`, so it is rejected. The declaration lives
// HERE, on line 5 of THIS file — which is what the diagnostic has to say.
type value Leaky {
    public fn int32 peek(UnsafePtr<int32> p) { return 0; }
}
EOF
cat > "$tmp/app.kama" <<'EOF'
import { lib::thing::Leaky };
fn int32 main() { Leaky l = Leaky(); return 0; }
EOF

out=$("$KAMA" check "$tmp/app.kama" "$tmp/lib/thing/broken.kama" 2>&1 || true)

if ! printf '%s' "$out" | grep -q 'broken\.kama:5'; then
    echo "check-diag-file: FAIL — a diagnostic about an IMPORTED declaration does not name its own file."
    echo "  expected a mention of broken.kama:5; got:"
    printf '%s\n' "$out" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out" | grep -q 'app\.kama:5'; then
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
export { Deep };
type value Deep {
    public ctor make() { }
    // The bad initializer is on line 6 of THIS file, inside a BODY — nothing about it is visible to the
    // collect pass, so only the emit walk can report it, and only this file owns it.
    public fn int32 oops() { int32 x = "not an int"; return 0; }
}
EOF
cat > "$tmp/consumer.kama" <<'EOF'
import { lib::body::Deep };
fn int32 main() { Deep d = Deep.make(); return d.oops(); }
EOF

out4=$("$KAMA" check "$tmp/consumer.kama" "$tmp/lib/body/deep.kama" 2>&1 || true)

if ! printf '%s' "$out4" | grep -q 'deep\.kama:6'; then
    echo "check-diag-file: FAIL — a diagnostic about an imported module's BODY does not name its own file."
    echo "  expected a mention of deep.kama:6; got:"
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
export { Holder };
type value Holder<T> {
    public T item;
    public ctor make(T item) { this.item = item; }
    // line 6, in a TEMPLATE body — re-walked once per instantiation, from the header pass
    public fn int32 oops() { int32 x = "not an int"; return 0; }
}
EOF
cat > "$tmp/instantiator.kama" <<'EOF'
import { lib::tmpl::Holder };
fn int32 main() { Holder<int32> h = Holder.make(item: 1); return h.oops(); }
EOF

out5=$("$KAMA" check "$tmp/instantiator.kama" "$tmp/lib/tmpl/holder.kama" 2>&1 || true)

if ! printf '%s' "$out5" | grep -q 'holder\.kama:6'; then
    echo "check-diag-file: FAIL — a diagnostic in an imported GENERIC body does not name the template's file."
    echo "  expected a mention of holder.kama:6; got:"
    printf '%s\n' "$out5" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out5" | grep -q 'instantiator\.kama:'; then
    echo "check-diag-file: FAIL — a template's mistake is reported against the file that instantiated it."
    printf '%s\n' "$out5" | sed 's/^/    /' | head -6
    exit 1
fi

# ---- 6. a module that does not resolve says which RULE refused it ---------------------------------------
#
# Not a file-attribution case like the five above, but the same defect class: the message used to end in
# "(from <dir>)" — the directory the SEARCH had started from. §2i deleted the search, so that named a
# place nothing had looked, and the three ways to arrive here want three different answers. Each is
# asserted on the sentence a user actually has to act on, because a diagnostic nobody checks drifts.
want() {   # want <output> <substring> <claim>
    printf '%s' "$1" | grep -qF -- "$2" \
        && echo "  ok: $3" \
        || { echo "check-diag-file: FAIL — $3" >&2
             echo "    expected: $2" >&2
             printf '%s\n' "$1" | sed 's/^/    /' | head -4 >&2; exit 1; }
}

# 6a. Loose, with no project anywhere above: the operands ARE the compilation, and this one is not.
mkdir -p "$tmp/loose"
printf 'import { nowhere::v };\nfn int32 main() { return v(); }\n' > "$tmp/loose/solo.kama"
out6=$("$KAMA" check "$tmp/loose/solo.kama" 2>&1 || true)
want "$out6" "cannot resolve module 'nowhere'" "an unresolved module names itself"
want "$out6" "modules are the files on the command line" "...and says a loose build compiles only what it was given"

# 6b. Loose, but the file belongs to a project — the case §2i is most likely to surprise someone with,
# since the very same file builds under its manifest. The fix IS the manifest, so the note names it.
mkdir -p "$tmp/proj6/src/mod"
printf '{ "name": "p6", "version": "0.1.0", "kind": "library",\n  "modules": { ".": { "visibility": "internal" }, "mod": { "visibility": "public" } } }\n' \
    > "$tmp/proj6/kama.json"
printf 'export { v };\nfn int32 v() { return 1; }\n' > "$tmp/proj6/src/mod/m.kama"
printf 'import { p6::mod::v };\nfn int32 use() { return v(); }\n' > "$tmp/proj6/src/consumer.kama"
out6b=$("$KAMA" check "$tmp/proj6/src/consumer.kama" 2>&1 || true)
# ⚠️ Matched on the tail, not on "$tmp/...": absolutePath is realpath(), so a mktemp path comes back as
# /private/var/... on macOS while $tmp says /var/... — the same spelling hazard M3.5 hit in check-lsp.
want "$out6b" 'proj6/kama.json — `kama build' "...or, when the file sits in a project, names its manifest"
"$KAMA" check "$tmp/proj6/kama.json" >/dev/null 2>&1 \
    && echo "  ok: ...which is not idle advice — that manifest does build it" \
    || { echo "check-diag-file: FAIL — the manifest the note recommends does not build the file" >&2; exit 1; }

# 6c/6d. In a project, the two lists that could be missing an entry, told apart by the first segment.
printf 'import { p6::unlisted::v };\nfn int32 use2() { return v(); }\n' > "$tmp/proj6/src/consumer.kama"
mkdir -p "$tmp/proj6/src/unlisted"
printf 'export { v };\nfn int32 v() { return 1; }\n' > "$tmp/proj6/src/unlisted/u.kama"
out6c=$("$KAMA" check "$tmp/proj6/kama.json" 2>&1 || true)
want "$out6c" 'lists no module `p6::unlisted`' "a folder in this project that nobody listed says so"
printf 'import { geo::v };\nfn int32 use3() { return v(); }\n' > "$tmp/proj6/src/consumer.kama"
out6d=$("$KAMA" check "$tmp/proj6/kama.json" 2>&1 || true)
want "$out6d" 'declares no dependency named `geo`' "...and a name that is not this project is a missing dependency"

# ---- 7. a SAME-MODULE import tells its three causes apart --------------------------------------------
#
# `import { X };` is checked by ONE negative set lookup — is `X` in this module's export set — and three
# unrelated mistakes fail it. One wording served all three, and for the loose-ROOT case it was FLATLY
# FALSE: it said no file exports `X` while the sibling's `export { X };` sat right there, and moving either
# file one directory down made the identical import succeed. Same "blame a rule that did not fire" shape
# the six cases above exist for.
#
# Two of the three now have xfail fixtures (import_loose_root, import_module_private, import_name_unloaded).
# The third CANNOT be one: `tests/xfail/<name>.d/` passes every `.kama` it contains, so a fixture is unable
# to express "this file exists on disk and the build was not given it" — which is exactly the mistake §2i
# made common, since a loose build compiles its operands and does not go looking. It needs a build the
# fixture harness cannot spell, so it lives here, like cases 1-6.
mkdir -p "$tmp/m7/geo"
printf 'export { v };\nfn int32 v() { return 1; }\n' > "$tmp/m7/geo/a.kama"
printf 'import { v };\nfn int32 use() { return v(); }\n'  > "$tmp/m7/geo/b.kama"
printf 'fn int32 main() { return 0; }\n' > "$tmp/m7/main.kama"

# 7a. `a.kama` is deliberately NOT an operand. It is on disk, it declares `v`, it exports it — and the
#     build never read it, so an `export` list is the wrong thing to send the reader off to edit.
out7=$("$KAMA" check "$tmp/m7/geo/b.kama" "$tmp/m7/main.kama" 2>&1 || true)
want "$out7" 'was loaded — no file of this module declares it' \
     "a sibling that was never passed says the name was never LOADED"
want "$out7" 'a loose build compiles only the files it is given' \
     "...and names the rule that actually refused it"

# 7b. THE CONTROL, and the reason 7a is not just a reworded lie: hand the SAME build that one extra file
#     and it compiles. Nothing about the export list changed between these two runs.
"$KAMA" check "$tmp/m7/geo/a.kama" "$tmp/m7/geo/b.kama" "$tmp/m7/main.kama" >/dev/null 2>&1 \
    && echo "  ok: ...which is not idle advice — passing that file does build it" \
    || { echo "check-diag-file: FAIL — the file 7a says to pass does not fix the build" >&2; exit 1; }

# ---- 8. a FAILED GENERIC BOUND is the use site's mistake, in the use site's file ------------------------
#
# Case 1 INVERTED, and the one direction that file did not cover. There the broken declaration lived in the
# library and was reported against the consumer; here the mistake is the CONSUMER's — a type argument that
# does not satisfy the template's bound — and it was reported against the LIBRARY. Same "line right, file
# wrong" shape, opposite direction, and this guard passed the whole time it was true.
#
# The cause was that `registerGenericTypeInst` swapped `_nsCtx` to the TEMPLATE's home context before
# calling `checkBounds` — a swap `checkBounds` already performs internally, narrowly, around the contract
# NAME lookup alone (BoundCtxScope). The wider one also moved the file `checkBounds` attributes its
# diagnostic to, so `Map<NotHashable, int32>` in a four-line user program reported four errors, not one of
# them in a file the user wrote:
#
#   lib/std/collections/map.kama:4:0: error: … does not satisfy bound `Hashable`     <- the USER's line 4
#   lib/std/collections/map.kama:4:0: error: … does not satisfy bound `Equatable`    <- map.kama:4 is a comment
#   lib/std/collections/map.kama:189:0: error: `NotHashable` has no method `equals`  <- a CONSEQUENCE
#   lib/std/collections/map.kama:188:0: error: `NotHashable` has no method `hash`    <- a CONSEQUENCE
#
# The generic FUNCTION path never had the swap and always named the right file, which is what identified it.
#
# Two assertions, because the two halves fail independently and a fix for one can leave the other:
#   8a. the bound diagnostic names the USER's file, not the template's.
#   8b. a failed bound is reported ONCE — the instantiation is abandoned rather than walked, so the
#       template's body cannot contribute follow-on errors about the argument it was already refused.
#       This is what Rust, Swift and C++20 concepts all do; the pre-concepts C++ alternative is above.
mkdir -p "$tmp/lib/keyed"
cat > "$tmp/lib/keyed/keyed.kama" <<'EOF'
export { Keyed };
// The BOUND is this file's. The bad ARGUMENT is not — and the mistake belongs to whoever wrote it.
type value Keyed<K: Hashable> {
    public K key;
    public ctor make(K key) { this.key = key; }
    public fn uint64 digest() { return this.key.hash(); }
}
EOF
cat > "$tmp/bound_user.kama" <<'EOF'
import { lib::keyed::Keyed };
type value NoHash {
    public int32 v;
    public ctor make(int32 v) { this.v = v; }
}
fn int32 main() {
    Keyed<NoHash> k = Keyed.make(key: NoHash.make(v: 1));   // line 7 — the mistake, in THIS file
    return 0;
}
EOF

out8=$("$KAMA" check "$tmp/bound_user.kama" "$tmp/lib/keyed/keyed.kama" 2>&1 || true)

# 8a. ⚠️ The library file is only 7 lines long and line 7 is its closing brace, so "keyed.kama:7" is not a
#     near miss — it is the user's line number wearing the library's name.
if ! printf '%s' "$out8" | grep -q 'bound_user\.kama:7.*does not satisfy bound'; then
    echo "check-diag-file: FAIL — a failed generic bound does not name the file that wrote the type argument."
    echo "  expected bound_user.kama:7; got:"
    printf '%s\n' "$out8" | sed 's/^/    /' | head -6
    exit 1
fi
if printf '%s' "$out8" | grep -q 'keyed\.kama:.*does not satisfy bound'; then
    echo "check-diag-file: FAIL — the USER's bad type argument is reported against the TEMPLATE's file."
    echo "  line right, file wrong — case 1 inverted:"
    printf '%s\n' "$out8" | sed 's/^/    /' | head -6
    exit 1
fi

# 8b. one refused argument, one diagnostic — the template's body must not be walked with it.
n8=$(printf '%s\n' "$out8" | grep -c 'error:' || true)
if [ "$n8" != 1 ]; then
    echo "check-diag-file: FAIL — one unsatisfied bound produced $n8 errors (expected 1)."
    echo "  a bound that failed must abandon the instantiation, not instantiate and report the fallout:"
    printf '%s\n' "$out8" | sed 's/^/    /' | head -6
    exit 1
fi

echo "check-diag-file: PASS (a diagnostic names the file that owns the declaration, imported or local,"
echo "                       from the collect pass, a body, or a generic template's body; and one mistake"
echo "                       in a generic body is reported once, not once per instantiation; an"
echo "                       unresolved module names the rule that refused it, not a directory; a"
echo "                       same-module import blames the cause that fired, not the export list; and a"
echo "                       failed generic bound is the USE SITE's mistake, reported once, in its file)"
