#ifndef __KAMA_CONTEXT_H__
#define __KAMA_CONTEXT_H__

// Parse-time context.
//
// The parser/lexer/AST need only a tiny context that tracks source position, the
// module name, and error reporting. The C backend lives in kama.cemit.*.

#include <string>
#include <memory>
#include <vector>
#include <iostream>
#include "kama.forward.h"
#include "kama.diagnostic.h"

class CodeGenContext
{
    int          _mMaxErrorCount;
    int          _mCurrentErrorCount;
    SharedString _mName;

public:
    // Updated by the lexer as it scans (see SAVE_TOKEN/RETURN_TOKEN in kama.l). `line`/`col` are the START
    // of the current token; `endLine`/`endCol` are one past its last column (the [start,end) span, T3).
    int line;
    int col;
    int endLine;
    int endCol;

    // Accumulated parse diagnostics (in ADDITION to the stderr print in handleError). A non-emitting
    // consumer (the LSP) reads these instead of scraping stderr; the CLI is unchanged.
    std::vector<Diagnostic> diagnostics;

    // How many times the TOP-LEVEL error-recovery arm fired (LSP M5.3, bumped in kama.y). Nonzero means
    // a whole `type`/`fn` was discarded, so every reference to it now reads as undeclared — the one
    // recovery outcome that carpets a file with false semantic errors. The LSP publishes semantic
    // diagnostics from a partial parse only when this is 0; the statement- and member-level arms lose
    // far less and do not set it.
    int droppedTopLevelDecl = 0;

    explicit CodeGenContext(SharedString moduleName, int maxErrorCount = 10)
        : _mMaxErrorCount(maxErrorCount)
        , _mCurrentErrorCount(0)
        , _mName(moduleName)
        , line(KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE)
        , col(KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE)
        , endLine(KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE)
        , endCol(KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE)
    {
    }

    SharedString getModuleName() { return _mName; }

    bool isErrorLimitReached() { return _mCurrentErrorCount >= _mMaxErrorCount; }
    int  errorCount() const { return _mCurrentErrorCount; }

    // Count an error past the reporting budget: no stderr line, no Diagnostic, but errorCount() must
    // still rise so every `errorCount() > 0` gate keeps failing the parse (see yyerror in kama.y).
    void countErrorOnly() { ++_mCurrentErrorCount; }

    // Returns non-zero (used as yyerror's return) so callers can propagate failure.
    bool handleError(int line, int column, const std::string& section, const std::string& error)
    {
        ++_mCurrentErrorCount;
        std::cerr << *_mName << ":" << line << ":" << column
                  << ": " << section << " error: " << error << std::endl;
        Diagnostic d;
        d.line = line; d.column = column;
        d.severity = DiagSeverity::Error;
        d.code = section;                 // e.g. "syntax"
        d.message = error;
        d.file = _mName ? *_mName : "";
        diagnostics.push_back(d);
        return 1;
    }
};

#endif // __KAMA_CONTEXT_H__
