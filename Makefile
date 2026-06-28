all: cstar

# Version: from the VERSION file (CI overrides with the git tag: make VERSION=1.2.3).
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

CXX      = clang++
CXXFLAGS = -std=c++14 -g -Wall -Wno-deprecated-register -DCSTAR_VERSION='"$(VERSION)"'
# Appended to both compile and link (the link rule reuses CXXFLAGS). CI sets this to
# build a macOS universal binary: EXTRA_CXXFLAGS="-arch arm64 -arch x86_64".
CXXFLAGS += $(EXTRA_CXXFLAGS)

# All build artifacts live under build/ (objects + generated parser/lexer), so
# the repo root stays sources-only and host(mach-o)/container(ELF) objects can't
# collide. The cstar binary stays at the root for stable tooling paths.
BUILD = build

# The grammar uses %code/api.pure full, which need bison >= 2.7. macOS ships
# 2.3, so prefer a Homebrew keg-only bison when present.
BISON = $(shell [ -x /opt/homebrew/opt/bison/bin/bison ] && echo /opt/homebrew/opt/bison/bin/bison || ([ -x /usr/local/opt/bison/bin/bison ] && echo /usr/local/opt/bison/bin/bison || echo bison))

OBJECTS = $(addprefix $(BUILD)/, \
            cstar.lexer.o  \
            cstar.parser.o \
            cstar.ast.o    \
            cstar.cemit.o  \
            cstar.driver.o)

$(BUILD):
	mkdir -p $(BUILD)

# Bison/flex: CLI -o/--defines/--header-file override the %output/%option names
# baked into the source, redirecting generated files into build/.
$(BUILD)/cstar.parser.cpp $(BUILD)/cstar.parser.hpp: cstar.y | $(BUILD)
	$(BISON) -o $(BUILD)/cstar.parser.cpp --defines=$(BUILD)/cstar.parser.hpp cstar.y

$(BUILD)/cstar.lexer.cpp $(BUILD)/cstar.lexer.hpp: cstar.l $(BUILD)/cstar.parser.hpp | $(BUILD)
	flex -o $(BUILD)/cstar.lexer.cpp --header-file=$(BUILD)/cstar.lexer.hpp cstar.l

# Header dependencies (the implicit rules can't see #includes). Listing all
# project headers against every object is coarse but cheap, and prevents stale
# object/ABI-skew bugs when a class layout in a header changes.
HEADERS = cstar.forward.h cstar.context.h cstar.ast.h cstar.cemit.h
$(OBJECTS): $(HEADERS)

# Generated-header dependencies.
$(BUILD)/cstar.lexer.o $(BUILD)/cstar.parser.o $(BUILD)/cstar.driver.o $(BUILD)/cstar.cemit.o: $(BUILD)/cstar.parser.hpp
$(BUILD)/cstar.lexer.o $(BUILD)/cstar.driver.o: $(BUILD)/cstar.lexer.hpp

# Compile: hand-written sources live in the root, generated ones in build/.
# -Ibuild so #include "cstar.parser.hpp" finds the generated header.
$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

$(BUILD)/%.o: $(BUILD)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

cstar: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o cstar

test: cstar
	./run_tests.sh

clean:
	rm -rf $(BUILD)
	rm -f cstar *~

.PHONY: all clean test
