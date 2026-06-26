// Definitions for AST member functions that are not inline in cstar.ast.h.
//
// Historically these lived in the LLVM backend (cstar.codegen.cpp). With the
// pivot to a C transpiler the only thing the AST itself needs is the base
// constructor, which snapshots the current source position from the context.

#include "cstar.ast.h"
#include "cstar.context.h"

ASTNode::ASTNode(CodeGenContext& context)
    : line(context.line)
    , column(context.col)
{
}
