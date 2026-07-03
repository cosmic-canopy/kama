#include "cstar.cemit.h"
#include "cstar.ast.h"
#include "cstar.context.h"    // CodeGenContext — to synthesize primitive type nodes (M27a)
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
    if (srcLine > 0) _curLine = srcLine;   // M26f-2: track for conditional-drop diagnostics
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
        return _classes.count(n) || _enums.count(n) || _interfaces.count(n) || _sigs.count(n)
            || _genericTypes.count(n);   // M27b: a generic-type template resolves to its scoped name too
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
    // M27a: inside a generic instantiation, a bare type-param name (`T`, no <...> of its own)
    // resolves to the concrete type it was bound to. Guarded on !genericArg so a real `List<T>`
    // still flows to the collection arm (whose element then hits this same substitution).
    if (!_typeSubst.empty() && type->value && !type->genericArg) {
        auto s = _typeSubst.find(*type->value);
        if (s != _typeSubst.end()) return cType(s->second);
    }
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
    // M27b: a user generic TYPE (`Box<int32>`) spells its specialized struct name (`Box_int32`).
    // Reached only for a non-reserved name with a type arg; under _typeSubst the arg's `T` resolves.
    if (type->genericArg && type->value) {   // genericArg mirrors genericArgs[0] (non-null iff there are args)
        std::string tmpl = resolveUserName(*type->value, type->qualifier);
        if (_genericTypes.count(tmpl)) return genericTypeMangle(tmpl, type->genericArgs);
    }
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

    if (auto* h = dynamic_cast<HandoffNode*>(n)) {
        // M26c/d: `give x` / `copy x`. The move (invalidate source) / retain side effects
        // need a statement context — handled where a value is HANDED OFF: an initializer,
        // assignment, argument, or return (emitDeclarator / the assignment arm / emitReorderedCall
        // / the return arm all unwrap the marker). Reaching here means the marker rides a bare
        // sub-expression (e.g. `give x` used as a statement or inside a larger expression), which
        // isn't a hand-off position — only a plain-value `copy` (a value copy) would be complete.
        std::string ic = exprClass(h->value);
        bool owned = isSmartPtrClass(ic) || (!ic.empty() && _classes.count(ic) && _classes[ic].isCollection);
        if (!owned)
            { if (h->isGive) unsupported("`give` applies to an owned value (a smart pointer or collection) — a plain value just copies", h->line); }
        else
            unsupported("`give`/`copy` mark a value being handed off — an initializer, assignment, argument, "
                        "or return — not a bare sub-expression", h->line);
        return emitExpression(h->value);
    }

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
        checkNotMoved(nm, v->line);   // M26f-2: reject reading a moved-from `resource` value
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
        // M26b / GOALS §3b: `== null` / `!= null` on a safe type is a compile error — a value,
        // smart pointer, or interface is never null (the C habit checks the wrong thing here).
        // `null` is only for `Ptr<T>` at the FFI boundary (exprClass is empty for those).
        if (v->token == EQEQ || v->token == NOTEQ) {
            bool lNull = dynamic_cast<NullNode*>(v->LHS.get()) != nullptr;
            bool rNull = dynamic_cast<NullNode*>(v->RHS.get()) != nullptr;
            if (lNull != rNull) {
                std::string oc = exprClass((lNull ? v->RHS : v->LHS));
                if (!oc.empty())
                    unsupported(("'" + oc + "' is never null in safe code — don't null-check it "
                                 "(a `Weak` uses `tryUpgrade`; `null` is only for `Ptr<T>` at the FFI boundary)").c_str(),
                                v->line);
            }
        }
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

// M26f-2: does this body end in a jump (so control doesn't fall through to a branch join)?
bool CEmitter::bodyDiverges(SharedStatement s)
{
    if (!s) return false;
    if (auto* b = dynamic_cast<BlockNode*>(s.get())) {
        if (b->statements && !b->statements->empty()) return stmtIsJump(b->statements->back());
        return false;
    }
    return stmtIsJump(s);
}

void CEmitter::emitScopeCleanup(const Scope& s, int depth)
{
    for (auto it = s.locals.rbegin(); it != s.locals.rend(); ++it) {
        auto ms = _moveState.find(it->cVar);
        if (ms != _moveState.end()) {
            if (ms->second == MoveState::Moved) continue;   // M26f-2: moved out — skip its drop
            if (ms->second == MoveState::MaybeMoved)        // moved on some paths, live here — undecidable drop
                unsupported(("`" + it->cVar + "` is moved on some paths but not others and is still live at "
                             "scope exit — move it on all paths or none, or use Optional<T> (M28)").c_str(), _curLine);
        }
        // M26h: a move-only value that owns nothing (an empty `resource`/token) is tracked for move
        // analysis but has no destructor — skip the drop.
        auto ci = _classes.find(it->className);
        if (ci != _classes.end() && !ci->second.destructible) continue;
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
    if (isMoveOnlyValue(className)) _moveState[cVar] = MoveState::NotMoved;  // M26f-2: track for move analysis
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
    // M26d: by-value smart-ptr params the callee owns drop at fn-end. Recorded FIRST in
    // the root scope, so they're destroyed LAST (after every local), at function exit.
    if (functionRoot && !_pendingParamDtors.empty()) {
        for (auto& l : _pendingParamDtors) _scopes.back().locals.push_back(l);
        _pendingParamDtors.clear();
    }

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

// M26i: write hoisted temp statements (inline-ctor-in-arg materialization) at `depth`, then clear.
// A leaf statement sets _hoistOK, builds its expression string (which may push here), then calls
// this BEFORE writing its own line — so the temps appear first. Pure ISO C, no `({ … })`.
void CEmitter::flushHoisted(int depth)
{
    for (auto& s : _hoisted) { indent(depth); *_out << s << "\n"; }
    _hoisted.clear();
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
            // M27b: a bare generic type without a type argument (`Box b` instead of `Box<int32> b`)
            // is not a usable type — the template is not a concrete class.
            if (declType->value && !declType->genericArg
                && _genericTypes.count(resolveUserName(*declType->value, declType->qualifier)))
                unsupported(("generic type `" + *declType->value + "` needs a type argument, e.g. `"
                             + *declType->value + "<int32>`").c_str(), n->line);
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
                    line(n->line);
                    std::string initStr;
                    if (d->initializer) {
                        bool ph = _hoistOK; _hoistOK = true;               // M26i: inline-ctor hoisting
                        initStr = " = " + emitExpression(d->initializer);
                        _hoistOK = ph;
                    }
                    flushHoisted(depth);                                   // temp decls first…
                    indent(depth);
                    *_out << ty << " " << nm << initStr << ";\n";          // …then this declaration
                    return;
                }

                // Class-typed local: declare the value, then construct in place.
                // Collections zero-init so an unconstructed one frees safely; extern
                // structs zero-init so unset descriptor fields are well-defined.
                line(n->line); indent(depth);
                bool zeroInit = _classes[ty].isCollection || _classes[ty].isExternStruct;
                *_out << ty << " " << nm << (zeroInit ? " = {0}" : "") << ";\n";
                // Track for RAII cleanup at scope exit (assumes init-at-decl).
                if (_classes[ty].destructible || isMoveOnlyValue(ty)) recordDestructibleLocal(nm, ty);  // M26h: track empty resources for move analysis
                if (!d->initializer) return;   // declared but uninitialized (non-const)

                // M26c: unwrap a give/copy hand-off marker — the inner NAMED value drives
                // move (give) vs duplicate (copy). A fresh rvalue never takes a marker.
                SharedExpression init = d->initializer;
                int handoff = 0;   // 0 none, 1 give, 2 copy
                if (auto* h = dynamic_cast<HandoffNode*>(d->initializer.get())) { handoff = h->isGive ? 1 : 2; init = h->value; }
                if (handoff && !isNamedValue(init.get()))
                    unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call result needs no marker", n->line);

                // M26a: `T(...)` with no `new` (an InvocationNode whose callee names the
                // declared class) is STACK construction; `new` is reserved for the heap.
                InvocationNode* stackCtor = nullptr;
                if (auto* iv = dynamic_cast<InvocationNode*>(init.get()))
                    if (iv->identifier && iv->identifier->value && isClass(ty)) {
                        std::string rn = resolveUserName(*iv->identifier->value, iv->identifier->qualifier);
                        // M27b: `Box<int32> b = Box(v: 7)` — the ctor names the bare template `Box`, but
                        // the declared type is the instance `Box_int32`; accept the template→instance match.
                        auto g = _genericTypeInstOf.find(ty);
                        if (rn == ty || (g != _genericTypeInstOf.end() && g->second == rn))
                            stackCtor = iv;
                    }

                if (auto* oc = dynamic_cast<ObjectCreationNode*>(init.get())) {
                    // M26a: `new` is the HEAP operator — it boxes a value into a smart
                    // pointer (Owned/Shared/Weak), naming the element type directly:
                    // `Owned<Box> p = new Box(...)`. (BindableFunctionPtr keeps `new` for
                    // its bind.) `new` into a plain value type is an error — drop `new`.
                    std::string octy = cType(oc->type);
                    if (isBindableClass(ty)) {
                        emitBindableNew(nm, ty, oc, depth);   // bind obj + method (M22)
                    } else if (isSmartPtrClass(ty)) {
                        std::string T = _classes[ty].collElemClass;
                        if (isSmartPtrClass(octy) || isBindableClass(octy))
                            unsupported(("`new` now names the element type — write `new " + T
                                         + "(...)`, not the wrapper").c_str(), n->line);
                        else if (isInterface(T)) {
                            // M26g: box a concrete class that implements interface T into an owned
                            // interface handle — malloc the concrete, ctor it, set {obj, vtbl}.
                            auto cit = _classes.find(octy);
                            bool implementsT = false;
                            if (cit != _classes.end())
                                for (auto& i : cit->second.interfaces) if (i == T) { implementsT = true; break; }
                            if (!implementsT)
                                unsupported(("`new " + octy + "` does not implement `" + T + "` — `" + ty
                                             + "` owns a class that satisfies the interface").c_str(), n->line);
                            else if (_classes[octy].isAbstractClass)
                                unsupported(("cannot instantiate abstract class '" + octy + "'").c_str(), n->line);
                            else {
                                line(n->line); indent(depth);
                                *_out << nm << ".obj = malloc(sizeof(" << octy << "));\n";
                                if (_classes[octy].hasCtor) {
                                    line(n->line);
                                    bool ph = _hoistOK; _hoistOK = true;               // M26i: hoist arg hand-offs
                                    std::string cc = emitReorderedCall(octy + "__ctor", "(" + octy + "*)" + nm + ".obj",
                                                              _classes[octy].ctorParams, oc->args, n->line);
                                    _hoistOK = ph; flushHoisted(depth);
                                    indent(depth); *_out << cc << ";\n";
                                }
                                indent(depth); *_out << nm << ".vtbl = &" << octy << "__as_" << T << ";\n";
                                if (smartKind(ty) == CollKind::Shared) {   // M26g-2: ref-counted owned interface
                                    indent(depth); *_out << nm << ".ctrl = cstar_ctrl_new();\n";
                                }
                            }
                        }
                        else if (octy != T)
                            unsupported(("`" + ty + "` boxes `" + T + "`, but got `new " + octy + "(...)`").c_str(), n->line);
                        else {
                            if (isClass(T) && _classes[T].isAbstractClass)
                                unsupported(("cannot instantiate abstract class '" + T + "'").c_str(), n->line);
                            line(n->line); indent(depth);
                            *_out << nm << ".ptr = (" << T << "*)malloc(sizeof(" << T << "));\n";
                            if (isClass(T) && _classes[T].hasCtor) {
                                line(n->line);
                                bool ph = _hoistOK; _hoistOK = true;               // M26i: hoist arg hand-offs
                                std::string cc = emitReorderedCall(T + "__ctor", nm + ".ptr",
                                                          _classes[T].ctorParams, oc->args, n->line);
                                _hoistOK = ph; flushHoisted(depth);
                                indent(depth); *_out << cc << ";\n";
                            }
                            if (smartKind(ty) == CollKind::Shared) {
                                indent(depth); *_out << nm << ".ctrl = cstar_ctrl_new();\n";
                            }
                            // T with no ctor: malloc leaves it default (callers init fields).
                        }
                    } else if (_classes.count(ty) && _classes[ty].isCollection) {
                        // Array/List/String — a value type that manages its own heap buffer;
                        // `new` constructs it in place (the generic `Array<T>(...)` call form
                        // doesn't parse, so collections keep `new`).
                        if (_classes[ty].hasCtor) {
                            line(n->line); indent(depth);
                            *_out << emitCtorCall(nm, _classes[ty], oc->args, n->line) << ";\n";
                        }
                    } else {
                        unsupported(("`new` allocates on the heap — wrap it in `Owned<" + octy
                                     + ">`/`Shared<" + octy + ">`, or drop `new` for a stack value "
                                     "(`" + ty + " v = " + octy + "(...)`)").c_str(), n->line);
                    }
                } else if (stackCtor) {
                    // STACK value, constructed in place (`Box b = Box(id: 5)`).
                    if (_classes[ty].isAbstractClass)
                        unsupported(("cannot instantiate abstract class '" + ty + "'").c_str(), n->line);
                    if (_classes[ty].hasCtor) {
                        line(n->line);
                        bool ph = _hoistOK; _hoistOK = true;               // M26i: hoist arg hand-offs
                        std::string cc = emitCtorCall(nm, _classes[ty], stackCtor->args, n->line);
                        _hoistOK = ph;
                        flushHoisted(depth);
                        indent(depth);
                        *_out << cc << ";\n";
                    }
                    // class with no ctor: left default-initialized
                } else if (isSmartPtrClass(ty) && smartKind(ty) == CollKind::Weak
                           && isSmartPtrLValue(init) && exprClass(init) != ty) {
                    // Shared->Weak conversion (different C structs, same layout):
                    // field-copy + weak retain. The source Shared stays valid. M26g: a fat
                    // interface Weak copies {obj, vtbl}; a thin Weak copies {ptr}.
                    line(n->line); indent(depth);
                    std::string src = emitExpression(init);
                    if (isInterface(_classes[ty].collElemClass))
                        *_out << nm << ".obj = (" << src << ").obj; " << nm << ".vtbl = (" << src << ").vtbl; "
                             << nm << ".ctrl = (" << src << ").ctrl;\n";
                    else
                        *_out << nm << ".ptr = (" << src << ").ptr; " << nm << ".ctrl = (" << src << ").ctrl;\n";
                    indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->weak++;\n";
                } else if (isBindableClass(ty)) {
                    // BindableFunctionPtr <- free function (promote) or another bindable (move).
                    line(n->line);
                    emitBindablePromote(nm, ty, init, depth);
                } else {
                    // Copy-initialize from another named value. M26c: the give/copy marker
                    // (or the type's default) decides move vs duplicate.
                    line(n->line);
                    bool ph = _hoistOK; _hoistOK = true;               // M26i: inline-ctor hoisting
                    std::string iv = emitExpression(init);
                    _hoistOK = ph;
                    flushHoisted(depth);
                    indent(depth);
                    *_out << nm << " = " << iv << ";\n";
                    if (isSmartPtrClass(ty) && isSmartPtrLValue(init)) {
                        CollKind k = smartKind(ty);
                        // Default: Owned -> give(move), Shared/Weak -> copy(retain). A marker overrides.
                        bool doGive = (handoff == 1) || (handoff == 0 && k == CollKind::Owned);
                        if (handoff == 2 && k == CollKind::Owned)
                            unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", n->line);
                        indent(depth);
                        if (doGive) *_out << smartPtrInvalidate(emitExpression(init), k, isInterface(_classes[ty].collElemClass)) << "\n";
                        else        *_out << nm << ".ctrl->" << (k == CollKind::Weak ? "weak" : "strong") << "++;\n";
                    } else if (_classes.count(ty) && _classes[ty].isCollection && isNamedValue(init.get())) {
                        // M26c/f-3: a collection move/deep-copy isn't a plain `=` — require a marker.
                        if (handoff == 0)
                            unsupported(("a collection hand-off must say `give` (move) or `copy` (deep) — write "
                                         "`" + ty + " v = give …`").c_str(), n->line);
                        else if (handoff == 2) {
                            // M26f-3/5: `copy` = a real deep copy (fresh buffer), element-wise. Valid iff
                            // each element is copyable — bitwise-copyable (owns nothing), OR a resource that
                            // opted into `Copyable` (M26f-5: `__copy` calls the element's `copy()`). A
                            // resource element WITHOUT the contract is rejected. Overwrites the blit above.
                            auto ci = _collections.find(ty);
                            if (ci != _collections.end() && ci->second.elemDestructible && !ci->second.elemCopyable)
                                unsupported(("`copy` of a `" + ty + "` needs copyable elements — its elements own "
                                             "resources but aren't `Copyable` (add a `copy` method to the element, "
                                             "or use `give` to move)").c_str(), n->line);
                            else { indent(depth); *_out << nm << " = " << ty << "__copy(&(" << emitExpression(init) << "));\n"; }
                        }
                        // give: the plain `=` already transferred the struct; null the source's buffer.
                        else { indent(depth); *_out << "(" << emitExpression(init) << ").data = NULL; ("
                                                     << emitExpression(init) << ").len = 0;\n"; }
                    } else if (isMoveOnlyValue(ty) && isNamedValue(init.get())) {
                        // M26f-2/f-4: a `resource` (destructible) VALUE. The `=` above blitted the
                        // struct. If the type opted into `Copyable`, the marker is MANDATORY (both
                        // ops plausible — "scream when ambiguous"): `copy` deep-copies via copy()
                        // (overwriting the blit; the source stays valid), `give` moves. A plain
                        // resource moves silently on a bare hand-off; `copy` on it is an error.
                        if (handoff == 2) {
                            if (!isCopyable(ty))
                                unsupported(("`" + ty + "` has no `copy` method — add one to opt into `Copyable`, "
                                             "or use `give` to move it").c_str(), n->line);
                            else { indent(depth); *_out << nm << " = " << ty << "__copy(&(" << emitExpression(init) << "));\n"; }
                        } else {
                            if (handoff == 0 && isCopyable(ty))
                                unsupported(("`" + ty + "` is copyable — a bare hand-off is ambiguous; say `give` "
                                             "(move) or `copy` (duplicate)").c_str(), n->line);
                            std::string mv = moveOnlySource(init, n->line);
                            if (!mv.empty()) markMoved(mv);
                        }
                    } else if (handoff == 1) {
                        unsupported("`give` applies to an owned value (a smart pointer or collection) — a plain value just copies", n->line);
                    }
                    // handoff == 2 (copy) of a pod/primitive: the plain `=` above IS the copy.
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
        // M26c/d: unwrap a give/copy hand-off marker; the inner value is what we return.
        SharedExpression retExpr = ret->expression;
        int handoff = 0;   // 0 none, 1 give, 2 copy
        if (retExpr)
            if (auto* h = dynamic_cast<HandoffNode*>(retExpr.get())) { handoff = h->isGive ? 1 : 2; retExpr = h->value; }
        // Capture the return value BEFORE running any destructors (it may
        // reference locals about to be destroyed), then unwind, then return.
        if (retExpr && _currentReturnCType != "void") {
            std::string tmp = "__ret_" + std::to_string(_tempCounter++);
            bool ph = _hoistOK; _hoistOK = true;                       // M26i: inline-ctor hoisting
            std::string rv = emitExpression(retExpr);
            _hoistOK = ph;
            flushHoisted(depth);
            indent(depth);
            *_out << _currentReturnCType << " " << tmp << " = " << rv << ";\n";
            // Smart-pointer hand-off to the caller: give (or a bare dying local/param) MOVES
            // out — invalidate the source BEFORE the unwind so the scope's dtor doesn't free/
            // decrement what the caller now owns (the factory landmine). `copy` RETAINS — the
            // source survives (e.g. a field), so the caller's ref is a fresh one.
            std::string rc = exprClass(retExpr);
            if (isSmartPtrClass(rc) && isNamedValue(retExpr.get())) {
                CollKind k = smartKind(rc);
                bool doGive = true;
                if (handoff == 1)      doGive = true;
                else if (handoff == 2) { if (k == CollKind::Owned)
                                             unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", n->line);
                                         doGive = false; }
                else if (isSmartPtrLValue(retExpr)) doGive = true;   // bare local/param: it dies here, move it out
                else unsupported("returning a smart-pointer field/element needs `give` (move it out) or "
                                 "`copy` (retain — the source stays valid)", n->line);
                indent(depth);
                if (doGive) *_out << smartPtrInvalidate(emitExpression(retExpr), k, isInterface(_classes[rc].collElemClass)) << "\n";
                else        *_out << "(" << emitExpression(retExpr) << ").ctrl->"
                                  << (k == CollKind::Weak ? "weak" : "strong") << "++;\n";
            }
            // M26f-2: returning a `resource` (destructible) VALUE moves it out — mark the source
            // moved so the unwind below skips its dtor; the caller now owns the returned bytes.
            // (Must precede the bindable branch, whose `dynamic_cast<IdentifierNode>` is a catch-all.)
            else if (isMoveOnlyValue(rc) && isNamedValue(retExpr.get())) {
                // M26f-4: a `Copyable` resource returns a fresh `copy` (source survives the unwind)
                // or `give`s (moves out, dtor suppressed); a bare return is ambiguous. Overwrite the
                // shallow blit captured into `tmp` above with a real deep copy for `copy`.
                if (handoff == 2) {
                    if (!isCopyable(rc))
                        unsupported(("`" + rc + "` has no `copy` method — add one to opt into `Copyable`, "
                                     "or use `give` to move it").c_str(), n->line);
                    else { indent(depth); *_out << tmp << " = " << rc << "__copy(&(" << emitExpression(retExpr) << "));\n"; }
                } else {
                    if (handoff == 0 && isCopyable(rc))
                        unsupported(("`" + rc + "` is copyable — a bare hand-off is ambiguous; say `give` "
                                     "(move) or `copy` (duplicate)").c_str(), n->line);
                    std::string mv = moveOnlySource(retExpr, n->line);
                    if (!mv.empty()) markMoved(mv);
                }
            }
            // Same move-out for a returned BindableFunctionPtr (M22): it may own its
            // bound object, so the scope dtor must NOT drop what the caller now owns.
            else if (auto* rid = dynamic_cast<IdentifierNode*>(retExpr.get())) {
                if (rid->value && isBindableClass(exprClass(retExpr))) {
                    std::string e = emitExpression(retExpr);
                    indent(depth);
                    *_out << "(" << e << ").obj = NULL; (" << e << ").ctrl = NULL; ("
                          << e << ").fn = NULL; (" << e << ").elemdtor = NULL;\n";
                }
            }
            emitUnwindAll(depth);
            indent(depth); *_out << "return " << tmp << ";\n";
        } else {
            if (retExpr) { indent(depth); *_out << emitExpression(retExpr) << ";\n"; }
            emitUnwindAll(depth);
            indent(depth); *_out << "return;\n";
        }
        return;
    }

    if (auto* f = dynamic_cast<IfNode*>(n)) {
        line(n->line); indent(depth);
        *_out << "if (" << emitExpression(f->booleanExpression) << ") ";
        // M26f-2: walk each branch from the SAME pre-if move-state, then merge at the join.
        // A branch that diverges (ends in return/break/continue) doesn't reach the join.
        auto before = _moveState;
        emitBody(f->ifStatement, depth, /*loopBoundary=*/false);
        auto thenState = _moveState;
        bool thenDiv = bodyDiverges(f->ifStatement);
        _moveState = before;
        bool elseDiv = false;
        if (f->elseStatement) {
            *_out << " else ";
            emitBody(f->elseStatement, depth, false);
            elseDiv = bodyDiverges(f->elseStatement);
        }
        auto elseState = _moveState;   // no else -> == before (the fall-through arm)
        for (auto& kv : before) {
            MoveState t = thenState.count(kv.first) ? thenState[kv.first] : kv.second;
            MoveState e = elseState.count(kv.first) ? elseState[kv.first] : kv.second;
            MoveState merged;
            if (thenDiv && elseDiv) merged = kv.second;   // join unreachable
            else if (thenDiv)       merged = e;
            else if (elseDiv)       merged = t;
            else if (t == e)        merged = t;
            else                    merged = MoveState::MaybeMoved;   // moved on one arm only
            _moveState[kv.first] = merged;
        }
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
        // M26f-2: each section is an independent arm off the pre-switch state; merge at the join.
        auto before = _moveState;
        std::vector<std::map<std::string, MoveState>> armEnd;
        std::vector<bool> armDiv;
        bool hasDefault = false;
        if (sw->switchsections) {
            for (auto& sec : *sw->switchsections) {
                _moveState = before;                       // restore before each section
                if (sec->labels) {
                    for (auto& lbl : *sec->labels) {
                        indent(depth + 1);
                        if (lbl->isDefault()) { *_out << "default:\n"; hasDefault = true; }
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
                armEnd.push_back(_moveState);
                armDiv.push_back(last && dynamic_cast<ReturnNode*>(last.get()));  // `return` diverges; `break` reaches the join
            }
        }
        indent(depth);
        *_out << "}\n";
        // Merge arms at the join. Uncovered values (no `default`) fall through with the pre-switch
        // state — an implicit arm. A local moved on ALL reaching arms -> Moved; on some -> MaybeMoved.
        _moveState = before;
        for (auto& kv : before) {
            bool any = false, allMoved = true, allNot = true;
            auto consider = [&](MoveState s){ any = true; if (s != MoveState::Moved) allMoved = false;
                                              if (s != MoveState::NotMoved) allNot = false; };
            for (size_t i = 0; i < armEnd.size(); ++i)
                if (!armDiv[i]) consider(armEnd[i].count(kv.first) ? armEnd[i][kv.first] : kv.second);
            if (!hasDefault) consider(kv.second);
            if (!any) { _moveState[kv.first] = kv.second; continue; }
            _moveState[kv.first] = allMoved ? MoveState::Moved
                                            : (allNot ? MoveState::NotMoved : MoveState::MaybeMoved);
        }
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
    // leak), copy, then either invalidate the source (give: move) or retain
    // (copy: refcount++). The null/retain is a statement, so it can't live in
    // an expression. M26c/d: a give/copy marker on the RHS overrides the default,
    // uniformly with init / argument / return.
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        if (as->token == EQ && isSmartPtrExpr(as->unaryExpression)) {
            checkConstWrite(as->unaryExpression, n->line);   // M24a: no reseating a const smart ptr
            std::string b   = emitExpression(as->unaryExpression);
            std::string ty  = exprClass(as->unaryExpression);      // Owned_T / Shared_T / Weak_T
            CollKind    knd = smartKind(ty);
            SharedExpression rhs = as->expression;
            int handoff = 0;   // 0 none, 1 give, 2 copy
            if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { handoff = h->isGive ? 1 : 2; rhs = h->value; }
            bool rhsLval = isSmartPtrLValue(rhs);
            if (handoff && !rhsLval)
                unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call result needs no marker", n->line);
            std::string src = emitExpression(rhs);                  // evaluate the RHS once
            line(n->line);
            // Step 4: self-assignment (`a = a`) would release a's pointee, then "copy" it
            // back onto the freed memory -> use-after-free. When the RHS is an lvalue, guard
            // the whole release-and-reseat with an address check (a no-op for `a = a`).
            int d2 = depth;
            if (rhsLval) { indent(depth); *_out << "if (&" << b << " != &(" << src << ")) {\n"; d2 = depth + 1; }
            indent(d2); *_out << ty << "__dtor(&" << b << ");\n";   // release b's old
            if (knd == CollKind::Weak && rhsLval && exprClass(rhs) != ty) {
                // Shared->Weak reseat: field-copy + weak retain. M26g: a fat interface Weak
                // copies {obj, vtbl}; a thin Weak copies {ptr}.
                indent(d2);
                if (isInterface(_classes[ty].collElemClass))
                    *_out << b << ".obj = (" << src << ").obj; " << b << ".vtbl = (" << src << ").vtbl; "
                         << b << ".ctrl = (" << src << ").ctrl;\n";
                else
                    *_out << b << ".ptr = (" << src << ").ptr; " << b << ".ctrl = (" << src << ").ctrl;\n";
                indent(d2); *_out << "if (" << b << ".ctrl) " << b << ".ctrl->weak++;\n";
            } else {
                indent(d2); *_out << b << " = " << src << ";\n";
                if (rhsLval) {                                       // reseat from a named smart ptr
                    // Default the natural op (Owned -> give, Shared/Weak -> copy); a marker overrides.
                    bool doGive = (handoff == 1) || (handoff == 0 && knd == CollKind::Owned);
                    if (handoff == 2 && knd == CollKind::Owned)
                        unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", n->line);
                    indent(d2);
                    if (doGive) *_out << smartPtrInvalidate(src, knd, isInterface(_classes[ty].collElemClass)) << "\n";
                    else *_out << b << ".ctrl->" << (knd == CollKind::Weak ? "weak" : "strong") << "++;\n";
                }
            }
            if (rhsLval) { indent(depth); *_out << "}\n"; }
            return;
        }
        // M26f-2: assigning a `resource` (destructible) VALUE from a NAMED source is a MOVE —
        // drop the target's current value (unless it was already moved out), blit, mark the
        // source moved. A fresh rvalue (new/ctor/call) keeps the generic copy path below.
        if (as->token == EQ && isMoveOnlyValue(exprClass(as->unaryExpression))) {
            SharedExpression rhs = as->expression;
            int handoff = 0;
            if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { handoff = h->isGive ? 1 : 2; rhs = h->value; }
            if (isNamedValue(rhs.get())) {
                std::string lty = exprClass(as->unaryExpression);
                checkConstWrite(as->unaryExpression, n->line);
                std::string lname;
                if (auto* lid = dynamic_cast<IdentifierNode*>(as->unaryExpression.get()))
                    if (lid->value && (!lid->qualifier || lid->qualifier->empty())) lname = *lid->value;
                // M26f-4: on a `Copyable` type the marker is mandatory — `copy` deep-copies via
                // copy() (source survives), `give` moves, bare is ambiguous. A plain resource moves.
                bool doCopy = false;
                if (handoff == 2) {
                    if (!isCopyable(lty))
                        unsupported(("`" + lty + "` has no `copy` method — add one to opt into `Copyable`, "
                                     "or use `give` to move it").c_str(), n->line);
                    doCopy = true;
                } else if (handoff == 0 && isCopyable(lty))
                    unsupported(("`" + lty + "` is copyable — a bare hand-off is ambiguous; say `give` "
                                 "(move) or `copy` (duplicate)").c_str(), n->line);
                // A move needs a movable local (reject moving out of a field/element); a copy reads
                // any lvalue. Either way, handing a value onto itself drops it then reads it — reject.
                std::string mv = doCopy ? std::string() : moveOnlySource(rhs, n->line);
                std::string rname;
                if (auto* rid = dynamic_cast<IdentifierNode*>(rhs.get()))
                    if (rid->value && (!rid->qualifier || rid->qualifier->empty())) rname = *rid->value;
                if (!lname.empty() && ((!mv.empty() && mv == lname) || (doCopy && rname == lname)))
                    unsupported("handing a value onto itself would use it after it was dropped", n->line);
                bool bMoved = (!lname.empty() && _moveState.count(lname) && _moveState[lname] == MoveState::Moved);
                if (!lname.empty()) _moveState[lname] = MoveState::NotMoved;   // write target: clear before emit
                std::string b   = emitExpression(as->unaryExpression);
                std::string src = emitExpression(rhs);
                line(n->line);
                if (!bMoved) { indent(depth); *_out << lty << "__dtor(&" << b << ");\n"; }   // free the old value
                indent(depth); *_out << b << " = " << (doCopy ? (lty + "__copy(&(" + src + "))") : src) << ";\n";
                if (!mv.empty()) markMoved(mv);
                return;
            }
        }
    }

    // Bare expression statement (e.g. an assignment or call used as a statement).
    if (dynamic_cast<ExpressionStatementNode*>(n)) {
        line(n->line);
        bool ph = _hoistOK; _hoistOK = true;                       // M26i: allow inline-ctor hoisting
        std::string s = emitExpression(std::dynamic_pointer_cast<ExpressionNode>(stmt));
        _hoistOK = ph;
        flushHoisted(depth);                                       // temp decls first…
        indent(depth);
        *_out << s << ";\n";                                       // …then the statement using them
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

        // M27a: a generic template (`fn max<T>(…)`) is registered for monomorphization and is
        // NOT emitted as-is (its `T` is unbound). Its FuncSig stays in _funcs so call sites reorder
        // named args off it; call emission redirects to the concrete instantiation instead.
        if (fn->typeParams && !fn->typeParams->empty()) {
            _generics[sig.cName]   = fn;
            _genericCtx[sig.cName] = _nsCtx;   // resolve the body's type refs in its home scope
        }
    }
}

// Collect interface declarations (method prototype lists).
void CEmitter::collectInterfaces(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        // Legacy `interface I { … }`.
        if (auto* id = dynamic_cast<InterfaceDeclarationNode*>(decl.get())) {
            if (!id->identifier || !id->identifier->value) continue;
            if (id->baseTypes && !id->baseTypes->empty())
                unsupported("interface inheritance (interface : interface) — deferred", id->line);
            InterfaceInfo ii;
            ii.name = qualify(*id->identifier->value); ii.scope = _nsCtx.scope; ii.usings = _nsCtx.usings;
            if (id->body)
                for (auto& m : *id->body)
                    if (m->name && m->name->value)
                        ii.methods.push_back({*m->name->value, m->returnType, m->parameters});
            _interfaces[ii.name] = ii;
            continue;
        }
        // M26h: `type contract C { … }` — a ClassDeclarationNode whose kind word is "contract".
        // Its methods parse as (bodiless) class methods; register them as a contract's slots.
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->typeKind || *cd->typeKind != "contract" || !cd->name || !cd->name->value) continue;
        InterfaceInfo ii;
        ii.name = qualify(*cd->name->value); ii.scope = _nsCtx.scope; ii.usings = _nsCtx.usings;
        if (cd->members)
            for (auto& m : *cd->members) {
                // M26h — a `contract` is a public guarantee: methods only, no bodies, no fields, no
                // ctor/dtor (it holds no state and constructs nothing).
                if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get())) {
                    if (md->body)
                        unsupported(("a `contract` method (`" + (md->name && md->name->value ? *md->name->value : std::string())
                                     + "`) has no body — it is a guarantee, not an implementation").c_str(), md->line);
                    if (md->name && md->name->value)
                        ii.methods.push_back({*md->name->value, md->returnType, md->params});
                } else if (dynamic_cast<ClassFieldDeclarationNode*>(m.get()) || dynamic_cast<ClassConstDeclarationNode*>(m.get())) {
                    unsupported(("a `contract` holds no state — remove the field from `" + ii.name + "`").c_str(), m->line);
                } else if (dynamic_cast<ClassConstructorDeclarationNode*>(m.get()) || dynamic_cast<ClassDestructorDeclarationNode*>(m.get())) {
                    unsupported(("a `contract` has no constructor/destructor — `" + ii.name + "` is a guarantee").c_str(), m->line);
                }
            }
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

        // M26h: `type <kind> Name` — map the kind word. `contract` is registered as an interface
        // (collectInterfaces), so skip it here; a bad kind word is a clear error.
        TypeKind kind = TypeKind::Legacy;
        if (cd->typeKind) {
            if      (*cd->typeKind == "value")    kind = TypeKind::Value;
            else if (*cd->typeKind == "resource") kind = TypeKind::Resource;
            else if (*cd->typeKind == "contract") continue;   // handled as an interface
            else unsupported(("unknown type kind `" + *cd->typeKind
                              + "` — expected `value`, `resource`, or `contract`").c_str(), cd->line);
        }

        // FFI (M16): an `extern class`/`extern value` is an external C struct — keep its literal
        // C name (not namespace-mangled) and don't emit/own it.
        bool isExt = false;
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers)
                if (mod->value && *mod->value == "extern") isExt = true;

        ClassInfo ci;
        ci.name  = isExt ? *cd->name->value : qualify(*cd->name->value);   // M14 mangle / M16 literal
        ci.kind  = kind;
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
        // M26h — `virtual`/`abstract`/`final` are qualifiers on a `resource` (extensible owned
        // hierarchy). A `value` is sealed — for polymorphism use a `contract`.
        if (ci.kind == TypeKind::Value && (ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
            unsupported("a `value` is sealed — `virtual`/`abstract`/`final` apply to a `resource`; "
                        "for polymorphism declare a `contract`", cd->line);

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
                    // M26h — a `value` picks field visibility PER FIELD (default private, `public`
                    // allowed; `protected` belongs to an extensible `resource`). A `resource` field is
                    // always private (ownership encapsulated). Legacy/`pod`/extern keep kind-driven
                    // exposure. (extern struct fields are public — the FFI struct owns its layout.)
                    Visibility fvis = fieldVisibility(ci, fd->modifiers, fd->line);
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
                        // M26h — `protected` belongs to a `resource` in an extensibility hierarchy
                        // (`virtual`/`abstract` declares protected members for subclasses; a `final`
                        // override still uses `protected` by NVI). It's meaningless on a `value`, a
                        // plain sealed `resource`, or a `contract` — those members are private/public.
                        if (ci.kind != TypeKind::Legacy && mi.visibility == Visibility::Protected
                            && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
                            unsupported(("`protected` belongs to a `virtual`/`abstract`/`final resource` — `" + ci.name
                                         + "` is a plain `value`/`resource`, so its members are `private` or `public`").c_str(), md->line);
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
                        // M26f-4: opting into the `Copyable` contract — a public, nullary `copy`
                        // returning the class's OWN type (unqualified, same simple name). Its presence
                        // makes the give/copy marker mandatory on this `resource` value. (The explicit
                        // `: Copyable` form, needing `This`, lands with M26h/M27; this is the interim.)
                        if (*md->name->value == "copy" && mi.params.empty()
                            && mi.visibility == Visibility::Public
                            && md->returnType && md->returnType->value
                            && (!md->returnType->qualifier || md->returnType->qualifier->empty())
                            && *md->returnType->value == *cd->name->value)
                            ci.copyable = true;
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
                    // M26h — `~dtor` ⟺ `resource`. A `value` owns nothing, so a destructor makes it
                    // a resource; that disagreement is the lesson in the message.
                    if (ci.kind == TypeKind::Value)
                        unsupported("a `value` owns nothing — a `~dtor` makes it a `resource`; "
                                    "declare it `type resource`", dd->line);
                    ci.hasDtor  = true;
                    ci.dtorNode = dd;
                } else if (auto* kd = dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    // M24d: a `const` data member — a normal struct field, written
                    // ONCE in the constructor (inline init or `this.f = …`), then
                    // immutable. Enforcement is at the cstar level; the C field is plain.
                    // M26h — visibility follows the same per-field rule (see fieldVisibility).
                    Visibility kvis = fieldVisibility(ci, kd->modifiers, kd->line);
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
                } else if (auto* fg = dynamic_cast<FriendGrantNode*>(mn)) {
                    // M25c — capture the grant raw; the accessor is resolved (against the
                    // full function/class tables) in resolveFriends() once all units load.
                    RawFriendGrant rg; rg.accessor = fg->accessor; rg.line = fg->line;
                    if (fg->members)                                  // null => `[...]` (all privates)
                        for (auto& m : *fg->members)
                            if (m && m->value) rg.members.insert(*m->value);
                    ci.friendGrantsRaw.push_back(rg);
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
        // M27b: a generic TYPE template (`type value Box<T>`) is kept OUT of _classes — it is
        // specialized per concrete `Box<Arg>` at discovery. Its ClassInfo shape (T-typed fields/
        // methods) is parked in _genericTypes; the specialized instances are the real classes.
        if (cd->typeParams && !cd->typeParams->empty()) {
            std::vector<std::string> ps;
            for (auto& p : *cd->typeParams) if (p) ps.push_back(*p);   // [A, B, …]
            _genericTypeParams[ci.name] = ps;
            _genericTypeCtx[ci.name]    = _nsCtx;
            _genericTypes[ci.name]      = ci;
        } else {
            _classes[ci.name] = ci;
        }
    }
}

// ---- Collections (M9) -----------------------------------------------------

// The mangling suffix for an element type: primitives use a short stable
// spelling; a class/enum uses its own name. Array<int32> -> "int32".
std::string CEmitter::mangleElem(SharedIdentifier elem)
{
    if (!elem) return "void";
    // M27a: substitute a bound type-param before mangling (mirrors cType).
    if (!_typeSubst.empty() && elem->value && !elem->genericArg) {
        auto s = _typeSubst.find(*elem->value);
        if (s != _typeSubst.end()) return mangleElem(s->second);
    }
    switch (elem->builtInVal) {
        case IDENTIFIER_STRING_VAL:  return "string";
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
        default: {  // class / generic element — resolve to its mangled name (the suffix)
            if (!elem->value) return "void";
            std::string base = resolveUserName(*elem->value, elem->qualifier);
            // M27b-beta: recurse into a nested generic arg so `Shared<Circle>` mangles to
            // `Shared_Circle` (not just `Shared`) — fixes the `List<Shared<Circle>>` collision.
            if (elem->genericArgs) for (auto& a : *elem->genericArgs) base += "_" + mangleElem(a);
            else if (elem->genericArg) base += "_" + mangleElem(elem->genericArg);
            return base;
        }
    }
}

// M27b: the mangled struct name for `Pair<A, B, …>` — the template's scoped name + one "_<mangle>"
// suffix per type arg. `mangleElem` resolves a bound `T` under _typeSubst, so this is used identically
// at discovery (concrete args), at cType (field/decl args under subst), and to name the specialized ClassInfo.
std::string CEmitter::genericTypeMangle(const std::string& tmpl, SharedIdentifierList args)
{
    std::string m = tmpl;
    if (args) for (auto& a : *args) m += "_" + mangleElem(a);
    return m;
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
        // M26g: a smart pointer over an INTERFACE owns the concrete object behind a fat element.
        if (isInterface(elemCType)) {
            if (isWeak) registerSmartPtr(CollKind::Shared, elem);   // upgrade()'s Shared<I> return
            registerSmartPtr(kind, elem);   // interface-element variant (elemClass = the interface)
            return;
        }
        if (elemClass.empty()) {
            unsupported("a smart pointer requires a class or interface element type", collType->line);
            return;
        }
        // A Weak needs its Shared (lock()'s return type + the source of a weak).
        if (isWeak) registerSmartPtr(CollKind::Shared, elem);
        registerSmartPtr(kind, elem);
        return;
    }

    // M26e: an interface borrows its object — it can't be a BARE collection element (a
    // `List`/`Array` of interface fat pointers would dangle). Own the object: store an
    // `Owned<I>`/`Shared<I>` (a `List<Shared<I>>`) instead — the smart-ptr-over-interface (M26g).
    if (isInterface(elemCType)) {
        std::string nm = (elem && elem->value) ? *elem->value : elemCType;
        unsupported(("an interface (`" + nm + "`) borrows its object, so it can't be a collection "
                     "element — it would dangle; store an owning `Shared<" + nm + ">` instead").c_str(),
                    collType->line);
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
    _collectionOrder.push_back(cName);

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
    bool elemIface = isInterface(elemCType);                          // M26g: fat-element variant
    std::string elemClass  = (isClass(elemCType) || elemIface) ? elemCType : "";
    if (elemClass.empty()) return;              // caller diagnosed
    std::string cName = (kind == CollKind::Owned  ? "Owned_"
                       : kind == CollKind::Shared ? "Shared_" : "Weak_") + elemMangle;
    if (_collections.count(cName)) return;      // dedup

    CollectionInfo info;
    info.kind = kind; info.cName = cName;
    info.elemCType = elemCType; info.elemMangle = elemMangle; info.elemClass = elemClass;
    info.elemIsInterface = elemIface;
    // An interface element's concrete object is dropped via its vtable's __dtor slot, not the
    // collection's ELEM_DTOR machinery, so elemDestructible stays false here.
    info.elemDestructible = !elemIface && _classes.count(elemClass) && _classes[elemClass].destructible;
    _collections[cName] = info;
    _collectionOrder.push_back(cName);

    ClassInfo ci;
    ci.name = cName; ci.isCollection = true; ci.collKind = kind;
    ci.collElemClass = elemClass; ci.destructible = true; ci.hasCtor = false;
    auto addM = [&](const std::string& m, std::vector<ParamSig> p) {
        MethodInfo mi; mi.cName = cName + "__" + m; mi.params = std::move(p);
        mi.isIntrinsic = true; ci.methods[m] = mi;
    };
    // Intrinsics (not auto-deref forwarded): Owned has none; Shared has valid();
    // Weak has upgrade() (-> Shared) and expired().
    if (kind == CollKind::Shared) addM("valid", {});
    if (kind == CollKind::Weak) { addM("upgrade", {}); addM("expired", {}); }
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
    _collectionOrder.push_back(cName);

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
    // Inner-first: register the element's collections / generic instances BEFORE the enclosing
    // type, so the outer's elemClass/elemDestructible resolve against an already-registered inner
    // (`List<Shared<IShape>>` must see `Shared_IShape` in _classes to drop each element; likewise
    // `List<List<T>>`, `List<Box<T>>`). Recurse ALL type args (M27b-beta) so a collection/generic in
    // a 2nd+ position (`Pair<int, List<int>>`) is discovered — not just the first arg.
    if (t->genericArgs) for (auto& a : *t->genericArgs) scanTypeForCollections(a);
    else if (t->genericArg) scanTypeForCollections(t->genericArg);
    scanTypeForGenericTypes(t);                                 // M27b: also discover Pair<A,B> here
    if (isCollectionType(t)) registerCollection(t);
}

// M27b: register the specialized instance for a user generic-type reference `Pair<A, B>`. The
// recursion into the args is driven by scanTypeForCollections (which calls this at each type node).
void CEmitter::scanTypeForGenericTypes(SharedIdentifier t)
{
    if (!t || !t->value || !t->genericArg) return;             // genericArg mirrors genericArgs[0]
    std::string tmpl = resolveUserName(*t->value, t->qualifier);
    if (_genericTypes.count(tmpl)) registerGenericTypeInst(tmpl, t->genericArgs);
}

// Build one synthetic specialized ClassInfo per `Box<Arg>` (mirrors registerCollection): copy the
// template shape, rewrite identity (struct name + method cNames), re-derive param signatures under
// _typeSubst, register in _classes, and transitively scan its substituted member types so a
// `Box<T>` holding `List<T>` registers `List_int32`. Deduped by the mangled name.
void CEmitter::registerGenericTypeInst(const std::string& tmpl, SharedIdentifierList args)
{
    if (!args || args->empty()) return;
    const std::vector<std::string>& params = _genericTypeParams[tmpl];

    // Resolve each bare-`T` arg (the nested/transitive case) to its concrete binding.
    std::vector<SharedIdentifier> concrete;
    for (auto& a : *args) {
        SharedIdentifier c = a;
        if (a && !_typeSubst.empty() && a->value && !a->genericArg) {
            auto s = _typeSubst.find(*a->value);
            if (s != _typeSubst.end()) c = s->second;
        }
        concrete.push_back(c);
    }
    // Arity: N type arguments must match the template's N type parameters.
    if (concrete.size() != params.size()) {
        unsupported(("wrong number of type arguments for generic type `" + tmpl + "` (expected "
                     + std::to_string(params.size()) + ", got " + std::to_string(concrete.size()) + ")").c_str(),
                    args->front() ? args->front()->line : 0);
        return;
    }

    std::string mangled = tmpl;
    for (auto& c : concrete) mangled += "_" + mangleElem(c);
    if (_genericTypeInsts.count(mangled)) return;               // dedup

    // Register the KEY first so the transitive scan below can't recurse into this same instance.
    _genericTypeInsts[mangled] = { tmpl, mangled, concrete };
    _genericTypeInstOf[mangled] = tmpl;
    _genericTypeInstOrder.push_back(mangled);

    NsCtx savedCtx = _nsCtx;
    std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
    _nsCtx = _genericTypeCtx[tmpl];
    _typeSubst.clear();
    for (size_t i = 0; i < params.size(); ++i) _typeSubst[params[i]] = concrete[i];   // zip params -> args

    ClassInfo ci = _genericTypes[tmpl];                         // copy the template shape
    ci.name = mangled;
    ci.isGenericInst = true;
    for (auto& kv : ci.methods) kv.second.cName = mangled + "__" + kv.first;
    // Re-derive ParamSig under substitution so call-site arg typing is concrete (not a stale "T").
    if (ci.ctorNode && ci.ctorNode->declarator)
        ci.ctorParams = paramSigsOf(ci.ctorNode->declarator->params);
    for (auto& kv : ci.methods)
        if (kv.second.node) kv.second.params = paramSigsOf(kv.second.node->params);
    _classes[mangled] = ci;

    // Transitive close: register any collection / generic type the substituted members use.
    for (auto& f : ci.fields) scanTypeForCollections(f.type);
    for (auto& kv : ci.methods) {
        scanTypeForCollections(kv.second.returnType);
        if (kv.second.node) for (auto& p : *kv.second.node->params) if (p) scanTypeForCollections(p->type);
    }
    if (ci.ctorNode && ci.ctorNode->declarator)
        for (auto& p : *ci.ctorNode->declarator->params) if (p) scanTypeForCollections(p->type);

    _typeSubst = savedSubst;
    _nsCtx = savedCtx;
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

// ---- Generics (M27a) ------------------------------------------------------
// Monomorphization. A `fn name<T,...>(…)` template is specialized once per concrete
// type-argument tuple reachable from a call site. Discovery infers the tuple from the
// call's arguments (literals + locally-typed values), registers a deduped instantiation,
// and records the target per call node. Emission clones nothing — it re-emits the template
// body with `_typeSubst` set, so cType/mangleElem resolve each `T` to its concrete type.

// A cached, synthesized type node for a primitive kind — inference needs a SharedIdentifier
// to feed _typeSubst / mangleElem, but a literal argument has no type node of its own.
SharedIdentifier CEmitter::primTypeNode(int builtInVal)
{
    auto it = _primTypeCache.find(builtInVal);
    if (it != _primTypeCache.end()) return it->second;
    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<generic>"));
    auto node = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>(""), builtInVal);
    _primTypeCache[builtInVal] = node;
    return node;
}

// The concrete type node of an argument expression (null if undeterminable). M27a inputs:
// literals -> their builtin kind; a bare identifier -> its declared type via `localTys`; an
// explicit new/cast -> its own type; an arithmetic expr -> an operand's type.
SharedIdentifier CEmitter::exprTypeNode(SharedExpression e, std::map<std::string, SharedIdentifier>& localTys)
{
    if (!e) return nullptr;
    ASTNode* n = e.get();
    if (dynamic_cast<Int8Node*>(n))    return primTypeNode(IDENTIFIER_INT8_VAL);
    if (dynamic_cast<Int16Node*>(n))   return primTypeNode(IDENTIFIER_INT16_VAL);
    if (dynamic_cast<Int32Node*>(n))   return primTypeNode(IDENTIFIER_INT32_VAL);
    if (dynamic_cast<Int64Node*>(n))   return primTypeNode(IDENTIFIER_INT64_VAL);
    if (dynamic_cast<UInt8Node*>(n))   return primTypeNode(IDENTIFIER_UINT8_VAL);
    if (dynamic_cast<UInt16Node*>(n))  return primTypeNode(IDENTIFIER_UINT16_VAL);
    if (dynamic_cast<UInt32Node*>(n))  return primTypeNode(IDENTIFIER_UINT32_VAL);
    if (dynamic_cast<UInt64Node*>(n))  return primTypeNode(IDENTIFIER_UINT64_VAL);
    if (dynamic_cast<Float32Node*>(n)) return primTypeNode(IDENTIFIER_FLOAT32_VAL);
    if (dynamic_cast<Float64Node*>(n)) return primTypeNode(IDENTIFIER_FLOAT64_VAL);
    if (dynamic_cast<BooleanNode*>(n)) return primTypeNode(IDENTIFIER_BOOL_VAL);
    if (dynamic_cast<StringNode*>(n))  return primTypeNode(IDENTIFIER_STRING_VAL);
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (!id->value) return nullptr;
        auto it = localTys.find(*id->value);
        return it != localTys.end() ? it->second : nullptr;
    }
    if (auto* oc = dynamic_cast<ObjectCreationNode*>(n)) return oc->type;
    if (auto* c  = dynamic_cast<CastNode*>(n))           return c->type;
    if (auto* b  = dynamic_cast<BinaryExpressionNode*>(n)) {
        SharedIdentifier l = exprTypeNode(b->LHS, localTys);
        return l ? l : exprTypeNode(b->RHS, localTys);
    }
    return nullptr;
}

// A type node usable as a generic type argument in M27a: a primitive, or a known
// class/enum/interface. A bare type-parameter (its name resolves to none of these) and a
// collection/smart-pointer type argument (a `List<…>` etc.) are NOT concrete here (M27b).
bool CEmitter::isConcreteTypeArg(SharedIdentifier t)
{
    if (!t) return false;
    if (t->builtInVal != IDENTIFIER_NONE_VAL) return true;   // primitive
    if (t->genericArg) return false;                          // List<…>/Shared<…> arg — M27b
    if (!t->value) return false;
    std::string m = resolveUserName(*t->value, t->qualifier);
    return _classes.count(m) || _enums.count(m) || _interfaces.count(m);
}

// Unify a generic call's arguments against the template's parameters -> a deduped instantiation.
// Each parameter whose declared type is a bare type-param binds it to the argument's concrete
// type; a second, conflicting binding, an unresolvable argument, or a return-only (unbound)
// type parameter each produce a clean diagnostic. Runs with _typeSubst empty (concrete mangles).
bool CEmitter::inferGenericInst(FunctionDeclarationNode* tmpl, const std::string& key, SharedArgumentList args,
                                std::map<std::string, SharedIdentifier>& localTys, int line, GenericInst& out)
{
    std::map<std::string, SharedExpression> byName;
    if (args) for (auto& a : *args) if (a && a->name && a->name->value) byName[*a->name->value] = a->expression;

    std::set<std::string> tps;
    for (auto& tp : *tmpl->typeParams) if (tp) tps.insert(*tp);

    std::map<std::string, SharedIdentifier> bind;
    if (tmpl->parameters) for (auto& p : *tmpl->parameters) {
        if (!p || !p->type || !p->type->value) continue;
        const std::string& pty = *p->type->value;
        if (p->type->genericArg || !tps.count(pty)) continue;   // not a bare type-param (List<T> etc. — M27b)
        std::string pname = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
        auto ai = byName.find(pname);
        if (ai == byName.end()) continue;   // missing arg — emitReorderedCall reports it precisely
        SharedIdentifier at = exprTypeNode(ai->second, localTys);
        if (!isConcreteTypeArg(at)) {
            unsupported(("cannot infer generic type parameter '" + pty + "' — argument '" + pname +
                         "' is not a literal or a locally-typed value").c_str(), line);
            return false;
        }
        auto b = bind.find(pty);
        if (b != bind.end() && mangleElem(b->second) != mangleElem(at)) {
            unsupported(("cannot unify type parameter '" + pty + "' (" + mangleElem(b->second) +
                         " vs " + mangleElem(at) + ")").c_str(), line);
            return false;
        }
        bind[pty] = at;
    }

    for (auto& tp : *tmpl->typeParams) {
        if (tp && !bind.count(*tp)) {
            unsupported(("cannot infer type parameter '" + *tp + "' from the call arguments "
                         "(explicit type arguments are not yet supported)").c_str(), line);
            return false;
        }
    }

    out.templateKey = key;
    out.typeArgs.clear();
    std::string mangled = key;
    for (auto& tp : *tmpl->typeParams) {
        SharedIdentifier a = bind[*tp];
        out.typeArgs.push_back(a);
        mangled += "__" + mangleElem(a);
    }
    out.mangledName = mangled;
    return true;
}

void CEmitter::scanExprForGenerics(SharedExpression e, std::map<std::string, SharedIdentifier>& localTys)
{
    if (!e) return;
    ASTNode* n = e.get();
    if (auto* oc = dynamic_cast<ObjectCreationNode*>(n)) {
        if (oc->args) for (auto& a : *oc->args) if (a) scanExprForGenerics(a->expression, localTys);
    } else if (auto* c = dynamic_cast<CastNode*>(n)) {
        scanExprForGenerics(c->unaryExpression, localTys);
    } else if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) {
        scanExprForGenerics(b->LHS, localTys); scanExprForGenerics(b->RHS, localTys);
    } else if (auto* l = dynamic_cast<LogicalAndOrNode*>(n)) {
        scanExprForGenerics(l->LHS, localTys); scanExprForGenerics(l->RHS, localTys);
    } else if (auto* tn = dynamic_cast<TernaryExpressionNode*>(n)) {
        scanExprForGenerics(tn->condition, localTys); scanExprForGenerics(tn->LHS, localTys); scanExprForGenerics(tn->RHS, localTys);
    } else if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        scanExprForGenerics(as->unaryExpression, localTys); scanExprForGenerics(as->expression, localTys);
    } else if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        scanExprForGenerics(inv->expression, localTys);
        if (inv->args) for (auto& a : *inv->args) if (a) scanExprForGenerics(a->expression, localTys);
        if (inv->identifier && inv->identifier->value) {
            std::string k = resolveFunc(*inv->identifier->value, inv->identifier->qualifier);
            auto git = _generics.find(k);
            if (git != _generics.end()) {
                GenericInst gi;
                if (inferGenericInst(git->second, k, inv->args, localTys, inv->line, gi)) {
                    if (!_genericInsts.count(gi.mangledName)) _genericInsts[gi.mangledName] = gi;
                    _callInst[inv] = gi.mangledName;   // one call node -> one instantiation
                }
            }
        }
    } else if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        scanExprForGenerics(ea->expression, localTys);
        if (ea->expressionlist) for (auto& x : *ea->expressionlist) scanExprForGenerics(x, localTys);
    } else if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        scanExprForGenerics(ma->expression, localTys);
    } else if (auto* pe = dynamic_cast<PreIncrDecrNode*>(n)) {
        scanExprForGenerics(pe->expression, localTys);
    } else if (auto* po = dynamic_cast<PostIncrDecrNode*>(n)) {
        scanExprForGenerics(po->expression, localTys);
    } else if (auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        scanExprForGenerics(su->expression, localTys);
    }
}

void CEmitter::scanStmtForGenerics(SharedStatement s, std::map<std::string, SharedIdentifier>& localTys)
{
    if (!s) return;
    ASTNode* n = s.get();
    if (auto* b = dynamic_cast<BlockNode*>(n)) {
        if (b->statements) for (auto& st : *b->statements) scanStmtForGenerics(st, localTys);
    } else if (auto* d = dynamic_cast<LocalVariableDeclaration*>(n)) {
        if (d->variables) for (auto& v : *d->variables) if (v) {
            scanExprForGenerics(v->initializer, localTys);
            // ponytail: flat name->type map, no scope-pop — correct without shadowing (fine for M27a).
            if (v->name && v->name->value && d->type) localTys[*v->name->value] = d->type;
        }
    } else if (auto* cd = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        if (cd->variables) for (auto& v : *cd->variables) if (v) {
            scanExprForGenerics(v->initializer, localTys);
            if (v->name && v->name->value && cd->type) localTys[*v->name->value] = cd->type;
        }
    } else if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        scanExprForGenerics(r->expression, localTys);
    } else if (auto* f = dynamic_cast<IfNode*>(n)) {
        scanExprForGenerics(f->booleanExpression, localTys);
        scanStmtForGenerics(f->ifStatement, localTys); scanStmtForGenerics(f->elseStatement, localTys);
    } else if (auto* w = dynamic_cast<WhileNode*>(n)) {
        scanExprForGenerics(w->booleanExpression, localTys); scanStmtForGenerics(w->whileStatement, localTys);
    } else if (auto* dw = dynamic_cast<DoWhileNode*>(n)) {
        scanExprForGenerics(dw->booleanExpression, localTys); scanStmtForGenerics(dw->doWhileStatement, localTys);
    } else if (auto* fr = dynamic_cast<ForNode*>(n)) {
        if (fr->initializerStatements) for (auto& st : *fr->initializerStatements) scanStmtForGenerics(st, localTys);
        scanExprForGenerics(fr->booleanExpression, localTys);
        if (fr->iteratorStatements) for (auto& st : *fr->iteratorStatements) scanStmtForGenerics(st, localTys);
        scanStmtForGenerics(fr->body, localTys);
    } else if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        scanExprForGenerics(fe->expression, localTys);
        scanStmtForGenerics(fe->body, localTys);
    } else if (auto* sw = dynamic_cast<SwitchNode*>(n)) {
        scanExprForGenerics(sw->expression, localTys);
        if (sw->switchsections) for (auto& sec : *sw->switchsections)
            if (sec && sec->statementList) for (auto& st : *sec->statementList) scanStmtForGenerics(st, localTys);
    } else if (dynamic_cast<ExpressionStatementNode*>(n)) {
        scanExprForGenerics(std::dynamic_pointer_cast<ExpressionNode>(s), localTys);
    }
}

// Pre-pass: discover every reachable generic-function instantiation (call sites in free
// functions + class members). Seeds `localTys` with the enclosing signature's params.
void CEmitter::collectGenericInsts(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    if (_generics.empty()) return;   // nothing generic in the program
    auto seed = [&](SharedParameterList params, std::map<std::string, SharedIdentifier>& lt) {
        if (params) for (auto& p : *params)
            if (p && p->identifier && p->identifier->value && p->type) lt[*p->identifier->value] = p->type;
    };
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (!fn->block) continue;
            std::map<std::string, SharedIdentifier> lt; seed(fn->parameters, lt);
            scanStmtForGenerics(fn->block, lt);
        } else if (auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get())) {
            if (cd->members) for (auto& m : *cd->members) {
                ASTNode* mn = m.get();
                if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(mn)) {
                    std::map<std::string, SharedIdentifier> lt; seed(md->params, lt);
                    scanStmtForGenerics(md->body, lt);
                } else if (auto* cc = dynamic_cast<ClassConstructorDeclarationNode*>(mn)) {
                    std::map<std::string, SharedIdentifier> lt;
                    if (cc->declarator) seed(cc->declarator->params, lt);
                    scanStmtForGenerics(cc->body, lt);
                } else if (auto* dd = dynamic_cast<ClassDestructorDeclarationNode*>(mn)) {
                    std::map<std::string, SharedIdentifier> lt;
                    scanStmtForGenerics(dd->body, lt);
                }
            }
        }
    }
}

// Emit one specialized `static` C function for an instantiation: a forward prototype
// (prototypeOnly) so instantiations may call one another / recurse, else the body. The
// template body is re-emitted with _typeSubst bound to this tuple, in the template's home
// namespace scope, under the instantiation's mangled name.
void CEmitter::emitGenericInst(const GenericInst& gi, bool prototypeOnly)
{
    auto tit = _generics.find(gi.templateKey);
    if (tit == _generics.end()) return;
    FunctionDeclarationNode* tmpl = tit->second;

    NsCtx savedCtx = _nsCtx;
    auto cit = _genericCtx.find(gi.templateKey);
    if (cit != _genericCtx.end()) _nsCtx = cit->second;

    _typeSubst.clear();
    for (size_t i = 0; i < tmpl->typeParams->size() && i < gi.typeArgs.size(); ++i)
        if ((*tmpl->typeParams)[i]) _typeSubst[*(*tmpl->typeParams)[i]] = gi.typeArgs[i];

    if (prototypeOnly) emitFunctionPrototype(tmpl, &gi.mangledName);   // emits `static` via nameOverride
    else               emitFunction(tmpl, &gi.mangledName);

    _typeSubst.clear();
    _nsCtx = savedCtx;
}

// Emit the CSTAR_*_{TYPE,FUNCS}(...) macro line per registered instantiation.
// `typesOnly` picks the struct-typedef half (emitted before class struct bodies so a
// class may hold a collection/smart-ptr BY VALUE) vs the funcs half (after class
// prototypes, where element dtors are declared).
void CEmitter::emitCollectionDefs(bool typesOnly)
{
    const char* suf = typesOnly ? "TYPE" : "FUNCS";
    // Iterate in registration (inner-first) order, not the map's alphabetical order: a collection's
    // dtor calls its element's dtor (`List<Shared<I>>` -> `Shared_I__dtor`), so the element's FUNCS
    // must be emitted first. Alphabetical order breaks e.g. `List_...` (emitted before `Shared_...`).
    for (const std::string& cName : _collectionOrder) {
        CollectionInfo& info = _collections[cName];
        std::string elemDtor = info.elemDestructible ? (info.elemClass + "__dtor") : "CSTAR_ELEM_NODTOR";
        std::string tail = typesOnly ? ")\n"                          // _TYPE(T, NAME)
                                     : (", " + elemDtor + ")\n");     // _FUNCS(T, NAME, ELEM_DTOR)
        // M26f-5: Array/List `__copy` deep-copies each element — a `Copyable` resource via its own
        // `Elem__copy`, else a memberwise (bitwise) copy. (Only these two kinds have `__copy`.)
        std::string elemCopy = info.elemCopyable ? (info.elemClass + "__copy") : "CSTAR_ELEM_MEMBERWISE";
        std::string collTail = typesOnly ? ")\n" : (", " + elemDtor + ", " + elemCopy + ")\n");
        if (info.kind == CollKind::Array)
            *_out << "CSTAR_ARRAY_" << suf << "(" << info.elemCType << ", " << info.cName << collTail;
        else if (info.kind == CollKind::List)
            *_out << "CSTAR_LIST_" << suf << "(" << info.elemCType << ", " << info.cName << collTail;
        else if (info.kind == CollKind::Owned && info.elemIsInterface)
            // M26g: fat-element `Owned<I>` — TYPE takes the vtbl type, FUNCS drops via the vtbl slot.
            *_out << "CSTAR_OWNED_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : ")\n");
        else if (info.kind == CollKind::Owned)
            *_out << "CSTAR_OWNED_" << suf << "(" << info.elemCType << ", " << info.cName << tail;
        else if (info.kind == CollKind::Shared && info.elemIsInterface)
            *_out << "CSTAR_SHARED_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : ")\n");
        else if (info.kind == CollKind::Shared)
            *_out << "CSTAR_SHARED_" << suf << "(" << info.elemCType << ", " << info.cName << tail;
        else if (info.kind == CollKind::Weak && info.elemIsInterface)
            // upgrade() returns the matching Shared<I> (its _TYPE is emitted in the same pass).
            *_out << "CSTAR_WEAK_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : (", Shared_" + info.elemMangle + ")\n"));
        else if (info.kind == CollKind::Weak)
            // upgrade() returns the matching Shared (its _TYPE is emitted in the same pass).
            *_out << "CSTAR_WEAK_" << suf << "(" << info.elemCType << ", " << info.cName
                 << (typesOnly ? ")\n" : (", Shared_" + info.elemMangle + ")\n"));
        else if (info.kind == CollKind::Bindable)
            // Fully type-erased — the signature drives only the invoke, not the layout.
            *_out << "CSTAR_BINDABLE_" << suf << "(" << info.cName << ")\n";
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

// A "named value" is an existing binding you can hand off (a variable / field /
// element / base member) — as opposed to a FRESH rvalue (a `new`/constructor, a
// call result, a literal), which is consumed in place and never needs a marker.
bool CEmitter::isNamedValue(ASTNode* e)
{
    return dynamic_cast<IdentifierNode*>(e) || dynamic_cast<MemberAccessNode*>(e)
        || dynamic_cast<ElementAccessNode*>(e) || dynamic_cast<BaseAccessNode*>(e);
}

// M26e: an interface value borrows its object, so it's a second-class view — it may
// be a parameter or local, but it can't be STORED beyond the call that produced it
// (a field, a return, a collection element) without dangling. Reject the bare-interface
// case with guidance toward owning the object (`Shared<I>` — M26f enables that).
void CEmitter::rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line)
{
    if (!ty || !isInterface(cType(ty))) return;
    std::string nm = (ty->value && !ty->value->empty()) ? *ty->value : cType(ty);
    unsupported(("an interface (`" + nm + "`) borrows its object, so it can't be " + whereClause
                 + " — it would dangle; own the object instead (e.g. `Shared<" + nm + ">`)").c_str(), line);
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
std::string CEmitter::smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem)
{
    // M26g: an owned INTERFACE handle's value field is the fat pointer `.obj`, not `.ptr`.
    if (kind == CollKind::Owned) return expr + (ifaceElem ? ".obj = NULL;" : ".ptr = NULL;");
    return expr + ".ctrl = NULL; " + expr + (ifaceElem ? ".obj = NULL;" : ".ptr = NULL;");  // Shared/Weak guard on ctrl
}

// ---- M26f-2: resource-value move analysis ---------------------------------

// A move-only VALUE: a destructible class value that isn't a smart-ptr / collection / extern
// struct. It moves on hand-off (its dtor is suppressed) and is never silently copied. (Trigger
// = destructibility — the interim proxy for a `resource` until the M26h vocabulary lands.)
bool CEmitter::isMoveOnlyValue(const std::string& cls) const
{
    auto it = _classes.find(cls);
    if (it == _classes.end() || it->second.isCollection || it->second.isExternStruct || isSmartPtrClass(cls))
        return false;
    const ClassInfo& ci = it->second;
    // M26h: move-only-ness is the DECLARED kind. A `resource` moves even if it owns nothing (an
    // empty resource is a move-only identity/token); a `value` copies. A legacy `class` (pre-marker)
    // keeps the interim destructibility proxy until the fixtures migrate (h-3).
    if (ci.kind == TypeKind::Resource) return true;
    if (ci.kind == TypeKind::Value)    return false;
    return ci.destructible;
}

// M26f-4: has this type opted into the `Copyable` contract? (Detected structurally at collection
// time — a public nullary `copy` returning the own type; see collectClasses.) Only ever consulted
// for a move-only value, where it flips the marker from "silent move" to "mandatory give/copy".
bool CEmitter::isCopyable(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.copyable;
}

void CEmitter::markMoved(const std::string& cVar)
{
    // M26f-2 (Increment 3): moving a local declared OUTSIDE the nearest enclosing loop would move
    // it again on the next iteration (double-move). Reject — conservative, no loop fixpoint. A value
    // declared INSIDE the loop body is fresh each iteration, so moving it is fine.
    int lb = -1;
    for (int i = (int)_scopes.size() - 1; i >= 0; --i) if (_scopes[i].isLoopBoundary) { lb = i; break; }
    if (lb >= 0) {
        int li = -1;
        for (int i = (int)_scopes.size() - 1; i >= 0 && li < 0; --i)
            for (auto& l : _scopes[i].locals) if (l.cVar == cVar) { li = i; break; }
        if (li >= 0 && li < lb)
            unsupported(("cannot `give` `" + cVar + "` inside a loop — it would be moved again on the next "
                         "iteration; move it after the loop, or move a value declared in the loop body").c_str(), _curLine);
    }
    _moveState[cVar] = MoveState::Moved;
}

void CEmitter::checkNotMoved(const std::string& cVar, int line)
{
    auto it = _moveState.find(cVar);
    if (it != _moveState.end() && it->second != MoveState::NotMoved)
        unsupported(("use of `" + cVar + "` after it was moved (a `give` consumed it)").c_str(), line);
}

// The source of a move hand-off. A bare move-only local -> its name (caller marks it moved).
// A field / element / base member -> reject: moving out would leave the owner holding a
// moved-from value (the field-move case is deferred to Optional<T>, M28).
std::string CEmitter::moveOnlySource(SharedExpression e, int line)
{
    if (auto* id = dynamic_cast<IdentifierNode*>(e.get())) {
        std::string nm = id->value ? *id->value : "";
        if ((!id->qualifier || id->qualifier->empty()) && _moveState.count(nm)) return nm;
    }
    unsupported("cannot `give` out of a field/element — it would leave the owner holding a "
                "moved-from value; move a local instead (Optional<T> comes in M28)", line);
    return "";
}

std::string CEmitter::emitSmartPtrCall(const std::string& cls, const std::string& recvExpr,
                                       const std::string& method, SharedArgumentList args, int srcLine)
{
    // An intrinsic on the pointer itself (lock/expired/valid)?
    if (_classes[cls].methods.count(method))
        return emitDispatch(cls, "&(" + recvExpr + ")", method, args, srcLine);
    // M26g: an owned INTERFACE handle dispatches polymorphically through its own {obj, vtbl}.
    if (isInterface(_classes[cls].collElemClass) && smartKind(cls) != CollKind::Weak)
        return emitInterfaceDispatch(recvExpr, _classes[cls].collElemClass, method, args, srcLine);
    // Otherwise auto-deref to the pointee T (Owned/Shared expose a T* ptr).
    if (smartKind(cls) != CollKind::Weak)
        return emitDispatch(_classes[cls].collElemClass, "(" + recvExpr + ").ptr", method, args, srcLine);
    unsupported(("Weak<T> has no member '" + method + "'; call .upgrade()").c_str(), srcLine);
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
    // M27b: specialized generic-type instances are emitted by a dedicated pass under _typeSubst,
    // not the normal class loops — exclude them here (their only consumer, header emission).
    for (auto& kv : _classes) if (!kv.second.isGenericInst) visit(&kv.second);
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
    // Seed: an explicit `~dtor` OR a collection/smart-ptr (always owns heap → RAII-dropped; its
    // ClassInfo carries destructible=true, which this reset must preserve — collections are
    // registered before this pass now, M26f-5).
    for (auto& kv : _classes) kv.second.destructible = kv.second.hasDtor || kv.second.isCollection;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& kv : _classes) {
            ClassInfo& ci = kv.second;
            if (ci.destructible || ci.isExternStruct) continue;   // cstar doesn't own external structs
            // M27b: a specialized instance's fields are typed in `T` — resolve them under its binding
            // (and the template's scope) so `Box<Resource>` correctly sees the owned resource.
            bool inst = ci.isGenericInst && _genericTypeInsts.count(ci.name);
            if (inst) {
                const GenericTypeInst& gi = _genericTypeInsts[ci.name];
                _nsCtx = _genericTypeCtx[gi.templateKey];
                _typeSubst.clear();
                const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
                for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
            } else {
                _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings;   // resolve field types in ci's scope
            }
            bool d = (ci.base && ci.base->destructible);
            if (!d)
                for (auto& f : ci.fields) {
                    auto it = _classes.find(cType(f.type));
                    if (it != _classes.end() && it->second.destructible) { d = true; break; }
                }
            if (inst) _typeSubst.clear();
            if (d) { ci.destructible = true; changed = true; }
        }
    }
    // Re-derive each collection's elemDestructible from the FINAL class destructibility — a
    // collection registered before the fixpoint saw only `~dtor`-based destructibility, so a
    // transitively-destructible element class would have been missed (element drops skipped → leak).
    for (auto& kv : _collections) {
        CollectionInfo& info = kv.second;
        if (info.kind == CollKind::Bindable) continue;   // fully type-erased; no element dtor
        auto it = _classes.find(info.elemClass);
        bool known = !info.elemClass.empty() && it != _classes.end();
        info.elemDestructible = known && it->second.destructible;
        info.elemCopyable     = known && it->second.copyable;   // M26f-5: deep-copy each element
    }
    // M26h — a `value` owns nothing. `destructible` (computed above, transitively over base + owned
    // fields + collections + smart-ptrs) is exactly "owns something to drop", so a destructible
    // `value` is a design/field disagreement: declare it a `resource`. (A raw `Ptr`/borrowed
    // contract confers no ownership → not destructible → correctly still a value.)
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.kind == TypeKind::Value && ci.destructible && !ci.isExternStruct)
            unsupported(("a `value` owns nothing, but `" + ci.name + "` transitively owns a resource "
                         "— declare it `type resource`").c_str(), ci.node ? ci.node->line : 0);
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
        // M26c/d: unwrap a give/copy hand-off marker. The inner value is what we emit;
        // the marker (give=move / copy=retain) only matters for a smart pointer passed
        // BY VALUE (ownership transfer) — it's meaningless on a borrow.
        SharedExpression argExpr = f->second->expression;
        int handoff = 0;   // 0 none, 1 give, 2 copy
        if (auto* h = dynamic_cast<HandoffNode*>(argExpr.get())) { handoff = h->isGive ? 1 : 2; argExpr = h->value; }
        // M26i: inline constructor in argument position — `f(x: Counter(start: 5))`. A ctor lowers to
        // `Cls__ctor(&dest, …)`, which needs an lvalue destination, so materialize a HOISTED temp
        // (declared on its own line before this statement — pure ISO C, no `({ … })`) and pass it by
        // value. Only for a by-value param of the exact class in a hoist-enabled statement context;
        // otherwise fall through to the normal path (which rejects an unsupported position cleanly).
        std::string val;
        InvocationNode* ctorIv = dynamic_cast<InvocationNode*>(argExpr.get());
        std::string ctorCls;
        if (ctorIv && ctorIv->identifier && ctorIv->identifier->value) {
            std::string rn = resolveUserName(*ctorIv->identifier->value, ctorIv->identifier->qualifier);
            if (isClass(rn) && _classes.count(rn)) ctorCls = rn;
            // M27b: `f(b: Box(v: 7))` — the ctor names template `Box`; the target param is instance `Box_int32`.
            else { auto g = _genericTypeInstOf.find(p.className);
                   if (g != _genericTypeInstOf.end() && g->second == rn) ctorCls = p.className; }
        }
        // M26i: an inline ctor is a temporary rvalue — it can't be borrowed (`ref`/`out`) or aliased
        // as an interface. Give a clear diagnostic instead of the generic "unknown function".
        if (!ctorCls.empty() && !_classes[ctorCls].isCollection && (p.byRef || isInterface(p.className)))
            unsupported("cannot pass an inline constructor to a `ref`/`out` or interface parameter — "
                        "bind it to a local first, then pass that", srcLine);
        if (_hoistOK && handoff == 0 && !ctorCls.empty() && ctorCls == p.className
            && !p.byRef && !isInterface(p.className) && !_classes[ctorCls].isCollection) {
            std::string t = "__ctorarg" + std::to_string(_tempCounter++);
            std::string ctor = emitCtorCall(t, _classes[ctorCls], ctorIv->args, srcLine);
            _hoisted.push_back(ctorCls + " " + t + "; " + ctor + ";");
            val = t;
        } else {
            val = emitExpression(argExpr);
        }
        if (handoff && (p.byRef || isInterface(p.className)))
            unsupported("`give`/`copy` transfer ownership by value — they don't apply to a `ref`/`out` "
                        "or interface borrow", srcLine);
        if (isInterface(p.className)) {
            std::string c = exprClass(argExpr);
            if (p.byRef) {
                // `ref`/`out` interface: the callee may reseat the caller's handle, so the
                // argument must be an actual interface variable (pass its address). A
                // concrete class would need a throwaway temp — reject it; bind first.
                if (!c.empty() && isClass(c))
                    unsupported(("cannot pass '" + c + "' by `ref`/`out` to interface parameter '" + p.name
                                 + "'; bind it to an `" + p.className + "` first "
                                 "(`" + p.className + " s = …; … ref s`)").c_str(), srcLine);
                if (!p.isConst) checkConstWrite(argExpr, srcLine);
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
            if (!p.isConst) checkConstWrite(argExpr, srcLine);
            // M26b: a borrow names the OBJECT (`ref T`). If the argument is a smart pointer
            // holding a T, auto-deref to its T* — `ref p` borrows the heap object, uniformly
            // with `ref stackValue`. (Weak can't be borrowed — it may be dead; tryUpgrade.)
            std::string argCls = exprClass(argExpr);
            if (isSmartPtrClass(argCls) && _classes[argCls].collElemClass == p.className) {
                if (smartKind(argCls) == CollKind::Weak)
                    unsupported(("cannot borrow through a `Weak<" + p.className
                                 + ">` (it may be dead) — `tryUpgrade` to a `Shared` first").c_str(), srcLine);
                s += "(" + val + ").ptr";
            } else {
                s += isClass(p.className) ? ("(" + p.className + "*)&(" + val + ")")  // upcast for ref Base
                                          : ("&(" + val + ")");
            }
        } else {
            // M26d: by value. A *named* smart pointer TRANSFERS into the param, which the
            // callee owns and drops at fn-end. The retain (copy) / invalidate (give) is a
            // statement, materialized as a HOISTED temp (M26i: pure ISO C — this replaced the
            // emitter's last GNU statement-expression).
            std::string argCls = exprClass(argExpr);
            bool collArg = !argCls.empty() && _classes.count(argCls) && _classes[argCls].isCollection;
            if (isSmartPtrClass(argCls) && isNamedValue(argExpr.get())) {
                CollKind k = smartKind(argCls);
                // Default the natural op: Owned -> give (move; copy illegal), Shared/Weak -> copy
                // (retain). A marker overrides (e.g. `give Shared` moves the handle).
                bool doGive = (handoff == 1) || (handoff == 0 && k == CollKind::Owned);
                if (handoff == 2 && k == CollKind::Owned)
                    unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", srcLine);
                std::string t = "__cstar_arg" + std::to_string(_tempCounter++);
                std::string side = doGive ? smartPtrInvalidate("(" + val + ")", k, isInterface(_classes[argCls].collElemClass))
                                          : ("(" + val + ").ctrl->" + (k == CollKind::Weak ? "weak" : "strong") + "++;");
                // M26i: hoist `T t = (x); <retain/invalidate>` as an ordinary statement (ISO C) —
                // this retired the emitter's statement-expressions everywhere a temp can precede its
                // statement (call/init/return/ctor sites). The `({ … })` form remains ONLY for a
                // by-value smart-ptr hand-off inside a loop/branch CONDITION (no preceding-statement
                // slot) — where it is also semantically REQUIRED: hoisting a per-iteration retain out
                // of a `while (...)` would run it once, not each time. No fixture reaches it (the ISO
                // `-pedantic-errors` gate confirms zero GNU extensions across the whole suite).
                if (_hoistOK) {
                    _hoisted.push_back(p.className + " " + t + " = (" + val + "); " + side);
                    s += t;
                } else {
                    s += "({ " + p.className + " " + t + " = (" + val + "); " + side + " " + t + "; })";
                }
            } else if (isSmartPtrClass(argCls)) {
                // A FRESH smart-ptr rvalue (factory/`new` result) — consumed in place, no source.
                if (handoff)
                    unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call "
                                "result needs no marker", srcLine);
                s += val;
            } else if (collArg && handoff) {
                unsupported("passing a collection by value is not yet supported — pass it by `ref` to borrow", srcLine);
            } else if (isMoveOnlyValue(argCls) && isNamedValue(argExpr.get())) {
                // M26f-2/f-4: a `resource` VALUE passed by value goes to the callee, which drops it at
                // fn-end. A `Copyable` type demands the marker: `copy` passes a fresh deep copy (the
                // source survives), `give`/bare move (mark the source moved), bare-on-copyable errors.
                if (handoff == 2) {
                    if (!isCopyable(argCls))
                        unsupported(("`" + argCls + "` has no `copy` method — add one to opt into `Copyable`, "
                                     "or use `give` to move it").c_str(), srcLine);
                    s += argCls + "__copy(&(" + val + "))";
                } else {
                    if (handoff == 0 && isCopyable(argCls))
                        unsupported(("`" + argCls + "` is copyable — a bare hand-off is ambiguous; say `give` "
                                     "(move) or `copy` (duplicate)").c_str(), srcLine);
                    std::string mv = moveOnlySource(argExpr, srcLine);
                    if (!mv.empty()) markMoved(mv);
                    s += val;
                }
            } else {
                if (handoff == 1)
                    unsupported("`give` applies to an owned value (a smart pointer or collection) — "
                                "a plain value just copies", srcLine);
                s += val;   // plain value copy (primitive / pod / collection borrow)
            }
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

// M26h — a field's visibility. A `value` picks it per field (default private, `public` allowed,
// `protected` rejected — protected belongs to an extensible resource). A `resource` field is always
// private (ownership encapsulated). An extern struct is public (FFI owns its layout). A legacy
// `class`/`pod` keeps the kind-driven all-or-nothing rule (pod public, else private).
Visibility CEmitter::fieldVisibility(const ClassInfo& ci, SharedModifierList mods, int line)
{
    if (ci.isExternStruct) return Visibility::Public;
    if (ci.kind == TypeKind::Value) {
        if (modHas(mods, "protected"))
            unsupported("a `value` field can't be `protected` — protected belongs to an extensible `resource`", line);
        return visibilityOf(mods, Visibility::Private, line);
    }
    if (modHas(mods, "public") || modHas(mods, "protected") || modHas(mods, "private")) {
        if (ci.kind == TypeKind::Resource)
            unsupported("a `resource` field is always private — expose behavior through methods", line);
        else
            unsupported("a field takes no visibility modifier — data exposure is the class kind "
                        "(`pod class` = public, otherwise private); expose data with an accessor method", line);
    }
    return Visibility::Private;   // a resource field is private
}

// Is a member (declared on `owner`, visibility `vis`) accessible from the current
// emission context (`_currentClass`; null = external/free function)? Compile error if not.
bool CEmitter::canAccess(ClassInfo* owner, Visibility vis, const std::string& member, int line)
{
    if (vis == Visibility::Public || !owner) return true;
    if (vis == Visibility::Protected) {                 // owner or any subclass of owner
        for (ClassInfo* c = _currentClass; c; c = c->base) if (c == owner) return true;
    } else {                                            // Private — owner itself, or a friend grant
        if (_currentClass == owner) return true;
        // M25c: an owner-granted `friend` may touch the named members. The accessing
        // context is the current class (any of its methods) or the current function/method.
        for (auto& g : owner->friendGrants) {
            if (!g.members.empty() && !g.members.count(member)) continue;   // empty => all privates
            if (g.accessorIsClass) { if (_currentClass && _currentClass->name == g.accessor) return true; }
            else                   { if (!_currentFunc.empty() && _currentFunc == g.accessor) return true; }
        }
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

// M25c — resolve each class's raw `friend` grants to match keys, once every unit's
// functions/classes are registered. An accessor is a class (matched vs _currentClass),
// a free function, or a `Class::method` (both matched vs _currentFunc's C-name).
void CEmitter::resolveFriends()
{
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.friendGrantsRaw.empty()) continue;
        _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings;
        for (auto& rg : ci.friendGrantsRaw) {
            FriendGrant g; g.members = rg.members;
            SharedIdentifier acc = rg.accessor;
            std::string     val  = (acc && acc->value) ? *acc->value : "";
            SharedStringList qual = acc ? acc->qualifier : nullptr;
            bool resolved = false;

            if (qual && !qual->empty()) {
                // `Class::method` — the qualifier names a class, `val` is its method.
                std::string clsName = resolveUserName(*qual->back(), nullptr);
                auto cit = _classes.find(clsName);
                if (cit != _classes.end() && cit->second.methods.count(val)) {
                    g.accessor = cit->second.methods[val].cName; g.accessorIsClass = false; resolved = true;
                }
                if (!resolved) {                                   // `Ns::func` — namespaced free function
                    std::string fk = resolveFunc(val, qual);
                    if (_funcs.count(fk)) { g.accessor = _funcs[fk].cName; g.accessorIsClass = false; resolved = true; }
                }
            } else {
                std::string clsName = resolveUserName(val, nullptr);   // a class
                if (_classes.count(clsName)) { g.accessor = clsName; g.accessorIsClass = true; resolved = true; }
                if (!resolved) {                                       // a free function
                    std::string fk = resolveFunc(val, nullptr);
                    if (_funcs.count(fk)) { g.accessor = _funcs[fk].cName; g.accessorIsClass = false; resolved = true; }
                }
            }
            if (!resolved) { unsupported(("unknown `friend` accessor '" + val + "' in '" + ci.name + "'").c_str(), rg.line); continue; }

            // Granted members must exist and be private (a grant on a public member, or a
            // typo'd name, is a mistake — keep grants honest and greppable).
            for (auto& m : g.members) {
                Visibility v = Visibility::Public; bool found = false;
                for (auto& f : ci.fields) if (f.name == m) { v = f.visibility; found = true; break; }
                if (!found) { auto mit = ci.methods.find(m); if (mit != ci.methods.end()) { v = mit->second.visibility; found = true; } }
                if (!found)
                    unsupported(("`friend` grant names unknown member '" + m + "' in '" + ci.name + "'").c_str(), rg.line);
                else if (v == Visibility::Public)
                    unsupported(("`friend` grant on public member '" + m + "' in '" + ci.name + "' is redundant").c_str(), rg.line);
            }
            ci.friendGrants.push_back(g);
        }
    }
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

    // M27a: a call to a generic function was resolved to a concrete instantiation at discovery.
    // Route it to that specialized C name; reorder named args off the template's param list.
    {
        auto ci = _callInst.find(call);
        if (ci != _callInst.end()) {
            const GenericInst& gi = _genericInsts[ci->second];
            return emitReorderedCall(gi.mangledName, "", _funcs[gi.templateKey].params, call->args, call->line);
        }
    }

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
            // M26b: a `ref`/`const ref` parameter may not name a smart pointer — you borrow
            // the OBJECT (`ref T`), or transfer ownership by value (`give`/`copy`). Borrowing
            // the handle never makes sense (and would make `ref p` ambiguous). `out` producing
            // a handle (tryUpgrade / factory-out) stays legal.
            if (paramByRef(p.get()) && p->modifier && p->modifier->value && *p->modifier->value == "ref"
                && isSmartPtrClass(cType(p->type)))
                unsupported(("a `ref` parameter may not name a smart pointer ('" + cType(p->type)
                             + "') — borrow the object with `ref " + _classes[cType(p->type)].collElemClass
                             + "`, or transfer ownership by value (`give`/`copy`)").c_str(), p->line);
            s += std::string(constPtr ? "const " : "") + cType(p->type)
               + (paramByRef(p.get()) ? "* " : " ") + nm;
        }
    }
    if (s.empty()) s = "void";
    return s;
}

void CEmitter::emitFunctionPrototype(FunctionDeclarationNode* fn, const std::string* nameOverride)
{
    bool isEntry = false;
    std::string name = nameOverride ? *nameOverride : mangledFunctionName(fn, isEntry);
    rejectStoredInterface(fn->returnType, "returned from a function", fn->line);
    *_out << (nameOverride ? "static " : "") << cType(fn->returnType) << " " << name
          << "(" << paramListC(fn->parameters, nullptr) << ");\n";
}

void CEmitter::emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride)
{
    bool isEntry = false;
    std::string name = nameOverride ? *nameOverride : mangledFunctionName(fn, isEntry);

    // Track by-ref params (deref on read) and param classes (for member calls).
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _moveState.clear();   // M26f-2: per-function move analysis
    _pendingParamDtors.clear();
    _currentClass = nullptr;
    _currentFunc  = name;   // M25c — a free function may be a `friend` accessor
    if (fn->parameters) {
        for (auto& p : *fn->parameters) {
            if (!p->identifier || !p->identifier->value) continue;
            const std::string& pn = *p->identifier->value;
            if (paramByRef(p.get())) _refParams.insert(pn);
            if (p->isConst) _constLocals.insert(pn);   // M24c: const param is immutable
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = (isClass(pty) || isInterface(pty) || isSigType(pty)) ? pty : "";   // record (incl. fnptr params)
            // M26d: a by-value smart-ptr param is OWNED by the callee — drop it at fn-end.
            // The function-root scope is created later (emitBlockScoped); stash it there.
            if (!paramByRef(p.get()) && (isSmartPtrClass(pty) || isMoveOnlyValue(pty))) {
                _pendingParamDtors.push_back({pn, pty});
                if (isMoveOnlyValue(pty)) _moveState[pn] = MoveState::NotMoved;   // M26f-2: track move-only param
            }
        }
    }

    _currentReturnCType = cType(fn->returnType);
    _tempCounter = 0;
    _scopes.clear();

    line(fn->line);
    *_out << (nameOverride ? "static " : "") << cType(fn->returnType) << " " << name
          << "(" << paramListC(fn->parameters, nullptr) << ")\n";

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
        rejectStoredInterface(f.type, "stored in a field", f.type ? f.type->line : (ci.node ? ci.node->line : 0));
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
std::string CEmitter::ifaceSlotSig(SharedParameterList params)
{
    std::string sig = "(void* self";
    if (params)
        for (auto& p : *params) {
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
        *_out << cType(m.returnType) << " (*" << m.name << ")" << ifaceSlotSig(m.params) << ";\n";
    }
    // M26g: a virtual-destructor slot so an OWNED interface (`Owned`/`Shared<I>`) can drop its
    // concrete object polymorphically. NULL for a non-destructible impl (drop just frees the obj).
    indent(1); *_out << "void (*__dtor)(void*);\n";
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
            *_out << "." << m.name << " = (" << cType(m.returnType) << "(*)" << ifaceSlotSig(m.params)
                 << ")&" << mi->cName << ",\n";
        }
        // M26g: the virtual-destructor slot — the concrete dtor (cast to the erased signature),
        // or NULL when this impl owns nothing to free.
        indent(1);
        if (ci.destructible) *_out << ".__dtor = (void(*)(void*))&" << ci.name << "__dtor,\n";
        else                 *_out << ".__dtor = (void(*)(void*))0,\n";
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
        std::vector<ParamSig> params = paramSigsOf(m.params);
        return emitReorderedCall("(" + fatExpr + ").vtbl->" + method, "(" + fatExpr + ").obj",
                                 params, args, srcLine);
    }
    unsupported("unknown interface method", srcLine);
    return "0";
}

void CEmitter::emitClassPrototypes(ClassInfo& ci)
{
    if (ci.isCollection || ci.isExternStruct) return;   // macro / header provides these
    const char* stat = _emitStaticClass ? "static inline " : "";   // M27b: specialized instances are header-static inline
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator)
        *_out << stat << "void " << ci.name << "__ctor("
             << paramListC(ci.ctorNode->declarator->params, ci.name.c_str()) << ");\n";
    else if (ci.synthCtor)                                // M19: synthesized default ctor
        *_out << stat << "void " << ci.name << "__ctor(" << paramListC(nullptr, ci.name.c_str()) << ");\n";
    if (ci.destructible)
        *_out << stat << "void " << ci.name << "__dtor(" << ci.name << "* self);\n";
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        if (mi.isAbstract) continue;   // pure: no definition, no prototype
        rejectStoredInterface(mi.returnType, "returned from a method",
                              mi.node ? mi.node->line : (ci.node ? ci.node->line : 0));
        *_out << stat << cType(mi.returnType) << " " << mi.cName << "("
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

    *_out << (_emitStaticClass ? "static inline " : "") << "void " << ci.name << "__dtor(" << ci.name << "* self)\n{\n";

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
    _currentFunc  = cName;   // M25c — a method may be a `Class::method` friend accessor
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _moveState.clear();   // M26f-2: per-method move analysis
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
            // M26d: a by-value smart-ptr param is owned by the callee — drop it at fn-end.
            // The root scope is already on the stack, so record it directly (dropped last).
            if (!paramByRef(p.get()) && (isSmartPtrClass(pty) || isMoveOnlyValue(pty))) recordDestructibleLocal(pn, pty);
        }
    }

    *_out << (_emitStaticClass ? "static inline " : "") << retType << " " << cName
          << "(" << paramListC(params, owner.name.c_str()) << ")\n{\n";

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
            } else {
                // A collection / smart-pointer field with no initializer must start as a
                // valid EMPTY value (zero = NULL buffer / null handle), or its first use
                // (`.add(...)`) and its RAII drop would touch garbage. (Plain class-value
                // fields still need their own ctor — a separate, deferred gap.)
                std::string fct = cType(f.type);
                if (_classes.count(fct) && _classes[fct].isCollection) {
                    indent(1);
                    *_out << "self->" << f.name << " = (" << fct << "){0};\n";
                }
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

// M27b: emit one specialized generic-type instance under its binding. phase 0 = struct typedef+body,
// 1 = ctor/dtor/method prototypes, 2 = bodies. Mirrors emitGenericInst (M27a): all specialized class
// functions are header-`static` (every module includes the header), so `_emitStaticClass` is set here.
void CEmitter::emitGenericTypeInst(const GenericTypeInst& gi, int phase)
{
    auto cit = _classes.find(gi.mangledName);
    if (cit == _classes.end()) return;
    ClassInfo& ci = cit->second;
    NsCtx savedCtx = _nsCtx;
    _nsCtx = _genericTypeCtx[gi.templateKey];
    _typeSubst.clear();
    const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
    for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
    _emitStaticClass = true;
    if      (phase == 0) { *_out << "typedef struct " << ci.name << " " << ci.name << ";\n"; emitStruct(ci); }
    else if (phase == 1) emitClassPrototypes(ci);
    else                 emitClassDefinitions(ci);
    _emitStaticClass = false;
    _typeSubst.clear();
    _nsCtx = savedCtx;
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

    // M26i: `list[i]` / `a[i]` resolves to the ELEMENT type, so `list[i].m()` finds the method.
    // Pure resolution (no emission) — mirrors the front of collectionElemAccess.
    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        SharedExpression recv = ea->expression ? ea->expression
                                               : std::static_pointer_cast<ExpressionNode>(ea->identifier);
        std::string cls = exprClass(recv);
        if (!cls.empty() && _classes.count(cls) && _classes[cls].isCollection)
            return _classes[cls].collElemClass;
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
    // A Weak can't be dereffed — it must be upgraded with upgrade() first.
    if (isSmartPtrClass(cls)) {
        if (smartKind(cls) == CollKind::Weak) {
            unsupported("cannot access a field through Weak<T>; call .upgrade()", ma->line);
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
    std::string recvPtr;
    if (auto* ea = dynamic_cast<ElementAccessNode*>(receiver.get())) {
        // M26i: `list[i].m()` — borrow the element IN PLACE via the bounds-checked `__at`
        // (a T* into the buffer). No copy, no temp; a plain nested call, strictly ISO C.
        std::string coll, recvExpr, idx;
        if (collectionElemAccess(ea, coll, recvExpr, idx))
            recvPtr = coll + "__at(&(" + recvExpr + "), " + idx + ")";
        else
            recvPtr = "&(" + emitExpression(receiver) + ")";
    } else {
        recvPtr = dynamic_cast<ThisAccessNode*>(receiver.get())
                ? std::string("self")
                : "&(" + emitExpression(receiver) + ")";
    }
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
                    // M26h: `type contract` pre-registers as an interface name, not a class.
                    if (cd->typeKind && *cd->typeKind == "contract") {
                        std::string n = qualify(*cd->name->value); _interfaces[n].name = n;
                    } else if (cd->typeParams && !cd->typeParams->empty()) {
                        // M27b: a generic TYPE template pre-registers in _genericTypes, NOT _classes
                        // (an empty _classes entry would be emitted as a bogus struct). collectClasses fills it.
                        std::string n = qualify(*cd->name->value); _genericTypes[n].name = n;
                    } else {
                        bool ext = false;
                        if (cd->modifiers) for (auto& mod : *cd->modifiers)
                            if (mod->value && *mod->value == "extern") ext = true;
                        std::string n = ext ? *cd->name->value : qualify(*cd->name->value);
                        _classes[n].name = n;
                        if (ext) { _classes[n].isExternStruct = true; _externNames.insert(n); }
                    }
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
    resolveFriends();   // M25c — after all classes/functions are registered
    // M26f-5: register collections BEFORE the destructibility fixpoint, so a class whose only
    // owning member is a collection field (`List<T>` etc., no explicit `~dtor`) is correctly seen
    // as a resource (destructible + move-only). computeDestructible then re-derives each
    // collection's elemDestructible from the final class destructibility.
    for (auto& u : units)
        if (u && u->codeDeclarationList) { _nsCtx = _unitCtx[u.get()]; collectCollections(u); }
    // M27a: discover generic-function instantiations after collections (a specialization may use
    // one) and before the destructibility fixpoint. Runs with _typeSubst empty (concrete mangles).
    for (auto& u : units)
        if (u && u->codeDeclarationList) { _nsCtx = _unitCtx[u.get()]; collectGenericInsts(u); }
    computeDestructible();
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

    // Collection/smart-pointer STRUCT typedefs (the `_TYPE` half) — before class struct
    // bodies, so a class may hold a collection/smart-pointer BY VALUE as a field. They
    // store only `T*`, so the element being forward-declared (above) is enough.
    emitCollectionDefs(/*typesOnly=*/true);

    // M27b: specialized generic-type instances — struct typedef + body BEFORE the normal class
    // struct bodies, so a normal class may hold a `Box<int32>` BY VALUE (complete type needed).
    // Registration order is inner-first (transitive close registers a held instance before its holder).
    for (const std::string& m : _genericTypeInstOrder)
        emitGenericTypeInst(_genericTypeInsts[m], /*phase=*/0);

    // vtable struct types + struct bodies (topological), then interface types.
    for (ClassInfo* ci : classes) {
        if (ci->isCollection || ci->isExternStruct) continue;   // macro / header provides it
        scopeOf(ci->scope, ci->usings);
        if (ci->hasVtable && ci->vtableRoot == ci->name) emitVtableType(*ci);
        emitStruct(*ci);
    }
    for (auto& kv : _interfaces) { scopeOf(kv.second.scope, kv.second.usings); emitInterfaceTypes(kv.second); }

    // Element destructor prototypes the collection/smart-pointer macros call, then the
    // macros themselves, then class prototypes — so a method (M26d: or any class member)
    // can pass a collection/smart-pointer wrapper BY VALUE in its signature, the wrapper
    // type being complete by then. The dtor protos are re-declared (identically, harmless)
    // by emitClassPrototypes. Free-function prototypes follow (they may use a wrapper too).
    for (ClassInfo* ci : classes)
        if (!ci->isCollection && !ci->isExternStruct && ci->destructible)
            *_out << "void " << ci->name << "__dtor(" << ci->name << "* self);\n";
    // M26f-5: a collection of `Copyable` elements deep-copies via the element's `copy()`, so its
    // prototype must precede the `_FUNCS` macro that calls it (re-declared identically by
    // emitClassPrototypes). The C signature is `Elem Elem__copy(Elem* self)` (nullary; paramListC).
    for (ClassInfo* ci : classes)
        if (!ci->isCollection && !ci->isExternStruct && ci->copyable)
            *_out << ci->name << " " << ci->name << "__copy(" << ci->name << "* self);\n";
    emitCollectionDefs(/*typesOnly=*/false);   // the `_FUNCS` half (ctor/dtor/methods)
    for (ClassInfo* ci : classes) { scopeOf(ci->scope, ci->usings); emitClassPrototypes(*ci); }
    // M27b: specialized generic-type instance prototypes (ctor/dtor/method), `static`.
    for (const std::string& m : _genericTypeInstOrder)
        emitGenericTypeInst(_genericTypeInsts[m], /*phase=*/1);
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
                if (fn->typeParams && !fn->typeParams->empty()) continue;   // M27a: template — instantiated below
                emitFunctionPrototype(fn);
                any = true;
            }
    }
    if (any) *_out << "\n";

    // M27a: generic-function instantiations — one `static` C function per (template, type-args),
    // in the header so every module can call them (like the collection macros). Forward-declare
    // all, then define, so a generic that calls another (or recurses) resolves.
    if (!_genericInsts.empty()) {
        for (auto& kv : _genericInsts) emitGenericInst(kv.second, /*prototypeOnly=*/true);
        *_out << "\n";
        for (auto& kv : _genericInsts) emitGenericInst(kv.second, /*prototypeOnly=*/false);
    }

    // M27b: specialized generic-type instance BODIES (ctor/method/dtor), `static`, in the header.
    for (const std::string& m : _genericTypeInstOrder)
        emitGenericTypeInst(_genericTypeInsts[m], /*phase=*/2);
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
            if (fn->typeParams && !fn->typeParams->empty()) continue;   // M27a: template — instantiations live in the header
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
