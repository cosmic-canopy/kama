#!/bin/sh
# check-buildsettings.sh — what a DEPENDENCY contributes to the C command line, and what it must not.
#
# `cflags`/`ldflags`/`link` used to be read only from the ROOT manifest. A dependency's copies were read
# nowhere at all — at build time a dep's kama.json was consulted for its `source`, its `name` and its
# `dependencies`, and for nothing else — so every consumer had to repeat the block and drift between the
# copies was silent. The first external project repeats it in two packages.
#
# Two halves, and the second is the one that would be missing from a guard written only for the fix:
# a rule that propagates EVERYTHING would pass every positive case here. So the negative half asserts
# that a dependency canNOT reach into the consumer's toolchain (`cc`, `sysroot`), canNOT impose
# `no-heap` (which changes what compiles, program-wide), and canNOT demand a `webgpu` SDK download.
#
# Everything uses a PATH dependency, so this guard needs no git, no curl and no sha256 — it has no SKIP
# leg, unlike check-packages.sh. `--cc echo` prints the fully assembled command line instead of running
# it, which is the instrument check-target.sh established.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"
if [ ! -x "$KAMA" ]; then echo "check-buildsettings: $KAMA not built" >&2; exit 1; fi

fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

ok()  { echo "  ok: $1"; }
bad() { echo "  FAIL: $1" >&2; fail=1; }

# A consumer project with N vendored path dependencies. The consumer's manifest comes in on stdin so
# each case can say something different about the root tier; every dep manifest is written by the case.
#
#   app <name> <dep>... <<'JSON' … JSON
app() {
    d="$tmp/$1"; shift
    rm -rf "$d"; mkdir -p "$d/src"
    printf 'fn int32 main() { return 7; }\n' > "$d/src/app.kama"
    for dep in "$@"; do
        mkdir -p "$d/vendor/$dep/src"
        printf 'public fn int32 %s_ping() { return 1; }\n' "$dep" > "$d/vendor/$dep/src/$dep.kama"
    done
    cat > "$d/kama.json"
}

# The assembled command line for a project, with the C compiler stubbed out. `pkg install` first: the
# dependency VIEW (`.kama/deps/<name>` -> the package) is what the build walks, and only install builds it.
cmdline() {
    d="$tmp/$1"
    if ! "$KAMA" pkg install "$d/kama.json" >"$tmp/o" 2>"$tmp/e"; then
        echo "__INSTALL_FAILED__"; cat "$tmp/e" >&2; return 0
    fi
    "$KAMA" build "$d/kama.json" --cc "echo CC:" -o "$d/app" 2>/dev/null || true
}

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: a dependency's build settings reach the build"

app dep1 geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cflags": ["-DROOT_TIER"],
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/dep1/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "cflags": ["-DDEP_PROJECT_TIER"], "ldflags": ["-Wl,--dep-ld"], "link": ["m"],
  "select": { "TARGET": { "HOST": { "cflags": ["-DDEP_TARGET_TIER"] } } },
  "modules": { ".": { "visibility": "public" } } }
JSON
line=$(cmdline dep1)
for want in "-DDEP_PROJECT_TIER" "-DDEP_TARGET_TIER" "-Wl,--dep-ld" "-lm"; do
    if printf '%s' "$line" | grep -qF -- "$want"; then
        ok "a dependency's $want reaches the command line"
    else
        bad "a dependency's $want did not reach the command line"
        printf '%s\n' "$line" | sed 's/^/    /' >&2
    fi
done

# ORDER: dependencies contribute first, then the project, then the CLI. So a consumer can always have
# the last word over a dependency wherever the C compiler resolves a repeated flag last-wins — which is
# the whole reason the order is fixed rather than incidental.
order=$(printf '%s' "$line" | tr ' ' '\n' | grep -x -- '-DDEP_PROJECT_TIER\|-DDEP_TARGET_TIER\|-DROOT_TIER' | tr '\n' ' ')
case "$order" in
    "-DDEP_PROJECT_TIER -DDEP_TARGET_TIER -DROOT_TIER"*)
        ok "dependency flags come BEFORE the project's, so the project has the last word" ;;
    *)  bad "flag order is wrong; got: $order" ;;
esac

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: two dependencies naming the same library link it once"

# `link` is a NAME list, so a repeat is pure noise and two packages both wanting -lm is the ordinary
# case. (`cflags`/`ldflags` are deliberately NOT deduped: they are raw text where a repeat can be
# load-bearing, and where last-wins is the semantic.)
app dup geo phys <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "link": ["m"],
  "dependencies": { "geo": { "path": "./vendor/geo" }, "phys": { "path": "./vendor/phys" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
for dep in geo phys; do
    cat > "$tmp/dup/vendor/$dep/kama.json" <<JSON
{ "name": "$dep", "version": "1.0.0", "kind": "library", "source": "src", "link": ["m"],
  "modules": { ".": { "visibility": "public" } } }
JSON
done
line=$(cmdline dup)
n=$(printf '%s' "$line" | tr ' ' '\n' | grep -c -x -- '-lm' || true)
[ "$n" = "1" ] \
    && ok "three declarations of \`m\` link it once" \
    || { bad "expected one -lm, got $n"; printf '%s\n' "$line" | sed 's/^/    /' >&2; }

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: a target's \`link\` replaces its OWN manifest's list, nobody else's"

# `link`-replace exists so a project can say "not that one, here" about ITS OWN list. If it were global,
# a dependency declaring select.TARGET.<T>.link would silently delete the consumer's -lm — i.e. adding a
# dependency could break your link. Resolution is therefore per-manifest, and only then concatenated.
app repl geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "link": ["m"],
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/repl/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src", "link": ["z"],
  "select": { "TARGET": { "HOST": { "link": ["curl"] } } },
  "modules": { ".": { "visibility": "public" } } }
JSON
line=$(cmdline repl)
if printf '%s' "$line" | grep -q -- '-lcurl' && printf '%s' "$line" | grep -qE -- '-lm( |$)' \
   && ! printf '%s' "$line" | grep -qE -- '-lz( |$)'; then
    ok "the dep's target \`link\` replaced the DEP's own list and left the consumer's alone"
else
    bad "per-manifest \`link\` replacement is wrong (want -lcurl and -lm, not -lz)"
    printf '%s\n' "$line" | sed 's/^/    /' >&2
fi

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: a dependency canNOT redefine the consumer's build"

# The line is: does this say how the dependency's own code must be compiled, or does it redefine the
# consumer's build? `no-heap` changes what COMPILES, program-wide — a dependency must be able neither to
# impose it nor to lift it. Asserted with a program that genuinely allocates: under `--no-heap` the
# string concatenation below is refused by name, so if the key propagated this build would fail.
app noheap geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/noheap/src/app.kama" <<'KAMA'
fn int32 main() {
    string a = "x";
    string b = a + "y";
    return cast<int32>(b.length()) + 5;   // 7
}
KAMA
cat > "$tmp/noheap/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src", "no-heap": true,
  "modules": { ".": { "visibility": "public" } } }
JSON
if "$KAMA" pkg install "$tmp/noheap/kama.json" >/dev/null 2>&1 \
   && "$KAMA" build "$tmp/noheap/kama.json" -o "$tmp/noheap/app" >"$tmp/o" 2>"$tmp/e"; then
    ok "a dependency's \`no-heap\` does not become the consumer's"
else
    bad "a dependency's \`no-heap\` reached the consumer's build"; head -2 "$tmp/e" >&2
fi
# ...and the control: the consumer saying it itself DOES refuse the same program, so the case above is
# not passing because the rule stopped working.
cat > "$tmp/noheap/kama.json" <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "no-heap": true,
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
if "$KAMA" build "$tmp/noheap/kama.json" -o "$tmp/noheap/app2" >"$tmp/o" 2>"$tmp/e"; then
    bad "the consumer's OWN \`no-heap\` no longer refuses a heap allocation — the case above proves nothing"
else
    ok "...while the consumer's own \`no-heap\` still refuses it (the case above is armed)"
fi

# `webgpu` selects an SDK and can hard-fail a build that has not fetched one. A dependency must not be
# able to make its consumer's build demand a download. Pointing KAMA_WGPU_DIR at nothing makes "the key
# reached the build" observable on any machine, which is the technique check-manifest.sh uses for the
# project-level key.
app wg geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/wg/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src", "webgpu": true,
  "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$tmp/wg/kama.json" >/dev/null 2>&1 || true
if KAMA_WGPU_DIR="$tmp/wg/nowhere" "$KAMA" build "$tmp/wg/kama.json" -o "$tmp/wg/app" >"$tmp/o" 2>"$tmp/e"; then
    ok "a dependency's \`webgpu\` does not make the consumer's build demand the SDK"
else
    bad "a dependency's \`webgpu\` reached the consumer's build"; head -2 "$tmp/e" >&2
fi

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: a dependency's RELATIVE include path is refused, not propagated"

# It would resolve against the CONSUMER's working directory rather than against the dependency — so it
# either fails loudly or, worse, silently finds the consumer's own include/. The root's own relative
# paths are untouched: those are the consumer's business and have always been CWD-relative.
app relpath geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/relpath/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src", "cflags": ["-Iinclude"],
  "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$tmp/relpath/kama.json" >/dev/null 2>&1 || true
if "$KAMA" build "$tmp/relpath/kama.json" -o "$tmp/relpath/app" >"$tmp/o" 2>"$tmp/e"; then
    bad "a dependency's relative -I was accepted"
elif grep -qF 'relative path in its build flags' "$tmp/e" && grep -qF 'geo' "$tmp/e"; then
    ok "refused, naming the dependency and the flag"
else
    bad "refused, but the message does not name the problem"; head -3 "$tmp/e" >&2
fi
# ...while the same flag on the ROOT is the consumer's own business. A SEPARATE project, deliberately:
# the walk reads the materialized `.kama/deps` view rather than the manifest's declared list (that is
# what makes it transitive, and it is the same view the import path uses), so merely deleting the
# `dependencies` key above would leave the stale view — and the dep — still in play.
app relroot <<'JSON'
{ "name": "relroot", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cflags": ["-Iinclude"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/relroot/kama.json" -o "$tmp/relroot/app" >"$tmp/o" 2>"$tmp/e" \
    && ok "...while the same flag on the root project is accepted" \
    || { bad "a relative -I on the ROOT was refused"; head -2 "$tmp/e" >&2; }

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: \`csources\` compiles C, and says what it does not compile"

# The runnable end-to-end proofs are tests/csources_basic.d/ and tests/csources_dep.d/ — they build a C
# file and call into it, on every leg. What lives here is the part an exit code cannot see: the
# refusals, where the object lands, and the collision two packages can cause.

# C++ is refused BY NAME, with the two reasons, rather than reaching the C compiler as a `-std=c11`
# compile of a .cpp and failing there.
app cpp <<'JSON'
{ "name": "cpp", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "csources": ["csrc/thing.cpp"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
if "$KAMA" build "$tmp/cpp/kama.json" -o "$tmp/cpp/app" >"$tmp/o" 2>"$tmp/e"; then
    bad "a .cpp in \`csources\` was accepted"
elif grep -qF 'compiles C only today' "$tmp/e" && grep -qF 'C++ runtime library' "$tmp/e"; then
    ok "a .cpp entry is refused, naming both obstacles"
else
    bad "a .cpp entry was refused without saying why"; head -3 "$tmp/e" >&2
fi

app absol <<'JSON'
{ "name": "absol", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "csources": ["/etc/shim.c"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/absol/kama.json" -o "$tmp/absol/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "an absolute \`csources\` path was accepted" \
    || { grep -qF 'relocatable' "$tmp/e" \
         && ok "an absolute path is refused — a manifest must stay relocatable" \
         || { bad "an absolute path was refused for the wrong reason"; head -2 "$tmp/e" >&2; }; }

app missing <<'JSON'
{ "name": "missing", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "csources": ["csrc/nope.c"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/missing/kama.json" -o "$tmp/missing/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "a \`csources\` path that does not exist was accepted" \
    || { grep -qF 'does not exist' "$tmp/e" \
         && ok "a path that is not there is named by kama, not by the C compiler" \
         || { bad "a missing csource failed for the wrong reason"; head -2 "$tmp/e" >&2; }; }

# TWO PACKAGES SHIPPING `shim.c`. Without an owner prefix on the object name they write the same file —
# a silent overwrite in one invocation, and a RACE under -j. `-j 2` forces the per-TU path, which is the
# only leg where the object names are actually used.
app twoshims geo phys <<'JSON'
{ "name": "twoshims", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" }, "phys": { "path": "./vendor/phys" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
for dep in geo phys; do
    mkdir -p "$tmp/twoshims/vendor/$dep/csrc"
    cat > "$tmp/twoshims/vendor/$dep/kama.json" <<JSON
{ "name": "$dep", "version": "1.0.0", "kind": "library", "source": "src", "csources": ["csrc/shim.c"],
  "modules": { ".": { "visibility": "public" } } }
JSON
    printf 'int kama_%s_shim(void) { return 1; }\n' "$dep" > "$tmp/twoshims/vendor/$dep/csrc/shim.c"
done
rc=0
"$KAMA" pkg install "$tmp/twoshims/kama.json" >/dev/null 2>&1 || rc=1
[ "$rc" = 0 ] && { "$KAMA" build "$tmp/twoshims/kama.json" -j 2 -o "$tmp/twoshims/app" >"$tmp/o" 2>"$tmp/e" || rc=1; }
# The program's own exit code is 7 by construction (see `app`), so only the BUILD is being judged here.
[ "$rc" = 0 ] && { arc=0; "$tmp/twoshims/app" >/dev/null 2>&1 || arc=$?; [ "$arc" = 7 ] || rc=1; }
if [ "$rc" = 0 ]; then
    ok "two packages both shipping csrc/shim.c compile to different objects"
else
    bad "two packages shipping the same C file name collided"; head -3 "$tmp/e" >&2
fi

# The objects land in dirname(-o) and the user's .c is never touched: `tools/check-clean-tree.sh` holds
# the general rule, and this is the csources-shaped instance of it.
[ -f "$tmp/twoshims/vendor/geo/csrc/shim.o" ] || [ -f "$tmp/twoshims/vendor/geo/csrc/shim.c.o" ] \
    && bad "a csource's object was written beside the user's source" \
    || ok "...and neither object was written into the package that owns the source"

# OUTPUT=OBJECT is one translation unit by definition, and a csource is a second one. Refused by name
# rather than as clang's "cannot specify -o when generating multiple output files".
app objkind <<'JSON'
{ "name": "objkind", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "csources": ["csrc/x.c"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
mkdir -p "$tmp/objkind/csrc"; printf 'int kama_objkind_x(void) { return 1; }\n' > "$tmp/objkind/csrc/x.c"
"$KAMA" build "$tmp/objkind/kama.json" --select OUTPUT=OBJECT -o "$tmp/objkind/app.o" >"$tmp/o" 2>"$tmp/e" \
    && bad "OUTPUT=OBJECT accepted a build with two translation units" \
    || { grep -qF 'single translation unit' "$tmp/e" \
         && ok "OUTPUT=OBJECT with a csource is refused by name, pointing at OUTPUT=STATIC" \
         || { bad "OUTPUT=OBJECT failed for the wrong reason"; head -2 "$tmp/e" >&2; }; }

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: a broken dependency manifest is fatal to a build, not to a query"

# The same split `strictImports` draws. A build that would read settings from a manifest it cannot parse
# must say so; refusing to answer a question about a file the user is looking at, over a manifest in
# some package, punishes the wrong thing.
app broken geo <<'JSON'
{ "name": "consumer", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/broken/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$tmp/broken/kama.json" >/dev/null 2>&1 || true
printf '{ "name": "geo", oops\n' > "$tmp/broken/vendor/geo/kama.json"
if "$KAMA" build "$tmp/broken/kama.json" -o "$tmp/broken/app" >"$tmp/o" 2>"$tmp/e"; then
    bad "a build read a dependency manifest it could not parse and said nothing"
else
    ok "a build refuses, naming the dependency's manifest"
fi
"$KAMA" query "$tmp/broken/src/app.kama" --symbols >"$tmp/o" 2>"$tmp/e" \
    && ok "...while \`kama query\` still answers" \
    || { bad "\`kama query\` refused over a broken dependency manifest"; head -2 "$tmp/e" >&2; }

[ "$fail" = 0 ] || { echo "check-buildsettings: FAILED" >&2; exit 1; }
echo "check-buildsettings: PASS (a dependency contributes cflags/ldflags/link, before the project and
  deduped for link, resolved per-manifest so its target's \`link\` replaces only its own; and it cannot
  reach the consumer's no-heap, webgpu or working directory)"
