all: kama

# Version: from the VERSION file (CI overrides with the git tag: make VERSION=1.2.3).
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

# ...plus the short commit, as SemVer build metadata, for every build that is NOT a CI release.
# `VERSION` is bumped by hand (tools/check-version.sh holds that down), and a bump that is forgotten
# would leave two different compilers both claiming the same number — which is the failure mode that
# makes `kama --version` useless in a bug report. The sha makes a build identifiable regardless, the
# way rustc, go and zig all do it. `$(origin VERSION)` is `command line` exactly when CI passed
# `make VERSION=1.2.3`, and there the bare release number is what should be printed.
ifneq ($(origin VERSION),command line)
GITSHA := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GITSHA),)
VERSION := $(VERSION)+g$(GITSHA)
endif
endif

CXX      = clang++
# The compiler's OWN optimization level. There was no -O flag here at all until 2026-08-10, so kama
# shipped unoptimized and every build-time number in ROADMAP_DETAIL §9 had been measured against an -O0
# binary. It is worth 8.2x on the front end (httpd `check`: 338 ms -> 41 ms) and 5.4x over the fixture
# corpus, for no change whatsoever in emitted C. -O1/-O2/-O3 measured within noise of each other
# (41.4 / 41.1 / 42.2 ms); -O2 is the conventional level and the marginal winner, and -O3 both built
# slower and ran slower, so there is nothing above here to chase. `tools/check-opt.sh` keeps it.
# Overridable for compiler debugging (`make OPT=-O0`). `-g` stays on unconditionally: a compiler you
# cannot get a backtrace out of is a bad trade for a few MB of binary.
OPT     ?= -O2
CXXFLAGS = -std=c++14 $(OPT) -g -Wall -Wno-deprecated-register -DKAMA_VERSION='"$(VERSION)"'
# Appended to both compile and link (the link rule reuses CXXFLAGS). CI sets this to
# build a macOS universal binary: EXTRA_CXXFLAGS="-arch arm64 -arch x86_64".
CXXFLAGS += $(EXTRA_CXXFLAGS)

# All build artifacts live under out/<os>-<arch>/ (objects + generated parser/lexer + the
# binary), so the repo root stays sources-only AND a host (mach-o) build coexists with a
# container (ELF) build instead of clobbering it — no `make clean` when switching between
# `make` and `tools/cdev make`. The root ./kama is a symlink to the last-built platform's
# binary, keeping the stable path every consumer (CI, release, docs) already uses.
#
# `out/` is the name `kama build` gives a project's artifacts (docs/targets.md), so the compiler's
# own tree is the one it teaches. The leaf is `uname -s`-`uname -m` rather than a kama triple
# because THIS build is clang++ and make, not kama — and a Makefile-side uname->triple table would
# be a second copy of what the driver already knows, free to drift from it.
PLATFORM ?= $(shell uname -s)-$(shell uname -m)
BUILD     = out/$(PLATFORM)

# Inheritance is a BUILD-TIME feature switch on the compiler, not a runtime flag (docs/SPEC.md,
# docs/design/inheritance.md). Two purposes: isolate what the feature costs the compiler — answerable only
# by building both ways and subtracting — and be the extraction point if inheritance is ever dropped, since
# the `#if KAMA_INHERITANCE` blocks are then the deletion list.
#
#   make                        # inheritance in, up to 2 levels below a root
#   make KAMA_INHERITANCE=0     # a compiler without it, into out/<platform>-noinherit
#   make KAMA_INHERIT_DEPTH=1   # no middle layer: a root plus ONE derived level
#
# KAMA_INHERIT_DEPTH is the CEILING on what a hierarchy may ask for. Each `virtual`/`abstract class` still
# has to state its own budget (`virtual(maxDepth: 2)`), which may not exceed this.
#
# The no-inheritance build gets its OWN directory: objects compiled under different macro values must never
# mix, and sharing out/<platform> would silently do exactly that (make sees the .o as up to date).
# `tools/check-no-inheritance.sh` builds it and proves the variant still works.
KAMA_INHERITANCE   ?= 1
KAMA_INHERIT_DEPTH ?= 2
# ⚠️ Refuse the EXTRA_CXXFLAGS spelling rather than silently ignoring it. These `-D`s are appended AFTER
# EXTRA_CXXFLAGS, so a `-DKAMA_INHERITANCE=0` passed that way loses to the default and you would get a
# compiler WITH inheritance while believing otherwise — and the build directory would not switch either,
# mixing objects compiled under different macro values. For a switch whose entire purpose is measurement,
# a silently-wrong build is the worst possible failure, so make it loud.
ifneq (,$(findstring KAMA_INHERIT,$(EXTRA_CXXFLAGS)))
$(error set KAMA_INHERITANCE / KAMA_INHERIT_DEPTH as make variables — `make KAMA_INHERITANCE=0` — not \
through EXTRA_CXXFLAGS, which is appended earlier and would be overridden without switching BUILD)
endif
CXXFLAGS += -DKAMA_INHERITANCE=$(KAMA_INHERITANCE) -DKAMA_INHERIT_DEPTH=$(KAMA_INHERIT_DEPTH)
ifeq ($(KAMA_INHERITANCE),0)
BUILD = out/$(PLATFORM)-noinherit
endif

# The grammar uses %code/api.pure full, which need bison >= 2.7. macOS ships
# 2.3, so prefer a Homebrew keg-only bison when present.
BISON = $(shell [ -x /opt/homebrew/opt/bison/bin/bison ] && echo /opt/homebrew/opt/bison/bin/bison || ([ -x /usr/local/opt/bison/bin/bison ] && echo /usr/local/opt/bison/bin/bison || echo bison))

OBJECTS = $(addprefix $(BUILD)/, \
            kama.lexer.o  \
            kama.parser.o \
            kama.ast.o    \
            kama.cemit.o  \
            kama.comptime.o \
            kama.query.o  \
            kama.lsp.o    \
            kama.driver.o \
            kama.prelude.gen.o \
            kama.agents.gen.o \
            kama.seed.gen.o)

# The built-in kama sources embedded into the binary (prelude core + the always-in-scope smart-ptr
# triad). tools/embed_prelude.sh wraps them in raw-string literals -> out/<platform>/kama.prelude.gen.cpp.
PRELUDE_GLOBAL  = prelude/global.kama
PRELUDE_MODULES = prelude/std/memory/owned.kama prelude/std/memory/shared.kama prelude/std/memory/weak.kama

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/kama.prelude.gen.cpp: $(PRELUDE_GLOBAL) $(PRELUDE_MODULES) tools/embed_prelude.sh | $(BUILD)
	sh tools/embed_prelude.sh $@ $(PRELUDE_GLOBAL) $(PRELUDE_MODULES)

# The agent-guidance files `kama agents` writes, embedded for the same reason as the prelude: a
# `--no-std` install ships only bin/kama. AGENTS.md is the content; every stub is a pointer to it,
# and declares its own destination path on line 1 (see tools/embed_agents.sh).
AGENTS_MD    = agents/AGENTS.md
AGENTS_SKILL = agents/skill/SKILL.md
AGENTS_STUBS = $(sort $(wildcard agents/stubs/*.md))

$(BUILD)/kama.agents.gen.cpp: $(AGENTS_MD) $(AGENTS_SKILL) $(AGENTS_STUBS) tools/embed_agents.sh | $(BUILD)
	sh tools/embed_agents.sh $@ $(AGENTS_MD) $(AGENTS_SKILL) $(AGENTS_STUBS)

# The project templates `kama seed` writes, embedded for the same reason as the prelude and the agent
# files: a `--no-std` install ships only bin/kama. Positional, not a wildcard — the four are four roles
# kama.seed.h names one by one (see tools/embed_seed.sh). seed/gitignore is DOTLESS on purpose: a literal
# seed/.gitignore would be a real gitignore governing seed/.
SEED_APP       = seed/app.kama
SEED_LIB       = seed/lib.kama
SEED_GITIGNORE = seed/gitignore
SEED_README    = seed/README.md

$(BUILD)/kama.seed.gen.cpp: $(SEED_APP) $(SEED_LIB) $(SEED_GITIGNORE) $(SEED_README) tools/embed_seed.sh | $(BUILD)
	sh tools/embed_seed.sh $@ $(SEED_APP) $(SEED_LIB) $(SEED_GITIGNORE) $(SEED_README)

# Bison/flex: CLI -o/--defines/--header-file override the %output/%option names
# baked into the source, redirecting generated files into out/<platform>/.
$(BUILD)/kama.parser.cpp $(BUILD)/kama.parser.hpp: src/kama.y | $(BUILD)
	$(BISON) -o $(BUILD)/kama.parser.cpp --defines=$(BUILD)/kama.parser.hpp src/kama.y

$(BUILD)/kama.lexer.cpp $(BUILD)/kama.lexer.hpp: src/kama.l $(BUILD)/kama.parser.hpp | $(BUILD)
	flex -o $(BUILD)/kama.lexer.cpp --header-file=$(BUILD)/kama.lexer.hpp src/kama.l

# Header dependencies (the implicit rules can't see #includes). Listing all
# project headers against every object is coarse but cheap, and prevents stale
# object/ABI-skew bugs when a class layout in a header changes.
HEADERS = $(addprefix src/, kama.forward.h kama.context.h kama.ast.h kama.cemit.h kama.prelude.h kama.diagnostic.h \
                            kama.query.h kama.lsp.h kama.agents.h kama.seed.h kama.json.h)
$(OBJECTS): $(HEADERS)

# Generated-header dependencies.
$(BUILD)/kama.lexer.o $(BUILD)/kama.parser.o $(BUILD)/kama.driver.o $(BUILD)/kama.cemit.o $(BUILD)/kama.comptime.o: $(BUILD)/kama.parser.hpp
$(BUILD)/kama.lexer.o $(BUILD)/kama.driver.o: $(BUILD)/kama.lexer.hpp

# Bison emits `int yynerrs` set-but-never-read. It's generated code (never edit it), so
# silence that one warning on this TU only — keeps the -Werror CI gate clean.
$(BUILD)/kama.parser.o: CXXFLAGS += -Wno-unused-but-set-variable

# The version string is baked into kama.driver.o by -DKAMA_VERSION, and NOTHING in the dependency graph
# mentions it — so bumping VERSION, or simply committing (which moves the +g<sha> suffix), left the .o
# up to date and `kama --version` kept reporting the string from whenever that TU last happened to
# rebuild. A version that lies is worse than no version at all, which is the whole point of the file.
#
# The fix is a stamp whose CONTENT is the version string, rewritten only when it differs. The rule runs
# every build (FORCE), but the file's mtime moves only on a real change, so exactly one TU rebuilds when
# the version changes and nothing rebuilds when it has not.
.PHONY: FORCE
FORCE:
$(BUILD)/kama.version.stamp: FORCE | $(BUILD)
	@printf '%s' '$(VERSION)' | cmp -s - $@ 2>/dev/null || printf '%s' '$(VERSION)' > $@
$(BUILD)/kama.driver.o: $(BUILD)/kama.version.stamp

# Compile: hand-written compiler sources live in src/, generated ones in out/<platform>/.
# -iquote $(BUILD) so #include "kama.parser.hpp" finds the generated header; -iquote src so both
# hand-written and generated TUs find the hand-written headers by bare name (every #include among
# them is a bare filename, which is why the move to src/ needed no source edit).
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote src -c $< -o $@

$(BUILD)/%.o: $(BUILD)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote src -c $< -o $@

# Windows links the compiler runtime STATICALLY, and this is not an optimization — without it the
# binary does not run at all outside the shell that built it. A mingw-w64 build links libstdc++-6.dll,
# libgcc_s_seh-1.dll and libwinpthread-1.dll out of /ucrt64/bin; nothing else on a Windows machine has
# those, so kama.exe died on startup with STATUS_DLL_NOT_FOUND (0xC0000135) the moment anything but an
# msys2 shell launched it — VS Code starting `kama lsp`, or a user running the RELEASE tarball, which
# ships bin/kama.exe and none of those DLLs. It surfaced as the language server silently never starting.
#
# Keyed off `uname -s` rather than a new switch because that is already how PLATFORM is decided, and the
# msys2 environments (MINGW64/UCRT64/CLANG64) all report MINGW*.
ifneq (,$(findstring MINGW,$(shell uname -s)))
LDFLAGS += -static
endif

$(BUILD)/kama: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Root ./kama — a symlink to this platform's binary, refreshed on every build. Consumers that
# must not care which platform built last (run_tests.sh, the VS Code extension) resolve
# out/$(PLATFORM)/kama directly instead. On msys2 `ln -s` degrades to a copy; that works too.
# PHONY on purpose: make stats through the symlink, so after the *other* platform built last it
# would see a newer file and skip the relink, leaving ./kama pointing at a foreign binary.
kama: $(BUILD)/kama
	ln -sf $(BUILD)/kama kama

# Cleans THIS platform only, on purpose: nuking out/ wholesale would defeat the coexistence
# the platform-scoped layout buys (a container `make clean` would wipe the host build).
clean:
	rm -rf $(BUILD)
	rm -f kama *~

test: kama
	./run_tests.sh

.PHONY: all clean test kama
