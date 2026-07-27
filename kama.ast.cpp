// Definitions for AST member functions that are not inline in kama.ast.h.
//
// Historically these lived in the LLVM backend (kama.codegen.cpp). With the
// pivot to a C transpiler the only thing the AST itself needs is the base
// constructor, which snapshots the current source position from the context.

#include "kama.ast.h"
#include "kama.context.h"

ASTNode::ASTNode(CodeGenContext& context)
    : line(context.line)
    , column(context.col)
    , endLine(context.endLine)
    , endColumn(context.endCol)
{
}
