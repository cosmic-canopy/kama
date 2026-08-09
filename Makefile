all: kama

# Version: from the VERSION file (CI overrides with the git tag: make VERSION=1.2.3).
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

CXX      = clang++
CXXFLAGS = -std=c++14 -g -Wall -Wno-deprecated-register -DKAMA_VERSION='"$(VERSION)"'
# Appended to both compile and link (the link rule reuses CXXFLAGS). CI sets this to
# build a macOS universal binary: EXTRA_CXXFLAGS="-arch arm64 -arch x86_64".
CXXFLAGS += $(EXTRA_CXXFLAGS)

# All build artifacts live under build/<os>-<arch>/ (objects + generated parser/lexer + the
# binary), so the repo root stays sources-only AND a host (mach-o) build coexists with a
# container (ELF) build instead of clobbering it — no `make clean` when switching between
# `make` and `tools/cdev make`. The root ./kama is a symlink to the last-built platform's
# binary, keeping the stable path every consumer (CI, release, docs) already uses.
PLATFORM ?= $(shell uname -s)-$(shell uname -m)
BUILD     = build/$(PLATFORM)

# Inheritance is a BUILD-TIME feature switch on the compiler, not a runtime flag (docs/SPEC.md,
# docs/design/inheritance.md). Two purposes: isolate what the feature costs the compiler — answerable only
# by building both ways and subtracting — and be the extraction point if inheritance is ever dropped, since
# the `#if KAMA_INHERITANCE` blocks are then the deletion list.
#
#   make                        # inheritance in, up to 2 levels below a root
#   make KAMA_INHERITANCE=0     # a compiler without it, into build/<platform>-noinherit
#   make KAMA_INHERIT_DEPTH=1   # no middle layer: a root plus ONE derived level
#
# KAMA_INHERIT_DEPTH is the CEILING on what a hierarchy may ask for. Each `virtual`/`abstract class` still
# has to state its own budget (`virtual(maxDepth: 2)`), which may not exceed this.
#
# The no-inheritance build gets its OWN directory: objects compiled under different macro values must never
# mix, and sharing build/<platform> would silently do exactly that (make sees the .o as up to date).
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
BUILD = build/$(PLATFORM)-noinherit
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
            kama.agents.gen.o)

# The built-in kama sources embedded into the binary (prelude core + the always-in-scope smart-ptr
# triad). tools/embed_prelude.sh wraps them in raw-string literals -> build/kama.prelude.gen.cpp.
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

# Bison/flex: CLI -o/--defines/--header-file override the %output/%option names
# baked into the source, redirecting generated files into build/.
$(BUILD)/kama.parser.cpp $(BUILD)/kama.parser.hpp: kama.y | $(BUILD)
	$(BISON) -o $(BUILD)/kama.parser.cpp --defines=$(BUILD)/kama.parser.hpp kama.y

$(BUILD)/kama.lexer.cpp $(BUILD)/kama.lexer.hpp: kama.l $(BUILD)/kama.parser.hpp | $(BUILD)
	flex -o $(BUILD)/kama.lexer.cpp --header-file=$(BUILD)/kama.lexer.hpp kama.l

# Header dependencies (the implicit rules can't see #includes). Listing all
# project headers against every object is coarse but cheap, and prevents stale
# object/ABI-skew bugs when a class layout in a header changes.
HEADERS = kama.forward.h kama.context.h kama.ast.h kama.cemit.h kama.prelude.h kama.diagnostic.h kama.query.h kama.lsp.h
$(OBJECTS): $(HEADERS)

# Generated-header dependencies.
$(BUILD)/kama.lexer.o $(BUILD)/kama.parser.o $(BUILD)/kama.driver.o $(BUILD)/kama.cemit.o $(BUILD)/kama.comptime.o: $(BUILD)/kama.parser.hpp
$(BUILD)/kama.lexer.o $(BUILD)/kama.driver.o: $(BUILD)/kama.lexer.hpp

# Bison emits `int yynerrs` set-but-never-read. It's generated code (never edit it), so
# silence that one warning on this TU only — keeps the -Werror CI gate clean.
$(BUILD)/kama.parser.o: CXXFLAGS += -Wno-unused-but-set-variable

# Compile: hand-written sources live in the root, generated ones in build/.
# -Ibuild so #include "kama.parser.hpp" finds the generated header.
$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

$(BUILD)/%.o: $(BUILD)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

$(BUILD)/kama: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Root ./kama — a symlink to this platform's binary, refreshed on every build. Consumers that
# must not care which platform built last (run_tests.sh, the VS Code extension) resolve
# build/$(PLATFORM)/kama directly instead. On msys2 `ln -s` degrades to a copy; that works too.
# PHONY on purpose: make stats through the symlink, so after the *other* platform built last it
# would see a newer file and skip the relink, leaving ./kama pointing at a foreign binary.
kama: $(BUILD)/kama
	ln -sf $(BUILD)/kama kama

# Cleans THIS platform only, on purpose: nuking build/ wholesale would defeat the coexistence
# the platform-scoped layout buys (a container `make clean` would wipe the host build).
clean:
	rm -rf $(BUILD)
	rm -f kama *~

test: kama
	./run_tests.sh

.PHONY: all clean test kama
