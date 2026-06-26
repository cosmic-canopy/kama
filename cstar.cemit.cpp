#include "cstar.cemit.h"
#include "cstar.ast.h"
#include "cstar.parser.hpp"   // bison token constants (PLUS, STAR, EQEQ, ...)

#include <cstdio>
#include <sstream>
#include <functional>

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
        // An unqualified name that is a field of the enclosing class (or an
        // ancestor) and not a local/param resolves to self->[__base.]…field.
        if (_currentClass && !_localTypes.count(nm)) {
            ClassInfo* owner = findFieldOwner(_currentClass, nm);
            if (owner) return "self->" + basePathTo(_currentClass, owner) + nm;
        }
        return nm;
    }

    if (dynamic_cast<ThisAccessNode*>(n)) return "self";

    if (auto* v = dynamic_cast<MemberAccessNode*>(n)) {
        return emitMemberAccess(v);
    }

    if (auto* ba = dynamic_cast<BaseAccessNode*>(n)) {
        // base.field (bare; base.method(...) is handled in emitInvocation).
        std::string name = (ba->identifier && ba->identifier->value) ? *ba->identifier->value : "";
        if (_currentClass && _currentClass->base) {
            ClassInfo* owner = findFieldOwner(_currentClass->base, name);
            if (owner) return "self->__base." + basePathTo(_currentClass->base, owner) + name;
        }
        unsupported("base access", ba->line);
        return name;
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
            // Store the C type spelling; whether it's a class is checked at the
            // call site (paramSigsOf may run before the class table is built).
            ps.className = p->type ? cType(p->type) : "";
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

        // Single inheritance (extends). Interfaces (implements) are M6b.
        if (cd->baseTypes) {
            if (cd->baseTypes->base && cd->baseTypes->base->value)
                ci.baseName = *cd->baseTypes->base->value;
            if (cd->baseTypes->interfaces && !cd->baseTypes->interfaces->empty())
                unsupported("interfaces (implements) — deferred to M6b", cd->line);
        }
        // Class-level `abstract` modifier.
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers)
                if (mod->value && *mod->value == "abstract") ci.isAbstractClass = true;

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
                        if (md->modifiers)
                            for (auto& mod : *md->modifiers) {
                                if (!mod->value) continue;
                                if (*mod->value == "virtual")  mi.isVirtual = true;
                                if (*mod->value == "override") { mi.isVirtual = true; mi.isOverride = true; }
                                if (*mod->value == "abstract") { mi.isVirtual = true; mi.isAbstract = true; }
                            }
                        if (!md->body) mi.isAbstract = mi.isVirtual = true;   // null body => pure
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

// Resolve `extends` names to ClassInfo pointers; error on unknown/cycle.
void CEmitter::linkBases()
{
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.baseName.empty()) continue;
        auto it = _classes.find(ci.baseName);
        if (it == _classes.end()) {
            unsupported("unknown base class", ci.node ? ci.node->line : 0);
            ci.baseName.clear();
        } else {
            ci.base = &it->second;
        }
    }
    // Break cycles defensively (A extends B extends A): cut the back-edge.
    for (auto& kv : _classes) {
        int hops = 0;
        for (ClassInfo* b = kv.second.base; b; b = b->base) {
            if (b == &kv.second || ++hops > 1000) { kv.second.base = nullptr; kv.second.baseName.clear(); break; }
        }
    }
}

// Classes in base-before-derived order.
std::vector<ClassInfo*> CEmitter::topoOrderClasses()
{
    std::vector<ClassInfo*> out;
    std::set<ClassInfo*> done;
    std::function<void(ClassInfo*)> visit = [&](ClassInfo* ci) {
        if (!ci || done.count(ci)) return;
        if (ci->base) visit(ci->base);
        done.insert(ci);
        out.push_back(ci);
    };
    for (auto& kv : _classes) visit(&kv.second);
    return out;
}

// Build the per-class virtual slot tables and the per-root vtable union.
void CEmitter::buildVtables()
{
    for (ClassInfo* ci : topoOrderClasses()) {
        // Inherit base's vtable model.
        if (ci->base && ci->base->hasVtable) {
            ci->hasVtable  = true;
            ci->vtableRoot = ci->base->vtableRoot;
            ci->slotImpl   = ci->base->slotImpl;   // inherited impls
        }
        for (auto& kv : ci->methods) {
            MethodInfo& mi = kv.second;
            const std::string& mname = kv.first;
            if (!mi.isVirtual) continue;
            if (mi.isOverride || ci->slotImpl.count(mname)) {
                // Re-slot an inherited virtual with this class's implementation.
                if (!mi.isAbstract) ci->slotImpl[mname] = mi.cName;
            } else {
                // New virtual slot (abstract slots have no impl until overridden).
                if (!ci->hasVtable) { ci->hasVtable = true; ci->vtableRoot = ci->name; }
                if (!mi.isAbstract) ci->slotImpl[mname] = mi.cName;
                VSlot vs; vs.name = mname; vs.owner = ci->name; vs.node = mi.node;
                _rootVtables[ci->vtableRoot].push_back(vs);
            }
        }
        // A class is abstract if marked, or any slot still resolves to a pure impl.
        if (ci->isAbstractClass) { /* keep */ }
    }
}

// A class is destructible if it declares a dtor, has a destructible field, OR
// its base is destructible (transitive). Fixed-point — cycle-safe.
void CEmitter::computeDestructible()
{
    for (auto& kv : _classes) kv.second.destructible = kv.second.hasDtor;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& kv : _classes) {
            ClassInfo& ci = kv.second;
            if (ci.destructible) continue;
            bool d = (ci.base && ci.base->destructible);
            if (!d)
                for (auto& f : ci.fields) {
                    auto it = _classes.find(cType(f.type));
                    if (it != _classes.end() && it->second.destructible) { d = true; break; }
                }
            if (d) { ci.destructible = true; changed = true; }
        }
    }
}

// The class in ci's ancestry that declares `field` (or nullptr).
ClassInfo* CEmitter::findFieldOwner(ClassInfo* ci, const std::string& field)
{
    for (; ci; ci = ci->base)
        if (ci->fieldNames.count(field)) return ci;
    return nullptr;
}

// The nearest method `name` in ci's ancestry; sets *owner to the declaring class.
MethodInfo* CEmitter::findMethod(ClassInfo* ci, const std::string& name, ClassInfo** owner)
{
    for (; ci; ci = ci->base) {
        auto it = ci->methods.find(name);
        if (it != ci->methods.end()) { if (owner) *owner = ci; return &it->second; }
    }
    return nullptr;
}

// "__base." repeated for each hop from `from` down to ancestor `to` ("" if equal).
std::string CEmitter::basePathTo(ClassInfo* from, ClassInfo* to)
{
    std::string path;
    for (ClassInfo* c = from; c && c != to; c = c->base) path += "__base.";
    return path;
}

std::string CEmitter::vptrPrefix(ClassInfo* ci)
{
    if (!ci || ci->vtableRoot.empty()) return "";
    auto it = _classes.find(ci->vtableRoot);
    return (it != _classes.end()) ? basePathTo(ci, &it->second) : "";
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
        if (p.byRef)
            s += isClass(p.className) ? ("(" + p.className + "*)&(" + val + ")")  // upcast for ref Base
                                      : ("&(" + val + ")");
        else
            s += val;
    }
    return s + ")";
}

// Lower a call, reordering named arguments to the callee's declared order.
std::string CEmitter::emitInvocation(InvocationNode* call)
{
    // Expression-form callee (this.method(...), base.method(...), parenthesized).
    if (!call->identifier || !call->identifier->value) {
        if (call->expression) {
            if (auto* ma = dynamic_cast<MemberAccessNode*>(call->expression.get()))
                return emitMethodCall(call, ma);
            if (auto* ba = dynamic_cast<BaseAccessNode*>(call->expression.get())) {
                // base.m(args) -> direct (non-virtual) call into the base.
                if (!_currentClass || !_currentClass->base) {
                    unsupported("base call outside a derived class", call->line); return "0";
                }
                std::string m = (ba->identifier && ba->identifier->value) ? *ba->identifier->value : "";
                ClassInfo* owner = nullptr;
                MethodInfo* mi = findMethod(_currentClass->base, m, &owner);
                if (!mi) { unsupported("unknown base method", call->line); return "0"; }
                std::string self = "(" + owner->name + "*)&self->__base";
                return emitReorderedCall(mi->cName, self, mi->params, call->args, call->line);
            }
        }
        unsupported("indirect call", call->line);
        return "0";
    }

    std::string name = *call->identifier->value;

    // A qualified callee `recv.method` parses as identifier{value=method,
    // qualifier=[recv...]}. (No namespaces yet, so a qualifier means a method
    // receiver.) Build the receiver C-expression + class from the qualifier chain.
    SharedStringList qual = call->identifier->qualifier;
    if (qual && !qual->empty()) {
        std::string recvExpr, recvClass;
        const std::string& head = *(*qual)[0];
        if (_localTypes.count(head) && !_localTypes[head].empty()) {
            // A ref/out param is already a pointer; deref so &(recv) is the pointer.
            recvExpr = _refParams.count(head) ? ("(*" + head + ")") : head;
            recvClass = _localTypes[head];
        } else if (_currentClass) {
            ClassInfo* fo = findFieldOwner(_currentClass, head);
            if (!fo) { unsupported("method receiver not a known object", call->line); return "0"; }
            recvExpr = "self->" + basePathTo(_currentClass, fo) + head;
            for (auto& f : fo->fields) if (f.name == head && f.type) recvClass = cType(f.type);
        } else { unsupported("method receiver not a known object", call->line); return "0"; }

        for (size_t i = 1; i < qual->size(); ++i) {
            if (recvClass.empty() || !_classes.count(recvClass)) {
                unsupported("method receiver chain not resolvable", call->line); return "0";
            }
            const std::string& fld = *(*qual)[i];
            ClassInfo* fo = findFieldOwner(&_classes[recvClass], fld);
            std::string fcls;
            if (fo) for (auto& f : fo->fields) if (f.name == fld && f.type) fcls = cType(f.type);
            recvExpr  = "(" + recvExpr + ")." + (fo ? basePathTo(&_classes[recvClass], fo) : "") + fld;
            recvClass = fcls;
        }

        if (recvClass.empty() || !_classes.count(recvClass)) {
            unsupported("method call on unresolved receiver", call->line); return "0";
        }
        return emitDispatch(recvClass, "&(" + recvExpr + ")", name, call->args, call->line);
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
    _out << "struct " << ci.name << " {\n";
    // Offset-0 invariant: the vptr (root only) or the embedded base comes FIRST.
    bool hasMember = false;
    if (ci.hasVtable && ci.vtableRoot == ci.name) {
        indent(1);
        _out << "const " << ci.name << "_vtable* __vptr;\n";
        hasMember = true;
    }
    if (ci.base) {
        indent(1);
        _out << ci.baseName << " __base;\n";
        hasMember = true;
    }
    for (auto& f : ci.fields) {
        indent(1);
        _out << cType(f.type) << " " << f.name << ";\n";
        hasMember = true;
    }
    if (!hasMember) {
        indent(1);
        _out << "char __empty; /* C forbids empty structs */\n";
    }
    _out << "};\n\n";
}

// "(Owner* self, T a, U b)" — the C signature of a vtable slot.
std::string CEmitter::vtableSlotSig(const VSlot& s)
{
    std::string sig = "(" + s.owner + "* self";
    if (s.node && s.node->params) {
        for (auto& p : *s.node->params) {
            std::string nm = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            sig += ", " + cType(p->type) + (paramByRef(p.get()) ? "* " : " ") + nm;
        }
    }
    return sig + ")";
}

// Vtable struct TYPE — one per root (the class that first introduces a virtual).
// Lists every slot in the hierarchy; fn-ptr self type is pinned to the slot owner.
void CEmitter::emitVtableType(ClassInfo& ci)
{
    auto it = _rootVtables.find(ci.name);
    if (it == _rootVtables.end()) return;
    _out << "struct " << ci.name << "_vtable {\n";
    for (auto& s : it->second) {
        indent(1);
        _out << cType(s.node->returnType) << " (*" << s.name << ")" << vtableSlotSig(s) << ";\n";
    }
    _out << "};\n\n";
}

// Static const vtable INSTANCE per class with a vtable, filled with the most-
// derived impl visible to this class (designated initializers; missing slots zero).
void CEmitter::emitVtableInstance(ClassInfo& ci)
{
    if (!ci.hasVtable) return;
    auto it = _rootVtables.find(ci.vtableRoot);
    if (it == _rootVtables.end()) return;
    _out << "static const " << ci.vtableRoot << "_vtable " << ci.name << "__vtable = {\n";
    for (auto& s : it->second) {
        auto impl = ci.slotImpl.find(s.name);
        if (impl == ci.slotImpl.end()) continue;   // not visible here -> zero
        indent(1);
        // cast the impl (declared with a derived* self) to the slot's owner* signature
        _out << "." << s.name << " = (" << cType(s.node->returnType) << "(*)"
             << vtableSlotSig(s) << ")&" << impl->second << ",\n";
    }
    _out << "};\n\n";
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
        if (mi.isAbstract) continue;   // pure: no definition, no prototype
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
    // Base destructor LAST.
    if (ci.base && ci.base->destructible) {
        indent(1);
        _out << ci.baseName << "__dtor(&self->__base);\n";
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
        // 1. Base constructor first (so derived overrides its effects + vptr).
        if (owner.base) {
            SharedArgumentList baseArgs;
            if (owner.ctorNode && owner.ctorNode->declarator && owner.ctorNode->declarator->initializer)
                baseArgs = owner.ctorNode->declarator->initializer->args;
            if (owner.base->hasCtor) {
                indent(1);
                _out << emitReorderedCall(owner.baseName + "__ctor", "&self->__base",
                                          owner.base->ctorParams, baseArgs, owner.node->line) << ";\n";
            } else if (baseArgs && !baseArgs->empty()) {
                unsupported("base has no constructor to receive arguments", owner.node->line);
            }
        }
        // 2. Set the vptr to THIS class's vtable (after base, so most-derived wins).
        if (owner.hasVtable) {
            indent(1);
            _out << "self->" << vptrPrefix(&owner) << "__vptr = &" << owner.name << "__vtable;\n";
        }
        // 3. Field initializers.
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
        if (mi.isAbstract) continue;   // pure: no body to emit
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
            ClassInfo* owner = findFieldOwner(_currentClass, *id->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *id->value && f.type && isClass(cType(f.type))) return cType(f.type);
        }
        return "";
    }

    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        std::string recv = exprClass(ma->expression);
        if (!recv.empty() && ma->identifier && ma->identifier->value && _classes.count(recv)) {
            ClassInfo* owner = findFieldOwner(&_classes[recv], *ma->identifier->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *ma->identifier->value && f.type && isClass(cType(f.type)))
                        return cType(f.type);
        }
        return "";
    }
    return "";
}

// obj.field / this.field — splice the __base. chain to the declaring ancestor.
std::string CEmitter::emitMemberAccess(MemberAccessNode* ma)
{
    std::string field = (ma->identifier && ma->identifier->value) ? *ma->identifier->value : "";
    if (!ma->expression) {
        unsupported("static member access", ma->line);
        return field;
    }
    std::string cls = exprClass(ma->expression);
    std::string basePath;
    if (!cls.empty() && _classes.count(cls)) {
        ClassInfo* owner = findFieldOwner(&_classes[cls], field);
        if (owner) basePath = basePathTo(&_classes[cls], owner);
    }
    if (dynamic_cast<ThisAccessNode*>(ma->expression.get()))
        return "self->" + basePath + field;
    return "(" + emitExpression(ma->expression) + ")." + basePath + field;
}

// Dispatch a method call on a receiver of static class `clsName`.
std::string CEmitter::emitDispatch(const std::string& clsName, const std::string& recvPtr,
                                   const std::string& method, SharedArgumentList args, int srcLine)
{
    if (!_classes.count(clsName)) { unsupported("call on unknown class", srcLine); return "0"; }
    ClassInfo* owner = nullptr;
    MethodInfo* mi = findMethod(&_classes[clsName], method, &owner);
    if (!mi) { unsupported("unknown method", srcLine); return "0"; }

    if (mi->isVirtual) {
        // Dynamic dispatch through the vptr (at offset 0 via the vtable root).
        const std::string& root = _classes[clsName].vtableRoot;
        std::string slotOwner = owner->name;
        auto rit = _rootVtables.find(root);
        if (rit != _rootVtables.end())
            for (auto& s : rit->second) if (s.name == method) { slotOwner = s.owner; break; }
        std::string self = "(" + slotOwner + "*)" + recvPtr;
        std::string vptr = "((" + root + "*)" + recvPtr + ")->__vptr";
        return emitReorderedCall(vptr + "->" + method, self, mi->params, args, srcLine);
    }
    // Static call; upcast self to the declaring class (offset-0 valid).
    std::string self = "(" + owner->name + "*)" + recvPtr;
    return emitReorderedCall(mi->cName, self, mi->params, args, srcLine);
}

// obj.method(args) — the member-access callee form (incl. this.method()).
std::string CEmitter::emitMethodCall(InvocationNode* call, MemberAccessNode* recv)
{
    std::string method = (recv->identifier && recv->identifier->value) ? *recv->identifier->value : "";
    SharedExpression receiver = recv->expression;
    std::string cls = exprClass(receiver);
    if (cls.empty() || !_classes.count(cls)) {
        unsupported("method call on unresolved receiver", call->line);
        return "0";
    }
    std::string recvPtr = dynamic_cast<ThisAccessNode*>(receiver.get())
                        ? std::string("self")
                        : "&(" + emitExpression(receiver) + ")";
    return emitDispatch(cls, recvPtr, method, call->args, call->line);
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

    // Pass 0: collect, link inheritance, build vtables, compute destructibility.
    collectSignatures(unit);
    collectClasses(unit);
    linkBases();
    buildVtables();
    computeDestructible();

    std::vector<ClassInfo*> classes = topoOrderClasses();   // base before derived

    // Pass S0: forward typedefs (classes + vtable types) so the struct bodies and
    // vtable types can reference each other and any class.
    for (ClassInfo* ci : classes) {
        _out << "typedef struct " << ci->name << " " << ci->name << ";\n";
        if (ci->hasVtable && ci->vtableRoot == ci->name)
            _out << "typedef struct " << ci->name << "_vtable " << ci->name << "_vtable;\n";
    }
    if (!classes.empty()) _out << "\n";

    // Pass S: vtable struct types (roots) + struct bodies (topological).
    for (ClassInfo* ci : classes) {
        if (ci->hasVtable && ci->vtableRoot == ci->name) emitVtableType(*ci);
        emitStruct(*ci);
    }

    // Pass A: prototypes — class ctors/dtors/methods, then free functions.
    // extern functions are provided by C (runtime/linked) — no prototype/def.
    for (ClassInfo* ci : classes) emitClassPrototypes(*ci);
    bool any = false;
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (isExtern(fn)) continue;
            emitFunctionPrototype(fn);
            any = true;
        }
    }
    if (any) _out << "\n";

    // Pass A2: vtable instances (after prototypes, which declare the fn names).
    for (ClassInfo* ci : classes) emitVtableInstance(*ci);

    // Pass B: definitions — class methods/ctors/dtors, then free functions.
    for (ClassInfo* ci : classes) emitClassDefinitions(*ci);
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
