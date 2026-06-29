#include "cstar.cemit.h"
#include "cstar.ast.h"
#include "cstar.parser.hpp"   // bison token constants (PLUS, STAR, EQEQ, ...)

#include <cstdio>
#include <sstream>
#include <functional>
#include <cctype>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CEmitter::CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives)
    : _out(&out)
    , _sourcePath(sourcePath)
    , _lines(emitLineDirectives)
    , _unsupported(0)
{
}

void CEmitter::indent(int depth)
{
    for (int i = 0; i < depth; ++i) *_out << "    ";
}

void CEmitter::line(int srcLine)
{
    if (_lines && srcLine > 0)
        *_out << "#line " << srcLine << " \"" << _sourcePath << "\"\n";
}

void CEmitter::unsupported(const char* what, int srcLine)
{
    ++_unsupported;
    std::fprintf(stderr, "cstar: warning: unsupported %s at %s:%d (not yet lowered)\n",
                 what, _sourcePath.c_str(), srcLine);
    *_out << "/* TODO(cstar): unsupported " << what << " */";
}

// ---------------------------------------------------------------------------
// Namespaces (M14): scope prefixes + name resolution
// ---------------------------------------------------------------------------

// "a.b.c" — the dotted source name from an identifier's qualifier + value.
std::string CEmitter::qualifiedName(SharedIdentifier id)
{
    std::string s;
    if (id->qualifier)
        for (auto& seg : *id->qualifier) s += *seg + ".";
    s += id->value ? *id->value : "";
    return s;
}

std::string CEmitter::mangleNs(const std::string& ns)
{
    std::string r = ns;
    for (size_t p; (p = r.find('.')) != std::string::npos; ) r.replace(p, 1, "__");
    return r;
}

// Build a file's namespace context: public (mangled `namespace X;`) or private
// (`_F<idx>`); collect its `using`s and aliases.
NsCtx CEmitter::ctxOf(SharedCompilationUnit unit, int fileIndex)
{
    NsCtx ctx;
    if (unit->nameSpace && unit->nameSpace->name) {
        ctx.scope = mangleNs(qualifiedName(unit->nameSpace->name));
        ctx.isPublic = true;
    } else {
        ctx.scope = "_F" + std::to_string(fileIndex);
        ctx.isPublic = false;
    }
    if (unit->usingDeclarationList)
        for (auto& u : *unit->usingDeclarationList) {
            if (!u || !u->identifier) continue;
            std::string mangled = mangleNs(qualifiedName(u->identifier));
            if (u->alias && u->alias->value) ctx.aliases[*u->alias->value] = mangled;
            else ctx.usings.push_back(mangled);
        }
    return ctx;
}

// Scope-prefix a declared name (registration). `main` stays the global entry.
std::string CEmitter::qualify(const std::string& name) const
{
    if (name == "main") return "cstar_main";
    return _nsCtx.scope + "__" + name;
}

bool CEmitter::isNamespace(const std::string& name) const
{
    if (_nsCtx.aliases.count(name)) return true;
    return _namespaces.count(mangleNs(name)) != 0;
}

// Resolve a class/enum/interface reference (bare or qualified) to its registered
// mangled name. Bare names search the file's own scope, then its `using`s — never
// another file's private symbols. Returns the bare name if unresolved.
std::string CEmitter::resolveUserName(const std::string& value, SharedStringList qualifier)
{
    auto known = [&](const std::string& n) {
        return _classes.count(n) || _enums.count(n) || _interfaces.count(n) || _sigs.count(n);
    };
    // FFI (M16): extern struct/handle names are global literal C names.
    if ((!qualifier || qualifier->empty()) && _externNames.count(value)) return value;
    if (qualifier && !qualifier->empty()) {
        // Qualified `A.B...value` — a namespace path (alias-expand a 1-segment head).
        std::string nsMangled;
        if (qualifier->size() == 1 && _nsCtx.aliases.count(*(*qualifier)[0]))
            nsMangled = _nsCtx.aliases[*(*qualifier)[0]];
        else {
            std::string path;
            for (auto& seg : *qualifier) path += (path.empty() ? "" : ".") + *seg;
            nsMangled = mangleNs(path);
        }
        std::string cand = nsMangled + "__" + value;
        return known(cand) ? cand : value;
    }
    std::string own = _nsCtx.scope + "__" + value;     // file's own scope
    if (known(own)) return own;
    for (auto& u : _nsCtx.usings) {                    // imported public namespaces
        std::string cand = u + "__" + value;
        if (known(cand)) return cand;
    }
    return value;   // builtin/forward/unresolved — caller handles
}

// Resolve a function reference to its mangled cName (same search as types).
std::string CEmitter::resolveFunc(const std::string& name, SharedStringList qualifier)
{
    if (name == "main") return "cstar_main";
    if (qualifier && !qualifier->empty()) {
        std::string nsMangled;
        if (qualifier->size() == 1 && _nsCtx.aliases.count(*(*qualifier)[0]))
            nsMangled = _nsCtx.aliases[*(*qualifier)[0]];
        else {
            std::string path;
            for (auto& seg : *qualifier) path += (path.empty() ? "" : ".") + *seg;
            nsMangled = mangleNs(path);
        }
        std::string cand = nsMangled + "__" + name;
        return _funcs.count(cand) ? cand : name;
    }
    std::string own = _nsCtx.scope + "__" + name;
    if (_funcs.count(own)) return own;
    for (auto& u : _nsCtx.usings) {
        std::string cand = u + "__" + name;
        if (_funcs.count(cand)) return cand;
    }
    return name;
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

std::string CEmitter::cType(SharedIdentifier type)
{
    if (!type) return "void";
    // FFI (M15): a raw C pointer carrier (opaque). Bare `Ptr` -> void* (the
    // universal handle / opaque pointer); `Ptr<T>` -> T*. usize/isize map to the
    // C size types. These are the explicit, extern-marked unsafe boundary.
    if (type->value && *type->value == "Ptr")
        return type->genericArg ? (cType(type->genericArg) + "*") : "void*";
    if (type->value && !type->genericArg) {
        if (*type->value == "usize") return "size_t";
        if (*type->value == "isize") return "ptrdiff_t";
    }

    // Collection / smart-pointer types spell their mangled struct name:
    // Array<int32> -> Array_int32 (M9); Owned<Node> -> Owned_Node (M10);
    // Shared<Tex> -> Shared_Tex (M11); Weak<Tex> -> Weak_Tex (M12).
    if (type->genericArg && type->value &&
        (*type->value == "Array" || *type->value == "List" || *type->value == "Owned" ||
         *type->value == "Shared" || *type->value == "Weak" || *type->value == "BindableFunctionPtr"))
        return *type->value + "_" + mangleElem(type->genericArg);
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
            // User-defined type (class/enum/interface) — resolve through the
            // current file's namespace scope + usings to its mangled C name.
            if (!type->value) return "void";
            return resolveUserName(*type->value, type->qualifier);
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
        // Enum member `[Ns.]Enum.Member` (value=Member, qualifier=[…,Enum]) —
        // resolve the enum name (last qualifier segment) through the file's scope.
        if (v->qualifier && !v->qualifier->empty()) {
            auto enumQual = std::make_shared<StringList>();
            for (size_t i = 0; i + 1 < v->qualifier->size(); ++i) enumQual->push_back((*v->qualifier)[i]);
            std::string en = resolveUserName(*v->qualifier->back(), enumQual);
            if (_enums.count(en)) return en + "_" + nm;
            // Object field access `obj.field[.field…]` (qualifier=[obj,…], value=field).
            const std::string& head = *(*v->qualifier)[0];
            if (_localTypes.count(head) && !_localTypes[head].empty()) {
                std::string e = _refParams.count(head) ? ("(*" + head + ")") : head;
                e = "(" + e + ")";
                for (size_t i = 1; i < v->qualifier->size(); ++i) e += "." + *(*v->qualifier)[i];
                return e + "." + nm;
            }
            if (_currentClass) {
                ClassInfo* owner = findFieldOwner(_currentClass, head);
                if (owner) {
                    checkFieldAccess(owner, head, v->line);   // M25
                    std::string e = "self->" + basePathTo(_currentClass, owner) + head;
                    for (size_t i = 1; i < v->qualifier->size(); ++i) e += "." + *(*v->qualifier)[i];
                    return e + "." + nm;
                }
            }
        }
        // A ref/out parameter is a pointer in C; reads dereference it.
        if (_refParams.count(nm)) return "(*" + nm + ")";
        // An unqualified name that is a field of the enclosing class (or an
        // ancestor) and not a local/param resolves to self->[__base.]…field.
        if (_currentClass && !_localTypes.count(nm)) {
            ClassInfo* owner = findFieldOwner(_currentClass, nm);
            if (owner) { checkFieldAccess(owner, nm, v->line); return "self->" + basePathTo(_currentClass, owner) + nm; }  // M25
        }
        // A bare **function name** used as a value (not a call) → its C function
        // pointer (M21) — enables binding/passing a free function to a FunctionPtr.
        if (!_localTypes.count(nm)) {
            auto fit = _funcs.find(resolveFunc(nm, v->qualifier));
            if (fit != _funcs.end()) return fit->second.cName;
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
            if (owner) { checkFieldAccess(owner, name, ba->line); return "self->__base." + basePathTo(_currentClass->base, owner) + name; }  // M25
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
        checkConstWrite(v->unaryExpression, v->line);   // M24a: no write to/through const
        // Indexed assignment to a collection lowers to __set, not `lhs = rhs`.
        if (auto* ea = dynamic_cast<ElementAccessNode*>(v->unaryExpression.get())) {
            std::string coll, recvExpr, idx;
            if (collectionElemAccess(ea, coll, recvExpr, idx)) {
                std::string rhs = emitExpression(v->expression);
                if (v->token == EQ)
                    return coll + "__set(&(" + recvExpr + "), " + idx + ", " + rhs + ")";
                // Compound (a[i] += x): set(get(...) <op> (x)). NB: index double-evaluated.
                std::string op = assignmentOperator(v->token);   // e.g. "+="
                if (op.size() >= 2 && op.back() == '=') op.pop_back();
                return coll + "__set(&(" + recvExpr + "), " + idx + ", "
                            + coll + "__get(&(" + recvExpr + "), " + idx + ") " + op + " (" + rhs + "))";
            }
            // Raw pointer store `p[i] = v` (M17) — only inside `unsafe { }`.
            SharedExpression recv = ea->expression ? ea->expression
                                  : std::static_pointer_cast<ExpressionNode>(ea->identifier);
            std::string ridx = (ea->expressionlist && !ea->expressionlist->empty())
                             ? emitExpression((*ea->expressionlist)[0]) : "0";
            if (!_inUnsafe) {
                unsupported("raw pointer access requires an `unsafe { }` block", ea->line);
                return "0";
            }
            return "((" + emitExpression(recv) + ")[" + ridx + "] "
                       + assignmentOperator(v->token) + " " + emitExpression(v->expression) + ")";
        }
        return "(" + emitExpression(v->unaryExpression) + " "
                   + assignmentOperator(v->token) + " " + emitExpression(v->expression) + ")";
    }

    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        std::string coll, recvExpr, idx;
        if (collectionElemAccess(ea, coll, recvExpr, idx))
            return coll + "__get(&(" + recvExpr + "), " + idx + ")";
        // Raw pointer read `p[i]` (M17) — only inside `unsafe { }`.
        SharedExpression recv = ea->expression ? ea->expression
                              : std::static_pointer_cast<ExpressionNode>(ea->identifier);
        std::string ridx = (ea->expressionlist && !ea->expressionlist->empty())
                         ? emitExpression((*ea->expressionlist)[0]) : "0";
        if (!_inUnsafe) {
            unsupported("raw pointer access requires an `unsafe { }` block", ea->line);
            return "0";
        }
        return "(" + emitExpression(recv) + ")[" + ridx + "]";
    }

    if (auto* v = dynamic_cast<PreIncrDecrNode*>(n)) {
        checkConstWrite(v->expression, v->line);   // M24a
        std::string op = (v->token == PLUSPLUS) ? "++" : "--";
        return "(" + op + emitExpression(v->expression) + ")";
    }

    if (auto* v = dynamic_cast<PostIncrDecrNode*>(n)) {
        checkConstWrite(v->expression, v->line);   // M24a
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
        *_out << it->className << "__dtor(&" << it->cVar << ");\n";
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

    *_out << "{\n";
    SharedStatement last;
    if (block && block->statements) {
        for (auto& stmt : *block->statements) { emitStatement(stmt, depth + 1); last = stmt; }
    }
    // Fall-through cleanup, unless the block already exited via a jump (which
    // ran its own cleanup) — the double-destruction guard.
    if (!(last && stmtIsJump(last)))
        emitScopeCleanup(_scopes.back(), depth + 1);

    indent(depth);
    *_out << "}";
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
        *_out << "\n";
        return;
    }

    // `unsafe { ... }` (M17): permit raw pointer index/store inside; otherwise a
    // plain scoped block. The single, explicit, greppable unsafe surface.
    if (auto* u = dynamic_cast<UnsafeNode*>(n)) {
        bool prev = _inUnsafe;
        _inUnsafe = true;
        emitStatement(u->body, depth);   // the BlockNode -> a normal scoped { … }
        _inUnsafe = prev;
        return;
    }

    {
        // Local declaration — plain or `const` (M24a). The two declarator node types
        // are structurally identical (name + initializer), so one generic body serves
        // both; a const decl additionally records each name as immutable (no
        // reassignment, and — deep const — no writes THROUGH the binding either).
        LocalVariableDeclaration* lvd = dynamic_cast<LocalVariableDeclaration*>(n);
        ConstLocalVariableDeclaration* cvd = lvd ? nullptr
                                            : dynamic_cast<ConstLocalVariableDeclaration*>(n);
        SharedIdentifier declType = lvd ? lvd->type : (cvd ? cvd->type : SharedIdentifier());
        bool isConstDecl = (cvd != nullptr);
        if (declType) {
            std::string ty = cType(declType);
            bool cls = isClass(ty);
            bool iface = isInterface(ty);
            auto emitDeclarator = [&](auto& d) {
                std::string nm = (d->name && d->name->value) ? *d->name->value : "";
                _localTypes[nm] = (cls || iface) ? ty : "";   // record all names (shadow fields)
                if (isConstDecl) {
                    if (!d->initializer)
                        unsupported("a const must be initialized (it is immutable)", n->line);
                    _constLocals.insert(nm);
                }

                // Interface-typed local: `I s = concrete;` -> a fat pointer borrowing
                // the concrete object (which must be an lvalue that outlives `s`).
                if (iface) {
                    line(n->line); indent(depth);
                    *_out << ty << " " << nm;
                    if (d->initializer) {
                        std::string c = exprClass(d->initializer);
                        if (!c.empty() && isClass(c))
                            *_out << " = " << fatPointer(ty, c, emitExpression(d->initializer));
                        else if (!c.empty() && isInterface(c))
                            *_out << " = " << emitExpression(d->initializer);  // already an interface value
                        else
                            unsupported("interface initializer must be a concrete object lvalue", n->line);
                    }
                    *_out << ";\n";
                    return;
                }

                // FunctionPtr<Sig> binding (M21): a signature-typed local — track the
                // sig (drives invoke), bind a free function (resolve + signature-check)
                // or another FunctionPtr; must be initialized (non-null).
                if (isSigType(ty)) {
                    _localTypes[nm] = ty;
                    line(n->line); indent(depth);
                    *_out << ty << " " << nm;
                    if (!d->initializer)
                        unsupported("a FunctionPtr must be initialized (it is non-null)", n->line);
                    else
                        *_out << " = " << emitFnPtrBind(ty, d->initializer, n->line);
                    *_out << ";\n";
                    return;
                }

                if (!cls) {
                    line(n->line); indent(depth);
                    *_out << ty << " " << nm;
                    if (d->initializer) *_out << " = " << emitExpression(d->initializer);
                    *_out << ";\n";
                    return;
                }

                // Class-typed local: declare the value, then construct in place.
                // Collections zero-init so an unconstructed one frees safely; extern
                // structs zero-init so unset descriptor fields are well-defined.
                line(n->line); indent(depth);
                bool zeroInit = _classes[ty].isCollection || _classes[ty].isExternStruct;
                *_out << ty << " " << nm << (zeroInit ? " = {0}" : "") << ";\n";
                // Track for RAII cleanup at scope exit (assumes init-at-decl).
                if (_classes[ty].destructible) recordDestructibleLocal(nm, ty);
                if (!d->initializer) return;   // declared but uninitialized (non-const)

                if (auto* oc = dynamic_cast<ObjectCreationNode*>(d->initializer.get())) {
                    std::string octy = cType(oc->type);
                    if (isSmartPtrClass(octy)) {
                        // new Owned/Shared<T>(args): box T on the heap, run T's
                        // ctor in place, and (Shared) allocate the control block.
                        std::string T = _classes[octy].collElemClass;
                        if (isClass(T) && _classes[T].isAbstractClass)   // Step 3: no abstract heap-alloc either
                            unsupported(("cannot instantiate abstract class '" + T + "'").c_str(), n->line);
                        line(n->line); indent(depth);
                        *_out << nm << ".ptr = (" << T << "*)malloc(sizeof(" << T << "));\n";
                        if (isClass(T) && _classes[T].hasCtor) {
                            line(n->line); indent(depth);
                            *_out << emitReorderedCall(T + "__ctor", nm + ".ptr",
                                                      _classes[T].ctorParams, oc->args, n->line) << ";\n";
                        }
                        if (smartKind(octy) == CollKind::Shared) {
                            indent(depth);
                            *_out << nm << ".ctrl = cstar_ctrl_new();\n";
                        }
                        // T with no ctor: malloc leaves it default (callers init fields).
                    } else if (isBindableClass(octy)) {
                        emitBindableNew(nm, octy, oc, depth);   // bind obj + method (M22)
                    } else if (isClass(octy) && _classes[octy].hasCtor) {
                        line(n->line); indent(depth);
                        *_out << emitCtorCall(nm, _classes[octy], oc->args, n->line) << ";\n";
                    } else if (!isClass(octy)) {
                        unsupported("`new` of a non-class type", n->line);
                    }
                    // class with no ctor: left default-initialized
                } else if (isSmartPtrClass(ty) && smartKind(ty) == CollKind::Weak
                           && isSmartPtrLValue(d->initializer) && exprClass(d->initializer) != ty) {
                    // Shared->Weak conversion (different C structs, same layout):
                    // field-copy + weak retain. The source Shared stays valid.
                    line(n->line); indent(depth);
                    std::string src = emitExpression(d->initializer);
                    *_out << nm << ".ptr = (" << src << ").ptr; " << nm << ".ctrl = (" << src << ").ctrl;\n";
                    indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->weak++;\n";
                } else if (isBindableClass(ty)) {
                    // BindableFunctionPtr <- free function (promote) or another bindable (move).
                    line(n->line);
                    emitBindablePromote(nm, ty, d->initializer, depth);
                } else {
                    // Copy-initialize from another expression.
                    line(n->line); indent(depth);
                    *_out << nm << " = " << emitExpression(d->initializer) << ";\n";
                    // Smart-pointer copy from an lvalue: Owned MOVES (invalidate the
                    // source so only `nm` drops it); Shared/Weak RETAIN (strong/weak++
                    // — both handles stay valid).
                    if (isSmartPtrLValue(d->initializer)) {
                        CollKind k = smartKind(ty);
                        indent(depth);
                        if (k == CollKind::Owned)
                            *_out << smartPtrInvalidate(emitExpression(d->initializer), k) << "\n";
                        else
                            *_out << nm << ".ctrl->" << (k == CollKind::Weak ? "weak" : "strong") << "++;\n";
                    }
                }
            };
            if (lvd && lvd->variables)
                for (auto& d : *lvd->variables) { if (d) emitDeclarator(d); }
            else if (cvd && cvd->variables)
                for (auto& d : *cvd->variables) { if (d) emitDeclarator(d); }
            return;
        }
    }

    if (auto* ret = dynamic_cast<ReturnNode*>(n)) {
        line(n->line);
        // Capture the return value BEFORE running any destructors (it may
        // reference locals about to be destroyed), then unwind, then return.
        if (ret->expression && _currentReturnCType != "void") {
            std::string tmp = "__ret_" + std::to_string(_tempCounter++);
            indent(depth);
            *_out << _currentReturnCType << " " << tmp << " = " << emitExpression(ret->expression) << ";\n";
            // Smart-pointer move-out: returning a smart-ptr local transfers the
            // ref/ownership to the caller. Invalidate it BEFORE the unwind so the
            // scope's dtor doesn't free/decrement what the caller now owns (the
            // factory-function landmine).
            if (isSmartPtrLValue(ret->expression)) {
                indent(depth);
                *_out << smartPtrInvalidate(emitExpression(ret->expression),
                                           smartKind(exprClass(ret->expression))) << "\n";
            }
            // Same move-out for a returned BindableFunctionPtr (M22): it may own its
            // bound object, so the scope dtor must NOT drop what the caller now owns.
            else if (auto* rid = dynamic_cast<IdentifierNode*>(ret->expression.get())) {
                if (rid->value && isBindableClass(exprClass(ret->expression))) {
                    std::string e = emitExpression(ret->expression);
                    indent(depth);
                    *_out << "(" << e << ").obj = NULL; (" << e << ").ctrl = NULL; ("
                          << e << ").fn = NULL; (" << e << ").elemdtor = NULL;\n";
                }
            }
            emitUnwindAll(depth);
            indent(depth); *_out << "return " << tmp << ";\n";
        } else {
            if (ret->expression) { indent(depth); *_out << emitExpression(ret->expression) << ";\n"; }
            emitUnwindAll(depth);
            indent(depth); *_out << "return;\n";
        }
        return;
    }

    if (auto* f = dynamic_cast<IfNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "if (" << emitExpression(f->booleanExpression) << ") ";
        emitBody(f->ifStatement, depth, /*loopBoundary=*/false);
        if (f->elseStatement) { *_out << " else "; emitBody(f->elseStatement, depth, false); }
        *_out << "\n";
        return;
    }

    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "while (" << emitExpression(w->booleanExpression) << ") ";
        emitBody(w->whileStatement, depth, /*loopBoundary=*/true);
        *_out << "\n";
        return;
    }

    if (auto* d = dynamic_cast<DoWhileNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "do ";
        emitBody(d->doWhileStatement, depth, /*loopBoundary=*/true);
        *_out << " while (" << emitExpression(d->booleanExpression) << ");\n";
        return;
    }

    if (auto* f = dynamic_cast<ForNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "for (" << emitForClause(f->initializerStatements) << "; "
             << (f->booleanExpression ? emitExpression(f->booleanExpression) : std::string()) << "; "
             << emitForClause(f->iteratorStatements) << ") ";
        emitBody(f->body, depth, /*loopBoundary=*/true);
        *_out << "\n";
        return;
    }

    if (dynamic_cast<BreakNode*>(n)) {
        line(n->line);
        emitUnwindToLoop(depth);   // dtors must run before the break keyword
        indent(depth); *_out << "break;\n";
        return;
    }
    if (dynamic_cast<ContinueNode*>(n)) {
        line(n->line);
        emitUnwindToLoop(depth);
        indent(depth); *_out << "continue;\n";
        return;
    }

    if (auto* sw = dynamic_cast<SwitchNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "switch (" << emitExpression(sw->expression) << ") {\n";
        if (sw->switchsections) {
            for (auto& sec : *sw->switchsections) {
                if (sec->labels) {
                    for (auto& lbl : *sec->labels) {
                        indent(depth + 1);
                        if (lbl->isDefault())
                            *_out << "default:\n";
                        else
                            *_out << "case " << emitExpression(lbl->constantExpression) << ":\n";
                    }
                }
                // Wrap the section body in a block: C forbids a declaration
                // directly after a `case` label (e.g. a `return`'s __ret temp).
                indent(depth + 1); *_out << "{\n";
                SharedStatement last;
                if (sec->statementList) {
                    for (auto& st : *sec->statementList) { emitStatement(st, depth + 2); last = st; }
                }
                // cstar switch sections don't fall through; add break unless the
                // section already ends in a break/return.
                bool ends = last && (dynamic_cast<BreakNode*>(last.get()) ||
                                     dynamic_cast<ReturnNode*>(last.get()));
                if (!ends) { indent(depth + 2); *_out << "break;\n"; }
                indent(depth + 1); *_out << "}\n";
            }
        }
        indent(depth);
        *_out << "}\n";
        return;
    }

    if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        line(n->line); indent(depth);
        std::string itCls = exprClass(fe->expression);
        if (itCls.empty() || !_classes.count(itCls) || !_classes[itCls].isCollection) {
            unsupported("foreach over a non-collection", n->line); *_out << "\n"; return;
        }
        const std::string& coll = _classes[itCls].name;
        std::string elemTy   = cType(fe->type);
        std::string elemClass = _classes[itCls].collElemClass;   // "" if primitive element
        std::string nm = (fe->name && fe->name->value) ? *fe->name->value : "__x";
        int id = _tempCounter++;
        std::string fp = "__fe" + std::to_string(id);
        std::string ix = "__i"  + std::to_string(id);
        std::string recvExpr = emitExpression(fe->expression);

        // Outer wrapper holds the receiver pointer (evaluate the receiver once).
        *_out << "{\n";
        indent(depth + 1); *_out << coll << "* " << fp << " = &(" << recvExpr << ");\n";
        indent(depth + 1);
        *_out << "for (size_t " << ix << " = 0; " << ix << " < " << coll << "__length(" << fp
             << "); ++" << ix << ") {\n";

        // Loop-body scope (a loop boundary so break/continue unwind correctly).
        Scope sc; sc.isLoopBoundary = true;
        _scopes.push_back(sc);
        bool hadType = _localTypes.count(nm);
        std::string prevType = hadType ? _localTypes[nm] : std::string();
        _localTypes[nm] = elemClass;   // element binding's class (for x.method() resolution)

        // The element binding is a borrowed copy — NOT recorded destructible.
        indent(depth + 2);
        *_out << elemTy << " " << nm << " = " << coll << "__get(" << fp << ", " << ix << ");\n";

        SharedStatement last;
        if (auto* b = dynamic_cast<BlockNode*>(fe->body.get())) {
            if (b->statements) for (auto& st : *b->statements) { emitStatement(st, depth + 2); last = st; }
        } else if (fe->body) {
            emitStatement(fe->body, depth + 2); last = fe->body;
        }
        if (!(last && stmtIsJump(last))) emitScopeCleanup(_scopes.back(), depth + 2);

        if (hadType) _localTypes[nm] = prevType; else _localTypes.erase(nm);
        _scopes.pop_back();

        indent(depth + 1); *_out << "}\n";   // close for
        indent(depth);     *_out << "}\n";   // close wrapper
        return;
    }

    // Smart-pointer assignment `b = a;` — release b's current pointee first (no
    // leak), copy, then either invalidate the source (Owned: move) or retain
    // (Shared: refcount++). The null/retain is a statement, so it can't live in
    // an expression.
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        if (as->token == EQ && isSmartPtrExpr(as->unaryExpression)) {
            checkConstWrite(as->unaryExpression, n->line);   // M24a: no reseating a const smart ptr
            std::string b   = emitExpression(as->unaryExpression);
            std::string ty  = exprClass(as->unaryExpression);      // Owned_T / Shared_T / Weak_T
            CollKind    knd = smartKind(ty);
            SharedExpression rhs = as->expression;
            bool rhsLval = isSmartPtrLValue(rhs);
            std::string src = emitExpression(rhs);                  // evaluate the RHS once
            line(n->line);
            // Step 4: self-assignment (`a = a`) would release a's pointee, then "copy" it
            // back onto the freed memory -> use-after-free. When the RHS is an lvalue, guard
            // the whole release-and-reseat with an address check (a no-op for `a = a`).
            int d2 = depth;
            if (rhsLval) { indent(depth); *_out << "if (&" << b << " != &(" << src << ")) {\n"; d2 = depth + 1; }
            indent(d2); *_out << ty << "__dtor(&" << b << ");\n";   // release b's old
            if (knd == CollKind::Weak && rhsLval && exprClass(rhs) != ty) {
                // Shared->Weak reseat: field-copy + weak retain.
                indent(d2); *_out << b << ".ptr = (" << src << ").ptr; " << b << ".ctrl = (" << src << ").ctrl;\n";
                indent(d2); *_out << "if (" << b << ".ctrl) " << b << ".ctrl->weak++;\n";
            } else {
                indent(d2); *_out << b << " = " << src << ";\n";
                if (rhsLval) {                                       // copy from a local
                    indent(d2);
                    if (knd == CollKind::Owned) *_out << smartPtrInvalidate(src, knd) << "\n";
                    else *_out << b << ".ctrl->" << (knd == CollKind::Weak ? "weak" : "strong") << "++;\n";
                }
            }
            if (rhsLval) { indent(depth); *_out << "}\n"; }
            return;
        }
    }

    // Bare expression statement (e.g. an assignment or call used as a statement).
    if (dynamic_cast<ExpressionStatementNode*>(n)) {
        line(n->line);
        indent(depth);
        *_out << emitExpression(std::dynamic_pointer_cast<ExpressionNode>(stmt)) << ";\n";
        return;
    }

    line(n->line);
    indent(depth);
    unsupported("statement", n->line);
    *_out << "\n";
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
        *_out << "{\n";
        emitStatement(stmt, depth + 1);
        if (!stmtIsJump(stmt))
            emitScopeCleanup(_scopes.back(), depth + 1);
        indent(depth);
        *_out << "}";
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
    return qualify(name);   // scope-prefixed (main -> cstar_main); _nsCtx set per file
}

std::vector<ParamSig> CEmitter::paramSigsOf(SharedParameterList params)
{
    std::vector<ParamSig> out;
    if (params) {
        for (auto& p : *params) {
            ParamSig ps;
            ps.name  = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            ps.byRef = paramByRef(p.get());
            ps.isConst = p->isConst;
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

        if (fn->modifier && fn->modifier->value && *fn->modifier->value == "export")
            unsupported("`export` is reserved (WASM/host export boundary) but not yet implemented", fn->line);

        // A bodiless top-level `fn ret Name(params);` (no body, not extern) is a
        // function-pointer SIGNATURE type (M21), not a callable — register in _sigs.
        if (!fn->block && !isExtern(fn)) {
            SigInfo si;
            si.cName    = qualify(*fn->name->value);
            si.retCType = cType(fn->returnType);
            si.params   = paramSigsOf(fn->parameters);
            _sigs[si.cName] = si;
            continue;
        }

        FuncSig sig;
        // extern functions are the FFI seam — keep their literal C name (never
        // namespace-mangle). Others are scope-prefixed (main -> cstar_main).
        sig.cName   = isExtern(fn) ? *fn->name->value : qualify(*fn->name->value);
        sig.retCType = cType(fn->returnType);
        sig.params  = paramSigsOf(fn->parameters);
        _funcs[sig.cName] = sig;
    }
}

// Collect interface declarations (method prototype lists).
void CEmitter::collectInterfaces(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* id = dynamic_cast<InterfaceDeclarationNode*>(decl.get());
        if (!id || !id->identifier || !id->identifier->value) continue;
        if (id->baseTypes && !id->baseTypes->empty())
            unsupported("interface inheritance (interface : interface) — deferred", id->line);
        InterfaceInfo ii;
        ii.name  = qualify(*id->identifier->value);   // M14
        ii.scope = _nsCtx.scope;
        ii.usings = _nsCtx.usings;
        if (id->body)
            for (auto& m : *id->body)
                if (m->name && m->name->value)
                    ii.methods.push_back({*m->name->value, m.get()});
        _interfaces[ii.name] = ii;
    }
}

// Collect enum declarations.
void CEmitter::collectEnums(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* ed = dynamic_cast<EnumDeclarationNode*>(decl.get());
        if (!ed || !ed->identifier || !ed->identifier->value) continue;
        EnumInfo ei;
        ei.name  = qualify(*ed->identifier->value);   // M14
        ei.scope = _nsCtx.scope;
        ei.usings = _nsCtx.usings;
        if (ed->body)
            for (auto& m : *ed->body)
                if (m->identifier && m->identifier->value)
                    ei.members.push_back({*m->identifier->value, m->constantExpression});
        _enums[ei.name] = ei;
    }
}

// enum Name { Name_M0, Name_M1 = <expr>, … }
void CEmitter::emitEnum(EnumInfo& ei)
{
    *_out << "typedef enum " << ei.name << " {\n";
    for (auto& m : ei.members) {
        indent(1);
        *_out << ei.name << "_" << m.name;
        if (m.value) *_out << " = " << emitExpression(m.value);
        *_out << ",\n";
    }
    *_out << "} " << ei.name << ";\n\n";
}

// Build the class table: ordered fields, methods, and the (single) constructor.
void CEmitter::collectClasses(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->name || !cd->name->value) continue;

        // FFI (M16): an `extern class` is an external C struct — keep its literal
        // C name (not namespace-mangled) and don't emit/own it.
        bool isExt = false;
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers)
                if (mod->value && *mod->value == "extern") isExt = true;

        ClassInfo ci;
        ci.name  = isExt ? *cd->name->value : qualify(*cd->name->value);   // M14 mangle / M16 literal
        ci.scope = _nsCtx.scope;
        ci.usings = _nsCtx.usings;
        ci.isExternStruct = isExt;
        if (isExt) _externNames.insert(ci.name);
        ci.node = cd;

        // Single inheritance (extends). Base/interface names are RESOLVED in
        // linkBases() once every file's declarations are registered.
        if (cd->baseTypes) {
            if (cd->baseTypes->base && cd->baseTypes->base->value)
                ci.baseName = *cd->baseTypes->base->value;   // bare; resolved in linkBases
            if (cd->baseTypes->interfaces)
                for (auto& itf : *cd->baseTypes->interfaces)
                    if (itf && itf->value) ci.interfaces.push_back(*itf->value);  // bare; resolved in linkBases
        }
        // Class-level KIND modifier (M25): pod | virtual | abstract | final.
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers) {
                if (!mod->value) continue;
                const std::string& mv = *mod->value;
                if (mv == "abstract") ci.isAbstractClass = true;
                else if (mv == "pod")      ci.isPod = true;
                else if (mv == "virtual")  ci.isVirtualClass = true;
                else if (mv == "final")    ci.isFinalClass = true;
                else if (mv == "export")
                    unsupported("`export` is reserved (WASM/host export boundary) but not yet implemented", cd->line);
                else if (mv == "volatile")
                    unsupported("`volatile` is reserved (embedded/MMIO) but not yet implemented", cd->line);
            }
        // M25b — a plain/`pod` class is sealed: only virtual/abstract/final classes may extend a base.
        // (The base must itself be extensible — checked in linkBases once names resolve.)
        if (!ci.baseName.empty() && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
            unsupported(("class '" + ci.name + "' extends '" + ci.baseName
                         + "'; only a `virtual`/`abstract`/`final class` may extend").c_str(), cd->line);

        if (cd->members) {
            for (auto& m : *cd->members) {
                ASTNode* mn = m.get();
                if (auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(mn)) {
                    if (fd->modifiers)
                        for (auto& mod : *fd->modifiers) {
                            if (!mod->value) continue;
                            if (*mod->value == "volatile")
                                unsupported("`volatile` is reserved (embedded/MMIO) but not yet implemented", fd->line);
                            if (*mod->value == "export")
                                unsupported("`export` is reserved (WASM/host export boundary) but not yet implemented", fd->line);
                        }
                    // M25b — a field takes NO visibility modifier: exposure is the class
                    // KIND (`pod`/extern struct = public; every other kind = private).
                    if (modHas(fd->modifiers, "public") || modHas(fd->modifiers, "protected") || modHas(fd->modifiers, "private"))
                        unsupported("a field takes no visibility modifier — data exposure is the class kind "
                                    "(`pod class` = public, otherwise private); expose data with an accessor method", fd->line);
                    Visibility fvis = (ci.isPod || ci.isExternStruct) ? Visibility::Public : Visibility::Private;
                    if (fd->declarators) {
                        for (auto& d : *fd->declarators) {
                            FieldInfo fi;
                            fi.name        = (d->name && d->name->value) ? *d->name->value : "";
                            fi.type        = fd->type;
                            fi.initializer = d->initializer;
                            fi.visibility  = fvis;
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
                        mi.isConst    = md->isConst;   // `const fn …` (M24b)
                        mi.visibility = visibilityOf(md->modifiers, Visibility::Private, md->line);  // M25
                        mi.isFinal    = modHas(md->modifiers, "final");                              // M25
                        if (md->modifiers)
                            for (auto& mod : *md->modifiers) {
                                if (!mod->value) continue;
                                if (*mod->value == "virtual")  mi.isVirtual = true;
                                if (*mod->value == "override") { mi.isVirtual = true; mi.isOverride = true; }
                                if (*mod->value == "abstract") { mi.isVirtual = true; mi.isAbstract = true; }
                                if (*mod->value == "export")
                                    unsupported("`export` is reserved (WASM/host export boundary) but not yet implemented", md->line);
                                if (*mod->value == "volatile")
                                    unsupported("`volatile` is reserved (embedded/MMIO) but not yet implemented", md->line);
                            }
                        if (!md->body) mi.isAbstract = mi.isVirtual = true;   // null body => pure
                        // M25b — polymorphism rules.
                        if (mi.isVirtual) {
                            const std::string& mname = *md->name->value;
                            const char* kw = mi.isAbstract ? "abstract" : mi.isOverride ? "override" : "virtual";
                            // (3) an overridable method is written `protected` (never public/private):
                            //     a private virtual can't be overridden, a public one is the interface's job.
                            if (mi.visibility != Visibility::Protected)
                                unsupported(("overridable method '" + mname + "' must be declared `protected` (write `protected "
                                    + kw + "`); public polymorphism belongs on an interface").c_str(), md->line);
                            // (4a) the class kind must opt in to the method's polymorphism.
                            if (mi.isAbstract && !ci.isAbstractClass)
                                unsupported(("class '" + ci.name + "' declares an abstract method; declare it `abstract class`").c_str(), md->line);
                            else if (mi.isOverride && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
                                unsupported(("class '" + ci.name + "' overrides a method; declare it `virtual`/`abstract`/`final class`").c_str(), md->line);
                            else if (!mi.isAbstract && !mi.isOverride && !ci.isVirtualClass && !ci.isAbstractClass)
                                unsupported(("class '" + ci.name + "' declares a virtual method; declare it `virtual class`").c_str(), md->line);
                        }
                        // M25b — `final` seals a virtual slot; reject the meaningless/contradictory cases.
                        if (mi.isFinal && mi.isAbstract)
                            unsupported(("`final abstract` on '" + *md->name->value + "' is a contradiction (an abstract method must be overridden)").c_str(), md->line);
                        else if (mi.isFinal && !mi.isVirtual)
                            unsupported(("`final` on '" + *md->name->value + "' applies only to an overridable (virtual/override) method").c_str(), md->line);
                        ci.methods[*md->name->value] = mi;
                    }
                } else if (auto* cc = dynamic_cast<ClassConstructorDeclarationNode*>(mn)) {
                    if (ci.hasCtor)
                        unsupported("multiple constructors (no overloading yet)", cc->line);
                    ci.hasCtor   = true;
                    ci.ctorNode  = cc;
                    ci.ctorVisibility = visibilityOf(cc->modifiers, Visibility::Private, cc->line);  // M25
                    if (cc->declarator) ci.ctorParams = paramSigsOf(cc->declarator->params);
                } else if (auto* dd = dynamic_cast<ClassDestructorDeclarationNode*>(mn)) {
                    ci.hasDtor  = true;
                    ci.dtorNode = dd;
                } else if (auto* kd = dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    // M24d: a `const` data member — a normal struct field, written
                    // ONCE in the constructor (inline init or `this.f = …`), then
                    // immutable. Enforcement is at the cstar level; the C field is plain.
                    // M25b — like any field, no visibility modifier; the kind decides exposure.
                    if (modHas(kd->modifiers, "public") || modHas(kd->modifiers, "protected") || modHas(kd->modifiers, "private"))
                        unsupported("a field takes no visibility modifier — data exposure is the class kind "
                                    "(`pod class` = public, otherwise private); expose data with an accessor method", kd->line);
                    Visibility kvis = (ci.isPod || ci.isExternStruct) ? Visibility::Public : Visibility::Private;
                    if (kd->declarators)
                        for (auto& d : *kd->declarators) {
                            FieldInfo fi;
                            fi.name        = (d->name && d->name->value) ? *d->name->value : "";
                            fi.type        = kd->type;
                            fi.initializer = d->initializer;
                            fi.visibility  = kvis;
                            ci.fields.push_back(fi);
                            ci.fieldNames.insert(fi.name);
                            ci.constFields.insert(fi.name);
                        }
                } else if (dynamic_cast<ClassOperatorDeclarationNode*>(mn)) {
                    unsupported("operator overload — deferred", mn->line);
                }
            }
        }
        // M25b — a `virtual`/`abstract class` must actually declare an overridable method
        // (else the qualifier is a lie); the reverse of rule 4a.
        if (ci.isVirtualClass || ci.isAbstractClass) {
            bool hasOverridable = false;
            for (auto& kv : ci.methods) if (kv.second.isVirtual) { hasOverridable = true; break; }
            if (!hasOverridable)
                unsupported(("`" + std::string(ci.isAbstractClass ? "abstract" : "virtual") + " class` '"
                             + ci.name + "' declares no overridable (virtual/abstract) method").c_str(), cd->line);
        }
        // M25b — `pod class` is plain data: no methods, no base, no vtable.
        if (ci.isPod && !ci.methods.empty())
            unsupported(("`pod class` '" + ci.name + "' may not declare methods (operators/static only)").c_str(), cd->line);
        _classes[ci.name] = ci;
    }
}

// ---- Collections (M9) -----------------------------------------------------

// The mangling suffix for an element type: primitives use a short stable
// spelling; a class/enum uses its own name. Array<int32> -> "int32".
std::string CEmitter::mangleElem(SharedIdentifier elem)
{
    if (!elem) return "void";
    switch (elem->builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "int8";
        case IDENTIFIER_INT16_VAL:   return "int16";
        case IDENTIFIER_INT32_VAL:   return "int32";
        case IDENTIFIER_INT64_VAL:   return "int64";
        case IDENTIFIER_UINT8_VAL:   return "uint8";
        case IDENTIFIER_UINT16_VAL:  return "uint16";
        case IDENTIFIER_UINT32_VAL:  return "uint32";
        case IDENTIFIER_UINT64_VAL:  return "uint64";
        case IDENTIFIER_BOOL_VAL:    return "bool";
        case IDENTIFIER_FLOAT32_VAL: return "float32";
        case IDENTIFIER_FLOAT64_VAL: return "float64";
        default:  // class element — resolve to its mangled name (the suffix)
            return elem->value ? resolveUserName(*elem->value, elem->qualifier) : "void";
    }
}

bool CEmitter::isCollectionType(SharedIdentifier t) const
{
    if (!t) return false;
    if (t->builtInVal == IDENTIFIER_STRING_VAL) return true;          // string / String
    return t->genericArg && t->value &&
           (*t->value == "Array" || *t->value == "List" || *t->value == "Owned" ||
            *t->value == "Shared" || *t->value == "Weak" || *t->value == "BindableFunctionPtr");
}

// Discover a used Coll<T> instantiation: register a CollectionInfo (drives the
// macro emission) and a synthetic ClassInfo (so dispatch/RAII/decl reuse works).
void CEmitter::registerCollection(SharedIdentifier collType)
{
    if (!isCollectionType(collType)) return;

    bool isStr    = collType->builtInVal == IDENTIFIER_STRING_VAL;
    bool isOwned  = !isStr && collType->value && *collType->value == "Owned";
    bool isShared = !isStr && collType->value && *collType->value == "Shared";
    bool isWeak   = !isStr && collType->value && *collType->value == "Weak";
    bool isSmart  = isOwned || isShared || isWeak;
    CollKind kind = isStr    ? CollKind::String
                  : isOwned  ? CollKind::Owned
                  : isShared ? CollKind::Shared
                  : isWeak   ? CollKind::Weak
                  : (*collType->value == "List") ? CollKind::List : CollKind::Array;
    SharedIdentifier elem = isStr ? SharedIdentifier() : collType->genericArg;
    std::string elemCType  = isStr ? "" : cType(elem);
    std::string elemMangle = isStr ? "" : mangleElem(elem);
    std::string elemClass  = (!isStr && isClass(elemCType)) ? elemCType : "";

    // BindableFunctionPtr<Sig> (M22) — element is a function SIGNATURE, not a class.
    if (!isStr && collType->value && *collType->value == "BindableFunctionPtr") {
        registerBindable(elem);
        return;
    }

    // Smart pointers (Owned/Shared/Weak) — registered via the shared helper.
    if (isSmart) {
        if (elemClass.empty()) {
            unsupported("a smart pointer requires a class element type", collType->line);
            return;
        }
        // A Weak needs its Shared (lock()'s return type + the source of a weak).
        if (isWeak) registerSmartPtr(CollKind::Shared, elem);
        registerSmartPtr(kind, elem);
        return;
    }

    std::string cName = isStr ? "cstar_string"
                      : (kind == CollKind::List ? "List_" : "Array_") + elemMangle;

    if (_collections.count(cName)) return;    // dedup

    CollectionInfo info;
    info.kind = kind; info.cName = cName;
    info.elemCType = elemCType; info.elemMangle = elemMangle; info.elemClass = elemClass;
    info.elemDestructible = !elemClass.empty() && _classes.count(elemClass) && _classes[elemClass].destructible;
    _collections[cName] = info;

    // Synthetic ClassInfo: a struct with a dtor, and (collections only) intrinsic methods.
    ClassInfo ci;
    ci.name = cName;
    ci.isCollection = true;
    ci.collKind = kind;
    ci.collElemClass = elemClass;
    ci.destructible = true;                    // owns heap -> RAII frees
    ci.hasCtor = !isStr;                        // strings come from literals/concat
    ci.ctorParams = (kind == CollKind::Array)
                        ? std::vector<ParamSig>{ ParamSig{"size", false, ""} }
                        : std::vector<ParamSig>{};

    auto addMethod = [&](const std::string& mname, std::vector<ParamSig> params, SharedIdentifier ret) {
        MethodInfo mi;
        mi.cName = cName + "__" + mname;
        mi.params = std::move(params);
        mi.returnType = ret;
        mi.isIntrinsic = true;
        ci.methods[mname] = mi;
    };
    if (kind == CollKind::String) {
        addMethod("length", {}, SharedIdentifier());
        addMethod("equals", { ParamSig{"other", false, ""} }, SharedIdentifier());
        addMethod("concat", { ParamSig{"other", false, ""} }, collType);   // returns a string
        addMethod("cstr",   {}, SharedIdentifier());                        // FFI: const char*
    } else {
        if (kind == CollKind::List)
            addMethod("add", { ParamSig{"item", false, elemClass} }, SharedIdentifier());
        addMethod("get",    { ParamSig{"index", false, ""} }, elem);
        addMethod("set",    { ParamSig{"index", false, ""}, ParamSig{"value", false, elemClass} }, SharedIdentifier());
        addMethod("length", {}, SharedIdentifier());
        addMethod("dataPtr", {}, SharedIdentifier());   // FFI bridge (M17): Ptr<T> to the buffer
        addMethod("byteLen", {}, SharedIdentifier());   // len * sizeof(T)
    }

    _classes[cName] = ci;
}

// Register a smart pointer (Owned/Shared/Weak) as a synthetic ClassInfo backed by
// a runtime macro. Smart pointers expose a T* `ptr` (auto-deref) and have no real
// ctor (construction is inline). Shared/Weak carry a few intrinsic methods.
void CEmitter::registerSmartPtr(CollKind kind, SharedIdentifier elem)
{
    std::string elemCType  = cType(elem);
    std::string elemMangle = mangleElem(elem);
    std::string elemClass  = isClass(elemCType) ? elemCType : "";
    if (elemClass.empty()) return;              // caller diagnosed
    std::string cName = (kind == CollKind::Owned  ? "Owned_"
                       : kind == CollKind::Shared ? "Shared_" : "Weak_") + elemMangle;
    if (_collections.count(cName)) return;      // dedup

    CollectionInfo info;
    info.kind = kind; info.cName = cName;
    info.elemCType = elemCType; info.elemMangle = elemMangle; info.elemClass = elemClass;
    info.elemDestructible = _classes.count(elemClass) && _classes[elemClass].destructible;
    _collections[cName] = info;

    ClassInfo ci;
    ci.name = cName; ci.isCollection = true; ci.collKind = kind;
    ci.collElemClass = elemClass; ci.destructible = true; ci.hasCtor = false;
    auto addM = [&](const std::string& m, std::vector<ParamSig> p) {
        MethodInfo mi; mi.cName = cName + "__" + m; mi.params = std::move(p);
        mi.isIntrinsic = true; ci.methods[m] = mi;
    };
    // Intrinsics (not auto-deref forwarded): Owned has none; Shared has valid();
    // Weak has lock() (-> Shared) and expired().
    if (kind == CollKind::Shared) addM("valid", {});
    if (kind == CollKind::Weak) { addM("lock", {}); addM("expired", {}); }
    _classes[cName] = ci;
}

// Register a BindableFunctionPtr<Sig> (M22) — a callable that may own a bound
// receiver. Element is a signature type (from `fnptr`), not a class. Backed by the
// fully type-erased CSTAR_BINDABLE_DEFINE struct; the sig drives only the invoke.
void CEmitter::registerBindable(SharedIdentifier elem)
{
    std::string sigCName = cType(elem);
    if (!isSigType(sigCName)) {
        unsupported("BindableFunctionPtr<Sig> requires a function-signature type (declared with `fnptr`)",
                    elem ? elem->line : 0);
        return;
    }
    std::string cName = "BindableFunctionPtr_" + mangleElem(elem);
    if (_collections.count(cName)) return;            // dedup

    CollectionInfo info;
    info.kind = CollKind::Bindable; info.cName = cName;
    info.elemCType = sigCName; info.elemMangle = mangleElem(elem); info.elemClass = "";
    info.elemDestructible = false;
    _collections[cName] = info;

    ClassInfo ci;
    ci.name = cName; ci.isCollection = true; ci.collKind = CollKind::Bindable;
    ci.collElemClass = sigCName;       // reused at invoke: the bound signature's cName
    ci.destructible = true;            // owns heap (when bound) -> RAII drop
    ci.hasCtor = false;                // constructed via the dedicated bind path, not a ctor
    _classes[cName] = ci;
}

bool CEmitter::isBindableClass(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isCollection && it->second.collKind == CollKind::Bindable;
}

void CEmitter::scanTypeForCollections(SharedIdentifier t)
{
    if (!t) return;
    if (isCollectionType(t)) registerCollection(t);
    if (t->genericArg) scanTypeForCollections(t->genericArg);   // nested (harmless)
}

void CEmitter::scanExprForCollections(SharedExpression e)
{
    if (!e) return;
    ASTNode* n = e.get();
    if (auto* oc = dynamic_cast<ObjectCreationNode*>(n)) {
        scanTypeForCollections(oc->type);
        if (oc->args) for (auto& a : *oc->args) if (a) scanExprForCollections(a->expression);
    } else if (auto* c = dynamic_cast<CastNode*>(n)) {
        scanTypeForCollections(c->type);
        scanExprForCollections(c->unaryExpression);
    } else if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) {
        scanExprForCollections(b->LHS); scanExprForCollections(b->RHS);
    } else if (auto* l = dynamic_cast<LogicalAndOrNode*>(n)) {
        scanExprForCollections(l->LHS); scanExprForCollections(l->RHS);
    } else if (auto* tn = dynamic_cast<TernaryExpressionNode*>(n)) {
        scanExprForCollections(tn->condition); scanExprForCollections(tn->LHS); scanExprForCollections(tn->RHS);
    } else if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        scanExprForCollections(as->unaryExpression); scanExprForCollections(as->expression);
    } else if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        scanExprForCollections(inv->expression);
        if (inv->args) for (auto& a : *inv->args) if (a) scanExprForCollections(a->expression);
    } else if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        scanExprForCollections(ea->expression);
        if (ea->expressionlist) for (auto& x : *ea->expressionlist) scanExprForCollections(x);
    } else if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        scanExprForCollections(ma->expression);
    } else if (auto* pe = dynamic_cast<PreIncrDecrNode*>(n)) {
        scanExprForCollections(pe->expression);
    } else if (auto* po = dynamic_cast<PostIncrDecrNode*>(n)) {
        scanExprForCollections(po->expression);
    } else if (auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        scanExprForCollections(su->expression);
    }
}

void CEmitter::scanStmtForCollections(SharedStatement s)
{
    if (!s) return;
    ASTNode* n = s.get();
    if (auto* b = dynamic_cast<BlockNode*>(n)) {
        if (b->statements) for (auto& st : *b->statements) scanStmtForCollections(st);
    } else if (auto* d = dynamic_cast<LocalVariableDeclaration*>(n)) {
        scanTypeForCollections(d->type);
        if (d->variables) for (auto& v : *d->variables) if (v) scanExprForCollections(v->initializer);
    } else if (auto* cd = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        scanTypeForCollections(cd->type);
    } else if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        scanExprForCollections(r->expression);
    } else if (auto* f = dynamic_cast<IfNode*>(n)) {
        scanExprForCollections(f->booleanExpression);
        scanStmtForCollections(f->ifStatement); scanStmtForCollections(f->elseStatement);
    } else if (auto* w = dynamic_cast<WhileNode*>(n)) {
        scanExprForCollections(w->booleanExpression); scanStmtForCollections(w->whileStatement);
    } else if (auto* dw = dynamic_cast<DoWhileNode*>(n)) {
        scanExprForCollections(dw->booleanExpression); scanStmtForCollections(dw->doWhileStatement);
    } else if (auto* fr = dynamic_cast<ForNode*>(n)) {
        if (fr->initializerStatements) for (auto& st : *fr->initializerStatements) scanStmtForCollections(st);
        scanExprForCollections(fr->booleanExpression);
        if (fr->iteratorStatements) for (auto& st : *fr->iteratorStatements) scanStmtForCollections(st);
        scanStmtForCollections(fr->body);
    } else if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        scanTypeForCollections(fe->type);
        scanExprForCollections(fe->expression);
        scanStmtForCollections(fe->body);
    } else if (auto* sw = dynamic_cast<SwitchNode*>(n)) {
        scanExprForCollections(sw->expression);
        if (sw->switchsections) for (auto& sec : *sw->switchsections)
            if (sec && sec->statementList) for (auto& st : *sec->statementList) scanStmtForCollections(st);
    } else if (dynamic_cast<ExpressionStatementNode*>(n)) {
        scanExprForCollections(std::dynamic_pointer_cast<ExpressionNode>(s));
    }
}

// Pre-pass: scan the whole program for Coll<T> instantiations.
void CEmitter::collectCollections(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            scanTypeForCollections(fn->returnType);
            if (fn->parameters) for (auto& p : *fn->parameters) if (p) scanTypeForCollections(p->type);
            scanStmtForCollections(fn->block);
        } else if (auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get())) {
            if (cd->members) for (auto& m : *cd->members) {
                ASTNode* mn = m.get();
                if (auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(mn)) {
                    scanTypeForCollections(fd->type);
                } else if (auto* kd = dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    scanTypeForCollections(kd->type);   // const field of a collection type (M24d)
                } else if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(mn)) {
                    scanTypeForCollections(md->returnType);
                    if (md->params) for (auto& p : *md->params) if (p) scanTypeForCollections(p->type);
                    scanStmtForCollections(md->body);
                } else if (auto* cc = dynamic_cast<ClassConstructorDeclarationNode*>(mn)) {
                    if (cc->declarator && cc->declarator->params)
                        for (auto& p : *cc->declarator->params) if (p) scanTypeForCollections(p->type);
                    scanStmtForCollections(cc->body);
                } else if (auto* dd = dynamic_cast<ClassDestructorDeclarationNode*>(mn)) {
                    scanStmtForCollections(dd->body);
                }
            }
        }
    }
}

// Pass C: emit the CSTAR_*_DEFINE(...) macro line per registered instantiation.
void CEmitter::emitCollectionDefs()
{
    for (auto& kv : _collections) {
        CollectionInfo& info = kv.second;
        std::string elemDtor = info.elemDestructible ? (info.elemClass + "__dtor") : "CSTAR_ELEM_NODTOR";
        if (info.kind == CollKind::Array) {
            *_out << "CSTAR_ARRAY_DEFINE(" << info.elemCType << ", " << info.cName
                 << ", " << elemDtor << ")\n";
        } else if (info.kind == CollKind::List) {
            *_out << "CSTAR_LIST_DEFINE(" << info.elemCType << ", " << info.cName
                 << ", " << elemDtor << ")\n";
        } else if (info.kind == CollKind::Owned) {
            *_out << "CSTAR_OWNED_DEFINE(" << info.elemCType << ", " << info.cName
                 << ", " << elemDtor << ")\n";
        } else if (info.kind == CollKind::Shared) {
            *_out << "CSTAR_SHARED_DEFINE(" << info.elemCType << ", " << info.cName
                 << ", " << elemDtor << ")\n";
        } else if (info.kind == CollKind::Weak) {
            // lock() returns the matching Shared (emitted earlier — map order Shared_ < Weak_).
            *_out << "CSTAR_WEAK_DEFINE(" << info.elemCType << ", " << info.cName
                 << ", Shared_" << info.elemMangle << ")\n";
        } else if (info.kind == CollKind::Bindable) {
            // Fully type-erased — the signature drives only the invoke, not the layout.
            *_out << "CSTAR_BINDABLE_DEFINE(" << info.cName << ")\n";
        }
    }
    if (!_collections.empty()) *_out << "\n";
}

// If `ea` indexes a collection, fill coll (cName), recvExpr, idx; return true.
bool CEmitter::collectionElemAccess(ElementAccessNode* ea, std::string& coll,
                                    std::string& recvExpr, std::string& idx)
{
    if (!ea) return false;
    SharedExpression recv = ea->expression ? ea->expression
                                           : std::static_pointer_cast<ExpressionNode>(ea->identifier);
    if (!recv) return false;
    std::string cls = exprClass(recv);
    if (cls.empty() || !_classes.count(cls) || !_classes[cls].isCollection) return false;
    coll     = _classes[cls].name;
    recvExpr = emitExpression(recv);
    idx      = (ea->expressionlist && !ea->expressionlist->empty())
                   ? emitExpression((*ea->expressionlist)[0]) : "0";
    return true;
}

// ---- Smart pointers (M10 Owned, M11 Shared) -------------------------------

bool CEmitter::isSmartPtrClass(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isCollection &&
           (it->second.collKind == CollKind::Owned || it->second.collKind == CollKind::Shared ||
            it->second.collKind == CollKind::Weak);
}

CollKind CEmitter::smartKind(const std::string& cls) const
{
    return _classes.at(cls).collKind;   // precondition: isSmartPtrClass(cls)
}

bool CEmitter::derefSmartPtr(std::string& cls, std::string& recvExpr)
{
    if (!isSmartPtrClass(cls)) return false;
    recvExpr = "(" + recvExpr + ").ptr";        // both Owned & Shared expose a T*
    cls      = _classes[cls].collElemClass;     // effective class = pointee T
    return true;
}

bool CEmitter::isSmartPtrExpr(SharedExpression e)
{
    return e && isSmartPtrClass(exprClass(e));
}

// A plain transferable lvalue: a bare identifier naming a smart-pointer local/
// param. A `new ...<T>(...)` initializer is NOT an lvalue (no source to touch).
bool CEmitter::isSmartPtrLValue(SharedExpression e)
{
    auto* id = dynamic_cast<IdentifierNode*>(e.get());
    return id && id->value && isSmartPtrExpr(e);
}

// Invalidate a moved-from smart pointer: null the field its dtor guards on, so
// the source's drop becomes a no-op (the ref/ownership transfers to the dest).
std::string CEmitter::smartPtrInvalidate(const std::string& expr, CollKind kind)
{
    return (kind == CollKind::Owned) ? (expr + ".ptr = NULL;")          // Owned dtor guards on ptr
                                     : (expr + ".ctrl = NULL; " + expr + ".ptr = NULL;");  // Shared/Weak guard on ctrl
}

std::string CEmitter::emitSmartPtrCall(const std::string& cls, const std::string& recvExpr,
                                       const std::string& method, SharedArgumentList args, int srcLine)
{
    // An intrinsic on the pointer itself (lock/expired/valid)?
    if (_classes[cls].methods.count(method))
        return emitDispatch(cls, "&(" + recvExpr + ")", method, args, srcLine);
    // Otherwise auto-deref to the pointee T (Owned/Shared expose a T* ptr).
    if (smartKind(cls) != CollKind::Weak)
        return emitDispatch(_classes[cls].collElemClass, "(" + recvExpr + ").ptr", method, args, srcLine);
    unsupported(("Weak<T> has no member '" + method + "'; call .lock() to upgrade").c_str(), srcLine);
    return "0";
}

// Resolve `extends` names to ClassInfo pointers; error on unknown/cycle.
void CEmitter::linkBases()
{
    // Now every file's declarations are registered: resolve each class's base +
    // interface references (bare/qualified) to their mangled names, in the
    // class's own namespace context.
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.isCollection) continue;
        _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings;
        if (ci.node && ci.node->baseTypes && ci.node->baseTypes->base && ci.node->baseTypes->base->value)
            ci.baseName = resolveUserName(*ci.node->baseTypes->base->value, ci.node->baseTypes->base->qualifier);
        for (auto& itf : ci.interfaces) itf = resolveUserName(itf, nullptr);
    }
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.baseName.empty()) continue;
        auto it = _classes.find(ci.baseName);
        if (it == _classes.end()) {
            unsupported("unknown base class", ci.node ? ci.node->line : 0);
            ci.baseName.clear();
        } else {
            ci.base = &it->second;
            // M25b — the base must be extensible: a `virtual`/`abstract class`, never a
            // plain/`pod`/`final` (sealed) class.
            int bl = ci.node ? ci.node->line : 0;
            if (ci.base->isFinalClass)
                unsupported(("cannot extend '" + ci.base->name + "': it is a `final class` (a sealed leaf)").c_str(), bl);
            else if (!(ci.base->isVirtualClass || ci.base->isAbstractClass))
                unsupported(("cannot extend '" + ci.base->name
                             + "': only a `virtual`/`abstract class` may be extended").c_str(), bl);
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
        auto baseHasVirtual = [](ClassInfo* c, const std::string& m) {
            for (ClassInfo* b = c->base; b; b = b->base) {
                auto it = b->methods.find(m);
                if (it != b->methods.end() && it->second.isVirtual) return true;
            }
            return false;
        };
        for (auto& kv : ci->methods) {
            MethodInfo& mi = kv.second;
            const std::string& mname = kv.first;
            if (!mi.isVirtual) continue;
            // Step 3: `override` must override an actual virtual method in a base class —
            // otherwise there is no vtable slot to re-seat and the emitted C is malformed.
            if (mi.isOverride && !baseHasVirtual(ci, mname))
                unsupported(("'override fn " + mname + "' overrides no virtual method in any base class "
                             "(the base method must be `virtual`/`abstract`)").c_str(),
                            mi.node ? mi.node->line : 0);
            // M25b — a `final` slot may not be re-overridden by any subclass.
            if (mi.isOverride)
                for (ClassInfo* b = ci->base; b; b = b->base) {
                    auto it = b->methods.find(mname);
                    if (it != b->methods.end() && it->second.isVirtual) {
                        if (it->second.isFinal)
                            unsupported(("cannot override '" + mname + "': it is `final` in '" + b->name + "'").c_str(),
                                        mi.node ? mi.node->line : 0);
                        break;   // nearest declaring base wins
                    }
                }
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
        // A class is abstract if marked OR any virtual slot still has no implementation
        // (an inherited pure method left un-overridden). Either way it cannot be `new`ed —
        // instantiating it would leave a NULL vtable slot and crash on the first call.
        if (ci->hasVtable && !ci->isAbstractClass) {
            for (auto& vs : _rootVtables[ci->vtableRoot])
                if (!ci->slotImpl.count(vs.name)) { ci->isAbstractClass = true; break; }
        }

        // Bug 1 fix: a polymorphic class without an explicit constructor must still
        // get one synthesized, or `new` leaves __vptr uninitialized and the first
        // virtual call crashes. Topo order means the base is flagged first, so a
        // derived synth ctor sees base->hasCtor and chains it.
        if (ci->hasVtable && !ci->hasCtor && !ci->isCollection && !ci->isExternStruct) {
            ci->synthCtor = true;
            ci->hasCtor   = true;   // `new` now calls the ctor; prototype gets emitted
        }
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
            if (ci.destructible || ci.isExternStruct) continue;   // cstar doesn't own external structs
            _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings;   // resolve field types in ci's scope
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
    size_t named = 0;
    if (args)
        for (auto& a : *args)
            if (a->name && a->name->value) { byName[*a->name->value] = a.get(); named++; }

    // Step 5: named arguments ARE cstar's calling convention — validate them so a typo or
    // a duplicate can't silently do the wrong thing. (Missing args are caught per-param below.)
    if (named != byName.size())
        unsupported("duplicate named argument in call", srcLine);   // map collapsed a repeat
    {
        std::set<std::string> paramNames;
        for (auto& p : params) paramNames.insert(p.name);
        for (auto& kv : byName)
            if (!paramNames.count(kv.first))
                unsupported(("unknown argument name '" + kv.first + "' in call").c_str(), srcLine);
    }

    std::string s = cName + "(";
    bool first = true;
    if (!leadArg.empty()) { s += leadArg; first = false; }
    for (auto& p : params) {
        if (!first) s += ", ";
        first = false;
        auto f = byName.find(p.name);
        if (f == byName.end()) { unsupported("missing argument in call", srcLine); s += "0"; continue; }
        std::string val = emitExpression(f->second->expression);
        if (isInterface(p.className)) {
            std::string c = exprClass(f->second->expression);
            if (p.byRef) {
                // `ref`/`out` interface: the callee may reseat the caller's handle, so the
                // argument must be an actual interface variable (pass its address). A
                // concrete class would need a throwaway temp — reject it; bind first.
                if (!c.empty() && isClass(c))
                    unsupported(("cannot pass '" + c + "' by `ref`/`out` to interface parameter '" + p.name
                                 + "'; bind it to an `" + p.className + "` first "
                                 "(`" + p.className + " s = …; … ref s`)").c_str(), srcLine);
                if (!p.isConst) checkConstWrite(f->second->expression, srcLine);
                s += "&(" + val + ")";
            } else {
                // by value: wrap a concrete object as an interface fat pointer (the borrow);
                // pass an existing interface value straight through.
                s += (!c.empty() && isClass(c)) ? fatPointer(p.className, c, val) : val;
            }
        } else if (p.byRef) {
            // M24 soundness: a non-const `ref`/`out` param can MUTATE its argument, so a
            // const binding (or a const field outside its ctor) may not be passed to one
            // — that would silently launder away const. (A `const ref` borrow is fine.)
            if (!p.isConst) checkConstWrite(f->second->expression, srcLine);
            s += isClass(p.className) ? ("(" + p.className + "*)&(" + val + ")")  // upcast for ref Base
                                      : ("&(" + val + ")");
        } else {
            // Passing a smart pointer by value is not supported (the ownership
            // transfer / retain would need a statement, and a param isn't auto-
            // dropped). Pass by `ref` to borrow it, or return it to transfer.
            // Flag rather than risk a silent double-free / refcount leak.
            if (isSmartPtrLValue(f->second->expression))
                unsupported("smart pointer passed by value (pass by `ref` to borrow, "
                            "or return it to transfer)", srcLine);
            s += val;
        }
    }
    return s + ")";
}

// M21: does a free function match a FunctionPtr signature? Positional, by C type.
bool CEmitter::sigMatches(const SigInfo& sig, const FuncSig& fn) const
{
    if (sig.retCType != fn.retCType) return false;
    if (sig.params.size() != fn.params.size()) return false;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (sig.params[i].className != fn.params[i].className) return false;
        if (sig.params[i].byRef    != fn.params[i].byRef)    return false;
    }
    return true;
}

// M24a: the root identifier a write ultimately targets, for deep-const checks.
// `x` -> "x";  `x.f`, `x[i]`, `x.f.g` -> "x";  `this`/`this.f` -> "this";  else "".
std::string CEmitter::rootBinding(SharedExpression e) const
{
    ASTNode* n = e.get();
    if (dynamic_cast<ThisAccessNode*>(n)) return "this";
    if (auto* id = dynamic_cast<IdentifierNode*>(n))
        return (id->value && (!id->qualifier || id->qualifier->empty())) ? *id->value : "";
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n))
        return ma->expression ? rootBinding(ma->expression) : "";
    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        if (ea->expression) return rootBinding(ea->expression);
        return (ea->identifier && ea->identifier->value) ? *ea->identifier->value : "";
    }
    return "";
}

// M24a/b: is a write/call root `const`? A const local/param, `this` inside a const
// method (recorded as "this" in _constLocals), or — in a const method — a bare field
// of the current class (a write to `f` is really `self->f`).
bool CEmitter::rootIsConst(const std::string& root) const
{
    if (root.empty()) return false;
    if (_constLocals.count(root)) return true;
    if (_constLocals.count("this") && _currentClass && !_localTypes.count(root)
        && const_cast<CEmitter*>(this)->findFieldOwner(_currentClass, root))
        return true;
    return false;
}

// M24d: is `target` a write to a `const` data member? (`this.f`, bare `f`, or `obj.f`
// where f is declared const). Used to forbid such writes outside the constructor.
bool CEmitter::isConstFieldWrite(SharedExpression target)
{
    ASTNode* n = target.get();
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        std::string field = (ma->identifier && ma->identifier->value) ? *ma->identifier->value : "";
        std::string cls;
        if (ma->expression) {
            cls = dynamic_cast<ThisAccessNode*>(ma->expression.get())
                ? (_currentClass ? _currentClass->name : "")
                : exprClass(ma->expression);
        }
        if (!cls.empty() && isSmartPtrClass(cls)) cls = _classes.at(cls).collElemClass;   // pointee
        auto it = _classes.find(cls);
        return it != _classes.end() && it->second.constFields.count(field);
    }
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {       // bare field inside a method
        return id->value && (!id->qualifier || id->qualifier->empty()) && _currentClass
            && !_localTypes.count(*id->value) && _currentClass->constFields.count(*id->value);
    }
    return false;
}

// M24a/d: writing TO or THROUGH a `const` binding is a hard error (deep const, so
// `c.field = …` / `c[i] = …` are caught too), and a `const` data member may only be
// written in the constructor.
void CEmitter::checkConstWrite(SharedExpression target, int srcLine)
{
    if (!target) return;
    std::string root = rootBinding(target);
    if (rootIsConst(root))
        unsupported(("cannot write to `const " + root + "` (const is deep — neither the "
                     "binding nor anything reached through it may be mutated)").c_str(), srcLine);
    else if (!_inCtor && isConstFieldWrite(target))
        unsupported("cannot assign to a `const` field outside the constructor", srcLine);
}

// M24b: a non-const method may not be invoked on a const receiver (it could mutate).
bool CEmitter::isConstReceiver(SharedExpression receiver) const
{
    return receiver && rootIsConst(rootBinding(receiver));
}

// M25 — access control --------------------------------------------------------
bool CEmitter::modHas(SharedModifierList mods, const char* name)
{
    if (mods) for (auto& m : *mods) if (m->value && *m->value == name) return true;
    return false;
}

// At most one of public/protected/private; default `dflt` when none is written.
Visibility CEmitter::visibilityOf(SharedModifierList mods, Visibility dflt, int line)
{
    Visibility v = dflt; int count = 0;
    if (mods) for (auto& m : *mods) {
        if (!m->value) continue;
        if      (*m->value == "public")    { v = Visibility::Public;    count++; }
        else if (*m->value == "protected") { v = Visibility::Protected; count++; }
        else if (*m->value == "private")   { v = Visibility::Private;   count++; }
    }
    if (count > 1) unsupported("a member may have at most one of public/protected/private", line);
    return v;
}

// Is a member (declared on `owner`, visibility `vis`) accessible from the current
// emission context (`_currentClass`; null = external/free function)? Compile error if not.
bool CEmitter::canAccess(ClassInfo* owner, Visibility vis, const std::string& member, int line)
{
    if (vis == Visibility::Public || !owner) return true;
    if (vis == Visibility::Protected) {                 // owner or any subclass of owner
        for (ClassInfo* c = _currentClass; c; c = c->base) if (c == owner) return true;
    } else {                                            // Private — owner itself only (M25c adds friends)
        if (_currentClass == owner) return true;
    }
    const char* vs = (vis == Visibility::Private) ? "private" : "protected";
    unsupported(("'" + member + "' is " + vs + " in '" + owner->name + "'").c_str(), line);
    return false;
}

// Field access through a resolved owner (looks up the field's visibility, then checks).
void CEmitter::checkFieldAccess(ClassInfo* owner, const std::string& field, int line)
{
    if (!owner) return;
    for (auto& f : owner->fields)
        if (f.name == field) { canAccess(owner, f.visibility, field, line); return; }
}

// M21: bind a value to a FunctionPtr<Sig> local — a free function name (resolve +
// signature-check) or another FunctionPtr (copy). Non-null: `null`/unknown is an error.
std::string CEmitter::emitFnPtrBind(const std::string& sigCName, SharedExpression init, int line)
{
    if (auto* id = dynamic_cast<IdentifierNode*>(init.get())) {
        if (id->value) {
            auto fit = _funcs.find(resolveFunc(*id->value, id->qualifier));
            if (fit != _funcs.end()) {
                if (!sigMatches(_sigs.at(sigCName), fit->second))
                    unsupported(("function '" + *id->value + "' does not match the FunctionPtr signature").c_str(), line);
                return fit->second.cName;   // the C function name decays to a pointer
            }
            // `Type::method` — an unbound method reference (M21b). The method lowers
            // to `Class__method(Class* self, …)`, so it's a function pointer over a
            // signature whose FIRST param is the receiver (`ref Class self`).
            if (id->qualifier && !id->qualifier->empty()) {
                auto prefix = std::make_shared<StringList>();
                for (size_t i = 0; i + 1 < id->qualifier->size(); ++i) prefix->push_back((*id->qualifier)[i]);
                std::string cls = resolveUserName(*id->qualifier->back(), prefix);
                if (_classes.count(cls)) {
                    ClassInfo* owner = nullptr;
                    MethodInfo* mi = findMethod(&_classes[cls], *id->value, &owner);
                    if (mi) {
                        FuncSig full;                          // receiver-first signature
                        full.cName    = mi->cName;
                        full.retCType = cType(mi->returnType);
                        full.params.push_back(ParamSig{"self", true, cls});
                        for (auto& p : mi->params) full.params.push_back(p);
                        if (!sigMatches(_sigs.at(sigCName), full))
                            unsupported(("method '" + cls + "::" + *id->value
                                         + "' does not match the fnptr signature (its first param must be `ref "
                                         + cls + "`)").c_str(), line);
                        return mi->cName;   // the method's C name decays to a fn pointer
                    }
                }
            }
        }
    }
    if (isSigType(exprClass(init)))         // copy from another FunctionPtr
        return emitExpression(init);
    unsupported("a FunctionPtr binds a free function name or another FunctionPtr (and may not be null)", line);
    return "0";
}

// M22: `new BindableFunctionPtr<Sig>(obj: x, method: T::m)` — bind an object + a
// method. Ownership follows x's pointer type: Owned MOVES in (sole owner), Shared
// RETAINS (shared owner). The receiver is hidden, so Sig excludes it.
void CEmitter::emitBindableNew(const std::string& nm, const std::string& octy,
                               ObjectCreationNode* oc, int depth)
{
    int ln = oc->type ? oc->type->line : 0;
    const std::string& sigCName = _classes[octy].collElemClass;

    SharedExpression objArg, methodArg;
    if (oc->args)
        for (auto& a : *oc->args) {
            if (!a || !a->name || !a->name->value) continue;
            if (*a->name->value == "obj")    objArg = a->expression;
            else if (*a->name->value == "method") methodArg = a->expression;
        }
    if (!objArg || !methodArg) {
        unsupported("BindableFunctionPtr needs obj: <Owned/Shared> and method: <Type::method>", ln); return;
    }

    // The object: a smart-pointer lvalue (Owned/Shared, not Weak).
    std::string objCls = exprClass(objArg);
    if (!isSmartPtrClass(objCls) || smartKind(objCls) == CollKind::Weak) {
        unsupported("BindableFunctionPtr obj: must be an Owned<T> or Shared<T>", ln); return;
    }
    CollKind ok = smartKind(objCls);
    std::string T = _classes[objCls].collElemClass;
    std::string objE = emitExpression(objArg);

    // The method: a `Type::method` unbound reference.
    auto* mid = dynamic_cast<IdentifierNode*>(methodArg.get());
    if (!mid || !mid->value || !mid->qualifier || mid->qualifier->empty()) {
        unsupported("BindableFunctionPtr method: must be a `Type::method` reference", ln); return;
    }
    auto prefix = std::make_shared<StringList>();
    for (size_t i = 0; i + 1 < mid->qualifier->size(); ++i) prefix->push_back((*mid->qualifier)[i]);
    std::string cls = resolveUserName(*mid->qualifier->back(), prefix);
    ClassInfo* owner = nullptr;
    MethodInfo* mi = _classes.count(cls) ? findMethod(&_classes[cls], *mid->value, &owner) : nullptr;
    if (!mi) { unsupported("unknown method in BindableFunctionPtr method:", ln); return; }
    if (cls != T) {
        unsupported("BindableFunctionPtr obj: type does not match the method's class", ln); return;
    }

    // Signature check: the method's params + return (receiver HIDDEN) must equal Sig.
    FuncSig stripped;
    stripped.cName = mi->cName; stripped.retCType = cType(mi->returnType); stripped.params = mi->params;
    if (!sigMatches(_sigs.at(sigCName), stripped))
        unsupported("the method does not match the BindableFunctionPtr signature (the receiver is hidden)", ln);

    bool destr = _classes.count(T) && _classes[T].destructible;
    indent(depth); *_out << nm << ".obj = (void*)(" << objE << ").ptr;\n";
    if (ok == CollKind::Shared) { indent(depth); *_out << nm << ".ctrl = (" << objE << ").ctrl;\n"; }
    indent(depth); *_out << nm << ".fn = (void (*)(void))" << mi->cName << ";\n";
    indent(depth); *_out << nm << ".elemdtor = "
                         << (destr ? ("(void (*)(void*))" + T + "__dtor") : "0") << ";\n";
    // Ownership transfer: Owned MOVES (invalidate the source); Shared RETAINS.
    if (ok == CollKind::Owned) { indent(depth); *_out << "(" << objE << ").ptr = NULL;\n"; }
    else { indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->strong++;\n"; }
}

// M22: `BindableFunctionPtr<Sig> b = <free fn | another bindable>;` — promote a free
// function (no object) or MOVE another bindable.
void CEmitter::emitBindablePromote(const std::string& nm, const std::string& ty,
                                   SharedExpression init, int depth)
{
    const std::string& sigCName = _classes[ty].collElemClass;

    if (auto* id = dynamic_cast<IdentifierNode*>(init.get())) {
        if (id->value) {
            // Another BindableFunctionPtr lvalue -> move (copy + invalidate the source).
            if (isBindableClass(exprClass(init))) {
                std::string src = emitExpression(init);
                indent(depth); *_out << nm << " = " << src << ";\n";
                indent(depth);
                *_out << "(" << src << ").obj = NULL; (" << src << ").ctrl = NULL; ("
                      << src << ").fn = NULL; (" << src << ").elemdtor = NULL;\n";
                return;
            }
            // A free function -> promote (obj = NULL; no RAII).
            auto fit = _funcs.find(resolveFunc(*id->value, id->qualifier));
            if (fit != _funcs.end()) {
                if (!sigMatches(_sigs.at(sigCName), fit->second))
                    unsupported("function does not match the BindableFunctionPtr signature", init->line);
                indent(depth);
                *_out << nm << ".obj = NULL; " << nm << ".ctrl = NULL; " << nm << ".fn = (void (*)(void))"
                      << fit->second.cName << "; " << nm << ".elemdtor = NULL;\n";
                return;
            }
            unsupported("a BindableFunctionPtr binds via `new BindableFunctionPtr<Sig>(obj:, method:)`, "
                        "a free function, or another BindableFunctionPtr", init->line);
            return;
        }
    }
    // A bindable-valued rvalue (e.g. a factory call) is already moved out — plain copy.
    std::string src = emitExpression(init);
    indent(depth); *_out << nm << " = " << src << ";\n";
}

// M22: invoke a bindable — branch on obj (bound: pass it first; free: call directly).
// The signature drives the fn-pointer casts and the named-arg reorder.
std::string CEmitter::emitBindableInvoke(const std::string& recv, const std::string& cls,
                                         SharedArgumentList args, int line)
{
    const SigInfo& sig = _sigs.at(_classes[cls].collElemClass);
    std::string plist;
    for (size_t i = 0; i < sig.params.size(); ++i)
        plist += (i ? ", " : "") + sig.params[i].className + (sig.params[i].byRef ? "*" : "");
    std::string boundT = sig.retCType + " (*)(void*" + (sig.params.empty() ? "" : ", " + plist) + ")";
    std::string freeT  = sig.retCType + " (*)(" + (sig.params.empty() ? std::string("void") : plist) + ")";
    std::string boundCall = emitReorderedCall("((" + boundT + ")" + recv + ".fn)", recv + ".obj",
                                              sig.params, args, line);
    std::string freeCall  = emitReorderedCall("((" + freeT + ")" + recv + ".fn)", "",
                                              sig.params, args, line);
    return "(" + recv + ".obj ? " + boundCall + " : " + freeCall + ")";
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

    // FunctionPtr invoke (M21): a bare local whose type is a signature → an indirect
    // call `c(reordered args)` (c IS the function pointer). Named-arg reorder off the sig.
    if ((!call->identifier->qualifier || call->identifier->qualifier->empty())
        && _localTypes.count(name) && isSigType(_localTypes[name])) {
        const SigInfo& sig = _sigs.at(_localTypes[name]);
        std::string callee = _refParams.count(name) ? ("(*" + name + ")") : name;
        return emitReorderedCall(callee, "", sig.params, call->args, call->line);
    }

    // BindableFunctionPtr invoke (M22): a bare local of bindable type → branch on the
    // bound object (call the method with it, or the free fn directly).
    if ((!call->identifier->qualifier || call->identifier->qualifier->empty())
        && _localTypes.count(name) && isBindableClass(_localTypes[name])) {
        return emitBindableInvoke(name, _localTypes[name], call->args, call->line);
    }

    // FFI (M16): `addr(x)` is a builtin — the address of a local/value (`&(x)`),
    // for out-params and passing a descriptor by pointer. A controlled operation
    // (it addresses a real value), so it needs no `unsafe`.
    if (name == "addr" && (!call->identifier->qualifier || call->identifier->qualifier->empty())
        && call->args && call->args->size() == 1)
        return "&(" + emitExpression((*call->args)[0]->expression) + ")";

    // (M21 retired the M18 `funcptr(of: fn)` builtin: a bare function name is now a
    // value — its C function pointer — so `FunctionPtr<Sig> c = fn;` / passing `fn`
    // directly replaces it.)

    // A `::`-qualified callee is **scope resolution**: `Namespace::fn(...)`. After
    // M20b, the head of a `::` is always a type/namespace — never an object (object
    // access is `.`/member_access) — so the old namespace-vs-object precedence hack
    // is gone. `resolveFunc` handles namespace + `using` + alias resolution.
    SharedStringList qual = call->identifier->qualifier;
    if (qual && !qual->empty()) {
        auto fit = _funcs.find(resolveFunc(name, qual));
        if (fit != _funcs.end())
            return emitReorderedCall(fit->second.cName, "", fit->second.params, call->args, call->line);
        unsupported("scope-qualified call resolves to no known function "
                    "(static `Type::method()` is not yet supported)", call->line);
        return "0";
    }

    // Free-function call — resolve the name through the file's scope + usings.
    auto it = _funcs.find(resolveFunc(name, call->identifier->qualifier));
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
            // M24e: `const Ptr<T>`/`const Ptr` emits `const T*`/`const void*` (FFI const
            // pointers — to match C const callback/API signatures). Only pointer types:
            // a `const ref <class>` stays plain (its methods take a non-const `self`).
            bool constPtr = p->isConst && p->type && p->type->value && *p->type->value == "Ptr";
            s += std::string(constPtr ? "const " : "") + cType(p->type)
               + (paramByRef(p.get()) ? "* " : " ") + nm;
        }
    }
    if (s.empty()) s = "void";
    return s;
}

void CEmitter::emitFunctionPrototype(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);
    *_out << cType(fn->returnType) << " " << name << "(" << paramListC(fn->parameters, nullptr) << ");\n";
}

void CEmitter::emitFunction(FunctionDeclarationNode* fn)
{
    bool isEntry = false;
    std::string name = mangledFunctionName(fn, isEntry);

    // Track by-ref params (deref on read) and param classes (for member calls).
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _currentClass = nullptr;
    if (fn->parameters) {
        for (auto& p : *fn->parameters) {
            if (!p->identifier || !p->identifier->value) continue;
            const std::string& pn = *p->identifier->value;
            if (paramByRef(p.get())) _refParams.insert(pn);
            if (p->isConst) _constLocals.insert(pn);   // M24c: const param is immutable
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = (isClass(pty) || isInterface(pty) || isSigType(pty)) ? pty : "";   // record (incl. fnptr params)
        }
    }

    _currentReturnCType = cType(fn->returnType);
    _tempCounter = 0;
    _scopes.clear();

    line(fn->line);
    *_out << cType(fn->returnType) << " " << name << "(" << paramListC(fn->parameters, nullptr) << ")\n";

    if (fn->block) {
        emitBlockScoped(fn->block.get(), 0, /*loopBoundary=*/false, /*functionRoot=*/true);
    } else {
        *_out << "{\n}";
    }
    *_out << "\n\n";

    _refParams.clear();

    if (isEntry) {
        // Synthesized portable entry point. Argument marshaling (List<String>)
        // arrives once collections land; for now args are ignored.
        *_out << "int main(int argc, char** argv) {\n"
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
    *_out << "struct " << ci.name << " {\n";
    // Offset-0 invariant: the vptr (root only) or the embedded base comes FIRST.
    bool hasMember = false;
    if (ci.hasVtable && ci.vtableRoot == ci.name) {
        indent(1);
        *_out << "const " << ci.name << "_vtable* __vptr;\n";
        hasMember = true;
    }
    if (ci.base) {
        indent(1);
        *_out << ci.baseName << " __base;\n";
        hasMember = true;
    }
    for (auto& f : ci.fields) {
        indent(1);
        *_out << cType(f.type) << " " << f.name << ";\n";
        hasMember = true;
    }
    if (!hasMember) {
        indent(1);
        *_out << "char __empty; /* C forbids empty structs */\n";
    }
    *_out << "};\n\n";
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
    *_out << "struct " << ci.name << "_vtable {\n";
    for (auto& s : it->second) {
        indent(1);
        *_out << cType(s.node->returnType) << " (*" << s.name << ")" << vtableSlotSig(s) << ";\n";
    }
    *_out << "};\n\n";
}

// Static const vtable INSTANCE per class with a vtable, filled with the most-
// derived impl visible to this class (designated initializers; missing slots zero).
void CEmitter::emitVtableInstance(ClassInfo& ci)
{
    if (!ci.hasVtable) return;
    auto it = _rootVtables.find(ci.vtableRoot);
    if (it == _rootVtables.end()) return;
    *_out << "static const " << ci.vtableRoot << "_vtable " << ci.name << "__vtable = {\n";
    for (auto& s : it->second) {
        auto impl = ci.slotImpl.find(s.name);
        if (impl == ci.slotImpl.end()) continue;   // not visible here -> zero
        indent(1);
        // cast the impl (declared with a derived* self) to the slot's owner* signature
        *_out << "." << s.name << " = (" << cType(s.node->returnType) << "(*)"
             << vtableSlotSig(s) << ")&" << impl->second << ",\n";
    }
    *_out << "};\n\n";
}

// ---- Interfaces (M6b) -----------------------------------------------------

// "(void* self, T a, U b)" — an interface slot's C signature (self is type-erased).
std::string CEmitter::ifaceSlotSig(FunctionDeclarationNode* m)
{
    std::string sig = "(void* self";
    if (m->parameters)
        for (auto& p : *m->parameters) {
            std::string nm = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            sig += ", " + cType(p->type) + (paramByRef(p.get()) ? "* " : " ") + nm;
        }
    return sig + ")";
}

// Interface I -> a vtable struct type `I_vtbl` and a fat-pointer value type `I`.
void CEmitter::emitInterfaceTypes(InterfaceInfo& ii)
{
    *_out << "struct " << ii.name << "_vtbl {\n";
    for (auto& m : ii.methods) {
        indent(1);
        *_out << cType(m.node->returnType) << " (*" << m.name << ")" << ifaceSlotSig(m.node) << ";\n";
    }
    *_out << "};\n";
    *_out << "struct " << ii.name << " { void* obj; const " << ii.name << "_vtbl* vtbl; };\n\n";
}

// For each interface C implements, a static const I_vtbl C__as_I mapping interface
// methods to the class's matching methods (cast to the type-erased slot signature).
void CEmitter::emitClassInterfaceVtables(ClassInfo& ci)
{
    for (auto& ifn : ci.interfaces) {
        auto it = _interfaces.find(ifn);
        if (it == _interfaces.end()) { unsupported("unknown interface in implements", ci.node->line); continue; }
        InterfaceInfo& ii = it->second;
        *_out << "static const " << ii.name << "_vtbl " << ci.name << "__as_" << ii.name << " = {\n";
        for (auto& m : ii.methods) {
            ClassInfo* owner = nullptr;
            MethodInfo* mi = findMethod(&ci, m.name, &owner);
            if (!mi) { unsupported(("class missing interface method '" + m.name + "'").c_str(), ci.node->line); continue; }
            // M25: an interface is a PUBLIC contract — a method that satisfies it must be
            // public too (else it's reachable through the interface but not by name: a leak).
            if (mi->visibility != Visibility::Public)
                unsupported(("method '" + m.name + "' implements interface '" + ii.name
                             + "' and must be declared `public`").c_str(),
                            mi->node ? mi->node->line : ci.node->line);
            indent(1);
            *_out << "." << m.name << " = (" << cType(m.node->returnType) << "(*)" << ifaceSlotSig(m.node)
                 << ")&" << mi->cName << ",\n";
        }
        *_out << "};\n\n";
    }
}

// (I){ (void*)&(lvalue), &C__as_I } — wrap a concrete class lvalue as interface I.
std::string CEmitter::fatPointer(const std::string& iface, const std::string& concrete, const std::string& lvalue)
{
    return "(" + iface + "){ (void*)&(" + lvalue + "), &" + concrete + "__as_" + iface + " }";
}

// s.m(args) where s is an interface value -> (s).vtbl->m((s).obj, <reordered args>)
std::string CEmitter::emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                            const std::string& method, SharedArgumentList args, int srcLine)
{
    auto it = _interfaces.find(iface);
    if (it == _interfaces.end()) { unsupported("dispatch on unknown interface", srcLine); return "0"; }
    for (auto& m : it->second.methods) {
        if (m.name != method) continue;
        std::vector<ParamSig> params = paramSigsOf(m.node->parameters);
        return emitReorderedCall("(" + fatExpr + ").vtbl->" + method, "(" + fatExpr + ").obj",
                                 params, args, srcLine);
    }
    unsupported("unknown interface method", srcLine);
    return "0";
}

void CEmitter::emitClassPrototypes(ClassInfo& ci)
{
    if (ci.isCollection || ci.isExternStruct) return;   // macro / header provides these
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator)
        *_out << "void " << ci.name << "__ctor("
             << paramListC(ci.ctorNode->declarator->params, ci.name.c_str()) << ");\n";
    else if (ci.synthCtor)                                // M19: synthesized default ctor
        *_out << "void " << ci.name << "__ctor(" << paramListC(nullptr, ci.name.c_str()) << ");\n";
    if (ci.destructible)
        *_out << "void " << ci.name << "__dtor(" << ci.name << "* self);\n";
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        if (mi.isAbstract) continue;   // pure: no definition, no prototype
        *_out << cType(mi.returnType) << " " << mi.cName << "("
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
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _currentReturnCType = "void";
    _tempCounter = 0;
    _scopes.clear();
    Scope root; root.isFunctionRoot = true;
    _scopes.push_back(root);

    *_out << "void " << ci.name << "__dtor(" << ci.name << "* self)\n{\n";

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
            *_out << cit->second.name << "__dtor(&self->" << it->name << ");\n";
        }
    }
    // Base destructor LAST.
    if (ci.base && ci.base->destructible) {
        indent(1);
        *_out << ci.baseName << "__dtor(&self->__base);\n";
    }
    *_out << "}\n\n";

    _scopes.clear();
    _currentClass = nullptr;
}

// Emit a method or constructor body with `self`/field/param context set up.
void CEmitter::emitMethodOrCtorBody(const std::string& cName, const char* retType,
                                    SharedParameterList params, SharedBlock body,
                                    ClassInfo& owner, bool isCtor, bool isConstMethod)
{
    _currentClass = &owner;
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _inCtor = isCtor;   // M24d: const fields are writable only here
    if (isConstMethod) _constLocals.insert("this");   // M24b: `this` is immutable (deep)
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
            if (p->isConst) _constLocals.insert(pn);   // M24c: const param is immutable
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = (isClass(pty) || isInterface(pty) || isSigType(pty)) ? pty : "";   // record (incl. fnptr params)
        }
    }

    *_out << retType << " " << cName << "(" << paramListC(params, owner.name.c_str()) << ")\n{\n";

    if (isCtor) {
        // 1. Base constructor first (so derived overrides its effects + vptr).
        if (owner.base) {
            SharedArgumentList baseArgs;
            if (owner.ctorNode && owner.ctorNode->declarator && owner.ctorNode->declarator->initializer)
                baseArgs = owner.ctorNode->declarator->initializer->args;
            if (owner.synthCtor && owner.base->ctorNode && !owner.base->ctorParams.empty()) {
                // A synthesized default ctor can't supply the base's required args.
                unsupported(("'" + owner.name + "' needs an explicit constructor to pass arguments to base '"
                             + owner.baseName + "'").c_str(), owner.node ? owner.node->line : 0);
            } else if (owner.base->hasCtor) {
                indent(1);
                *_out << emitReorderedCall(owner.baseName + "__ctor", "&self->__base",
                                          owner.base->ctorParams, baseArgs, owner.node->line) << ";\n";
            } else if (baseArgs && !baseArgs->empty()) {
                unsupported("base has no constructor to receive arguments", owner.node->line);
            }
        }
        // 2. Set the vptr to THIS class's vtable (after base, so most-derived wins).
        if (owner.hasVtable) {
            indent(1);
            *_out << "self->" << vptrPrefix(&owner) << "__vptr = &" << owner.name << "__vtable;\n";
        }
        // 3. Field initializers.
        for (auto& f : owner.fields) {
            if (f.initializer) {
                indent(1);
                *_out << "self->" << f.name << " = " << emitExpression(f.initializer) << ";\n";
            }
        }
    }
    SharedStatement last;
    if (body && body->statements) {
        for (auto& st : *body->statements) { emitStatement(st, 1); last = st; }
    }
    if (!(last && stmtIsJump(last)))
        emitScopeCleanup(_scopes.back(), 1);
    *_out << "}\n\n";

    _scopes.clear();
    _currentClass = nullptr;
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
}

void CEmitter::emitClassDefinitions(ClassInfo& ci)
{
    if (ci.isCollection || ci.isExternStruct) return;   // macro / header provides these
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator) {
        line(ci.ctorNode->line);
        emitMethodOrCtorBody(ci.name + "__ctor", "void",
                             ci.ctorNode->declarator->params, ci.ctorNode->body, ci, true);
    } else if (ci.synthCtor) {                            // M19: emit the synthesized default ctor
        emitMethodOrCtorBody(ci.name + "__ctor", "void",
                             SharedParameterList(), SharedBlock(), ci, true);
    }
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        if (mi.isAbstract) continue;   // pure: no body to emit
        line(mi.node->line);
        std::string ret = cType(mi.returnType);
        emitMethodOrCtorBody(mi.cName, ret.c_str(), mi.node->params, mi.node->body, ci, false, mi.isConst);
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
        if (isSmartPtrClass(recv)) recv = _classes[recv].collElemClass;   // auto-deref: look up on T
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
    // Auto-deref an Owned/Shared: `n.field` -> `(n).ptr->[base]field` (T*).
    // A Weak can't be dereffed — it must be upgraded with lock() first.
    if (isSmartPtrClass(cls)) {
        if (smartKind(cls) == CollKind::Weak) {
            unsupported("cannot access a field through Weak<T>; call .lock() to upgrade", ma->line);
            return field;
        }
        std::string T = _classes[cls].collElemClass;
        std::string basePath;
        ClassInfo* owner = findFieldOwner(&_classes[T], field);
        if (owner) { basePath = basePathTo(&_classes[T], owner); checkFieldAccess(owner, field, ma->line); }  // M25
        return "(" + emitExpression(ma->expression) + ").ptr->" + basePath + field;
    }
    std::string basePath;
    if (!cls.empty() && _classes.count(cls)) {
        ClassInfo* owner = findFieldOwner(&_classes[cls], field);
        if (owner) { basePath = basePathTo(&_classes[cls], owner); checkFieldAccess(owner, field, ma->line); }  // M25
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
    if (!mi->isIntrinsic) canAccess(owner, mi->visibility, method, srcLine);   // M25

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
    // M24b: a non-const method may not be called on a const receiver (deep const).
    // The method lives on the pointee for a smart-pointer receiver (auto-deref).
    if (isConstReceiver(receiver)) {
        std::string mcls = isSmartPtrClass(cls) ? _classes[cls].collElemClass : cls;
        ClassInfo* owner = nullptr;
        MethodInfo* mi = _classes.count(mcls) ? findMethod(&_classes[mcls], method, &owner) : nullptr;
        if (mi && !mi->isConst && !mi->isIntrinsic)
            unsupported(("cannot call non-const method `" + method + "` on a const receiver "
                         "(declare it `const fn` if it does not mutate)").c_str(), call->line);
    }
    // Smart-pointer receiver: an intrinsic (lock/expired/valid) or auto-deref to T.
    if (isSmartPtrClass(cls))
        return emitSmartPtrCall(cls, emitExpression(receiver), method, call->args, call->line);
    if (isInterface(cls))
        return emitInterfaceDispatch(emitExpression(receiver), cls, method, call->args, call->line);
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
    if (ci.isAbstractClass)   // M25/Step 3: instantiating one crashes on a NULL vtable slot
        unsupported(("cannot instantiate abstract class '" + ci.name
                     + "' (it has an unimplemented method)").c_str(), srcLine);
    if (ci.hasCtor && !ci.isCollection)   // M25: private ctor blocks external `new` (intrinsics exempt)
        canAccess(&ci, ci.ctorVisibility, "constructor", srcLine);
    return emitReorderedCall(ci.name + "__ctor", "&" + cVar, ci.ctorParams, args, srcLine);
}

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

// Whole-program symbol table: run the collect passes for every unit (they append
// to the shared maps), then resolve inheritance/vtables/destructibility once.
void CEmitter::collectProgram(const std::vector<SharedCompilationUnit>& units)
{
    // Assign each file its namespace context (public namespace or _F<idx> private)
    // and register public namespaces, before any name resolution.
    for (size_t i = 0; i < units.size(); ++i) {
        if (!units[i]) continue;
        NsCtx ctx = ctxOf(units[i], (int)i);
        _unitCtx[units[i].get()] = ctx;
        if (ctx.isPublic) _namespaces.insert(ctx.scope);
    }
    // Pre-register every type's mangled NAME so references resolve regardless of
    // file/declaration order (a class method param may reference a type declared
    // later, or in another file). The full collect below overwrites these.
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& decl : *u->codeDeclarationList) {
            ASTNode* d = decl.get();
            if (auto* cd = dynamic_cast<ClassDeclarationNode*>(d)) {
                if (cd->name && cd->name->value) {
                    bool ext = false;
                    if (cd->modifiers) for (auto& mod : *cd->modifiers)
                        if (mod->value && *mod->value == "extern") ext = true;
                    std::string n = ext ? *cd->name->value : qualify(*cd->name->value);
                    _classes[n].name = n;
                    if (ext) { _classes[n].isExternStruct = true; _externNames.insert(n); }
                }
            } else if (auto* ed = dynamic_cast<EnumDeclarationNode*>(d)) {
                if (ed->identifier && ed->identifier->value) { std::string n = qualify(*ed->identifier->value); _enums[n].name = n; }
            } else if (auto* id = dynamic_cast<InterfaceDeclarationNode*>(d)) {
                if (id->identifier && id->identifier->value) { std::string n = qualify(*id->identifier->value); _interfaces[n].name = n; }
            }
        }
    }
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        collectSignatures(u);
        collectEnums(u);
        collectInterfaces(u);
        collectClasses(u);
    }
    linkBases();
    buildVtables();
    computeDestructible();
    for (auto& u : units)
        if (u && u->codeDeclarationList) { _nsCtx = _unitCtx[u.get()]; collectCollections(u); }
}

// All DECLARATIONS (the shared header): typedefs, enums, struct/vtable types,
// interface types, collection/smart-pointer macros, and every prototype.
void CEmitter::emitHeaderContent(const std::vector<SharedCompilationUnit>& units)
{
    std::vector<ClassInfo*> classes = topoOrderClasses();   // base before derived

    // Forward typedefs so bodies can reference each other and any class.
    for (ClassInfo* ci : classes) {
        if (ci->isCollection || ci->isExternStruct) continue;   // macro / header provides it
        *_out << "typedef struct " << ci->name << " " << ci->name << ";\n";
        if (ci->hasVtable && ci->vtableRoot == ci->name)
            *_out << "typedef struct " << ci->name << "_vtable " << ci->name << "_vtable;\n";
    }
    for (auto& kv : _interfaces) {
        *_out << "typedef struct " << kv.first << "_vtbl " << kv.first << "_vtbl;\n";
        *_out << "typedef struct " << kv.first << " " << kv.first << ";\n";
    }
    if (!classes.empty() || !_interfaces.empty()) *_out << "\n";

    // Function-pointer signature typedefs (M21): `typedef ret (*Name)(params);`.
    // After the class forward-typedefs so a signature may take/return a class.
    for (auto& kv : _sigs) {
        SigInfo& si = kv.second;
        *_out << "typedef " << si.retCType << " (*" << si.cName << ")(";
        if (si.params.empty()) *_out << "void";
        for (size_t i = 0; i < si.params.size(); ++i) {
            const ParamSig& p = si.params[i];
            // M24e: const pointer params -> `const T*` (FFI). className already ends
            // in `*` for a Ptr<T>/Ptr; a const-ref class param keeps its self mutable.
            bool constPtr = p.isConst && !p.className.empty() && p.className.back() == '*';
            *_out << (i ? ", " : "") << (constPtr ? "const " : "") << p.className << (p.byRef ? "*" : "");
        }
        *_out << ");\n";
    }
    if (!_sigs.empty()) *_out << "\n";

    // Set the name-resolution scope from the type/file being emitted (M14).
    auto scopeOf = [&](const std::string& scope, const std::vector<std::string>& usings) {
        _nsCtx = NsCtx{}; _nsCtx.scope = scope; _nsCtx.usings = usings;
    };

    for (auto& kv : _enums) { scopeOf(kv.second.scope, kv.second.usings); emitEnum(kv.second); }

    // vtable struct types + struct bodies (topological), then interface types.
    for (ClassInfo* ci : classes) {
        if (ci->isCollection || ci->isExternStruct) continue;   // macro / header provides it
        scopeOf(ci->scope, ci->usings);
        if (ci->hasVtable && ci->vtableRoot == ci->name) emitVtableType(*ci);
        emitStruct(*ci);
    }
    for (auto& kv : _interfaces) { scopeOf(kv.second.scope, kv.second.usings); emitInterfaceTypes(kv.second); }

    // Class prototypes, then the collection/smart-pointer macros (which reference
    // element struct/dtor decls), then free-function prototypes (which may use a
    // collection/smart-pointer type in their signature).
    for (ClassInfo* ci : classes) { scopeOf(ci->scope, ci->usings); emitClassPrototypes(*ci); }
    emitCollectionDefs();
    // Prototypes for cstar's OWN free functions. cstar never emits prototypes for
    // `extern` C functions: an `extern` decl is purely cstar's call signature
    // (name + named params, for lowering) — the C prototype comes from the header
    // you `extern "<…>";` (or one the runtime already includes). This keeps the
    // FFI rule a single explicit sentence and makes redeclaration conflicts
    // impossible (cstar can't always spell a C type exactly, e.g. const char*).
    bool any = false;
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& decl : *u->codeDeclarationList)
            if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
                if (isExtern(fn) || !fn->block) continue;   // skip extern + signature types
                emitFunctionPrototype(fn);
                any = true;
            }
    }
    if (any) *_out << "\n";
}

// This file's DEFINITIONS: its classes' vtable instances + interface vtables +
// method/ctor/dtor bodies, then its free-function bodies. Prototypes for anything
// referenced across files live in the shared header.
void CEmitter::emitModuleContent(SharedCompilationUnit unit)
{
    _nsCtx = _unitCtx[unit.get()];   // resolve this file's body references in its scope (M14)
    auto classOf = [&](ASTNode* d) -> ClassInfo* {
        auto* cd = dynamic_cast<ClassDeclarationNode*>(d);
        if (cd && cd->name && cd->name->value) {
            std::string mangled = qualify(*cd->name->value);
            if (_classes.count(mangled)) return &_classes[mangled];
        }
        return nullptr;
    };
    // vtable instances + interface vtables first (referenced by ctor bodies).
    for (auto& decl : *unit->codeDeclarationList)
        if (ClassInfo* ci = classOf(decl.get())) emitVtableInstance(*ci);
    for (auto& decl : *unit->codeDeclarationList)
        if (ClassInfo* ci = classOf(decl.get())) emitClassInterfaceVtables(*ci);
    // class definitions, then free-function definitions.
    for (auto& decl : *unit->codeDeclarationList)
        if (ClassInfo* ci = classOf(decl.get())) emitClassDefinitions(*ci);
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (!isExtern(fn) && fn->block) emitFunction(fn);   // skip signature types (no body)
        } else if (dynamic_cast<ClassDeclarationNode*>(decl.get())) {
            // emitted above
        } else if (dynamic_cast<InterfaceDeclarationNode*>(decl.get())) {
            // type-only (header)
        } else if (dynamic_cast<EnumDeclarationNode*>(decl.get())) {
            // emitted in the header
        } else if (dynamic_cast<IncludeNode*>(decl.get())) {
            // FFI #include — emitted in the header by emitIncludes
        } else if (decl) {
            unsupported("top-level declaration", decl->line);
            *_out << "\n";
        }
    }
}

// FFI (M16): emit a C `#include` per `extern "<header>";` directive, deduped.
void CEmitter::emitIncludes(const std::vector<SharedCompilationUnit>& units)
{
    std::set<std::string> seen;
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        for (auto& decl : *u->codeDeclarationList)
            if (auto* inc = dynamic_cast<IncludeNode*>(decl.get())) {
                std::string h = inc->header ? *inc->header : "";
                if (h.empty() || seen.count(h)) continue;
                seen.insert(h);
                if (h[0] == '<') *_out << "#include " << h << "\n";       // <stdlib.h>
                else             *_out << "#include \"" << h << "\"\n";    // "my.h"
            }
    }
    *_out << "\n";
}

// Single self-contained TU (transpile / single-file build): header content +
// module definitions in one stream.
int CEmitter::emit(SharedCompilationUnit unit)
{
    *_out << "/* Generated by cstar. Do not edit. */\n";
    *_out << "#include \"cstar_runtime.h\"\n";
    if (!unit || !unit->codeDeclarationList)
        return _unsupported;
    emitIncludes({unit});           // FFI #include directives
    collectProgram({unit});
    emitHeaderContent({unit});
    emitModuleContent(unit);
    return _unsupported;
}

// Multi-file program: a guarded shared header of all declarations, then one .c of
// definitions per source file (each #include-ing the header).
int CEmitter::emitProgram(const std::vector<SharedCompilationUnit>& units,
                          const std::string& headerName, std::ostream& header,
                          const std::vector<std::ostream*>& moduleStreams,
                          const std::vector<std::string>& sourcePaths)
{
    collectProgram(units);

    std::string guard = "CSTAR_GEN_";
    for (char c : headerName) guard += (isalnum((unsigned char)c) ? (char)toupper(c) : '_');

    _out = &header;
    header << "/* Generated by cstar. Do not edit. */\n";
    header << "#ifndef " << guard << "\n#define " << guard << "\n";
    header << "#include \"cstar_runtime.h\"\n";
    emitIncludes(units);        // FFI #include directives (before any type decls)
    emitHeaderContent(units);   // declarations only — no bodies, so no #line needed
    header << "#endif /* " << guard << " */\n";

    for (size_t i = 0; i < units.size(); ++i) {
        _out = moduleStreams[i];
        _sourcePath = sourcePaths[i];   // #line in this module points to its own source
        *_out << "/* Generated by cstar. Do not edit. */\n";
        *_out << "#include \"" << headerName << "\"\n\n";
        emitModuleContent(units[i]);
    }
    return _unsupported;
}
