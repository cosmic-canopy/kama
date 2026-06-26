#include "cstar.cemit.h"
#include "cstar.ast.h"
#include "cstar.parser.hpp"   // bison token constants (PLUS, STAR, EQEQ, ...)

#include <cstdio>
#include <sstream>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CEmitter::CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives)
    : _out(out)
    , _sourcePath(sourcePath)
    , _lines(emitLineDirectives)
    , _unsupported(0)
{
}

void CEmitter::indent(int depth)
{
    for (int i = 0; i < depth; ++i) _out << "    ";
}

void CEmitter::line(int srcLine)
{
    if (_lines && srcLine > 0)
        _out << "#line " << srcLine << " \"" << _sourcePath << "\"\n";
}

void CEmitter::unsupported(const char* what, int srcLine)
{
    ++_unsupported;
    std::fprintf(stderr, "cstar: warning: unsupported %s at %s:%d (not yet lowered)\n",
                 what, _sourcePath.c_str(), srcLine);
    _out << "/* TODO(cstar): unsupported " << what << " */";
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

std::string CEmitter::cType(SharedIdentifier type)
{
    if (!type) return "void";
    switch (type->builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "int8_t";
        case IDENTIFIER_INT16_VAL:   return "int16_t";
        case IDENTIFIER_INT32_VAL:   return "int32_t";
        case IDENTIFIER_INT64_VAL:   return "int64_t";
        case IDENTIFIER_UINT8_VAL:   return "uint8_t";
        case IDENTIFIER_UINT16_VAL:  return "uint16_t";
        case IDENTIFIER_UINT32_VAL:  return "uint32_t";
        case IDENTIFIER_UINT64_VAL:  return "uint64_t";
        case IDENTIFIER_BOOL_VAL:    return "bool";
        case IDENTIFIER_FLOAT32_VAL: return "float";
        case IDENTIFIER_FLOAT64_VAL: return "double";
        case IDENTIFIER_STRING_VAL:  return "cstar_string";
        case IDENTIFIER_VOID_VAL:    return "void";
        default:
            // User-defined type (class/enum). Real mangling arrives with M4.
            return type->value ? *type->value : "void";
    }
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

std::string CEmitter::binaryOperator(int token)
{
    switch (token) {
        case PLUS:    return "+";
        case MINUS:   return "-";
        case STAR:    return "*";
        case SLASH:   return "/";
        case PERCENT: return "%";
        case LTLT:    return "<<";
        case GTGT:    return ">>";
        case LT:      return "<";
        case GT:      return ">";
        case LEQ:     return "<=";
        case GEQ:     return ">=";
        case EQEQ:    return "==";
        case NOTEQ:   return "!=";
        case AMP:     return "&";
        case CARET:   return "^";
        case BAR:     return "|";
        default:      return "/*?op*/";
    }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

std::string CEmitter::emitExpression(SharedExpression expr)
{
    if (!expr) return "";
    ASTNode* n = expr.get();

    if (auto* v = dynamic_cast<Int8Node*>(n))   return std::to_string((int)v->value);
    if (auto* v = dynamic_cast<Int16Node*>(n))  return std::to_string((int)v->value);
    if (auto* v = dynamic_cast<Int32Node*>(n))  return std::to_string(v->value);
    if (auto* v = dynamic_cast<Int64Node*>(n))  return std::to_string((long long)v->value) + "LL";
    if (auto* v = dynamic_cast<UInt8Node*>(n))  return std::to_string((unsigned)v->value) + "U";
    if (auto* v = dynamic_cast<UInt16Node*>(n)) return std::to_string((unsigned)v->value) + "U";
    if (auto* v = dynamic_cast<UInt32Node*>(n)) return std::to_string(v->value) + "U";
    if (auto* v = dynamic_cast<UInt64Node*>(n)) return std::to_string((unsigned long long)v->value) + "ULL";

    if (auto* v = dynamic_cast<Float64Node*>(n)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v->value);
        return buf;
    }
    if (auto* v = dynamic_cast<Float32Node*>(n)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.9gf", (double)v->value);
        return buf;
    }
    if (auto* v = dynamic_cast<BooleanNode*>(n)) return v->value ? "true" : "false";
    if (dynamic_cast<NullNode*>(n))              return "NULL";

    if (auto* v = dynamic_cast<StringNode*>(n)) {
        // Lower to a borrowed runtime string. The lexer already produced the
        // raw bytes; emit them as a C string literal (escaping is a later pass).
        const std::string& s = v->value ? *v->value : std::string();
        std::ostringstream os;
        os << "cstar_string_lit(\"";
        for (char c : s) {
            switch (c) {
                case '\\': os << "\\\\"; break;
                case '"':  os << "\\\""; break;
                case '\n': os << "\\n";  break;
                case '\t': os << "\\t";  break;
                case '\r': os << "\\r";  break;
                default:   os << c;      break;
            }
        }
        os << "\", " << s.size() << ")";
        return os.str();
    }

    if (auto* v = dynamic_cast<IdentifierNode*>(n)) {
        // Variable/parameter reference. Qualified/member resolution is later.
        std::string nm = v->value ? *v->value : "";
        // A ref/out parameter is a pointer in C; reads dereference it.
        if (_refParams.count(nm)) return "(*" + nm + ")";
        return nm;
    }

    if (auto* v = dynamic_cast<BinaryExpressionNode*>(n)) {
        return "(" + emitExpression(v->LHS) + " " + binaryOperator(v->token) + " "
                   + emitExpression(v->RHS) + ")";
    }

    if (auto* v = dynamic_cast<LogicalAndOrNode*>(n)) {
        std::string op = (v->token == ANDAND) ? "&&" : "||";
        return "(" + emitExpression(v->LHS) + " " + op + " " + emitExpression(v->RHS) + ")";
    }

    if (auto* v = dynamic_cast<TernaryExpressionNode*>(n)) {
        return "(" + emitExpression(v->condition) + " ? " + emitExpression(v->LHS)
                   + " : " + emitExpression(v->RHS) + ")";
    }

    if (auto* v = dynamic_cast<InvocationNode*>(n)) {
        return emitInvocation(v);
    }

    unsupported("expression", n->line);
    return "0";
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void CEmitter::emitBlock(BlockNode* block, int depth)
{
    _out << "{\n";
    if (block && block->statements) {
        for (auto& stmt : *block->statements)
            emitStatement(stmt, depth + 1);
    }
    indent(depth);
    _out << "}";
}

void CEmitter::emitStatement(SharedStatement stmt, int depth)
{
    if (!stmt) return;
    ASTNode* n = stmt.get();

    if (auto* block = dynamic_cast<BlockNode*>(n)) {
        line(n->line);
        indent(depth);
        emitBlock(block, depth);
        _out << "\n";
        return;
    }

    if (auto* decl = dynamic_cast<LocalVariableDeclaration*>(n)) {
        std::string ty = cType(decl->type);
        if (decl->variables) {
            for (auto& d : *decl->variables) {
                line(n->line);
                indent(depth);
                _out << ty << " " << (d->name && d->name->value ? *d->name->value : "");
                if (d->initializer)
                    _out << " = " << emitExpression(d->initializer);
                _out << ";\n";
            }
        }
        return;
    }

    if (auto* ret = dynamic_cast<ReturnNode*>(n)) {
        line(n->line);
        indent(depth);
        if (ret->expression)
            _out << "return " << emitExpression(ret->expression) << ";\n";
        else
            _out << "return;\n";
        return;
    }

    // Bare expression statement (e.g. an assignment or call used as a statement).
    if (dynamic_cast<ExpressionStatementNode*>(n)) {
        line(n->line);
        indent(depth);
        _out << emitExpression(std::dynamic_pointer_cast<ExpressionNode>(stmt)) << ";\n";
        return;
    }

    line(n->line);
    indent(depth);
    unsupported("statement", n->line);
    _out << "\n";
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

std::string CEmitter::cFunctionName(const std::string& cstarName)
{
    // The user `main` becomes `cstar_main`; a real C `main` wrapper is synthesized.
    return (cstarName == "main") ? "cstar_main" : cstarName;
}

std::string CEmitter::mangledFunctionName(FunctionDeclarationNode* fn, bool& isEntryPoint)
{
    std::string name = (fn->name && fn->name->value) ? *fn->name->value : "anon";
    isEntryPoint = (name == "main");
    return cFunctionName(name);
}

// Build the function signature table so call sites can reorder named arguments
// into C's positional order.
void CEmitter::collectSignatures(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get());
        if (!fn || !fn->name || !fn->name->value) continue;

        FuncSig sig;
        sig.cName = cFunctionName(*fn->name->value);
        if (fn->parameters) {
            for (auto& p : *fn->parameters) {
                ParamSig ps;
                ps.name  = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
                ps.byRef = p->modifier && p->modifier->value &&
                           (*p->modifier->value == "ref" || *p->modifier->value == "out");
                sig.params.push_back(ps);
            }
        }
        _funcs[*fn->name->value] = sig;
    }
}

// Lower a call, reordering named arguments to the callee's declared order.
std::string CEmitter::emitInvocation(InvocationNode* call)
{
    // Only simple/qualified function-name callees are handled here; method calls
    // (the `expression` form, e.g. obj.method(...)) arrive with classes (M4).
    if (!call->identifier || !call->identifier->value) {
        unsupported("method/indirect call", call->line);
        return "0";
    }
    std::string name = *call->identifier->value;

    auto it = _funcs.find(name);
    if (it == _funcs.end()) {
        // Unknown callee (extern/forward/typo). Best effort: emit args in source
        // order. Names can't be reordered without a signature.
        unsupported("call to unknown function (args kept in source order)", call->line);
        std::string s = cFunctionName(name) + "(";
        bool first = true;
        if (call->args) for (auto& a : *call->args) {
            if (!first) s += ", ";
            first = false;
            s += emitExpression(a->expression);
        }
        return s + ")";
    }
    const FuncSig& sig = it->second;

    // Index the supplied arguments by their (named) parameter name.
    std::map<std::string, ArgumentNode*> byName;
    if (call->args) {
        for (auto& a : *call->args) {
            if (a->name && a->name->value) byName[*a->name->value] = a.get();
        }
    }

    // NOTE: arguments are emitted in declared (param) order, which can differ
    // from source order. With side-effecting args this changes evaluation order;
    // a temp-hoisting pass (plan risk #3) is a later refinement.
    std::string s = sig.cName + "(";
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (i) s += ", ";
        const ParamSig& p = sig.params[i];
        auto found = byName.find(p.name);
        if (found == byName.end()) {
            unsupported("missing argument in call", call->line);
            s += "/*missing:" + p.name + "*/0";
            continue;
        }
        std::string val = emitExpression(found->second->expression);
        s += p.byRef ? ("&(" + val + ")") : val;
    }
    return s + ")";
}

bool CEmitter::paramByRef(FunctionParameterNode* p)
{
    return p && p->modifier && p->modifier->value &&
           (*p->modifier->value == "ref" || *p->modifier->value == "out");
}

// C parameter list. ref/out parameters become pointers.
std::string CEmitter::paramListC(FunctionDeclarationNode* fn)
{
    if (!fn->parameters || fn->parameters->empty())
        return "void";
    std::string s;
    bool first = true;
    for (auto& p : *fn->parameters) {
        if (!first) s += ", ";
        first = false;
        std::string nm = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
        s += cType(p->type) + (paramByRef(p.get()) ? "* " : " ") + nm;
    }
    return s;
}

void CEmitter::emitFunctionPrototype(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);
    _out << cType(fn->returnType) << " " << name << "(" << paramListC(fn) << ");\n";
}

void CEmitter::emitFunction(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);

    // Track by-ref params so reads of them in the body emit a dereference.
    _refParams.clear();
    if (fn->parameters) {
        for (auto& p : *fn->parameters) {
            if (paramByRef(p.get()) && p->identifier && p->identifier->value)
                _refParams.insert(*p->identifier->value);
        }
    }

    line(fn->line);
    _out << cType(fn->returnType) << " " << name << "(" << paramListC(fn) << ")\n";

    if (fn->block) {
        emitBlock(fn->block.get(), 0);
    } else {
        _out << "{\n}";
    }
    _out << "\n\n";

    _refParams.clear();

    if (isEntry) {
        // Synthesized portable entry point. Argument marshaling (List<String>)
        // arrives once collections land; for now args are ignored.
        _out << "int main(int argc, char** argv) {\n"
             << "    (void)argc; (void)argv;\n"
             << "    return (int)cstar_main();\n"
             << "}\n\n";
    }
}

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

int CEmitter::emit(SharedCompilationUnit unit)
{
    _out << "/* Generated by cstar. Do not edit. */\n";
    _out << "#include \"cstar_runtime.h\"\n\n";

    if (!unit || !unit->codeDeclarationList)
        return _unsupported;

    // Pass 0: collect signatures so calls can reorder named args.
    collectSignatures(unit);

    // Pass A: prototypes for all top-level functions (order-independent calls).
    bool any = false;
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            emitFunctionPrototype(fn);
            any = true;
        }
    }
    if (any) _out << "\n";

    // Pass B: definitions.
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            emitFunction(fn);
        } else if (decl) {
            unsupported("top-level declaration", decl->line);
            _out << "\n";
        }
    }

    return _unsupported;
}
