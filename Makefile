all: cstar

CXX      = clang++
CXXFLAGS = -std=c++14 -g -Wall -Wno-deprecated-register

# The grammar uses %code/api.pure full, which need bison >= 2.7. macOS ships
# 2.3, so prefer a Homebrew keg-only bison when present.
BISON = $(shell [ -x /opt/homebrew/opt/bison/bin/bison ] && echo /opt/homebrew/opt/bison/bin/bison || ([ -x /usr/local/opt/bison/bin/bison ] && echo /usr/local/opt/bison/bin/bison || echo bison))

OBJECTS = cstar.lexer.o  \
          cstar.parser.o \
          cstar.ast.o    \
          cstar.cemit.o  \
          cstar.driver.o

# Bison emits cstar.parser.cpp/.hpp (see %output/%defines in cstar.y).
cstar.parser.cpp cstar.parser.hpp: cstar.y
	$(BISON) cstar.y

# Flex emits cstar.lexer.cpp/.hpp (see %option outfile/header-file in cstar.l).
cstar.lexer.cpp cstar.lexer.hpp: cstar.l cstar.parser.hpp
	flex cstar.l

# Generated-header dependencies (implicit rule below can't see these).
cstar.lexer.o cstar.parser.o cstar.driver.o cstar.cemit.o: cstar.parser.hpp
cstar.lexer.o cstar.driver.o: cstar.lexer.hpp

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

cstar: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: cstar
	./run_tests.sh

clean:
	rm -f *.o *~ *.output bison_report
	rm -f cstar.lexer.cpp cstar.lexer.hpp cstar.parser.cpp cstar.parser.hpp
	rm -f cstar

.PHONY: all clean test
