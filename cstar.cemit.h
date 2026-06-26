#ifndef __CSTAR_CEMIT_H__
#define __CSTAR_CEMIT_H__

// C-emitting backend. An external visitor over the AST (dispatch via
// dynamic_cast for now) that writes portable C. Kept entirely out of the AST
// headers so emission can evolve without recompiling the world.

#include <ostream>
#include <string>
#include "cstar.forward.h"

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // Emit a full translation unit. Returns the number of unsupported nodes
    // encountered (0 == fully lowered).
    int emit(SharedCompilationUnit unit);

private:
    std::ostream& _out;
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Declarations / top level
    void emitFunctionPrototype(FunctionDeclarationNode* fn);
    void emitFunction(FunctionDeclarationNode* fn);

    // Statements
    void emitStatement(SharedStatement stmt, int depth);
    void emitBlock(BlockNode* block, int depth);

    // Expressions -> C expression text
    std::string emitExpression(SharedExpression expr);

    // Helpers
    std::string cType(SharedIdentifier type);
    std::string mangledFunctionName(FunctionDeclarationNode* fn, bool& isEntryPoint);
    std::string binaryOperator(int token);

    void unsupported(const char* what, int srcLine);
};

#endif // __CSTAR_CEMIT_H__
