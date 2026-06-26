#ifndef __CSTAR_CONTEXT_H__
#define __CSTAR_CONTEXT_H__

// LLVM-free parse-time context.
//
// This used to be the LLVM-heavy CodeGenContext (see cstar.codegen.h, kept on
// disk for reference only). The transpiler no longer depends on LLVM, so the
// parser/lexer/AST only need a tiny context that tracks source position, the
// module name, and error reporting. The C backend lives in cstar.cemit.*.

#include <string>
#include <memory>
#include <iostream>
#include "cstar.forward.h"

class CodeGenContext
{
    int          _mMaxErrorCount;
    int          _mCurrentErrorCount;
    SharedString _mName;

public:
    // Updated by the lexer as it scans (see SAVE_TOKEN/RETURN_TOKEN in cstar.l).
    int line;
    int col;

    explicit CodeGenContext(SharedString moduleName, int maxErrorCount = 10)
        : _mMaxErrorCount(maxErrorCount)
        , _mCurrentErrorCount(0)
        , _mName(moduleName)
        , line(CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE)
        , col(CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE)
    {
    }

    SharedString getModuleName() { return _mName; }

    bool isErrorLimitReached() { return _mCurrentErrorCount >= _mMaxErrorCount; }
    int  errorCount() const { return _mCurrentErrorCount; }

    // Returns non-zero (used as yyerror's return) so callers can propagate failure.
    bool handleError(int line, int column, const std::string& section, const std::string& error)
    {
        ++_mCurrentErrorCount;
        std::cerr << *_mName << ":" << line << ":" << column
                  << ": " << section << " error: " << error << std::endl;
        return 1;
    }
};

#endif // __CSTAR_CONTEXT_H__
