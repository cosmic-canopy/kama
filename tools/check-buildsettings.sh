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
echo "check-buildsettings: \`cincludes\` puts a package's include tree on the path, under the path rules"

# The runnable proof is tests/cincludes_dep.d/: a dependency whose header lives in include/, not beside
# its .c, reached by both the C and the kama extern. What lives here is the refusals — the same three
# every file-naming key shares — and that a directory which is not there is named by kama.
app incabs <<'JSON'
{ "name": "incabs", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cincludes": ["/usr/include"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/incabs/kama.json" -o "$tmp/incabs/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "an absolute \`cincludes\` path was accepted" \
    || { grep -qF 'relocatable' "$tmp/e" \
         && ok "an absolute include dir is refused — a manifest must stay relocatable" \
         || { bad "an absolute include dir was refused for the wrong reason"; head -2 "$tmp/e" >&2; }; }

app incdot <<'JSON'
{ "name": "incdot", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cincludes": ["../include"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/incdot/kama.json" -o "$tmp/incdot/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "a \`cincludes\` path escaping with .. was accepted" \
    || { grep -qF 'escapes the project' "$tmp/e" \
         && ok "an include dir escaping the package with .. is refused" \
         || { bad "the .. include dir was refused for the wrong reason"; head -2 "$tmp/e" >&2; }; }

app incmiss <<'JSON'
{ "name": "incmiss", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cincludes": ["include"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/incmiss/kama.json" -o "$tmp/incmiss/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "a \`cincludes\` directory that does not exist was accepted" \
    || { grep -qF 'is not a directory' "$tmp/e" \
         && ok "a directory that is not there is named by kama, not by a missing header three steps later" \
         || { bad "a missing include dir failed for the wrong reason"; head -2 "$tmp/e" >&2; }; }

# A non-array is refused by shape, like every other list key.
app incshape <<'JSON'
{ "name": "incshape", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "cincludes": "include",
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/incshape/kama.json" -o "$tmp/incshape/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "a string \`cincludes\` was accepted" \
    || ok "\`cincludes\` must be an array"

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: \`emSettings\` MERGES with kama's own, instead of losing to them"

# The defect: emcc is LAST-WINS on a repeated `-s`, and kama emitted its own
# `-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8` AFTER the project's `cflags` — so the only way a
# project could say this at all was also the way it silently lost. Now the key is structured, kama's
# settings declare their own kind, and a LIST unions.
#
# These are command-line facts an exit code cannot see, and a wasm-only fixture does not fit
# run_tests.sh's every-leg model — so they live here rather than in tests/.
emline() {
    "$KAMA" build "$tmp/$1/kama.json" --target wasm --cc "echo emcc" -o "$tmp/$1/app.html" 2>/dev/null || true
}

app emmerge <<'JSON'
{ "name": "emmerge", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "emSettings": { "EXPORTED_RUNTIME_METHODS": ["ccall"], "STACK_SIZE": "4MB",
                  "ALLOW_MEMORY_GROWTH": true },
  "modules": { ".": { "visibility": "internal" } } }
JSON
# The program has to reach std::net::web, because that is what makes kama emit the two runtime methods
# its JS glue needs — the setting there is nothing to union WITH otherwise, and the case would pass
# while testing nothing.
cat > "$tmp/emmerge/src/app.kama" <<'KAMA'
import { std::net::web::WsConnection };

fn int32 main() { return 7; }
KAMA
line=$(emline emmerge)
printf '%s' "$line" | grep -qF -- "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,ccall" \
    && ok "a list setting UNIONS: the stdlib's names survive and the project's is added" \
    || { bad "the project's EXPORTED_RUNTIME_METHODS did not merge with kama's"
         printf '%s\n' "$line" | tr ' ' '\n' | grep -- '-s' | sed 's/^/    /' >&2; }
printf '%s' "$line" | grep -qF -- "-sSTACK_SIZE=4MB" \
    && ok "a string scalar reaches the command line verbatim" \
    || bad "a string scalar did not reach the command line"
printf '%s' "$line" | grep -qF -- "-sALLOW_MEMORY_GROWTH=1" \
    && ok "a boolean renders as =1" \
    || bad "a boolean setting did not render"
# ...and every -s is in the LINK tail, after the inputs, because that is what it is. (Before this they
# sat among the compile flags, which is exactly how the collision above went unnoticed.)
#
# ⚠️ Matched on a quote-STRIPPED copy. kama quotes every input path, and `--cc "echo emcc"` — the
# instrument that makes this line readable at all — goes through the platform's system(): /bin/sh strips
# those quotes before `echo` sees them, cmd.exe does not. So the same correct ordering reads `…app.c `
# on POSIX and `…app.c" ` on Windows, and a pattern written against one reports the OTHER as "the
# setting is still among the compile flags". The order is what is under test; the quoting is the
# instrument's.
case "$(printf '%s' "$line" | tr -d '"')" in
    *".c "*"-sEXIT_RUNTIME"*) ok "the settings are emitted in the link tail, after the inputs" ;;
    *) bad "an -s setting is still emitted among the compile flags" ;;
esac

# The project beats kama's own default for a scalar.
app emscalar <<'JSON'
{ "name": "emscalar", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "emSettings": { "EXIT_RUNTIME": 0 },
  "modules": { ".": { "visibility": "internal" } } }
JSON
line=$(emline emscalar)
printf '%s' "$line" | grep -qF -- "-sEXIT_RUNTIME=0" && ! printf '%s' "$line" | grep -qF -- "-sEXIT_RUNTIME=1" \
    && ok "the project overrides one of kama's own scalars" \
    || { bad "the project could not override -sEXIT_RUNTIME"; printf '%s\n' "$line" | sed 's/^/    /' >&2; }

# TWO DEPENDENCIES disagreeing on one scalar is refused by name. Picking the alphabetically-later
# package would be a silent answer to a question only the consumer can settle.
app emconf geo phys <<'JSON'
{ "name": "emconf", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" }, "phys": { "path": "./vendor/phys" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/emconf/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "emSettings": { "STACK_SIZE": "4MB" }, "modules": { ".": { "visibility": "public" } } }
JSON
cat > "$tmp/emconf/vendor/phys/kama.json" <<'JSON'
{ "name": "phys", "version": "1.0.0", "kind": "library", "source": "src",
  "emSettings": { "STACK_SIZE": "16MB" }, "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$tmp/emconf/kama.json" >/dev/null 2>&1 || true
if "$KAMA" build "$tmp/emconf/kama.json" --target wasm --cc "echo emcc" -o "$tmp/emconf/app.html" \
        >"$tmp/o" 2>"$tmp/e"; then
    bad "two dependencies disagreeing on a scalar setting was resolved silently"
elif grep -qF 'geo' "$tmp/e" && grep -qF 'phys' "$tmp/e" && grep -qF 'STACK_SIZE' "$tmp/e"; then
    ok "two dependencies disagreeing is refused, naming both and the setting"
else
    bad "the conflict was refused without naming both"; head -3 "$tmp/e" >&2
fi
# ...and the project stating it settles the question, with no error.
cat > "$tmp/emconf/kama.json" <<'JSON'
{ "name": "emconf", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "emSettings": { "STACK_SIZE": "8MB" },
  "dependencies": { "geo": { "path": "./vendor/geo" }, "phys": { "path": "./vendor/phys" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
line=$(emline emconf)
printf '%s' "$line" | grep -qF -- "-sSTACK_SIZE=8MB" \
    && ok "...and the project stating it settles the conflict" \
    || { bad "the project's value did not settle the dependency conflict"; printf '%s\n' "$line" | sed 's/^/    /' >&2; }

# `jsLibraries` is a plain append list — emcc accumulates `--js-library`, so there is no collision to
# resolve. A dependency's reaches the build for the same reason its `csources` does: a package that
# binds a browser API ships the glue that binds it.
app jslib geo <<'JSON'
{ "name": "jslib", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "jsLibraries": ["js/own.js"],
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
mkdir -p "$tmp/jslib/js" "$tmp/jslib/vendor/geo/js"
printf 'mergeInto(LibraryManager.library, {});\n' > "$tmp/jslib/js/own.js"
printf 'mergeInto(LibraryManager.library, {});\n' > "$tmp/jslib/vendor/geo/js/dep.js"
cat > "$tmp/jslib/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src", "jsLibraries": ["js/dep.js"],
  "modules": { ".": { "visibility": "public" } } }
JSON
"$KAMA" pkg install "$tmp/jslib/kama.json" >/dev/null 2>&1 || true
line=$(emline jslib)
if printf '%s' "$line" | grep -qF -- "own.js" && printf '%s' "$line" | grep -qF -- "dep.js"; then
    ok "both the project's and a dependency's \`jsLibraries\` reach the link"
else
    bad "a \`jsLibraries\` entry did not reach the link"; printf '%s\n' "$line" | sed 's/^/    /' >&2
fi
"$KAMA" build "$tmp/jslib/kama.json" -o "$tmp/jslib/app" >"$tmp/o" 2>"$tmp/e" \
    && ok "...and they are inert on a native build" \
    || { bad "jsLibraries broke a native build"; head -2 "$tmp/e" >&2; }

# INERT on a non-wasm target, not refused: the same manifest builds both, and `subsystem` set the
# precedent for an accepted no-op. The SHAPE is still validated everywhere, which is the half that
# makes a typo findable by whoever wrote it rather than by whoever ships to the web.
nat=$("$KAMA" build "$tmp/emmerge/kama.json" --cc "echo CC:" -o "$tmp/emmerge/app" 2>/dev/null || true)
[ -n "$nat" ] && ! printf '%s' "$nat" | grep -q -- '-sSTACK_SIZE' \
    && ok "emSettings are inert on a native build, not an error" \
    || { bad "emSettings leaked into a native build (or the build failed)"; printf '%s\n' "$nat" | sed 's/^/    /' >&2; }

app emshape <<'JSON'
{ "name": "emshape", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "emSettings": { "STACK_SIZE": { "nope": 1 } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$tmp/emshape/kama.json" -o "$tmp/emshape/app" >"$tmp/o" 2>"$tmp/e" \
    && bad "an object-valued emSetting was accepted" \
    || { grep -qF 'must be an array of strings, a string, a number or a boolean' "$tmp/e" \
         && ok "...while a value of no legal shape is refused on EVERY target, naming the four" \
         || { bad "a malformed emSetting was refused for the wrong reason"; head -2 "$tmp/e" >&2; }; }

# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: \`reproducible-float\` forbids the C compiler fusing a*b+c"

# ⚠️ THIS CANNOT BE A tests/*.d FIXTURE, and finding out why is the reason the oracle lives here.
# run_tests.sh builds every fixture in DEBUG, where `a * b + c` is emitted as
# `KAMA_ADD(KAMA_MUL(a, b), c)` — the trap-checking macros, which already break the single expression
# clang would have contracted. So a debug fixture passes identically with and without the key: it would
# look like coverage and assert nothing. The divergence only exists in a release build, which only a
# guard can ask for.
fp="$tmp/fp"
mkdir -p "$fp/src"
cat > "$fp/src/main.kama" <<'KAMA'
// Exit code = how many of 64 triples give a different result for `a * b + c` than for the same
// multiply and add written as two statements. Under clang's default `-ffp-contract=on` the
// one-expression form may fuse into an FMA and the two-statement form may not, so on a target WITH an
// FMA the two disagree — which is what makes a cross-target program's numerics depend on its target.
fn int32 main() {
    int32 differ = 0;
    int32 i = 1;
    while (i <= 64) {
        float64 a = cast<float64>(i) * 0.7853981633974483f64 + 1.0f64;
        float64 b = cast<float64>(i) * 1.4142135623730951f64 + 0.5f64;
        float64 c = cast<float64>(i) * 2.7182818284590452f64 + 0.25f64;
        float64 fused = a * b + c;
        float64 prod  = a * b;
        float64 split = prod + c;
        if (fused != split) { differ = differ + 1; }
        i = i + 1;
    }
    return differ;
}
KAMA
cat > "$fp/kama.json" <<'JSON'
{ "name": "fp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$fp/kama.json" --release -o "$fp/off" >"$tmp/o" 2>"$tmp/e" \
    || bad "the fp fixture would not build"
base=0; [ -x "$fp/off" ] && { "$fp/off" >/dev/null 2>&1 || base=$?; }
cat > "$fp/kama.json" <<'JSON'
{ "name": "fp", "version": "0.1.0", "kind": "executable", "entry": "src/main.kama", "source": "src",
  "reproducible-float": true,
  "modules": { ".": { "visibility": "internal" } } }
JSON
"$KAMA" build "$fp/kama.json" --release -o "$fp/on" >"$tmp/o" 2>"$tmp/e" \
    || bad "the fp fixture would not build with the key"
withkey=0; [ -x "$fp/on" ] && { "$fp/on" >/dev/null 2>&1 || withkey=$?; }
if [ "$base" -eq 0 ]; then
    # No FMA on this host (baseline x86-64 SSE2, or a target that never contracts), so there is nothing
    # for the key to prevent. Said out loud rather than reported as a pass: this leg is inert here.
    ok "the oracle is inert on this host (nothing contracts: $base of 64 differ without the key)"
    [ "$withkey" -eq 0 ] || bad "the key made results differ where nothing contracted ($withkey of 64)"
elif [ "$withkey" -eq 0 ]; then
    ok "$base of 64 triples differ without the key, 0 with it"
else
    bad "the key did not stop contraction ($base of 64 without, $withkey with)"
fi

# The command line, which is checkable on every host and on a target this machine cannot run.
app rfl <<'JSON'
{ "name": "rfl", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "reproducible-float": true,
  "modules": { ".": { "visibility": "internal" } } }
JSON
line=$("$KAMA" build "$tmp/rfl/kama.json" --cc "echo CC:" -o "$tmp/rfl/app" 2>/dev/null || true)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && ok "the flag reaches a native build" || bad "the flag did not reach a native build"
line=$("$KAMA" build "$tmp/rfl/kama.json" --target wasm --cc "echo emcc" -o "$tmp/rfl/a.html" 2>/dev/null || true)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && ok "...and wasm, where it is a no-op but the command line must not fork per target" \
    || bad "the flag did not reach the wasm build"

# A target says "not this one", exactly as it can for `no-heap` and `webgpu`.
app rfloff <<'JSON'
{ "name": "rfloff", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "reproducible-float": true,
  "select": { "TARGET": { "HOST": { "reproducible-float": false } } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
line=$("$KAMA" build "$tmp/rfloff/kama.json" --cc "echo CC:" -o "$tmp/rfloff/app" 2>/dev/null || true)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && bad "a target's \`reproducible-float: false\` did not turn it off" \
    || ok "a target turns it off again, the way it can for no-heap and webgpu"

# ...and the control, so the two cases above cannot both pass by the flag never being emitted at all.
app rflnone <<'JSON'
{ "name": "rflnone", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "modules": { ".": { "visibility": "internal" } } }
JSON
line=$("$KAMA" build "$tmp/rflnone/kama.json" --cc "echo CC:" -o "$tmp/rflnone/app" 2>/dev/null || true)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && bad "a project that never asked for it got -ffp-contract=off" \
    || ok "...and a project that never asked for it gets nothing"

# A DEPENDENCY's `reproducible-float` reaches the consumer. Unlike `no-heap` and `webgpu` (asserted
# above NOT to propagate) this key can only turn contraction off, and it states a requirement of the
# dependency's own arithmetic — which the consumer compiles. 0.9.169 shipped it grouped with the other
# two, so the first consumer's raw `-ffp-contract=off` cflag propagated and the key replacing it did not.
app rfldep geo <<'JSON'
{ "name": "rfldep", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/rfldep/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "reproducible-float": true,
  "modules": { ".": { "visibility": "public" } } }
JSON
line=$(cmdline rfldep)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && ok "a dependency's project-tier \`reproducible-float\` reaches the consumer's build" \
    || { bad "a dependency's \`reproducible-float\` did not reach the consumer's build"
         printf '%s\n' "$line" | sed 's/^/    /' >&2; }

# ...from the dependency's TARGET tier too — its own `select.TARGET` overrides its own project tier,
# per-manifest, before the join (the same rule `link` follows).
cat > "$tmp/rfldep/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "select": { "TARGET": { "HOST": { "reproducible-float": true } } },
  "modules": { ".": { "visibility": "public" } } }
JSON
line=$(cmdline rfldep)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && ok "...and from the dependency's target tier" \
    || bad "a dependency's target-tier \`reproducible-float\` did not reach the consumer's build"

# ...and it is ONE-WAY: a dependency saying `false` takes nothing from a consumer that asked.
app rfldepoff geo <<'JSON'
{ "name": "rfldepoff", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "reproducible-float": true,
  "dependencies": { "geo": { "path": "./vendor/geo" } },
  "modules": { ".": { "visibility": "internal" } } }
JSON
cat > "$tmp/rfldepoff/vendor/geo/kama.json" <<'JSON'
{ "name": "geo", "version": "1.0.0", "kind": "library", "source": "src",
  "reproducible-float": false,
  "modules": { ".": { "visibility": "public" } } }
JSON
line=$(cmdline rfldepoff)
printf '%s' "$line" | grep -qF -- "-ffp-contract=off" \
    && ok "...and a dependency's \`false\` cannot turn it off for a consumer that asked" \
    || bad "a dependency's \`reproducible-float: false\` overrode the consumer's \`true\`"

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
# ---------------------------------------------------------------------------------------------------
echo "check-buildsettings: the section-GC link flags ride the link line only"
# `--release` adds `-Wl,-dead_strip` (ld64) / `-Wl,--gc-sections -s` (GNU). They were appended to the
# flag prefix every per-TU `-c` job shares, and clang says "'linker' input unused" once per compile-only
# job — 121 lines for a package vendoring libsodium, the first thing its consumer saw (ROADMAP row 3).
# A `csources` entry is what makes the build per-TU (two inputs), and `--cc "echo CC:"` prints each job.
app gcl <<'JSON'
{ "name": "gcl", "version": "0.1.0", "kind": "executable", "entry": "src/app.kama", "source": "src",
  "csources": ["csrc/shim.c"],
  "modules": { ".": { "visibility": "internal" } } }
JSON
mkdir -p "$tmp/gcl/csrc"; printf 'int gcl_shim(void) { return 1; }\n' > "$tmp/gcl/csrc/shim.c"
lines=$("$KAMA" build "$tmp/gcl/kama.json" --release --cc "echo CC:" -o "$tmp/gcl/app" 2>/dev/null | grep "^CC:" || true)
if printf '%s\n' "$lines" | grep -- " -c " | grep -qE -- "dead_strip|gc-sections"; then
    bad "a per-TU compile line carries the section-GC LINK flag ('linker' input unused, once per TU)"
    printf '%s\n' "$lines" | sed 's/^/    /' >&2
else ok "no per-TU compile line carries a link flag"; fi
if printf '%s\n' "$lines" | grep -v -- " -c " | grep -qE -- "dead_strip|gc-sections"; then
    ok "the link line carries the section-GC flag"
else
    bad "the link line lost the section-GC flag"; printf '%s\n' "$lines" | sed 's/^/    /' >&2
fi

echo "check-buildsettings: PASS (a dependency contributes cflags/ldflags/link, before the project and
  deduped for link, resolved per-manifest so its target's \`link\` replaces only its own; and it cannot
  reach the consumer's no-heap, webgpu or working directory; the section-GC flags ride the link line only)"
