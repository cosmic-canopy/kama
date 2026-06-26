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

std::string CEmitter::assignmentOperator(int token)
{
    switch (token) {
        case EQ:      return "=";
        case PLUSEQ:  return "+=";
        case MINUSEQ: return "-=";
        case STAREQ:  return "*=";
        case DIVEQ:   return "/=";
        case MODEQ:   return "%=";
        case XOREQ:   return "^=";
        case ANDEQ:   return "&=";
        case OREQ:    return "|=";
        case GTGTEQ:  return ">>=";
        case LTLTEQ:  return "<<=";
        default:      return "/*?assign*/=";
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
        // An unqualified name that is a field of the enclosing class (and not a
        // local/param) resolves to self->field.
        if (_currentClass && !_localTypes.count(nm) && _currentClass->fieldNames.count(nm))
            return "self->" + nm;
        return nm;
    }

    if (dynamic_cast<ThisAccessNode*>(n)) return "self";

    if (auto* v = dynamic_cast<MemberAccessNode*>(n)) {
        return emitMemberAccess(v);
    }

    if (auto* v = dynamic_cast<ObjectCreationNode*>(n)) {
        // `new T(...)` is supported only as a local-variable initializer in M4
        // (handled in LocalVariableDeclaration). Bare expression position needs
        // a temp/statement context that arrives with RAII (M5).
        unsupported("`new` outside a local-variable initializer", v->line);
        return "0";
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

    if (auto* v = dynamic_cast<AssignmentNode*>(n)) {
        return "(" + emitExpression(v->unaryExpression) + " "
                   + assignmentOperator(v->token) + " " + emitExpression(v->expression) + ")";
    }

    if (auto* v = dynamic_cast<PreIncrDecrNode*>(n)) {
        std::string op = (v->token == PLUSPLUS) ? "++" : "--";
        return "(" + op + emitExpression(v->expression) + ")";
    }

    if (auto* v = dynamic_cast<PostIncrDecrNode*>(n)) {
        std::string op = (v->token == PLUSPLUS) ? "++" : "--";
        return "(" + emitExpression(v->expression) + op + ")";
    }

    if (auto* v = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        std::string op;
        switch (v->token) {
            case EXCLAMATION: op = "!"; break;
            case TILDE:       op = "~"; break;
            case PLUS:        op = "+"; break;
            case MINUS:       op = "-"; break;
            default:          op = "/*?unary*/"; break;
        }
        return "(" + op + emitExpression(v->expression) + ")";
    }

    if (auto* v = dynamic_cast<CastNode*>(n)) {
        return "((" + cType(v->type) + ")(" + emitExpression(v->unaryExpression) + "))";
    }

    unsupported("expression", n->line);
    return "0";
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

// --- RAII cleanup helpers (M5) ---------------------------------------------

bool CEmitter::stmtIsJump(SharedStatement s)
{
    ASTNode* n = s.get();
    return dynamic_cast<ReturnNode*>(n) || dynamic_cast<BreakNode*>(n) || dynamic_cast<ContinueNode*>(n);
}

void CEmitter::emitScopeCleanup(const Scope& s, int depth)
{
    for (auto it = s.locals.rbegin(); it != s.locals.rend(); ++it) {
        indent(depth);
        _out << it->className << "__dtor(&" << it->cVar << ");\n";
    }
}

// Destroy scopes from innermost up to & including the nearest loop boundary.
void CEmitter::emitUnwindToLoop(int depth)
{
    for (size_t i = _scopes.size(); i-- > 0; ) {
        emitScopeCleanup(_scopes[i], depth);
        if (_scopes[i].isLoopBoundary) break;
    }
}

// Destroy all scopes from innermost down to the function root (for return).
void CEmitter::emitUnwindAll(int depth)
{
    for (size_t i = _scopes.size(); i-- > 0; )
        emitScopeCleanup(_scopes[i], depth);
}

void CEmitter::recordDestructibleLocal(const std::string& cVar, const std::string& className)
{
    if (!_scopes.empty())
        _scopes.back().locals.push_back({cVar, className});
}

// --- Blocks ----------------------------------------------------------------

void CEmitter::emitBlock(BlockNode* block, int depth)
{
    emitBlockScoped(block, depth, /*loopBoundary=*/false, /*functionRoot=*/false);
}

void CEmitter::emitBlockScoped(BlockNode* block, int depth, bool loopBoundary, bool functionRoot)
{
    Scope sc; sc.isLoopBoundary = loopBoundary; sc.isFunctionRoot = functionRoot;
    _scopes.push_back(sc);

    _out << "{\n";
    SharedStatement last;
    if (block && block->statements) {
        for (auto& stmt : *block->statements) { emitStatement(stmt, depth + 1); last = stmt; }
    }
    // Fall-through cleanup, unless the block already exited via a jump (which
    // ran its own cleanup) — the double-destruction guard.
    if (!(last && stmtIsJump(last)))
        emitScopeCleanup(_scopes.back(), depth + 1);

    indent(depth);
    _out << "}";
    _scopes.pop_back();
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
        bool cls = isClass(ty);
        if (decl->variables) {
            for (auto& d : *decl->variables) {
                std::string nm = (d->name && d->name->value) ? *d->name->value : "";
                _localTypes[nm] = cls ? ty : "";   // record all names (shadow fields)

                if (!cls) {
                    line(n->line); indent(depth);
                    _out << ty << " " << nm;
                    if (d->initializer) _out << " = " << emitExpression(d->initializer);
                    _out << ";\n";
                    continue;
                }

                // Class-typed local: declare the value, then construct in place.
                line(n->line); indent(depth);
                _out << ty << " " << nm << ";\n";
                // Track for RAII cleanup at scope exit (assumes init-at-decl).
                if (_classes[ty].destructible) recordDestructibleLocal(nm, ty);
                if (!d->initializer) continue;

                if (auto* oc = dynamic_cast<ObjectCreationNode*>(d->initializer.get())) {
                    std::string octy = cType(oc->type);
                    if (isClass(octy) && _classes[octy].hasCtor) {
                        line(n->line); indent(depth);
                        _out << emitCtorCall(nm, _classes[octy], oc->args, n->line) << ";\n";
                    } else if (!isClass(octy)) {
                        unsupported("`new` of a non-class type", n->line);
                    }
                    // class with no ctor: left default-initialized
                } else {
                    // Copy-initialize from another expression.
                    line(n->line); indent(depth);
                    _out << nm << " = " << emitExpression(d->initializer) << ";\n";
                }
            }
        }
        return;
    }

    if (auto* ret = dynamic_cast<ReturnNode*>(n)) {
        line(n->line);
        // Capture the return value BEFORE running any destructors (it may
        // reference locals about to be destroyed), then unwind, then return.
        if (ret->expression && _currentReturnCType != "void") {
            std::string tmp = "__ret_" + std::to_string(_tempCounter++);
            indent(depth);
            _out << _currentReturnCType << " " << tmp << " = " << emitExpression(ret->expression) << ";\n";
            emitUnwindAll(depth);
            indent(depth); _out << "return " << tmp << ";\n";
        } else {
            if (ret->expression) { indent(depth); _out << emitExpression(ret->expression) << ";\n"; }
            emitUnwindAll(depth);
            indent(depth); _out << "return;\n";
        }
        return;
    }

    if (auto* f = dynamic_cast<IfNode*>(n)) {
        line(n->line); indent(depth);
        _out << "if (" << emitExpression(f->booleanExpression) << ") ";
        emitBody(f->ifStatement, depth, /*loopBoundary=*/false);
        if (f->elseStatement) { _out << " else "; emitBody(f->elseStatement, depth, false); }
        _out << "\n";
        return;
    }

    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        line(n->line); indent(depth);
        _out << "while (" << emitExpression(w->booleanExpression) << ") ";
        emitBody(w->whileStatement, depth, /*loopBoundary=*/true);
        _out << "\n";
        return;
    }

    if (auto* d = dynamic_cast<DoWhileNode*>(n)) {
        line(n->line); indent(depth);
        _out << "do ";
        emitBody(d->doWhileStatement, depth, /*loopBoundary=*/true);
        _out << " while (" << emitExpression(d->booleanExpression) << ");\n";
        return;
    }

    if (auto* f = dynamic_cast<ForNode*>(n)) {
        line(n->line); indent(depth);
        _out << "for (" << emitForClause(f->initializerStatements) << "; "
             << (f->booleanExpression ? emitExpression(f->booleanExpression) : std::string()) << "; "
             << emitForClause(f->iteratorStatements) << ") ";
        emitBody(f->body, depth, /*loopBoundary=*/true);
        _out << "\n";
        return;
    }

    if (dynamic_cast<BreakNode*>(n)) {
        line(n->line);
        emitUnwindToLoop(depth);   // dtors must run before the break keyword
        indent(depth); _out << "break;\n";
        return;
    }
    if (dynamic_cast<ContinueNode*>(n)) {
        line(n->line);
        emitUnwindToLoop(depth);
        indent(depth); _out << "continue;\n";
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(n)) {
        line(n->line); indent(depth);
        _out << "switch (" << emitExpression(sw->expression) << ") {\n";
        if (sw->switchsections) {
            for (auto& sec : *sw->switchsections) {
                if (sec->labels) {
                    for (auto& lbl : *sec->labels) {
                        indent(depth + 1);
                        if (lbl->isDefault())
                            _out << "default:\n";
                        else
                            _out << "case " << emitExpression(lbl->constantExpression) << ":\n";
                    }
                }
                SharedStatement last;
                if (sec->statementList) {
                    for (auto& st : *sec->statementList) { emitStatement(st, depth + 2); last = st; }
                }
                // cstar switch sections don't fall through; add break unless the
                // section already ends in a break/return.
                bool ends = last && (dynamic_cast<BreakNode*>(last.get()) ||
                                     dynamic_cast<ReturnNode*>(last.get()));
                if (!ends) { indent(depth + 2); _out << "break;\n"; }
            }
        }
        indent(depth);
        _out << "}\n";
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

// A brace-wrapped body for if/while/for/do. Reuses an existing block as-is.
// Always introduces a scope (even for a single-statement body) so RAII cleanup
// and the loop boundary are tracked correctly.
void CEmitter::emitBody(SharedStatement stmt, int depth, bool loopBoundary)
{
    if (auto* b = dynamic_cast<BlockNode*>(stmt.get())) {
        emitBlockScoped(b, depth, loopBoundary, /*functionRoot=*/false);
    } else {
        Scope sc; sc.isLoopBoundary = loopBoundary;
        _scopes.push_back(sc);
        _out << "{\n";
        emitStatement(stmt, depth + 1);
        if (!stmtIsJump(stmt))
            emitScopeCleanup(_scopes.back(), depth + 1);
        indent(depth);
        _out << "}";
        _scopes.pop_back();
    }
}

// A statement rendered for a for-clause: no trailing semicolon or newline.
std::string CEmitter::inlineStatement(SharedStatement stmt)
{
    if (!stmt) return "";
    ASTNode* n = stmt.get();

    if (auto* decl = dynamic_cast<LocalVariableDeclaration*>(n)) {
        std::string s = cType(decl->type) + " ";
        bool first = true;
        if (decl->variables) {
            for (auto& d : *decl->variables) {
                if (!first) s += ", ";
                first = false;
                s += (d->name && d->name->value) ? *d->name->value : "";
                if (d->initializer) s += " = " + emitExpression(d->initializer);
            }
        }
        return s;
    }

    if (dynamic_cast<ExpressionStatementNode*>(n))
        return emitExpression(std::dynamic_pointer_cast<ExpressionNode>(stmt));

    unsupported("for-clause statement", n->line);
    return "";
}

std::string CEmitter::emitForClause(SharedStatementList list)
{
    if (!list || list->empty()) return "";
    std::string s;
    bool first = true;
    for (auto& st : *list) {
        if (!first) s += ", ";
        first = false;
        s += inlineStatement(st);
    }
    return s;
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

std::vector<ParamSig> CEmitter::paramSigsOf(SharedParameterList params)
{
    std::vector<ParamSig> out;
    if (params) {
        for (auto& p : *params) {
            ParamSig ps;
            ps.name  = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            ps.byRef = paramByRef(p.get());
            out.push_back(ps);
        }
    }
    return out;
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
        sig.cName  = cFunctionName(*fn->name->value);
        sig.params = paramSigsOf(fn->parameters);
        _funcs[*fn->name->value] = sig;
    }
}

// Build the class table: ordered fields, methods, and the (single) constructor.
void CEmitter::collectClasses(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->name || !cd->name->value) continue;

        ClassInfo ci;
        ci.name = *cd->name->value;
        ci.node = cd;

        if (cd->baseTypes)
            unsupported("class inheritance (extends/implements) — deferred to M6", cd->line);

        if (cd->members) {
            for (auto& m : *cd->members) {
                ASTNode* mn = m.get();
                if (auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(mn)) {
                    if (fd->declarators) {
                        for (auto& d : *fd->declarators) {
                            FieldInfo fi;
                            fi.name        = (d->name && d->name->value) ? *d->name->value : "";
                            fi.type        = fd->type;
                            fi.initializer = d->initializer;
                            ci.fields.push_back(fi);
                            ci.fieldNames.insert(fi.name);
                        }
                    }
                } else if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(mn)) {
                    if (md->name && md->name->value) {
                        MethodInfo mi;
                        mi.cName      = ci.name + "__" + *md->name->value;
                        mi.returnType = md->returnType;
                        mi.params     = paramSigsOf(md->params);
                        mi.node       = md;
                        ci.methods[*md->name->value] = mi;
                    }
                } else if (auto* cc = dynamic_cast<ClassConstructorDeclarationNode*>(mn)) {
                    if (ci.hasCtor)
                        unsupported("multiple constructors (no overloading yet)", cc->line);
                    ci.hasCtor   = true;
                    ci.ctorNode  = cc;
                    if (cc->declarator) ci.ctorParams = paramSigsOf(cc->declarator->params);
                } else if (auto* dd = dynamic_cast<ClassDestructorDeclarationNode*>(mn)) {
                    ci.hasDtor  = true;
                    ci.dtorNode = dd;
                } else if (dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    unsupported("class const member", mn->line);
                } else if (dynamic_cast<ClassOperatorDeclarationNode*>(mn)) {
                    unsupported("operator overload — deferred", mn->line);
                }
            }
        }
        _classes[ci.name] = ci;
    }
}

// A class is destructible if it declares a dtor or has a destructible field
// (transitive). Fixed-point pass — cycle-safe by construction.
void CEmitter::computeDestructible()
{
    for (auto& kv : _classes) kv.second.destructible = kv.second.hasDtor;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& kv : _classes) {
            ClassInfo& ci = kv.second;
            if (ci.destructible) continue;
            for (auto& f : ci.fields) {
                auto it = _classes.find(cType(f.type));
                if (it != _classes.end() && it->second.destructible) {
                    ci.destructible = true;
                    changed = true;
                    break;
                }
            }
        }
    }
}

bool CEmitter::isExtern(FunctionDeclarationNode* fn)
{
    return fn && fn->modifier && fn->modifier->value && *fn->modifier->value == "extern";
}

// Emit `cName(leadArg, <args reordered to declared param order>)`.
//
// NOTE: arguments are emitted in declared (param) order, which can differ from
// source order. With side-effecting args this changes evaluation order; a
// temp-hoisting pass (plan risk #3) is a later refinement.
std::string CEmitter::emitReorderedCall(const std::string& cName, const std::string& leadArg,
                                        const std::vector<ParamSig>& params,
                                        SharedArgumentList args, int srcLine)
{
    std::map<std::string, ArgumentNode*> byName;
    if (args)
        for (auto& a : *args)
            if (a->name && a->name->value) byName[*a->name->value] = a.get();

    std::string s = cName + "(";
    bool first = true;
    if (!leadArg.empty()) { s += leadArg; first = false; }
    for (auto& p : params) {
        if (!first) s += ", ";
        first = false;
        auto f = byName.find(p.name);
        if (f == byName.end()) { unsupported("missing argument in call", srcLine); s += "0"; continue; }
        std::string val = emitExpression(f->second->expression);
        s += p.byRef ? ("&(" + val + ")") : val;
    }
    return s + ")";
}

// Lower a call, reordering named arguments to the callee's declared order.
std::string CEmitter::emitInvocation(InvocationNode* call)
{
    // Expression-form callee (this.method(...), parenthesized, etc.).
    if (!call->identifier || !call->identifier->value) {
        if (call->expression) {
            if (auto* ma = dynamic_cast<MemberAccessNode*>(call->expression.get()))
                return emitMethodCall(call, ma);
        }
        unsupported("indirect call", call->line);
        return "0";
    }

    std::string name = *call->identifier->value;

    // A qualified callee `recv.method` parses as identifier{value=method,
    // qualifier=[recv...]}. (With no namespaces in M4, a qualifier means a method
    // receiver.) Build the receiver C-expression + class from the qualifier chain.
    SharedStringList qual = call->identifier->qualifier;
    if (qual && !qual->empty()) {
        std::string recvExpr, recvClass;
        const std::string& head = *(*qual)[0];
        if (_localTypes.count(head)) { recvExpr = head; recvClass = _localTypes[head]; }
        else if (_currentClass && _currentClass->fieldNames.count(head)) {
            recvExpr = "self->" + head;
            for (auto& f : _currentClass->fields)
                if (f.name == head && f.type) recvClass = cType(f.type);
        } else { unsupported("method receiver not a known object", call->line); return "0"; }

        for (size_t i = 1; i < qual->size(); ++i) {
            if (recvClass.empty() || !_classes.count(recvClass)) {
                unsupported("method receiver chain not resolvable", call->line); return "0";
            }
            const std::string& fld = *(*qual)[i];
            std::string fcls;
            for (auto& f : _classes[recvClass].fields)
                if (f.name == fld && f.type) fcls = cType(f.type);
            recvExpr  = "(" + recvExpr + ")." + fld;
            recvClass = fcls;
        }

        if (recvClass.empty() || !_classes.count(recvClass)) {
            unsupported("method call on unresolved receiver", call->line); return "0";
        }
        ClassInfo& ci = _classes[recvClass];
        auto mit = ci.methods.find(name);
        if (mit == ci.methods.end()) { unsupported("unknown method", call->line); return "0"; }
        return emitReorderedCall(mit->second.cName, "&(" + recvExpr + ")",
                                 mit->second.params, call->args, call->line);
    }

    // Free-function call.
    auto it = _funcs.find(name);
    if (it == _funcs.end()) {
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
    return emitReorderedCall(it->second.cName, "", it->second.params, call->args, call->line);
}

bool CEmitter::paramByRef(FunctionParameterNode* p)
{
    return p && p->modifier && p->modifier->value &&
           (*p->modifier->value == "ref" || *p->modifier->value == "out");
}

// C parameter list. ref/out parameters become pointers. When selfType is set,
// a leading `selfType* self` is prepended (for methods/constructors).
std::string CEmitter::paramListC(SharedParameterList params, const char* selfType)
{
    std::string s;
    bool first = true;
    if (selfType) { s += std::string(selfType) + "* self"; first = false; }
    if (params) {
        for (auto& p : *params) {
            if (!first) s += ", ";
            first = false;
            std::string nm = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            s += cType(p->type) + (paramByRef(p.get()) ? "* " : " ") + nm;
        }
    }
    if (s.empty()) s = "void";
    return s;
}

void CEmitter::emitFunctionPrototype(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);
    _out << cType(fn->returnType) << " " << name << "(" << paramListC(fn->parameters, nullptr) << ");\n";
}

void CEmitter::emitFunction(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);

    // Track by-ref params (deref on read) and param classes (for member calls).
    _refParams.clear();
    _localTypes.clear();
    _currentClass = nullptr;
    if (fn->parameters) {
        for (auto& p : *fn->parameters) {
            if (!p->identifier || !p->identifier->value) continue;
            const std::string& pn = *p->identifier->value;
            if (paramByRef(p.get())) _refParams.insert(pn);
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = isClass(pty) ? pty : "";   // record all names (shadow fields)
        }
    }

    _currentReturnCType = cType(fn->returnType);
    _tempCounter = 0;
    _scopes.clear();

    line(fn->line);
    _out << cType(fn->returnType) << " " << name << "(" << paramListC(fn->parameters, nullptr) << ")\n";

    if (fn->block) {
        emitBlockScoped(fn->block.get(), 0, /*loopBoundary=*/false, /*functionRoot=*/true);
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
// Classes
// ---------------------------------------------------------------------------

void CEmitter::emitStruct(ClassInfo& ci)
{
    _out << "typedef struct " << ci.name << " {\n";
    for (auto& f : ci.fields) {
        indent(1);
        _out << cType(f.type) << " " << f.name << ";\n";
    }
    if (ci.fields.empty()) {
        indent(1);
        _out << "char __empty; /* C forbids empty structs */\n";
    }
    _out << "} " << ci.name << ";\n\n";
}

void CEmitter::emitClassPrototypes(ClassInfo& ci)
{
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator)
        _out << "void " << ci.name << "__ctor("
             << paramListC(ci.ctorNode->declarator->params, ci.name.c_str()) << ");\n";
    if (ci.destructible)
        _out << "void " << ci.name << "__dtor(" << ci.name << "* self);\n";
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        _out << cType(mi.returnType) << " " << mi.cName << "("
             << paramListC(mi.node->params, ci.name.c_str()) << ");\n";
    }
}

// void Name__dtor(Name* self): user body first, then destructible fields in
// reverse declaration order. (Early return inside a dtor body is unsupported.)
void CEmitter::emitDtorDefinition(ClassInfo& ci)
{
    line(ci.dtorNode ? ci.dtorNode->line : ci.node->line);
    _currentClass = &ci;
    _refParams.clear();
    _localTypes.clear();
    _currentReturnCType = "void";
    _tempCounter = 0;
    _scopes.clear();
    Scope root; root.isFunctionRoot = true;
    _scopes.push_back(root);

    _out << "void " << ci.name << "__dtor(" << ci.name << "* self)\n{\n";

    SharedStatement last;
    if (ci.dtorNode && ci.dtorNode->body && ci.dtorNode->body->statements) {
        for (auto& st : *ci.dtorNode->body->statements) { emitStatement(st, 1); last = st; }
    }
    if (!(last && stmtIsJump(last)))
        emitScopeCleanup(_scopes.back(), 1);

    // Field destructors, reverse declaration order.
    for (auto it = ci.fields.rbegin(); it != ci.fields.rend(); ++it) {
        auto cit = _classes.find(cType(it->type));
        if (cit != _classes.end() && cit->second.destructible) {
            indent(1);
            _out << cit->second.name << "__dtor(&self->" << it->name << ");\n";
        }
    }
    _out << "}\n\n";

    _scopes.clear();
    _currentClass = nullptr;
}

// Emit a method or constructor body with `self`/field/param context set up.
void CEmitter::emitMethodOrCtorBody(const std::string& cName, const char* retType,
                                    SharedParameterList params, SharedBlock body,
                                    ClassInfo& owner, bool isCtor)
{
    _currentClass = &owner;
    _refParams.clear();
    _localTypes.clear();
    _currentReturnCType = retType;
    _tempCounter = 0;
    _scopes.clear();
    Scope root; root.isFunctionRoot = true;
    _scopes.push_back(root);
    if (params) {
        for (auto& p : *params) {
            if (!p->identifier || !p->identifier->value) continue;
            const std::string& pn = *p->identifier->value;
            if (paramByRef(p.get())) _refParams.insert(pn);
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = isClass(pty) ? pty : "";   // record all names (shadow fields)
        }
    }

    _out << retType << " " << cName << "(" << paramListC(params, owner.name.c_str()) << ")\n{\n";

    if (isCtor) {
        for (auto& f : owner.fields) {
            if (f.initializer) {
                indent(1);
                _out << "self->" << f.name << " = " << emitExpression(f.initializer) << ";\n";
            }
        }
    }
    SharedStatement last;
    if (body && body->statements) {
        for (auto& st : *body->statements) { emitStatement(st, 1); last = st; }
    }
    if (!(last && stmtIsJump(last)))
        emitScopeCleanup(_scopes.back(), 1);
    _out << "}\n\n";

    _scopes.clear();
    _currentClass = nullptr;
    _refParams.clear();
    _localTypes.clear();
}

void CEmitter::emitClassDefinitions(ClassInfo& ci)
{
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator) {
        line(ci.ctorNode->line);
        emitMethodOrCtorBody(ci.name + "__ctor", "void",
                             ci.ctorNode->declarator->params, ci.ctorNode->body, ci, true);
    }
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        line(mi.node->line);
        std::string ret = cType(mi.returnType);
        emitMethodOrCtorBody(mi.cName, ret.c_str(), mi.node->params, mi.node->body, ci, false);
    }
    if (ci.destructible)
        emitDtorDefinition(ci);
}

// The static class type of an expression ("" if primitive/unknown).
std::string CEmitter::exprClass(SharedExpression e)
{
    if (!e) return "";
    ASTNode* n = e.get();

    if (dynamic_cast<ThisAccessNode*>(n))
        return _currentClass ? _currentClass->name : "";

    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (!id->value) return "";
        auto it = _localTypes.find(*id->value);
        if (it != _localTypes.end()) return it->second;
        if (_currentClass) {
            for (auto& f : _currentClass->fields)
                if (f.name == *id->value && f.type && isClass(cType(f.type)))
                    return cType(f.type);
        }
        return "";
    }

    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        std::string recv = exprClass(ma->expression);
        if (!recv.empty() && ma->identifier && ma->identifier->value && _classes.count(recv)) {
            for (auto& f : _classes[recv].fields)
                if (f.name == *ma->identifier->value && f.type && isClass(cType(f.type)))
                    return cType(f.type);
        }
        return "";
    }
    return "";
}

// obj.field / this.field
std::string CEmitter::emitMemberAccess(MemberAccessNode* ma)
{
    std::string field = (ma->identifier && ma->identifier->value) ? *ma->identifier->value : "";
    if (!ma->expression) {
        unsupported("static member access", ma->line);
        return field;
    }
    if (dynamic_cast<ThisAccessNode*>(ma->expression.get()))
        return "self->" + field;
    return "(" + emitExpression(ma->expression) + ")." + field;
}

// obj.method(args) -> Class__method(&obj, reordered args)
std::string CEmitter::emitMethodCall(InvocationNode* call, MemberAccessNode* recv)
{
    std::string method = (recv->identifier && recv->identifier->value) ? *recv->identifier->value : "";
    SharedExpression receiver = recv->expression;
    std::string cls = exprClass(receiver);
    if (cls.empty() || !_classes.count(cls)) {
        unsupported("method call on unresolved receiver", call->line);
        return "0";
    }
    ClassInfo& ci = _classes[cls];
    auto mit = ci.methods.find(method);
    if (mit == ci.methods.end()) {
        unsupported("unknown method", call->line);
        return "0";
    }
    MethodInfo& mi = mit->second;
    std::string selfArg = dynamic_cast<ThisAccessNode*>(receiver.get())
                        ? std::string("self")
                        : "&(" + emitExpression(receiver) + ")";
    return emitReorderedCall(mi.cName, selfArg, mi.params, call->args, call->line);
}

std::string CEmitter::emitCtorCall(const std::string& cVar, ClassInfo& ci, SharedArgumentList args, int srcLine)
{
    return emitReorderedCall(ci.name + "__ctor", "&" + cVar, ci.ctorParams, args, srcLine);
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

    // Pass 0: collect function signatures and the class table.
    collectSignatures(unit);
    collectClasses(unit);
    computeDestructible();

    // Pass S: struct typedefs for all classes (so prototypes can use them).
    // NOTE: emitted in map order; a class holding another class by value would
    // need a topological sort (deferred — not exercised by current fixtures).
    for (auto& kv : _classes) emitStruct(kv.second);

    // Pass A: prototypes — class ctors/methods, then free functions.
    // extern functions are provided by C (runtime/linked) — no prototype/def.
    for (auto& kv : _classes) emitClassPrototypes(kv.second);
    bool any = false;
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (isExtern(fn)) continue;
            emitFunctionPrototype(fn);
            any = true;
        }
    }
    if (any) _out << "\n";

    // Pass B: definitions — class methods/ctors, then free functions.
    for (auto& kv : _classes) emitClassDefinitions(kv.second);
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (isExtern(fn)) continue;   // provided externally
            emitFunction(fn);
        } else if (dynamic_cast<ClassDeclarationNode*>(decl.get())) {
            // already emitted via the class passes
        } else if (decl) {
            unsupported("top-level declaration", decl->line);
            _out << "\n";
        }
    }

    return _unsupported;
}
