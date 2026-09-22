#ifndef __KAMA_CONTEXT_H__
#define __KAMA_CONTEXT_H__

// Parse-time context.
//
// The parser/lexer/AST need only a tiny context that tracks source position, the
// module name, and error reporting. The C backend lives in kama.cemit.*.

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <iostream>
#include "kama.forward.h"
#include "kama.diagnostic.h"
#include "kama.query.h"

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

    // Per-SEGMENT source spans for the `::`-separated name lists the grammar keeps as plain strings —
    // a qualifier (`Color::Green`), an import path (`std::collections`), an export manifest (LSP M6 B3f).
    // Those lists are `%type <strings>` and read as strings by ~107 places in the emitter, so the
    // positions travel BESIDE them rather than in them.
    //
    // Keyed by the LIST OBJECT, not a single pending vector: qualifiers NEST, so in `A::B<C::D>` the
    // inner `C::` reduces while the outer `A::B<…>` reduction is still pending and would clobber it.
    // Each list object is exactly one source occurrence (the recursive arms push onto `$1` and return
    // it), so the pointer removes any dependence on reduction order.
    std::map<const StringList*, std::vector<SrcRange>> listSegPos;

    // Every `a::b::X` the source spells OUTSIDE an import (KR-87): a module path belongs in the import list
    // only, and judging each occurrence where it was written is what makes that rule total. Resolution asks
    // about qualified names from ~80 places, many with no node to report at, so it cannot be the judge.
    // Moved onto the CompilationUnit when the unit reduces.
    std::vector<SharedIdentifier> qualifiedIds;
    void unjudgeQualified(const IdentifierNode* id)       // a `friend` grant's path — see friend_declaration
    {
        for (auto it = qualifiedIds.rbegin(); it != qualifiedIds.rend(); ++it)
            if (it->get() == id) { qualifiedIds.erase(std::next(it).base()); return; }
    }

    // An integer literal whose magnitude is EXACTLY one past its type's maximum — the only out-of-range
    // magnitude a unary minus can rescue, because the negation fits exactly. `2147483648` unsuffixed IS
    // INT32_MIN written down; so is `128i8` for `int8`. The check therefore cannot fire at the literal:
    // it parks itself here, `MINUS` takes the entry when it consumes one (the node already holds the
    // wrapped value, which is the right answer at every width), and compilation_unit reports whatever is
    // left over. Keyed by the NODE, like listSegPos and for the same reason — literals nest inside other
    // reductions, so a single pending slot would clobber.
    //
    // The entry carries its own MESSAGE rather than just the digits: the two producers phrase it
    // differently (an unsuffixed literal is told to write the width it means; a suffixed one already
    // stated a width and is told the range that width has), and a reporter that branched on which
    // producer parked the entry would be the same fact written twice.
    struct WideLit { std::string msg; int line; int col; };
    std::map<const void*, WideLit> pendingWideLits;

    // TAKE, not read — see listSegPos. True iff `node` was a parked wide literal, which is exactly the
    // case where a unary minus makes it representable.
    bool takeWideLit(const void* node)
    {
        auto it = pendingWideLits.find(node);
        if (it == pendingWideLits.end()) return false;
        pendingWideLits.erase(it);
        return true;
    }

    // Anything still parked at the end of the parse was never negated, so it really does not fit.
    void reportPendingWideLits()
    {
        for (auto& kv : pendingWideLits)
            handleError(kv.second.line, kv.second.col, "Parse", kv.second.msg.c_str());
        pendingWideLits.clear();
    }

    void stampSeg(const StringList* list, int line, int col, int endLine, int endCol)
    {
        if (list) listSegPos[list].push_back(SrcRange{ line, col, endLine, endCol });
    }

    // TAKE, not read: erasing at the consuming reduction bounds the map to the lists still in flight, and
    // stops a list discarded by an error-recovery arm from leaving a stale entry behind for whatever the
    // allocator hands out at that address next.
    std::vector<SrcRange> takeSegs(const StringList* list)
    {
        auto it = listSegPos.find(list);
        if (it == listSegPos.end()) return {};
        std::vector<SrcRange> out = std::move(it->second);
        listSegPos.erase(it);
        return out;
    }

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
