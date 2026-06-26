all: cstar

LLVMCONFIG = llvm-config
CPPFLAGS = `$(LLVMCONFIG) --cxxflags` -std=c++11
LDFLAGS = `$(LLVMCONFIG) --ldflags` -lpthread -ldl -lz -lncurses -rdynamic
LIBS = `$(LLVMCONFIG) --libs`

LEGACY_FLEX_WARNS = -Wno-deprecated-register

OBJECTS = cstar.lexer.o  \
          cstar.parser.o \
          cstar.driver.o \
          cstar.codegen.o \

cstar.parser.hpp: cstar.parser.cpp

cstar.parser.cpp: cstar.y
	bison -d $^ --verbose --report=all --report-file=bison_report

cstar.lexer.cpp: cstar.l cstar.parser.hpp
	flex  cstar.l

%.o: %.cpp
	clang++ -I /usr/local/llvm/include $(LEGACY_FLEX_WARNS) -c $(CPPFLAGS) $< -o $@

cstar: $(OBJECTS)
	clang++ -I /usr/local/llvm/include -g $^ $(LIBS) $(LDFLAGS) -o $@

clean:
	rm -f *.o *.bc *~ *.output
	rm -f cstar.lexer.cpp cstar.lexer.hpp cstar.parser.cpp cstar.parser.hpp
	rm -f cstar bison_report
