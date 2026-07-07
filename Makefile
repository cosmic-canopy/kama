all: kama

# Version: from the VERSION file (CI overrides with the git tag: make VERSION=1.2.3).
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

CXX      = clang++
CXXFLAGS = -std=c++14 -g -Wall -Wno-deprecated-register -DKAMA_VERSION='"$(VERSION)"'
# Appended to both compile and link (the link rule reuses CXXFLAGS). CI sets this to
# build a macOS universal binary: EXTRA_CXXFLAGS="-arch arm64 -arch x86_64".
CXXFLAGS += $(EXTRA_CXXFLAGS)

# All build artifacts live under build/ (objects + generated parser/lexer), so
# the repo root stays sources-only and host(mach-o)/container(ELF) objects can't
# collide. The kama binary stays at the root for stable tooling paths.
BUILD = build

# The grammar uses %code/api.pure full, which need bison >= 2.7. macOS ships
# 2.3, so prefer a Homebrew keg-only bison when present.
BISON = $(shell [ -x /opt/homebrew/opt/bison/bin/bison ] && echo /opt/homebrew/opt/bison/bin/bison || ([ -x /usr/local/opt/bison/bin/bison ] && echo /usr/local/opt/bison/bin/bison || echo bison))

OBJECTS = $(addprefix $(BUILD)/, \
            kama.lexer.o  \
            kama.parser.o \
            kama.ast.o    \
            kama.cemit.o  \
            kama.driver.o)

$(BUILD):
	mkdir -p $(BUILD)

# Bison/flex: CLI -o/--defines/--header-file override the %output/%option names
# baked into the source, redirecting generated files into build/.
$(BUILD)/kama.parser.cpp $(BUILD)/kama.parser.hpp: kama.y | $(BUILD)
	$(BISON) -o $(BUILD)/kama.parser.cpp --defines=$(BUILD)/kama.parser.hpp kama.y

$(BUILD)/kama.lexer.cpp $(BUILD)/kama.lexer.hpp: kama.l $(BUILD)/kama.parser.hpp | $(BUILD)
	flex -o $(BUILD)/kama.lexer.cpp --header-file=$(BUILD)/kama.lexer.hpp kama.l

# Header dependencies (the implicit rules can't see #includes). Listing all
# project headers against every object is coarse but cheap, and prevents stale
# object/ABI-skew bugs when a class layout in a header changes.
HEADERS = kama.forward.h kama.context.h kama.ast.h kama.cemit.h
$(OBJECTS): $(HEADERS)

# Generated-header dependencies.
$(BUILD)/kama.lexer.o $(BUILD)/kama.parser.o $(BUILD)/kama.driver.o $(BUILD)/kama.cemit.o: $(BUILD)/kama.parser.hpp
$(BUILD)/kama.lexer.o $(BUILD)/kama.driver.o: $(BUILD)/kama.lexer.hpp

# Compile: hand-written sources live in the root, generated ones in build/.
# -Ibuild so #include "kama.parser.hpp" finds the generated header.
$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

$(BUILD)/%.o: $(BUILD)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -iquote $(BUILD) -iquote . -c $< -o $@

kama: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o kama

test: kama
	./run_tests.sh

clean:
	rm -rf $(BUILD)
	rm -f kama *~

.PHONY: all clean test
