#include "kama.cemit.h"
#include "kama.ast.h"
#include "kama.context.h"    // CodeGenContext — to synthesize primitive type nodes
#include "kama.parser.hpp"   // bison token constants (PLUS, STAR, EQEQ, ...)

#include <cstdio>
#include <sstream>
#include <functional>
#include <cctype>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static bool isStableStringRef(ASTNode* n);   // defined below; used by string-index materialization

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
    if (srcLine > 0) _curLine = srcLine;   // track for conditional-drop diagnostics
    if (_lines && srcLine > 0) {
        // The path is a C string literal: escape `\` and `"` so a Windows path (`D:\a\…\tests\foo.kama`)
        // isn't read as escape sequences (`\o`/`\x`/… are errors, not just the file name we meant).
        *_out << "#line " << srcLine << " \"";
        for (char c : _sourcePath) { if (c == '\\' || c == '"') *_out << '\\'; *_out << c; }
        *_out << "\"\n";
    }
}

void CEmitter::unsupported(const char* what, int srcLine)
{
    ++_unsupported;
    std::fprintf(stderr, "kama: warning: unsupported %s at %s:%d (not yet lowered)\n",
                 what, _sourcePath.c_str(), srcLine);
    *_out << "/* TODO(kama): unsupported " << what << " */";
}

// ---------------------------------------------------------------------------
// Namespaces: scope prefixes + name resolution
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
    // Imports (`::`-path already resolved to source by the driver). Populate scope bindings:
    //   import a::b;            -- nothing (qualified-only, a::b::X)
    //   import a::b as m;       -- module alias  -> aliases[m] = a__b, so m::X resolves
    //   import a::b::{X,Y as Z} -- per-symbol    -> symbolAliases[X]=a__b__X, [Z]=a__b__Y (unqualified)
    if (unit->importDeclarationList)
        for (auto& imp : *unit->importDeclarationList) {
            if (!imp || !imp->modulePath || imp->modulePath->empty()) continue;
            std::string path;                                  // dotted, for mangleNs
            for (auto& s : *imp->modulePath) path += (path.empty() ? "" : ".") + *s;
            std::string mod = mangleNs(path);                  // "a__b"
            if (imp->moduleAlias && !imp->moduleAlias->empty()) {
                ctx.aliases[*imp->moduleAlias] = mod;
            } else if (imp->symbols) {
                for (auto& sym : *imp->symbols) {
                    if (!sym || !sym->identifier || !sym->identifier->value) continue;
                    std::string local = (sym->alias && sym->alias->value) ? *sym->alias->value
                                                                          : *sym->identifier->value;
                    std::string target = mod + "__" + *sym->identifier->value;
                    auto it = ctx.symbolAliases.find(local);
                    if (it != ctx.symbolAliases.end() && it->second != target)
                        unsupported(("import of `" + local + "` collides with another import — disambiguate with `as`").c_str(), imp->line);
                    ctx.symbolAliases[local] = target;
                }
            }
        }
    // The smart-pointer triad is a built-in module (std::memory), always in scope — add an implicit
    // `using` so bare `Owned`/`Shared`/`Weak` resolve everywhere with no `import std::memory`. Consulted
    // AFTER an explicit import's symbolAliases and the unit's own scope (see resolveUserName/resolveFunc),
    // so it never shadows a user's own name and a redundant explicit import stays a harmless no-op.
    ctx.usings.push_back("std__memory");
    return ctx;
}

// Scope-prefix a declared name (registration). `main` stays the global entry.
std::string CEmitter::qualify(const std::string& name) const
{
    if (name == "main") return "kama_main";
    if (_nsCtx.scope.empty()) return name;   // the prelude's global namespace -> bare names
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
            || _genericTypes.count(n)       // a generic-type template resolves to its scoped name too
            || _genericContracts.count(n);  // as does a generic-contract template (`Iterator<T>`)
    };
    // FFI: extern struct/handle names are global literal C names.
    if ((!qualifier || qualifier->empty()) && _externNames.count(value)) return value;
    // A per-symbol import binds a bare local name to a fully-mangled global (`import a::b::{X as Y}`).
    if (!qualifier || qualifier->empty()) {
        auto sa = _nsCtx.symbolAliases.find(value);
        if (sa != _nsCtx.symbolAliases.end() && known(sa->second)) return sa->second;
    }
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
    if (name == "main") return "kama_main";
    if (!qualifier || qualifier->empty()) {
        auto sa = _nsCtx.symbolAliases.find(name);   // per-symbol `import a::b::{fn as g}`
        if (sa != _nsCtx.symbolAliases.end() && _funcs.count(sa->second)) return sa->second;
    }
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
    // inside a generic instantiation, a bare type-param name (`T`, no <...> of its own)
    // resolves to the concrete type it was bound to. Guarded on !genericArg so a real `List<T>`
    // still flows to the collection arm (whose element then hits this same substitution).
    if (!_typeSubst.empty() && type->value && !type->genericArg) {
        auto s = _typeSubst.find(*type->value);
        if (s != _typeSubst.end()) return cType(s->second);
    }
    // `This` (the self-type) resolves to the enclosing type — the concrete class while emitting a
    // class body/prototype/struct (_thisType), or the interface type inside its vtbl slot. In a bounded
    // generic the receiver's concrete class carries `This` (resolved when that class was emitted), so no
    // binding is needed at the call site. `This` outside a type/contract is a clean error.
    if (type->value && *type->value == "This" && !type->genericArg) {
        if (!_thisType.empty()) return _thisType;
        if (_currentClass)      return _currentClass->name;
        unsupported("`This` (the self-type) is only valid inside a `type` or `contract`", type->line);
        return "void";
    }
    // FFI: a raw C pointer carrier (opaque). Bare `Ptr` -> void* (the
    // universal handle / opaque pointer); `Ptr<T>` -> T*. usize/isize map to the
    // C size types. These are the explicit, extern-marked unsafe boundary.
    if (type->value && *type->value == "Ptr")
        return type->genericArg ? (cType(type->genericArg) + "*") : "void*";
    if (type->value && !type->genericArg) {
        if (*type->value == "usize") return "size_t";
        if (*type->value == "isize") return "ptrdiff_t";
    }

    // Fixed<T, N> spells `Fixed_<mangleT>_<N>` (two args — the const size mangles to its value).
    if (type->genericArg && type->value && *type->value == "Fixed" && type->genericArgs) {
        std::string m = "Fixed";
        for (auto& a : *type->genericArgs) m += "_" + mangleElem(a);
        return m;
    }
    // `BindableFunctionPtr<Sig>` spells its mangled struct name. (List AND Array are library generic
    // types now — std::collections — and smart pointers likewise; they flow through the generic-type
    // arm below.)
    if (type->genericArg && type->value && *type->value == "BindableFunctionPtr")
        return *type->value + "_" + mangleElem(type->genericArg);
    // a user generic TYPE (`Box<int32>`) spells its specialized struct name (`Box_int32`).
    // Reached only for a non-reserved name with a type arg; under _typeSubst the arg's `T` resolves.
    if (type->genericArg && type->value) {   // genericArg mirrors genericArgs[0] (non-null iff there are args)
        std::string tmpl = resolveUserName(*type->value, type->qualifier);
        // a generic TYPE (`Box<int32>`) or a generic CONTRACT (`Iterator<int32>`) both spell their
        // specialized name; the contract instance lives in _interfaces after discovery.
        if (_genericTypes.count(tmpl) || _genericContracts.count(tmpl))
            return genericTypeMangle(tmpl, type->genericArgs);
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
        case IDENTIFIER_STRING_VAL:  return "kama_string";
        case IDENTIFIER_VOID_VAL:    return "void";
        case IDENTIFIER_CHAR_VAL:    return "uint32_t";   // `char` = a Unicode scalar value (codepoint)
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

// map an overloadable operator token + arity-class to a stable C-safe method name.
// arity 0 => a UNARY operator on `this`; arity >= 1 => a BINARY operator (method or free form,
// same name). Returns "" when the operator has no form for that arity (e.g. unary `*`, binary `!`).
std::string CEmitter::operatorMangle(int opToken, int arity)
{
    bool unary = (arity == 0);
    switch (opToken) {
        case PLUS:        return unary ? "op_pos"  : "op_add";
        case MINUS:       return unary ? "op_neg"  : "op_sub";
        case STAR:        return unary ? ""        : "op_mul";
        case SLASH:       return unary ? ""        : "op_div";
        case PERCENT:     return unary ? ""        : "op_mod";
        case EQEQ:        return unary ? ""        : "op_eq";
        case NOTEQ:       return unary ? ""        : "op_ne";
        case LT:          return unary ? ""        : "op_lt";
        case GT:          return unary ? ""        : "op_gt";
        case LEQ:         return unary ? ""        : "op_le";
        case GEQ:         return unary ? ""        : "op_ge";
        case AMP:         return unary ? ""        : "op_band";
        case BAR:         return unary ? ""        : "op_bor";
        case CARET:       return unary ? ""        : "op_bxor";
        case LTLT:        return unary ? ""        : "op_shl";
        case GTGT:        return unary ? ""        : "op_shr";
        case EXCLAMATION: return unary ? "op_not"  : "";
        case TILDE:       return unary ? "op_bnot" : "";
        case PLUSPLUS:    return unary ? "op_inc"  : "";   // pre/post both map here (mutating in place)
        case MINUSMINUS:  return unary ? "op_dec"  : "";
        case LEFT_BRACKET: return unary ? ""       : "op_index";   // `a[i]` — method form (self + index)
        default:          return "";
    }
}

// Type-based dispatch — the full operator name = the base mangle + an operand-type suffix, so a
// type can carry several `operator*` distinguished by operand type (mat*vec vs mat*mat). Rule:
//   arity 2 (free/static form)              -> `op_<sym>__free`
//   arity 1 (method form), DIFFERENT user rhs -> `op_<sym>__<RhsClass>`
//   arity 1, same-type rhs (incl. `This`), primitive rhs, or a unary op -> bare `op_<sym>`
// The same-type/`This` case keeping the bare name is what lets a `contract` bound (`This operator+(This)`)
// match a concrete `operator+(Vec2)` structurally, with no `This`-in-name special-casing.
std::string CEmitter::operatorName(int opToken, int arity, SharedIdentifier paramType, const std::string& owner)
{
    std::string base = operatorMangle(opToken, arity);
    if (base.empty()) return "";
    if (arity == 2) return base + "__free";
    if (arity == 1 && paramType) {
        std::string pk = (paramType->value && *paramType->value == "This") ? owner : cType(paramType);
        if (!pk.empty() && pk != owner && isClass(pk)) return base + "__" + pk;   // different user type
    }
    return base;
}

// synthesize a ParameterList from an operator declarator's param1/param2 (0/1/2 params) so all
// normal method machinery (paramListC, paramSigsOf, emitMethodOrCtorBody's binding) is reused verbatim.
SharedParameterList CEmitter::operatorParamList(ClassOperatorDeclaratorNode* d)
{
    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<operator>"));
    auto list = std::make_shared<ParameterList>();
    if (d->param1Type)
        list->push_back(std::make_shared<FunctionParameterNode>(*_synthCtx, SharedModifier(), d->param1Type, d->param1Name));
    if (d->param2Type)
        list->push_back(std::make_shared<FunctionParameterNode>(*_synthCtx, SharedModifier(), d->param2Type, d->param2Name));
    return list;
}

// is `cls` a user type that may carry operator overloads (a `value`/`resource`/class, not a
// collection/smart-pointer, whose arithmetic is a real C `struct` with no built-in `+`)?
static inline bool userOperandType(const std::string& cls, std::map<std::string, ClassInfo>& classes)
{
    if (cls.empty()) return false;
    auto it = classes.find(cls);
    return it != classes.end() && !it->second.isIntrinsicColl;
}

// `&<operand>` for a method-form/unary operator's `self`. A simple lvalue (a local, a field/
// member access, `this`) is addressed directly; an rvalue (a nested operator result, a call, `a[i]`)
// is first materialized into a hoisted temp — ISO C, no statement-expressions — so `&` is legal and
// chained `a + b + c` works. In a non-hoistable slot (a raw `if`/`while` condition) a chained operand
// is a clean error rather than bad C.
// produce the `self` pointer for a method-form/unary operator. A simple lvalue (a local, a
// field/member access, `this`) is addressed directly. An rvalue (a nested operator result, a call —
// `a + b + c`, `-(a + b)`) can't be `&`'d, so it is wrapped in a C99 compound-literal array: `(V[]){e}`
// decays to `V*` and the temporary lives to the end of the enclosing block. This is ISO C11 (the same
// construct used for variants), needs no statement slot, and so works in ANY position — including
// a raw `if`/`while` condition — with no hoisting.
// the class `e` constructs if it is a bare inline constructor (`Vec3(x: …)` — not a method call
// or a `Type::variant`), else "".
std::string CEmitter::bareCtorClass(SharedExpression e)
{
    auto* iv = dynamic_cast<InvocationNode*>(e.get());
    if (!iv || iv->expression || !iv->identifier || !iv->identifier->value) return "";
    if (iv->identifier->qualifier && !iv->identifier->qualifier->empty()) return "";   // Type::variant, not a ctor
    std::string rn = resolveUserName(*iv->identifier->value, iv->identifier->qualifier);
    return (isClass(rn) && _classes.count(rn) && !_classes[rn].isIntrinsicColl) ? rn : "";
}

// an inline constructor operand, hoisted into a temp (needs a statement slot); "" otherwise.
std::string CEmitter::hoistCtorIfInline(SharedExpression e)
{
    std::string rn = bareCtorClass(e);
    return rn.empty() ? "" : tryHoistInlineCtor(e, rn, e->line);   // "" when !_hoistOK (e.g. a raw condition)
}

// a clean diagnostic for a bare inline ctor operand that has no statement slot to hoist into
// (e.g. inside a raw `if`/`while` condition) — the same boundary an inline `match`/ctor hits elsewhere.
void CEmitter::rejectUnhoistableCtor(SharedExpression e)
{
    if (!bareCtorClass(e).empty())
        unsupported("an inline constructor as an operator operand here has no statement slot — bind it to a local first", e->line);
}

// emit an operator operand by value, materializing an inline constructor into a hoisted temp.
std::string CEmitter::emitOperandByValue(SharedExpression e)
{
    std::string t = hoistCtorIfInline(e);
    if (!t.empty()) return t;
    rejectUnhoistableCtor(e);
    return emitExpression(e);
}

std::string CEmitter::addrOfOperand(SharedExpression e, const std::string& cls, int line)
{
    ASTNode* n = e.get();
    if (dynamic_cast<ThisAccessNode*>(n)) return emitExpression(e);   // `this` is already `self` (a pointer)
    std::string ct = hoistCtorIfInline(e);
    if (!ct.empty()) return "&" + ct;   // inline ctor operand → a hoisted, addressable temp
    rejectUnhoistableCtor(e);
    bool lvalue = dynamic_cast<IdentifierNode*>(n) || dynamic_cast<MemberAccessNode*>(n);
    std::string em = emitExpression(e);
    if (lvalue || cls.empty()) return "&(" + em + ")";
    return "(" + cls + "[]){ " + em + " }";   // rvalue → addressable compound-literal temporary
}

// resolve `a OP b` to an operator method by OPERAND TYPES. Priority: the method form (arity 1) on
// the LHS type — a different-user-type rhs (`op_<sym>__<Rc>`) before the same-type/scalar form
// (`op_<sym>`) — then the free form (`op_<sym>__free`) on either operand's type. Returns null if none.
MethodInfo* CEmitter::findBinaryOperator(int token, const std::string& lc, const std::string& rc, ClassInfo** ownerOut)
{
    std::string opBase = operatorMangle(token, 1);
    if (opBase.empty()) return nullptr;
    if (userOperandType(lc, _classes)) {
        std::vector<std::string> names;
        if (userOperandType(rc, _classes) && rc != lc) names.push_back(opBase + "__" + rc);   // mat * vec
        names.push_back(opBase);                                                               // same-type / scalar
        for (auto& nm : names) {
            ClassInfo* owner = nullptr;
            MethodInfo* mi = findMethod(&_classes[lc], nm, &owner);
            if (mi && mi->isOperator && mi->arity == 1) { if (ownerOut) *ownerOut = owner; return mi; }
        }
    }
    for (const std::string& cls : { lc, rc }) {   // free/static form (scalar-on-the-left, etc.)
        if (!userOperandType(cls, _classes)) continue;
        ClassInfo* owner = nullptr;
        MethodInfo* mi = findMethod(&_classes[cls], opBase + "__free", &owner);
        if (mi && mi->isOperator && mi->arity == 2) { if (ownerOut) *ownerOut = owner; return mi; }
    }
    return nullptr;
}

// a binary expression with a user-typed operand dispatches to an operator overload; a purely
// primitive expression keeps the raw-C path (so the whole numeric fixture suite is untouched). The
// method form passes `self` by pointer (an rvalue is wrapped by addrOfOperand); the free form passes
// both operands by value.
std::string CEmitter::emitBinaryOperator(int token, SharedExpression lhs, SharedExpression rhs, int line)
{
    std::string lc = exprClass(lhs);
    std::string rc = exprClass(rhs);
    bool lUser = userOperandType(lc, _classes);
    bool rUser = userOperandType(rc, _classes);
    // `string` is a primitive (kama_string), not a user-operator type, so `+`/`==`/`!=` are compiler
    // special-cases lowered to the runtime intrinsics — not overloads. Both operands must be `string`
    // (no implicit conversion; `string + number` would need a Display/to-string substrate — a later
    // roadmap item). `self` goes by pointer (an rvalue literal / nested `a+b` wraps in a compound-literal
    // temp via addrOfOperand), the other operand by value, exactly like the user-operator lowering.
    bool lStr = exprIsString(lhs), rStr = exprIsString(rhs);
    if (lStr || rStr) {
        if (!(lStr && rStr)) {
            unsupported("operator on `string` requires both operands to be `string` "
                        "(no implicit conversion — build the other side into a string first)", line);
            return "0";
        }
        // An owned-string rvalue operand is hoisted into a scope-dtor'd temp so it isn't leaked; a
        // literal/lvalue keeps the leak-free addrOfOperand/by-value path.
        auto ptrOf = [&](SharedExpression e) {
            std::string t = hoistStringTemp(e);
            return t.empty() ? addrOfOperand(e, "kama_string", line) : ("&" + t);
        };
        auto valOf = [&](SharedExpression e) {
            std::string t = hoistStringTemp(e);
            return t.empty() ? emitOperandByValue(e) : t;
        };
        if (token == PLUS)  return "kama_string__concat(" + ptrOf(lhs) + ", " + valOf(rhs) + ")";
        if (token == EQEQ)  return "kama_string__equals(" + ptrOf(lhs) + ", " + valOf(rhs) + ")";
        if (token == NOTEQ) return "(!kama_string__equals(" + ptrOf(lhs) + ", " + valOf(rhs) + "))";
        unsupported(("operator '" + binaryOperator(token) + "' is not defined for `string` "
                     "(only `+`, `==`, `!=`)").c_str(), line);
        return "0";
    }
    if (!lUser && !rUser) {
        // A signed LEFT shift into the sign bit is UB in C; route it through `kama_lshift` (shifts in the
        // matching unsigned type — defined) so there's no UB even in debug (release's `-fwrapv` also
        // defines it, but debug has none). Right shift of a signed value is impl-defined, not UB.
        if (token == LTLT)
            return "kama_lshift(" + emitExpression(lhs) + ", " + emitExpression(rhs) + ")";
        return "(" + emitExpression(lhs) + " " + binaryOperator(token) + " "
                   + emitExpression(rhs) + ")";   // primitives — unchanged
    }

    ClassInfo* owner = nullptr;
    MethodInfo* mi = findBinaryOperator(token, lc, rc, &owner);
    if (!mi) {
        unsupported(("no operator '" + binaryOperator(token) + "' for operand type '"
                     + (lUser ? lc : rc) + "' — define `operator" + binaryOperator(token) + "` on the type").c_str(), line);
        return "0";
    }
    canAccess(owner, mi->visibility, mi->cName, line);
    if (mi->arity == 1)   // method form: `A__op(&lhs, rhs)` (rvalue lhs -> compound-literal temporary)
        return mi->cName + "(" + addrOfOperand(lhs, lc, line) + ", " + emitOperandByValue(rhs) + ")";
    return mi->cName + "(" + emitOperandByValue(lhs) + ", " + emitOperandByValue(rhs) + ")";   // free form: both by value
}

// a compound-assignment token maps to its binary operator (`a += b` == `a = a + b`) for a
// user type. Returns 0 (not a valid token) for a plain `=` or an unmapped token.
int CEmitter::compoundToBinary(int token)
{
    switch (token) {
        case PLUSEQ:  return PLUS;
        case MINUSEQ: return MINUS;
        case STAREQ:  return STAR;
        case DIVEQ:   return SLASH;
        case MODEQ:   return PERCENT;
        case ANDEQ:   return AMP;
        case OREQ:    return BAR;
        case XOREQ:   return CARET;
        case LTLTEQ:  return LTLT;
        case GTGTEQ:  return GTGT;
        default:      return 0;
    }
}

// a unary / increment / decrement on a user type dispatches to a 0-param (on-`this`) operator.
// Returns "" when the operand is NOT a user type (caller keeps its raw-C path).
std::string CEmitter::emitUnaryUserOp(int opToken, SharedExpression operand, int line)
{
    std::string oc = exprClass(operand);
    if (!userOperandType(oc, _classes)) return "";
    std::string opName = operatorMangle(opToken, 0);   // op_neg / op_not / op_bnot / op_pos / op_inc / op_dec
    MethodInfo* mi = opName.empty() ? nullptr : findMethod(&_classes[oc], opName, nullptr);
    if (!mi || !mi->isOperator) {
        unsupported(("no unary operator for type '" + oc + "' — define the matching `operator` on the type").c_str(), line);
        return "0";
    }
    canAccess(&_classes[oc], mi->visibility, opName, line);
    return mi->cName + "(" + addrOfOperand(operand, oc, line) + ")";   // rvalue operand → hoisted temp
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

std::string CEmitter::emitExpression(SharedExpression expr)
{
    if (!expr) return "";
    ASTNode* n = expr.get();

    if (auto* mm = dynamic_cast<MatchNode*>(n)) return emitMatch(mm);   // value-producing match (lifted)
    if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) return emitArrayLiteral(al);   // `[…]` -> a Fixed value

    if (auto* v = dynamic_cast<Int8Node*>(n))   return std::to_string((int)v->value);
    if (auto* v = dynamic_cast<Int16Node*>(n))  return std::to_string((int)v->value);
    if (auto* v = dynamic_cast<Int32Node*>(n))  return std::to_string(v->value);
    if (auto* v = dynamic_cast<Int64Node*>(n))  return std::to_string((long long)v->value) + "LL";
    if (auto* v = dynamic_cast<UInt8Node*>(n))  return std::to_string((unsigned)v->value) + "U";
    if (auto* v = dynamic_cast<UInt16Node*>(n)) return std::to_string((unsigned)v->value) + "U";
    if (auto* v = dynamic_cast<UInt32Node*>(n)) return std::to_string(v->value) + "U";
    if (auto* v = dynamic_cast<CharNode*>(n))   return std::to_string(v->value) + "U";   // codepoint literal
    if (auto* v = dynamic_cast<UInt64Node*>(n)) return std::to_string((unsigned long long)v->value) + "ULL";

    if (auto* v = dynamic_cast<Float64Node*>(n)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v->value);
        return buf;
    }
    if (auto* v = dynamic_cast<Float32Node*>(n)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.9g", (double)v->value);
        std::string s(buf);
        // A whole-number %g (`16`) needs a decimal point before the `f` suffix — `16f` is an INVALID C
        // literal (integer with a float suffix). Make it `16.0f`. (`2.5`/`1e9` already have `.`/`e`.)
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
            && s.find('E') == std::string::npos && s.find_first_of("0123456789") != std::string::npos)
            s += ".0";
        return s + "f";
    }
    if (auto* v = dynamic_cast<BooleanNode*>(n)) return v->value ? "true" : "false";
    if (dynamic_cast<NullNode*>(n))              return "NULL";

    if (auto* h = dynamic_cast<HandoffNode*>(n)) {
        // `give x` / `copy x`. The move (invalidate source) / retain side effects
        // need a statement context — handled where a value is HANDED OFF: an initializer,
        // assignment, argument, or return (emitDeclarator / the assignment arm / emitReorderedCall
        // / the return arm all unwrap the marker). Reaching here means the marker rides a bare
        // sub-expression (e.g. `give x` used as a statement or inside a larger expression), which
        // isn't a hand-off position — only a plain-value `copy` (a value copy) would be complete.
        std::string ic = exprClass(h->value);
        // A marker on an OWNED value (smart-ptr / collection / any destructible resource) in a bare
        // sub-expression is a position error. On a plain value it's harmless — `give`/`copy` of a value
        // is just that value (a copy), so yield it.
        bool owned = isSmartPtrClass(ic)
                   || (!ic.empty() && _classes.count(ic) && (_classes[ic].isIntrinsicColl || _classes[ic].destructible));
        if (owned)
            unsupported("`give`/`copy` mark a value being handed off — an initializer, assignment, argument, "
                        "or return — not a bare sub-expression", h->line);
        return emitExpression(h->value);
    }

    if (auto* v = dynamic_cast<StringNode*>(n)) {
        // Lower to a borrowed runtime string. The lexer already produced the
        // raw bytes; emit them as a C string literal (escaping is a later pass).
        const std::string& s = v->value ? *v->value : std::string();
        std::ostringstream os;
        os << "kama_string_lit(\"";
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
            // `Union::Variant` with no payload -> `(Union){ .tag = Union_Variant }`
            // (`Optional<int32>::None` resolves the instance via the target-type context).
            if (ClassInfo* vt = resolveVariantType(en))
                for (auto& vc : vt->variants)
                    if (vc.name == nm)
                        return emitVariantConstruction(*vt, nm, SharedArgumentList(), v->line);
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
                    if (_inStaticMethod) unsupported("a `static` method has no `this` — access the field through an instance", v->line);
                    checkFieldAccess(owner, head, v->line);
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
            if (owner) {
                if (_inStaticMethod) unsupported("a `static` method has no `this` — access the field through an instance", v->line);
                checkFieldAccess(owner, nm, v->line); return "self->" + basePathTo(_currentClass, owner) + nm;
            }
        }
        // A bare **function name** used as a value (not a call) → its C function
        // pointer — enables binding/passing a free function to a FunctionPtr.
        if (!_localTypes.count(nm)) {
            auto fit = _funcs.find(resolveFunc(nm, v->qualifier));
            if (fit != _funcs.end()) return fit->second.cName;
        }
        checkNotMoved(nm, v->line);   // reject reading a moved-from `resource` value
        return nm;
    }

    if (auto* tn = dynamic_cast<ThisAccessNode*>(n)) {
        if (_inStaticMethod) unsupported("a `static` method has no `this`", tn->line);
        return "self";
    }

    if (auto* v = dynamic_cast<MemberAccessNode*>(n)) {
        return emitMemberAccess(v);
    }

    if (auto* ba = dynamic_cast<BaseAccessNode*>(n)) {
        // base.field (bare; base.method(...) is handled in emitInvocation).
        std::string name = (ba->identifier && ba->identifier->value) ? *ba->identifier->value : "";
        if (_currentClass && _currentClass->base) {
            ClassInfo* owner = findFieldOwner(_currentClass->base, name);
            if (owner) { checkFieldAccess(owner, name, ba->line); return "self->__base." + basePathTo(_currentClass->base, owner) + name; }
        }
        unsupported("base access", ba->line);
        return name;
    }

    if (auto* v = dynamic_cast<ObjectCreationNode*>(n)) {
        // `new T(...)` is supported only as a local-variable initializer
        // (handled in LocalVariableDeclaration). Bare expression position needs
        // a temp/statement context that arrives with RAII.
        unsupported("`new` outside a local-variable initializer", v->line);
        return "0";
    }

    if (auto* v = dynamic_cast<BinaryExpressionNode*>(n)) {
        // GOALS §3b: `== null` / `!= null` on a safe type is a compile error — a value,
        // smart pointer, or contract is never null (the C habit checks the wrong thing here).
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
        return emitBinaryOperator(v->token, v->LHS, v->RHS, v->line);   // user operand → dispatch, else raw C
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
        checkConstWrite(v->unaryExpression, v->line);   // no write to/through const
        // Indexed assignment to a collection lowers to __set, not `lhs = rhs`.
        if (auto* ea = dynamic_cast<ElementAccessNode*>(v->unaryExpression.get())) {
            std::string coll, recvExpr, idx;
            if (collectionElemAccess(ea, coll, recvExpr, idx)) {
                std::string rhs = emitExpression(v->expression);
                if (v->token == EQ)
                    return coll + "__set(&(" + recvExpr + "), " + idx + ", " + rhs + ")";
                // Compound (a[i] += x): mutate the element in place through the bounds-checked `*__at`
                // place — the index is evaluated ONCE (no double `__get`+`__set`), and a nested
                // `a[i][j] += x` works because `recvExpr` is itself a place.
                return "((*" + coll + "__at(&(" + recvExpr + "), " + idx + ")) "
                            + assignmentOperator(v->token) + " (" + rhs + "))";
            }
            // A user place-returning `operator[]`: assign/compound-assign THROUGH the place.
            if (indexesUserOp(ea)) {
                std::string place = emitPlace(v->unaryExpression);
                std::string rhs = emitExpression(v->expression);
                return "(" + place + " " + assignmentOperator(v->token) + " (" + rhs + "))";
            }
            // Raw pointer store `p[i] = v` — only inside `unsafe { }`.
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
        // compound assignment on a user type (`a += b`) lowers to `a = a <op> b` via the
        // operator, since a struct has no built-in `+=`. Plain `=` and primitives keep the raw path.
        int binTok = compoundToBinary(v->token);
        if (binTok && userOperandType(exprClass(v->unaryExpression), _classes))
            return "(" + emitExpression(v->unaryExpression) + " = "
                       + emitBinaryOperator(binTok, v->unaryExpression, v->expression, v->line) + ")";
        return "(" + emitExpression(v->unaryExpression) + " "
                   + assignmentOperator(v->token) + " " + emitExpression(v->expression) + ")";
    }

    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        std::string coll, recvExpr, idx;
        if (collectionElemAccess(ea, coll, recvExpr, idx)) {
            // `__get` borrows its receiver by address (`&recv`). A string RVALUE receiver — a literal
            // (`"abc"[0]`, emitted as a `kama_string_lit(…)` call) or a computed piece (`s.concat(x)[0]`) —
            // has no address, so materialize it into a temp first (single-eval: reuse the already-emitted
            // `recvExpr`, don't re-emit). A named var / field / `this` stays a direct borrow. A computed
            // temp that owns a heap buffer is RAII-dropped at scope end.
            // The receiver is `ea->expression`, or `ea->identifier` for a bare-name receiver (`s[i]`).
            ASTNode* recvNode = ea->expression ? ea->expression.get()
                              : (ea->identifier ? (ASTNode*)ea->identifier.get() : nullptr);
            bool addressable = isStableStringRef(recvNode) && !dynamic_cast<StringNode*>(recvNode);
            if (coll == "kama_string" && _hoistOK && !addressable) {
                std::string t = "__stridx" + std::to_string(_tempCounter++);
                _hoisted.push_back("kama_string " + t + " = " + recvExpr + ";");
                recordDestructibleLocal(t, "kama_string");
                return coll + "__get(&" + t + ", " + idx + ")";
            }
            return coll + "__get(&(" + recvExpr + "), " + idx + ")";
        }
        // A user place-returning `operator[]`: read the value out of the place.
        if (indexesUserOp(ea)) return emitPlace(expr);
        // Raw pointer read `p[i]` — only inside `unsafe { }`.
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
        checkConstWrite(v->expression, v->line);
        std::string uop = emitUnaryUserOp(v->token, v->expression, v->line);   // op_inc/op_dec on a user type
        if (!uop.empty()) return uop;
        std::string op = (v->token == PLUSPLUS) ? "++" : "--";
        return "(" + op + emitExpression(v->expression) + ")";
    }

    if (auto* v = dynamic_cast<PostIncrDecrNode*>(n)) {
        checkConstWrite(v->expression, v->line);
        std::string uop = emitUnaryUserOp(v->token, v->expression, v->line);   //  (mutating in place — pre/post alike)
        if (!uop.empty()) return uop;
        std::string op = (v->token == PLUSPLUS) ? "++" : "--";
        return "(" + emitExpression(v->expression) + op + ")";
    }

    if (auto* v = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        std::string uop = emitUnaryUserOp(v->token, v->expression, v->line);   // op_neg/op_not/op_bnot/op_pos
        if (!uop.empty()) return uop;
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

    // `sizeof(T)`/`alignof(T)` -> C `sizeof(<cType>)`/`_Alignof(<cType>)` (a compile-time `size_t`/
    // `usize`). `cType` resolves a generic `T` under substitution, so `n * sizeof(T)` inside a
    // `Vec<T>` monomorphizes to the concrete size. `_Alignof` is C11 (kama emits strict ISO C11).
    if (auto* v = dynamic_cast<SizeofNode*>(n)) {
        return std::string(v->isAlign ? "_Alignof(" : "sizeof(") + cType(v->type) + ")";
    }

    // Compiler-internal zero-init (`(T){0}`), spliced into a synthesized `deserialize` for bypass-ctor
    // construction (no user grammar). `cType` resolves a generic `T` under substitution.
    if (auto* v = dynamic_cast<ZeroValueNode*>(n)) {
        return "(" + cType(v->type) + "){0}";
    }

    unsupported("expression", n->line);
    return "0";
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

// --- RAII cleanup helpers ---------------------------------------------

bool CEmitter::stmtIsJump(SharedStatement s)
{
    ASTNode* n = s.get();
    return dynamic_cast<ReturnNode*>(n) || dynamic_cast<BreakNode*>(n) || dynamic_cast<ContinueNode*>(n);
}

// does this body end in a jump (so control doesn't fall through to a branch join)?
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
            if (ms->second == MoveState::Moved) continue;   // moved out — skip its drop
            if (ms->second == MoveState::MaybeMoved)        // moved on some paths, live here — undecidable drop
                unsupported(("`" + it->cVar + "` is moved on some paths but not others and is still live at "
                             "scope exit — move it on all paths or none, or use Optional<T>").c_str(), _curLine);
        }
        // A move-only value that owns nothing (an empty `resource`/token) is tracked for move
        // analysis but has no destructor — skip the drop.
        auto ci = _classes.find(it->className);
        if (ci != _classes.end() && !ci->second.destructible) continue;
        indent(depth);
        *_out << it->className << "__dtor(&" << it->cVar << ");\n";
    }
}

// Drop the destructible temps a condition hoisted into the current scope since `preLoc`, then
// unregister them. Used by `if`/`while`/`for` after a value-producing condition: the temps were
// declared in the condition's wrapper block (or the loop's `while(1){…}` body), so their dtor must
// name them there, not at the outer scope where recordDestructibleLocal parked them. Mirrors
// emitScopeCleanup's guards (skip Moved / a temp of a non-destructible class) — a condition temp is
// only ever borrowed, so this is a no-op filter for strings, but the guards keep it sound now that
// arbitrary `ref`-arg class temps can hoist here. The full [preLoc,end) range is ALWAYS erased, even
// when a dtor was skipped, so a skipped temp isn't double-visited at real scope exit.
void CEmitter::dropCondTemps(size_t preLoc, int depth)
{
    if (_scopes.empty()) return;
    auto& locs = _scopes.back().locals;
    for (size_t i = locs.size(); i-- > preLoc; ) {
        auto ms = _moveState.find(locs[i].cVar);
        if (ms != _moveState.end() && ms->second != MoveState::NotMoved) continue;   // moved / maybe-moved: no drop
        auto ci = _classes.find(locs[i].className);
        if (ci != _classes.end() && !ci->second.destructible) continue;              // owns nothing
        indent(depth); *_out << locs[i].className << "__dtor(&" << locs[i].cVar << ");\n";
    }
    if (locs.size() > preLoc) locs.erase(locs.begin() + preLoc, locs.end());
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
    // Track for move analysis: move-only resources AND heap-owning collections/strings (so `give s`
    // suppresses the source's scope-drop, and a use-after-move is caught). A never-`give`n collection/
    // string stays NotMoved → drops normally, exactly as before.
    if (ownsByValue(className)) _moveState[cVar] = MoveState::NotMoved;
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
    // by-value smart-ptr params the callee owns drop at fn-end. Recorded FIRST in
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

// write hoisted temp statements (inline-ctor-in-arg materialization) at `depth`, then clear.
// A leaf statement sets _hoistOK, builds its expression string (which may push here), then calls
// this BEFORE writing its own line — so the temps appear first. Pure ISO C, no `({ … })`.
void CEmitter::flushHoisted(int depth)
{
    for (auto& s : _hoisted) { indent(depth); *_out << s << "\n"; }
    _hoisted.clear();
}

// emit a condition with hoisting enabled so an inline ctor / `match` works in `if`/`while`/`for`.
// An inline ctor knows its own type; a value-producing `match` needs a result type, and a condition is
// boolean — so a DIRECTLY-`match` condition is typed `bool`. A `match` nested in a larger condition keeps
// no target type and stays the clean "must appear in a typed position" error (bind it to a local).
std::string CEmitter::emitCondition(SharedExpression cond)
{
    if (!cond) return "";
    bool ph = _hoistOK; _hoistOK = true;
    std::string pmt = _matchTargetCType;
    if (dynamic_cast<MatchNode*>(cond.get())) _matchTargetCType = "bool";
    std::string s = emitExpression(cond);
    _matchTargetCType = pmt;
    _hoistOK = ph;
    return s;
}

void CEmitter::emitStatement(SharedStatement stmt, int depth)
{
    if (!stmt) return;
    ASTNode* n = stmt.get();

    // `:= expr;` is the value of a value-producing `match` arm — the match emitter consumes it as the
    // arm's final statement. Reaching it here means it is misplaced (not last, or in a match used as a
    // statement, or outside any match).
    if (dynamic_cast<ArmValueNode*>(n)) {
        unsupported("`:=` produces a match arm's value — it is only valid as the final statement of a "
                    "value-producing `match` arm", n->line);
        return;
    }

    if (auto* block = dynamic_cast<BlockNode*>(n)) {
        line(n->line);
        indent(depth);
        emitBlock(block, depth);
        *_out << "\n";
        return;
    }

    // `unsafe { ... }`: permit raw pointer index/store inside; otherwise a
    // plain scoped block. The single, explicit, greppable unsafe surface.
    if (auto* u = dynamic_cast<UnsafeNode*>(n)) {
        bool prev = _inUnsafe;
        _inUnsafe = true;
        emitStatement(u->body, depth);   // the BlockNode -> a normal scoped { … }
        _inUnsafe = prev;
        return;
    }

    {
        // Local declaration — plain or `const`. The two declarator node types
        // are structurally identical (name + initializer), so one generic body serves
        // both; a const decl additionally records each name as immutable (no
        // reassignment, and — deep const — no writes THROUGH the binding either).
        LocalVariableDeclaration* lvd = dynamic_cast<LocalVariableDeclaration*>(n);
        ConstLocalVariableDeclaration* cvd = lvd ? nullptr
                                            : dynamic_cast<ConstLocalVariableDeclaration*>(n);
        SharedIdentifier declType = lvd ? lvd->type : (cvd ? cvd->type : SharedIdentifier());
        bool isConstDecl = (cvd != nullptr);
        if (declType) {
            // a bare generic type without a type argument (`Box b` instead of `Box<int32> b`)
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
                _localCTypes[nm] = ty;                        // full C type (incl. primitives) for assignment-RHS lowering
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
                            unsupported("contract initializer must be a concrete object lvalue", n->line);
                    }
                    *_out << ";\n";
                    return;
                }

                // FunctionPtr<Sig> binding: a signature-typed local — track the
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
                        bool ph = _hoistOK; _hoistOK = true;               // inline-ctor hoisting
                        std::string pmt = _matchTargetCType; _matchTargetCType = ty;   // value-producing match result type
                        initStr = " = " + emitExpression(d->initializer);
                        _matchTargetCType = pmt;
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
                bool zeroInit = _classes[ty].isIntrinsicColl || _classes[ty].isExternStruct;
                *_out << ty << " " << nm << (zeroInit ? " = {0}" : "") << ";\n";
                // Track for RAII cleanup at scope exit (assumes init-at-decl).
                if (_classes[ty].destructible || isMoveOnlyValue(ty)) recordDestructibleLocal(nm, ty);  // track empty resources for move analysis
                if (!d->initializer) {
                    // No initializer: the scope-exit dtor recorded above WILL run, so the object must be
                    // brought to a valid state now — not left as stack garbage (which frees fine on glibc
                    // but aborts on macOS's malloc: "pointer being freed was not allocated"). If the class
                    // has a zero-arg default ctor, call it (runs its field-init / vtable setup, exactly like
                    // `= List()`); intrinsic collections / extern structs have no kama ctor — the `= {0}`
                    // above is their default empty state.
                    if (!zeroInit && !_classes[ty].isAbstractClass
                        && ((_classes[ty].hasCtor && _classes[ty].ctorParams.empty()) || _classes[ty].synthCtor)) {
                        line(n->line); indent(depth);
                        *_out << emitCtorCall(nm, _classes[ty], nullptr, n->line) << ";\n";
                    }
                    return;   // otherwise declared-only (a non-destructible value; nothing to construct)
                }

                // unwrap a give/copy hand-off marker — the inner NAMED value drives
                // move (give) vs duplicate (copy). A fresh rvalue never takes a marker.
                SharedExpression init = d->initializer;
                int handoff = 0;   // 0 none, 1 give, 2 copy
                if (auto* h = dynamic_cast<HandoffNode*>(d->initializer.get())) { handoff = h->isGive ? 1 : 2; init = h->value; }
                if (handoff && !isNamedValue(init.get()))
                    unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call result needs no marker", n->line);

                // `T(...)` with no `new` (an InvocationNode whose callee names the
                // declared class) is STACK construction; `new` is reserved for the heap.
                InvocationNode* stackCtor = nullptr;
                if (auto* iv = dynamic_cast<InvocationNode*>(init.get()))
                    if (iv->identifier && iv->identifier->value && isClass(ty)) {
                        std::string rn = resolveUserName(*iv->identifier->value, iv->identifier->qualifier);
                        // `Box<int32> b = Box(v: 7)` — the ctor names the bare template `Box`, but
                        // the declared type is the instance `Box_int32`; accept the template→instance match.
                        auto g = _genericTypeInstOf.find(ty);
                        if (rn == ty || (g != _genericTypeInstOf.end() && g->second == rn))
                            stackCtor = iv;
                    }

                if (auto* oc = dynamic_cast<ObjectCreationNode*>(init.get())) {
                    // `new` is the HEAP operator — it boxes a value into a smart
                    // pointer (Owned/Shared/Weak), naming the element type directly:
                    // `Owned<Box> p = new Box(...)`. (BindableFunctionPtr keeps `new` for
                    // its bind.) `new` into a plain value type is an error — drop `new`.
                    // NOTE: the intrinsic-iface + library-adopt boxing below is mirrored (as a
                    // hoisted-temp string) in tryHoistInlineNew for return/arg/payload positions —
                    // keep the two in sync (esp. the Shared ctrl asymmetry: iface path emits
                    // kama_ctrl_new(), the library-adopt path lets `adopt` allocate it).
                    std::string octy = cType(oc->type);
                    if (isBindableClass(ty)) {
                        emitBindableNew(nm, ty, oc, depth);   // bind obj + method
                    } else if (isSmartPtrClass(ty)) {
                        std::string T = _classes[ty].collElemClass;
                        if (isSmartPtrClass(octy) || isBindableClass(octy))
                            unsupported(("`new` now names the element type — write `new " + T
                                         + "(...)`, not the wrapper").c_str(), n->line);
                        else if (isInterface(T)) {
                            // box a concrete class that implements interface T into an owned
                            // interface handle — malloc the concrete, ctor it, set {obj, vtbl}.
                            auto cit = _classes.find(octy);
                            bool implementsT = false;
                            if (cit != _classes.end())
                                for (auto& i : cit->second.interfaces) if (i == T) { implementsT = true; break; }
                            if (!implementsT)
                                unsupported(("`new " + octy + "` does not implement `" + T + "` — `" + ty
                                             + "` owns a class that satisfies the contract").c_str(), n->line);
                            else if (_classes[octy].isAbstractClass)
                                unsupported(("cannot instantiate abstract class '" + octy + "'").c_str(), n->line);
                            else {
                                line(n->line); indent(depth);
                                *_out << nm << ".obj = malloc(sizeof(" << octy << "));\n";
                                indent(depth); *_out << "if (!" << nm << ".obj) kama_panic(kama_string_lit(\"out of memory\", 13));\n";
                                if (_classes[octy].hasCtor) {
                                    line(n->line);
                                    bool ph = _hoistOK; _hoistOK = true;               // hoist arg hand-offs
                                    std::string cc = emitReorderedCall(octy + "__ctor", "(" + octy + "*)" + nm + ".obj",
                                                              _classes[octy].ctorParams, oc->args, n->line);
                                    _hoistOK = ph; flushHoisted(depth);
                                    indent(depth); *_out << cc << ";\n";
                                }
                                indent(depth); *_out << nm << ".vtbl = &" << octy << "__as_" << T << ";\n";
                                if (smartKind(ty) == CollKind::Shared) {   // ref-counted owned interface
                                    indent(depth); *_out << nm << ".ctrl = kama_ctrl_new();\n";
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
                            indent(depth); *_out << "if (!" << nm << ".ptr) kama_panic(kama_string_lit(\"out of memory\", 13));\n";
                            if (isClass(T) && _classes[T].hasCtor) {
                                line(n->line);
                                bool ph = _hoistOK; _hoistOK = true;               // hoist arg hand-offs
                                std::string cc = emitReorderedCall(T + "__ctor", nm + ".ptr",
                                                          _classes[T].ctorParams, oc->args, n->line);
                                _hoistOK = ph; flushHoisted(depth);
                                indent(depth); *_out << cc << ";\n";
                            }
                            if (smartKind(ty) == CollKind::Shared) {
                                indent(depth); *_out << nm << ".ctrl = kama_ctrl_new();\n";
                            }
                            // T with no ctor: malloc leaves it default (callers init fields).
                        }
                    } else if (!heapOwnerTarget(ty).empty()) {
                        // a LIBRARY heap owner (`Box<T> implements HeapOwner<T>`): `new T(args)`
                        // placement-constructs T on the heap and adopts the raw ptr — ZERO copies, same
                        // as the intrinsic. `new` stays valid ONLY into an RAII owner, so it can't leak.
                        std::string T = heapOwnerTarget(ty);
                        // `new Derived` into a `Shared`/`Owned<Base>` widens (upcast): build the DERIVED,
                        // then adopt its base subobject (offset-0 `__base` chain) — destruction stays
                        // virtual (Base__vdrop), so no slicing. `octy == T` is the plain same-element case.
                        bool upcastNew = (octy != T) && isClass(octy) && isBaseOf(T, octy);
                        std::string C = upcastNew ? octy : T;              // the concrete actually built
                        if (octy != T && !upcastNew)
                            unsupported(("`" + ty + "` owns `" + T + "`, but got `new " + octy
                                         + "(...)` — name the element type or a derived of it, not the owner").c_str(), n->line);
                        else if (isClass(C) && _classes[C].isAbstractClass)
                            unsupported(("cannot instantiate abstract class '" + C + "'").c_str(), n->line);
                        else {
                            ClassInfo* ao = nullptr;
                            MethodInfo* adoptM = findMethod(&_classes[ty], "adopt", &ao);
                            if (!adoptM) { unsupported(("`" + ty + "` implements HeapOwner but has no `adopt` method").c_str(), n->line); }
                            else {
                                std::string hp = "__heap" + std::to_string(_tempCounter++);
                                line(n->line); indent(depth);
                                *_out << C << "* " << hp << " = (" << C << "*)malloc(sizeof(" << C << "));\n";
                                indent(depth); *_out << "if (!" << hp << ") kama_panic(kama_string_lit(\"out of memory\", 13));\n";
                                if (isClass(C) && _classes[C].hasCtor) {
                                    line(n->line);
                                    bool ph = _hoistOK; _hoistOK = true;               // hoist arg hand-offs
                                    std::string cc = emitReorderedCall(C + "__ctor", hp, _classes[C].ctorParams, oc->args, n->line);
                                    _hoistOK = ph; flushHoisted(depth);
                                    indent(depth); *_out << cc << ";\n";
                                }
                                // adopt the T* — the base subobject when widening (offset-0), else the ptr itself.
                                std::string adoptArg = hp;
                                if (upcastNew) {
                                    std::string bp = basePathTo(&_classes[C], &_classes[T]);
                                    if (!bp.empty()) bp.pop_back();
                                    adoptArg = "(" + T + "*)&(" + hp + "->" + bp + ")";
                                }
                                indent(depth); *_out << nm << " = " << adoptM->cName << "(" << adoptArg << ");\n";
                            }
                        }
                    } else if (_classes.count(ty) && _classes[ty].isIntrinsicColl) {
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
                        bool ph = _hoistOK; _hoistOK = true;               // hoist arg hand-offs
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
                    // field-copy + weak retain. The source Shared stays valid. A fat
                    // contract Weak copies {obj, vtbl}; a thin Weak copies {ptr}.
                    line(n->line); indent(depth);
                    std::string src = emitExpression(init);
                    if (isInterface(_classes[ty].collElemClass))
                        *_out << nm << ".obj = (" << src << ").obj; " << nm << ".vtbl = (" << src << ").vtbl; "
                             << nm << ".ctrl = (" << src << ").ctrl;\n";
                    else
                        *_out << nm << ".ptr = (" << src << ").ptr; " << nm << ".ctrl = (" << src << ").ctrl;\n";
                    indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->weak++;\n";
                } else if (isSmartPtrUpcast(ty, init)) {
                    // Cross-element upcast: widen a concrete-element `Shared`/`Owned` into this
                    // contract-element (intrinsic fat) handle. The pointee is shared/moved; `nm`
                    // was already zero-declared above.
                    line(n->line);
                    emitSmartPtrUpcast(nm, ty, init, handoff, depth, n->line);
                } else if (isSmartPtrBaseUpcast(ty, init)) {
                    // Base-class upcast: widen a derived-class handle into this base-class handle.
                    line(n->line);
                    emitSmartPtrBaseUpcast(nm, ty, init, handoff, depth, n->line);
                } else if (isSmartPtrHandoffMismatch(ty, init)) {
                    // Both sides own, but the widening isn't valid — a clear diagnostic instead of
                    // the misleading "collection hand-off" message below.
                    std::string k = declType->value ? *declType->value : "handle";
                    std::string e = (declType->genericArg && declType->genericArg->value)
                                        ? *declType->genericArg->value : "T";
                    unsupported(("cannot widen this handle into `" + k + "<" + e + ">` — an owning-handle "
                                 "upcast needs the same ownership kind and an `is a` element (a concrete that "
                                 "implements the contract `" + e + "`, or a derived of the base `" + e + "`)").c_str(),
                                n->line);
                } else if (isBindableClass(ty)) {
                    // BindableFunctionPtr <- free function (promote) or another bindable (move).
                    line(n->line);
                    emitBindablePromote(nm, ty, init, depth);
                } else {
                    // Copy-initialize from another named value. The give/copy marker
                    // (or the type's default) decides move vs duplicate.
                    line(n->line);
                    bool ph = _hoistOK; _hoistOK = true;               // inline-ctor hoisting
                    std::string pvt = _variantTargetType; _variantTargetType = ty;   // `Optional<int32> o = Optional::Some(…)`
                    std::string pmt = _matchTargetCType; _matchTargetCType = ty;      // `string s = match(…)` / `List l = match(…)`
                    std::string iv = emitExpression(init);
                    _matchTargetCType = pmt;
                    _variantTargetType = pvt;
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
                    } else if (_classes.count(ty) && _classes[ty].isIntrinsicColl && !isFixedColl(ty) && isNamedValue(init.get())) {
                        // a collection move/deep-copy isn't a plain `=` — require a marker. (A
                        // `Fixed` is a value: the plain `=` above already copied it — no marker.)
                        if (handoff == 0)
                            unsupported(("a collection hand-off must say `give` (move) or `copy` (deep) — write "
                                         "`" + ty + " v = give …`").c_str(), n->line);
                        else if (handoff == 2) {
                            // `copy` = a real deep copy (fresh buffer), element-wise. Valid iff
                            // each element is copyable — bitwise-copyable (owns nothing), OR a resource that
                            // opted into `Copyable` (`__copy` calls the element's `copy()`). A
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
                        // A destructible `resource` value. Movable is universal: `give` MOVES (relocate —
                        // the `=` blit already transferred the bytes; move-tracking suppresses the source
                        // dtor). Copyable is opt-in: `copy` duplicates via copy() (overwriting the blit;
                        // source stays valid). A BARE hand-off follows the type's declared `bare:` default
                        // (give→move, copy→copy()/retain). A non-Copyable resource has no copy() → move.
                        bool cpy = isCopyable(ty);
                        bool doCopy;
                        if (handoff == 2) {                              // explicit `copy`
                            if (!cpy) unsupported(("`" + ty + "` has no `copy` method — add `implements "
                                                   "Copyable(bare: …)`, or use `give` to move it").c_str(), n->line);
                            doCopy = cpy;
                        } else if (handoff == 1) doCopy = false;         // explicit `give` = move (always allowed)
                        else doCopy = cpy && _classes[ty].bareDefault == COPY;   // bare — the declared default
                        if (doCopy) { indent(depth); *_out << nm << " = " << ty << "__copy(&(" << emitExpression(init) << "));\n"; }
                        else { std::string mv = moveOnlySource(init, n->line); if (!mv.empty()) markMoved(mv); }
                    }
                    // A value/primitive: the plain `=` above IS the hand-off — `copy` and `give` are both
                    // just that copy (a value's "move" is a copy; the source stays valid, so no marker error).
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
        // Unwrap a give/copy marker for the place-return check below; the value path passes the original
        // `ret->expression` (marker and all) to emitOwnedValueInto, which re-unwraps + applies the matrix.
        SharedExpression retExpr = ret->expression;
        if (retExpr)
            if (auto* h = dynamic_cast<HandoffNode*>(retExpr.get())) retExpr = h->value;
        // A `ref T operator[]` returns a PLACE: address the lvalue directly (`return &(place)`) — no
        // by-value return-temp (you can't copy a place). The place borrows `self`, which outlives the
        // call, so it's valid for the caller's enclosing statement (used transiently, never stored).
        if (_returnIsPlace) {
            // A returned place must borrow something that OUTLIVES the call — `this` (the caller owns
            // the receiver) or a `ref` parameter (a borrow the caller holds). A place rooted at a
            // LOCAL would dangle the moment the callee returns. Targeted rule, no lifetimes.
            std::string root = retExpr ? rootBinding(retExpr) : std::string();
            if (root != "this" && !_refParams.count(root))
                unsupported("a `ref T` result must borrow `this` or a `ref` parameter — returning a "
                            "place into a local would dangle", n->line);
            bool ph = _hoistOK; _hoistOK = true;
            std::string p = retExpr ? emitPlace(retExpr) : std::string("0");
            _hoistOK = ph;
            flushHoisted(depth);
            emitUnwindAll(depth);
            indent(depth); *_out << "return &(" << p << ");\n";
            return;
        }
        // Capture the return value BEFORE running any destructors (it may
        // reference locals about to be destroyed), then unwind, then return.
        if (retExpr && _currentReturnCType != "void") {
            // Capture the return value into a temp BEFORE any destructors run (it may reference locals
            // about to be destroyed), handling an owned hand-off (give/copy, an inline ctor/`new`, or a
            // bare generic ctor) uniformly with a value-producing `match` arm, then unwind, then return.
            std::string tmp = "__ret_" + std::to_string(_tempCounter++);
            indent(depth); *_out << _currentReturnCType << " " << tmp << ";\n";
            emitOwnedValueInto(tmp, _currentReturnCType, ret->expression, n->line, depth);
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
        line(n->line);
        // A value-producing construct in the condition (an inline ctor / `match`) hoists a temp; a raw
        // condition has no statement slot, so wrap the whole `if` in a block and flush the temps first.
        // An owned-string temp hoisted here (`if (s.trim() == "x")`) is registered destructible in the
        // current scope but *declared* in this wrapper block, so we dtor+unregister it before the wrapper
        // closes (below) rather than at the outer scope — else its drop would name an out-of-scope temp.
        size_t preLoc = _scopes.empty() ? 0 : _scopes.back().locals.size();
        std::string cond = emitCondition(f->booleanExpression);
        bool hoist = !_hoisted.empty();
        int bd = depth;
        if (hoist) { indent(depth); *_out << "{\n"; flushHoisted(depth + 1); bd = depth + 1; }
        indent(bd);
        *_out << "if (" << cond << ") ";
        // walk each branch from the SAME pre-if move-state, then merge at the join.
        // A branch that diverges (ends in return/break/continue) doesn't reach the join.
        auto before = _moveState;
        emitBody(f->ifStatement, bd, /*loopBoundary=*/false);
        auto thenState = _moveState;
        bool thenDiv = bodyDiverges(f->ifStatement);
        _moveState = before;
        bool elseDiv = false;
        if (f->elseStatement) {
            *_out << " else ";
            emitBody(f->elseStatement, bd, false);
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
        if (hoist) {
            // dtor + unregister the destructible temps hoisted for the condition (declared in this wrapper
            // block). A diverging arm already dropped them via emitUnwindAll, so on that path this line is
            // simply not reached — each temp is dropped exactly once.
            dropCondTemps(preLoc, bd);
            indent(depth); *_out << "}\n";
        }
        return;
    }

    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        line(n->line);
        size_t preLoc = _scopes.empty() ? 0 : _scopes.back().locals.size();
        std::string cond = emitCondition(w->booleanExpression);
        if (_hoisted.empty()) {                                   // fast path — unchanged
            indent(depth);
            *_out << "while (" << cond << ") ";
            emitBody(w->whileStatement, depth, /*loopBoundary=*/true);
            *_out << "\n";
        } else {                                                  // loop-and-a-half: recompute cond each pass
            bool drop = !_scopes.empty() && _scopes.back().locals.size() > preLoc;
            indent(depth); *_out << "while (1) {\n";
            flushHoisted(depth + 1);                              // condition temps — re-run each iteration
            if (drop) {                                           // owned condition temp: eval, drop, then break
                std::string lc = "__loopcond" + std::to_string(_tempCounter++);
                indent(depth + 1); *_out << "bool " << lc << " = (" << cond << ");\n";
                dropCondTemps(preLoc, depth + 1);                // drop before the body — cond doesn't bind into it
                indent(depth + 1); *_out << "if (!" << lc << ") break;\n";
            } else {
                indent(depth + 1); *_out << "if (!(" << cond << ")) break;\n";
            }
            emitBody(w->whileStatement, depth + 1, /*loopBoundary=*/true);
            *_out << "\n";
            indent(depth); *_out << "}\n";
        }
        return;
    }

    if (auto* d = dynamic_cast<DoWhileNode*>(n)) {
        line(n->line); indent(depth);
        // `do`/`while` condition sits at the bottom and its `continue` must skip TO it, which a naive
        // while(1) rewrite breaks — so a hoisting construct here stays a clean error (bind to a local).
        *_out << "do ";
        emitBody(d->doWhileStatement, depth, /*loopBoundary=*/true);
        *_out << " while (" << emitExpression(d->booleanExpression) << ");\n";
        return;
    }

    if (auto* f = dynamic_cast<ForNode*>(n)) {
        line(n->line);
        std::string init = emitForClause(f->initializerStatements);
        size_t preLoc = _scopes.empty() ? 0 : _scopes.back().locals.size();
        std::string cond = emitCondition(f->booleanExpression);
        std::string iter = emitForClause(f->iteratorStatements);
        if (_hoisted.empty()) {                                   // fast path — unchanged
            indent(depth);
            *_out << "for (" << init << "; " << cond << "; " << iter << ") ";
            emitBody(f->body, depth, /*loopBoundary=*/true);
            *_out << "\n";
        } else {                                                  // loop-and-a-half; iter stays in the C header
            bool drop = !_scopes.empty() && _scopes.back().locals.size() > preLoc;
            indent(depth); *_out << "for (" << init << "; ; " << iter << ") {\n";
            flushHoisted(depth + 1);                              // condition temps — re-run each iteration
            if (drop) {                                           // owned condition temp: eval, drop, then break
                std::string lc = "__loopcond" + std::to_string(_tempCounter++);
                indent(depth + 1); *_out << "bool " << lc << " = (" << cond << ");\n";
                dropCondTemps(preLoc, depth + 1);                // drop before the body — cond doesn't bind into it
                indent(depth + 1); *_out << "if (!" << lc << ") break;\n";
            } else {
                indent(depth + 1); *_out << "if (!(" << cond << ")) break;\n";
            }
            emitBody(f->body, depth + 1, /*loopBoundary=*/true);
            *_out << "\n";
            indent(depth); *_out << "}\n";
        }
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

    if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        line(n->line); indent(depth);
        std::string itCls = exprClass(fe->expression);
        // A user type (not a built-in collection) iterates via the iterator protocol (structural).
        if (!itCls.empty() && _classes.count(itCls) && !_classes[itCls].isIntrinsicColl) {
            emitForeachIterator(fe, itCls, depth); return;
        }
        if (itCls.empty() || !_classes.count(itCls) || !_classes[itCls].isIntrinsicColl) {
            unsupported("foreach over a non-collection (a user type needs `iterator()`/`next()` or `iterMut()`)", n->line); *_out << "\n"; return;
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

        // `foreach (ref T e …)` binds each element by PLACE (a bounds-checked `T*` via `__at`), so
        // mutation persists; `e` joins `_refParams` so reads/writes in the body deref it (`(*e)`),
        // exactly like a `ref` parameter. Plain `foreach` binds a borrowed COPY (`__get`). Neither
        // is recorded destructible (the collection owns the element).
        bool hadRef = _refParams.count(nm);
        indent(depth + 2);
        if (fe->isRef) {
            *_out << elemTy << "* " << nm << " = " << coll << "__at(" << fp << ", " << ix << ");\n";
            _refParams.insert(nm);
        } else {
            *_out << elemTy << " " << nm << " = " << coll << "__get(" << fp << ", " << ix << ");\n";
        }

        // Mark this collection as "being iterated": growing it (`add`) mid-loop reallocs its buffer
        // and invalidates the element references — a use-after-free (verified) for a `ref` binding, a
        // runaway loop otherwise. `emitMethodCall` rejects `add` on a root in this stack.
        std::string iterRoot = rootBinding(fe->expression);
        if (!iterRoot.empty()) _foreachColls.push_back(iterRoot);

        SharedStatement last;
        if (auto* b = dynamic_cast<BlockNode*>(fe->body.get())) {
            if (b->statements) for (auto& st : *b->statements) { emitStatement(st, depth + 2); last = st; }
        } else if (fe->body) {
            emitStatement(fe->body, depth + 2); last = fe->body;
        }
        if (!(last && stmtIsJump(last))) emitScopeCleanup(_scopes.back(), depth + 2);

        if (!iterRoot.empty()) _foreachColls.pop_back();
        if (hadType) _localTypes[nm] = prevType; else _localTypes.erase(nm);
        if (fe->isRef && !hadRef) _refParams.erase(nm);
        _scopes.pop_back();

        indent(depth + 1); *_out << "}\n";   // close for
        indent(depth);     *_out << "}\n";   // close wrapper
        return;
    }

    // Smart-pointer assignment `b = a;` — release b's current pointee first (no
    // leak), copy, then either invalidate the source (give: move) or retain
    // (copy: refcount++). The null/retain is a statement, so it can't live in
    // an expression. a give/copy marker on the RHS overrides the default,
    // uniformly with init / argument / return.
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        // Base-class upcast reseat: `b = [give] d` where `b: Shared<Base>` and `d: Shared<Derived>`
        // (both thin library handles — the LHS isn't an intrinsic smart ptr, so this precedes the
        // smart-ptr path below). Release the handle's old pointee, then widen the derived handle in.
        if (as->token == EQ) {
            SharedExpression rhs0 = as->expression;
            int handoff0 = 0;   // 0 none, 1 give, 2 copy
            if (auto* h = dynamic_cast<HandoffNode*>(rhs0.get())) { handoff0 = h->isGive ? 1 : 2; rhs0 = h->value; }
            std::string lty0 = exprClass(as->unaryExpression);
            if (isSmartPtrBaseUpcast(lty0, rhs0)) {
                checkConstWrite(as->unaryExpression, n->line);
                std::string b = emitExpression(as->unaryExpression);
                line(n->line);
                indent(depth); *_out << lty0 << "__dtor(&" << b << ");\n";
                emitSmartPtrBaseUpcast(b, lty0, rhs0, handoff0, depth, n->line);
                return;
            }
        }
        // Bare in-place ctor assigned to a class-typed lvalue (`this.m = Map()`, `x = Point(v: 1)`): a bare
        // stack ctor has no rvalue form — it constructs into a place — so build it DIRECTLY into the lvalue,
        // exactly like the local-init `stackCtor` path, after releasing the old value. Fires only for a fresh
        // ctor whose callee names the lvalue's (possibly generic) class; a `give`/`copy`-marked or named RHS
        // never matches (not an InvocationNode), so the ownership paths below are untouched.
        if (as->token == EQ) {
            std::string lty = exprClass(as->unaryExpression);
            if (auto* iv = dynamic_cast<InvocationNode*>(as->expression.get()))
                if (iv->identifier && iv->identifier->value && isClass(lty) && _classes[lty].hasCtor
                    && !isSmartPtrClass(lty)) {
                    std::string rn = resolveUserName(*iv->identifier->value, iv->identifier->qualifier);
                    auto g = _genericTypeInstOf.find(lty);
                    if (rn == lty || (g != _genericTypeInstOf.end() && g->second == rn)) {
                        checkConstWrite(as->unaryExpression, n->line);
                        std::string lname;
                        if (auto* lid = dynamic_cast<IdentifierNode*>(as->unaryExpression.get()))
                            if (lid->value && (!lid->qualifier || lid->qualifier->empty())) lname = *lid->value;
                        bool bMoved = (!lname.empty() && _moveState.count(lname) && _moveState[lname] == MoveState::Moved);
                        std::string b = emitExpression(as->unaryExpression);
                        line(n->line);
                        if (!bMoved && _classes[lty].destructible)                 // release the old value first
                            { indent(depth); *_out << lty << "__dtor(&" << b << ");\n"; }
                        if (!lname.empty()) _moveState[lname] = MoveState::NotMoved;   // target is live again
                        bool ph = _hoistOK; _hoistOK = true;                       // hoist any arg hand-offs
                        std::string cc = emitCtorCall(b, _classes[lty], iv->args, n->line);
                        _hoistOK = ph;
                        flushHoisted(depth);
                        indent(depth); *_out << cc << ";\n";
                        return;
                    }
                }
        }
        // Owned value into an `operator[]` PLACE: `a[i] = [give/copy] <owned>` where the element OWNS
        // something (string/collection/resource/smart-ptr). Release the old element, then move/copy the new
        // one THROUGH the place — an `ElementAccessNode` LHS isn't a named lvalue, so the branches below miss
        // it, and the RHS `give`/`copy` marker would otherwise hit the "bare sub-expression" reject. A
        // primitive element keeps the plain store path; a raw `Ptr<T>` slot is handled just below.
        if (as->token == EQ && !_inUnsafe) {
            if (auto* ea = dynamic_cast<ElementAccessNode*>(as->unaryExpression.get())) {
                std::string dcoll, drecv, didx, et;
                if (collectionElemAccess(ea, dcoll, drecv, didx) && _collections.count(dcoll))
                    et = _collections[dcoll].elemClass;            // "" for a primitive element
                else if (indexesUserOp(ea))
                    et = exprClass(as->unaryExpression);            // user `ref T operator[]` element
                if (!et.empty() && _classes.count(et) && _classes[et].destructible) {
                    SharedExpression rhs = as->expression;
                    int handoff = 0;
                    if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { handoff = h->isGive ? 1 : 2; rhs = h->value; }
                    checkConstWrite(as->unaryExpression, n->line);
                    bool named  = isNamedValue(rhs.get());
                    bool isColl = _classes[et].isIntrinsicColl && !isSmartPtrClass(et);   // List/Array/string element
                    bool copyable = isColl || isCopyable(et);
                    bool doCopy;
                    if (handoff == 2) {
                        if (!copyable) unsupported(("`copy` of a `" + et + "` element needs a `Copyable` element "
                                                    "— use `give` to move it").c_str(), n->line);
                        doCopy = true;
                    } else if (handoff == 1) {
                        doCopy = false;                                                 // give = move
                    } else {
                        // bare: a NAMED owning source needs a marker; a FRESH owned rvalue (`"…"`, `List()`,
                        // a call result) moves in with no marker.
                        if (named) unsupported("an owned element hand-off must say `give` (move) or `copy` "
                                               "(deep) — write `a[i] = give …` or `a[i] = copy …`", n->line);
                        doCopy = false;
                    }
                    // Evaluate the RHS into a temp FIRST (it may read the old `a[i]`, e.g. `a[i] = a[i].concat`),
                    // then take the place ONCE (a `__at`/`operator[]` call — bounds-checked), release the old
                    // element, and move/copy the new one in. An inline ctor/`new` RHS (`a[i] = Tag(…)`)
                    // materializes into a hoisted temp from the element type.
                    bool ph = _hoistOK; _hoistOK = true;
                    std::string src = tryHoistInlineCtor(rhs, et, n->line);
                    if (src.empty()) src = tryHoistInlineNew(rhs, et, n->line);
                    if (src.empty()) src = emitExpression(rhs);
                    _hoistOK = ph;
                    std::string tv = "__elv" + std::to_string(_tempCounter++);
                    std::string sp = "__esl" + std::to_string(_tempCounter++);
                    line(n->line);
                    flushHoisted(depth);
                    indent(depth); *_out << et << " " << tv << " = "
                                         << (doCopy ? (et + "__copy(&(" + src + "))") : src) << ";\n";
                    std::string place = emitPlace(as->unaryExpression);
                    indent(depth); *_out << et << "* " << sp << " = &(" << place << ");\n";
                    indent(depth); *_out << et << "__dtor(" << sp << ");\n";           // release the old element
                    indent(depth); *_out << "*" << sp << " = " << tv << ";\n";         // move/copy the new one in
                    if (!doCopy && named) {   // consume the moved source (a copy leaves it valid; a fresh rvalue has none)
                        if (isColl) { indent(depth); *_out << "(" << src << ").data = NULL; (" << src << ").len = 0;\n"; }
                        else { std::string mv = moveOnlySource(rhs, n->line); if (!mv.empty()) markMoved(mv); }
                    }
                    return;
                }
            }
        }
        // A RAW pointer-slot store `ptr[i] = give x` / `ptr[i] = x` in unsafe manual-memory code: the
        // exprClass of a `Ptr<T>` index is unknown (not a collection / user `operator[]`), so it's a raw
        // C store. Blit the value in, DON'T release the (uninitialized) old slot, and consume a moved
        // resource source (mark it moved so its scope dtor is skipped). This is how a library container
        // relocates an element into its buffer. Only intercepted for an owned RHS (marker or resource) —
        // a plain `ptr[i] = value` or a value-producing RHS keeps the generic path below.
        if (as->token == EQ && !ptrElemType(as->unaryExpression).empty()) {
            SharedExpression rhs = as->expression;
            bool give = false, marked = false;
            if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { give = h->isGive; rhs = h->value; marked = true; }
            std::string rc = exprClass(rhs);
            bool ownedRhs = marked || (!rc.empty() && (isMoveOnlyValue(rc) || isSmartPtrClass(rc)));
            if (ownedRhs) {
                std::string b = emitExpression(as->unaryExpression), src = emitExpression(rhs);
                line(n->line);
                // `copy` of an owning collection/`string` into the raw slot deep-copies (`__copy`), so the
                // slot and the source own separate buffers (the source survives). Otherwise blit.
                if (marked && !give && ownsByValue(rc) && !isMoveOnlyValue(rc)) {
                    indent(depth); *_out << b << " = " << rc << "__copy(&(" << src << "));\n";
                    return;
                }
                indent(depth); *_out << b << " = " << src << ";\n";
                if (give || !marked) {
                    // consume the moved source so it isn't dropped again: an IFACE smart pointer is nulled
                    // at runtime (its callee-drop then no-ops); a resource OR collection/`string` is
                    // move-tracked (its scope-drop is skipped — the raw slot now owns the buffer).
                    if (isSmartPtrClass(rc)) { indent(depth); *_out << smartPtrInvalidate(src, smartKind(rc), isInterface(_classes[rc].collElemClass)) << "\n"; }
                    else if (ownsByValue(rc)) { std::string mv = moveOnlySource(rhs, n->line); if (!mv.empty()) markMoved(mv); }
                }
                return;
            }
        }
        if (as->token == EQ && isSmartPtrExpr(as->unaryExpression)) {
            checkConstWrite(as->unaryExpression, n->line);   // no reseating a const smart ptr
            std::string b   = emitExpression(as->unaryExpression);
            std::string ty  = exprClass(as->unaryExpression);      // Owned_T / Shared_T / Weak_T
            CollKind    knd = smartKind(ty);
            SharedExpression rhs = as->expression;
            int handoff = 0;   // 0 none, 1 give, 2 copy
            if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { handoff = h->isGive ? 1 : 2; rhs = h->value; }
            if (isSmartPtrUpcast(ty, rhs)) {
                // Cross-element upcast reseat: release the handle's old pointee, then widen a
                // concrete-element `Shared`/`Owned` into this contract-element intrinsic handle.
                line(n->line);
                indent(depth); *_out << ty << "__dtor(&" << b << ");\n";
                emitSmartPtrUpcast(b, ty, rhs, handoff, depth, n->line);
                return;
            }
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
                // Shared->Weak reseat: field-copy + weak retain. A fat contract Weak
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
        // Reassigning a destructible `resource` VALUE from a NAMED source: release the target's old
        // value, then move or copy the new one in. `give` MOVES (relocate; mark source moved); `copy`
        // deep-copies via copy() (source survives); a BARE assign follows the type's `bare:` default. A
        // self-COPY (`s = s`) is a guarded no-op; a self-MOVE is rejected. A fresh rvalue keeps the
        // generic path below.
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
                bool cpy = isCopyable(lty);
                bool doCopy;
                if (handoff == 2) {
                    if (!cpy) unsupported(("`" + lty + "` has no `copy` method — add `implements "
                                           "Copyable(bare: …)`, or use `give` to move it").c_str(), n->line);
                    doCopy = cpy;
                } else if (handoff == 1) doCopy = false;
                else doCopy = cpy && _classes[lty].bareDefault == COPY;
                // A move needs a movable local (reject moving out of a field/element); a copy reads any lvalue.
                std::string mv = doCopy ? std::string() : moveOnlySource(rhs, n->line);
                std::string rname;
                if (auto* rid = dynamic_cast<IdentifierNode*>(rhs.get()))
                    if (rid->value && (!rid->qualifier || rid->qualifier->empty())) rname = *rid->value;
                if (!lname.empty() && doCopy && rname == lname) return;   // self-copy (retain onto self): no-op
                if (!lname.empty() && !mv.empty() && mv == lname)
                    unsupported("handing a value onto itself would use it after it was dropped", n->line);
                bool bMoved = (!lname.empty() && _moveState.count(lname) && _moveState[lname] == MoveState::Moved);
                if (!lname.empty()) _moveState[lname] = MoveState::NotMoved;   // write target: clear before emit
                std::string b   = emitExpression(as->unaryExpression);
                std::string src = emitExpression(rhs);
                line(n->line);
                if (!bMoved && _classes.count(lty) && _classes[lty].destructible)             // free the old value (if it owns anything)
                    { indent(depth); *_out << lty << "__dtor(&" << b << ");\n"; }
                indent(depth); *_out << b << " = " << (doCopy ? (lty + "__copy(&(" + src + "))") : src) << ";\n";
                if (!mv.empty()) markMoved(mv);
                return;
            }
        }

        // Reassigning a collection/`string` lvalue (`s = …`, `this.a = …`). exprClass() is "" for a
        // collection/string, so resolve via lvalueCType. Release the target's old buffer, then: from a
        // NAMED source require a marker (bare→error, like the decl handler) — `give` moves (relocate struct
        // + null source + markMoved), `copy` deep-copies (`__copy`); a FRESH owned rvalue moves in bare.
        // Without this branch a named RHS aliases (double-free) and a fresh-rvalue RHS leaks the old buffer.
        if (as->token == EQ) {
            std::string lty = lvalueCType(as->unaryExpression);
            if (ownsByValue(lty) && _classes.count(lty) && _classes[lty].isIntrinsicColl) {
                SharedExpression rhs = as->expression;
                int handoff = 0;
                if (auto* h = dynamic_cast<HandoffNode*>(rhs.get())) { handoff = h->isGive ? 1 : 2; rhs = h->value; }
                checkConstWrite(as->unaryExpression, n->line);
                std::string lname;
                if (auto* lid = dynamic_cast<IdentifierNode*>(as->unaryExpression.get()))
                    if (lid->value && (!lid->qualifier || lid->qualifier->empty())) lname = *lid->value;

                if (isNamedValue(rhs.get())) {
                    if (handoff == 0)
                        unsupported("a collection/`string` hand-off must say `give` (move) or `copy` (deep) "
                                    "— write `… = give …` or `… = copy …`, or pass by `ref` to borrow", n->line);
                    std::string rname;
                    if (auto* rid = dynamic_cast<IdentifierNode*>(rhs.get()))
                        if (rid->value && (!rid->qualifier || rid->qualifier->empty())) rname = *rid->value;
                    if (handoff == 2) {                                   // copy = deep copy via __copy
                        auto ci = _collections.find(lty);
                        if (ci != _collections.end() && ci->second.elemDestructible && !ci->second.elemCopyable)
                            unsupported(("`copy` of a `" + lty + "` needs copyable elements — its elements own "
                                         "resources but aren't `Copyable`; use `give` to move").c_str(), n->line);
                        if (!lname.empty() && rname == lname) return;     // self-copy: no-op
                        std::string b = emitExpression(as->unaryExpression), src = emitExpression(rhs);
                        line(n->line);
                        indent(depth); *_out << lty << "__dtor(&" << b << ");\n";
                        indent(depth); *_out << b << " = " << lty << "__copy(&(" << src << "));\n";
                        return;
                    }
                    // give = move: relocate the struct, then null the source so its scope-drop is a no-op.
                    std::string mv = moveOnlySource(rhs, n->line);
                    if (!lname.empty() && !mv.empty() && mv == lname)
                        unsupported("handing a value onto itself would use it after it was dropped", n->line);
                    if (!lname.empty()) _moveState[lname] = MoveState::NotMoved;   // target live again
                    std::string b = emitExpression(as->unaryExpression), src = emitExpression(rhs);
                    line(n->line);
                    indent(depth); *_out << lty << "__dtor(&" << b << ");\n";
                    indent(depth); *_out << b << " = " << src << ";\n";
                    indent(depth); *_out << "(" << src << ").data = NULL; (" << src << ").len = 0;\n";
                    if (!mv.empty()) markMoved(mv);
                    return;
                }
                // Fresh owned rvalue (concat/substring/literal): the RHS may READ the old LHS
                // (`s = s.concat(…)`), so hoist it into a temp BEFORE releasing the old buffer, then assign.
                std::string b = emitExpression(as->unaryExpression);
                std::string rv = emitExpression(as->expression);
                std::string t = "__asgn" + std::to_string(_tempCounter++);
                line(n->line);
                indent(depth); *_out << lty << " " << t << " = " << rv << ";\n";
                indent(depth); *_out << lty << "__dtor(&" << b << ");\n";
                indent(depth); *_out << b << " = " << t << ";\n";
                return;
            }
        }
    }

    // `lhs = match(…)` / `lhs = Optional::Some(…)` — a value-producing RHS (a value-producing
    // `match`, or a generic-variant construction that needs its instance from context) needs the LHS's
    // C type threaded. The assignment paths above don't handle these RHS kinds; this fires ONLY for
    // them (a match or a `Union::Variant` construction), so ordinary assignments are untouched.
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        if (as->token == EQ) {
            ASTNode* r = as->expression.get();
            bool isMatch = dynamic_cast<MatchNode*>(r) != nullptr;
            // an array literal RHS (`v = [1,2,3]`) is value-producing too — it needs the LHS Fixed
            // type threaded so the compound literal / `__fill` names the right struct.
            bool isArrayLit = dynamic_cast<ArrayLiteralNode*>(r) != nullptr;
            bool isVariantCtor = false;
            if (!isMatch && !isArrayLit) {
                SharedStringList qual;
                if (auto* iv = dynamic_cast<InvocationNode*>(r)) { if (iv->identifier) qual = iv->identifier->qualifier; }
                else if (auto* id = dynamic_cast<IdentifierNode*>(r)) qual = id->qualifier;
                if (qual && !qual->empty()) {
                    auto q = std::make_shared<StringList>();
                    for (size_t i = 0; i + 1 < qual->size(); ++i) q->push_back((*qual)[i]);
                    std::string en = resolveUserName(*qual->back(), q);
                    isVariantCtor = (_classes.count(en) && _classes[en].isVariant) || _genericTypeParams.count(en);
                }
            }
            if (isMatch || isVariantCtor || isArrayLit) {
                std::string lhsCType = exprClass(as->unaryExpression);      // class/union name
                std::string lname;
                if (auto* lid = dynamic_cast<IdentifierNode*>(as->unaryExpression.get()))
                    if (lid->value && (!lid->qualifier || lid->qualifier->empty())) {
                        lname = *lid->value;
                        if (lhsCType.empty() && _localCTypes.count(lname)) lhsCType = _localCTypes[lname];   // primitive/Fixed LHS
                    }
                if (lhsCType.empty()) {
                    unsupported("a value-producing `match` / variant construction here needs a typed "
                                "assignment target — assign to a plain local", n->line);
                    *_out << "\n"; return;
                }
                checkConstWrite(as->unaryExpression, n->line);
                bool destructible = _classes.count(lhsCType) && _classes[lhsCType].destructible;
                bool bMoved = (!lname.empty() && _moveState.count(lname) && _moveState[lname] == MoveState::Moved);
                std::string lhs = emitExpression(as->unaryExpression);
                line(n->line);
                bool ph = _hoistOK; _hoistOK = true;
                std::string pm = _matchTargetCType, pv = _variantTargetType;
                _matchTargetCType = _variantTargetType = lhsCType;
                std::string rv = emitExpression(as->expression);
                _matchTargetCType = pm; _variantTargetType = pv;
                _hoistOK = ph;
                flushHoisted(depth);
                // RAII: drop a live destructible LHS before the blit (its owned resource would leak).
                if (destructible && !bMoved) { indent(depth); *_out << lhsCType << "__dtor(&" << lhs << ");\n"; }
                if (!lname.empty()) _moveState[lname] = MoveState::NotMoved;   // target is live again
                indent(depth); *_out << lhs << " = " << rv << ";\n";
                return;
            }
        }
    }

    // Bare expression statement (e.g. an assignment or call used as a statement).
    // a statement-position `match` (value discarded) — emit the switch directly, not as an
    // expression (which would try to lift a result temp). Must precede the generic expr-statement path.
    if (auto* mm = dynamic_cast<MatchNode*>(n)) { emitMatchStatement(mm, depth); return; }

    if (dynamic_cast<ExpressionStatementNode*>(n)) {
        line(n->line);
        bool ph = _hoistOK; _hoistOK = true;                       // allow inline-ctor hoisting
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

std::string CEmitter::cFunctionName(const std::string& kamaName)
{
    // The user `main` becomes `kama_main`; a real C `main` wrapper is synthesized.
    return (kamaName == "main") ? "kama_main" : kamaName;
}

std::string CEmitter::mangledFunctionName(FunctionDeclarationNode* fn, bool& isEntryPoint)
{
    std::string name = (fn->name && fn->name->value) ? *fn->name->value : "anon";
    isEntryPoint = (name == "main");
    return qualify(name);   // scope-prefixed (main -> kama_main); _nsCtx set per file
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

        if (fn->modifier && fn->modifier->value && *fn->modifier->value == "expose")
            unsupported("`expose` is reserved (WASM/host export boundary) but not yet implemented", fn->line);

        // A bodiless top-level `fn ret Name(params);` (no body, not extern) is a
        // function-pointer SIGNATURE type, not a callable — register in _sigs.
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
        // namespace-mangle). Others are scope-prefixed (main -> kama_main).
        sig.cName   = isExtern(fn) ? *fn->name->value : qualify(*fn->name->value);
        sig.retCType = cType(fn->returnType);
        sig.params  = paramSigsOf(fn->parameters);
        sig.isPlaceReturn = fn->isRef;   // `fn ref T …` — the call site derefs the returned place
        _funcs[sig.cName] = sig;

        // a generic template (`fn max<T>(…)`) is registered for monomorphization and is
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
        // `type contract C { … }` — a ClassDeclarationNode whose kind word is "contract".
        // Its methods parse as (bodiless) class methods; register them as a contract's slots.
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->typeKind || *cd->typeKind != "contract" || !cd->name || !cd->name->value) continue;
        InterfaceInfo ii;
        ii.name = qualify(*cd->name->value); ii.scope = _nsCtx.scope; ii.usings = _nsCtx.usings;
        ii.symbolAliases = _nsCtx.symbolAliases;   // so contract sigs can name imported/library-generic types
        // Kind-gate: `for value|resource|both` is MANDATORY on a contract — the designer must state which
        // kinds may implement it (`both` / listing both = either).
        if (cd->forKinds) for (auto& k : *cd->forKinds) {
            if (!k) continue;
            if      (*k == "value")    ii.allowsValue = true;
            else if (*k == "resource") ii.allowsResource = true;
            else if (*k == "both")     { ii.allowsValue = true; ii.allowsResource = true; }
            else unsupported(("a contract's `for` clause takes `value`, `resource`, or `both` — not `"
                              + *k + "`").c_str(), cd->line);
        }
        if (!ii.allowsValue && !ii.allowsResource)
            unsupported(("contract `" + *cd->name->value + "` must declare which kinds may implement it: "
                         "`type contract " + *cd->name->value + " for value|resource|both { … }`").c_str(), cd->line);
        if (cd->members)
            for (auto& m : *cd->members) {
                // a `contract` is a public guarantee: methods only, no bodies, no fields, no
                // ctor/dtor (it holds no state and constructs nothing).
                if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get())) {
                    if (md->body)
                        unsupported(("a `contract` method (`" + (md->name && md->name->value ? *md->name->value : std::string())
                                     + "`) has no body — it is a guarantee, not an implementation").c_str(), md->line);
                    if (md->name && md->name->value)
                        ii.methods.push_back({*md->name->value, md->returnType, md->params, md->isRef});
                } else if (auto* od = dynamic_cast<ClassOperatorDeclarationNode*>(m.get())) {
                    // an operator in a `contract` is a bound for generic math: `IArithmetic`
                    // declares `This operator+(This rhs)`. Register it under the SAME synthetic name
                    // (`op_add`) the concrete class's `operator+` uses, so classSatisfiesBound matches
                    // it structurally and the monomorphized `a + b` lowers to `Concrete__op_add(&a, b)`.
                    if (od->body)
                        unsupported("a `contract` operator has no body — it is a guarantee, not an implementation", od->line);
                    auto* d = od->operatorDeclarator.get();
                    int arity = (d->param1Type ? 1 : 0) + (d->param2Type ? 1 : 0);
                    // owner="" — a `This` param is same-type → the bare name `op_add`, matching the concrete
                    // class's same-type operator; a different-type contract operand keeps its `__<Type>` suffix.
                    std::string opName = operatorName(d->opToken, arity, d->param1Type, "");
                    if (opName.empty())
                        unsupported((std::string("operator '") + binaryOperator(d->opToken)
                                     + "' has no " + (arity == 0 ? "unary" : "binary") + " form").c_str(), od->line);
                    ii.methods.push_back({opName, d->returnType, operatorParamList(d)});
                } else if (dynamic_cast<ClassFieldDeclarationNode*>(m.get()) || dynamic_cast<ClassConstDeclarationNode*>(m.get())) {
                    unsupported(("a `contract` holds no state — remove the field from `" + ii.name + "`").c_str(), m->line);
                } else if (dynamic_cast<ClassConstructorDeclarationNode*>(m.get()) || dynamic_cast<ClassDestructorDeclarationNode*>(m.get())) {
                    unsupported(("a `contract` has no constructor/destructor — `" + ii.name + "` is a guarantee").c_str(), m->line);
                }
            }
        // a generic CONTRACT template (`type contract Iterator<T>`) is kept OUT of _interfaces — its
        // method sigs name the raw `T`, so an eager vtable would be bogus. Its shape is parked in
        // _genericContracts and specialized per concrete `Iterator<Arg>` at discovery (mirrors the
        // generic-TYPE divert in collectClasses).
        // the prelude `Deref<T>` gates auto-deref — remember its resolved name (by source name, so a
        // user's own `Deref` in a namespace still matches). Empty if no `Deref` is in scope → inert.
        if (cd->name && cd->name->value && *cd->name->value == "Deref") _derefContract = ii.name;
        // the prelude `HeapOwner<T>` — a type implementing it is a `new` placement target (via `adopt`).
        if (cd->name && cd->name->value && *cd->name->value == "HeapOwner") _heapOwnerContract = ii.name;
        // the prelude ownership markers — `Movable` (implicit on resources; `!Movable` subtracts it) and
        // `Copyable` (duplicable). Recognized by source name so the give/copy discipline can consult them.
        if (cd->name && cd->name->value && *cd->name->value == "Movable")  _movableContract  = ii.name;
        if (cd->name && cd->name->value && *cd->name->value == "Copyable") _copyableContract = ii.name;
        // Refinement: `type contract Animated implements Drawable` — record the parent contract names
        // (resolved in this contract's scope); linkContracts() merges their methods in transitively.
        if (cd->baseTypes && cd->baseTypes->interfaces)
            for (auto& p : *cd->baseTypes->interfaces)
                if (p && p->value) ii.refines.push_back(resolveUserName(*p->value, p->qualifier));
        if (cd->typeParams && !cd->typeParams->empty()) {
            std::vector<std::string> ps;
            for (auto& p : *cd->typeParams) if (p) ps.push_back(*p);
            _genericContractParams[ii.name] = ps;
            _genericContractCtx[ii.name]    = _nsCtx;
            _genericContracts[ii.name]      = ii;
        } else {
            _interfaces[ii.name] = ii;
        }
    }
}

// Collect enum declarations.
// an `enum` is a tagged union (discriminated union) if any variant carries a payload, or it
// is generic (parameterized, so it can't be a bare C `enum`). A plain, non-generic, payloadless enum
// stays a C integer.
static bool enumIsTagged(EnumDeclarationNode* ed)
{
    if (ed->typeParams && !ed->typeParams->empty()) return true;
    if (ed->body)
        for (auto& m : *ed->body)
            if (m->payload && !m->payload->empty()) return true;
    return false;
}

// Build the ClassInfo backing a payload/generic enum — a discriminant tag + a union of the
// per-variant payloads. Reuses ClassInfo so monomorphization, RAII, and move analysis all apply.
// A variant is flagged `isVariant`; its move-only-ness follows destructibility (owns a resource →
// moves), not the `kind` (which stays the neutral `Intrinsic`).
ClassInfo CEmitter::buildVariantClassInfo(EnumDeclarationNode* ed, const std::string& name)
{
    ClassInfo ci;
    ci.name      = name;
    ci.kind      = TypeKind::Intrinsic;   // neutral; move-only-ness is driven by isVariant + destructible
    ci.scope     = _nsCtx.scope;
    ci.usings    = _nsCtx.usings;
    ci.symbolAliases = _nsCtx.symbolAliases;
    ci.isVariant = true;
    ci.tagCType  = ed->underlyingType ? cType(ed->underlyingType) : "";
    if (ed->body)
        for (auto& m : *ed->body) {
            if (!m->identifier || !m->identifier->value) continue;
            VariantCase vc;
            vc.name = *m->identifier->value;
            if (m->payload)
                for (auto& p : *m->payload)
                    if (p && p->identifier && p->identifier->value) {
                        FieldInfo fi;
                        fi.name = *p->identifier->value;
                        fi.type = p->type;
                        vc.payload.push_back(fi);
                    }
            ci.variants.push_back(vc);
        }
    return ci;
}

void CEmitter::collectEnums(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* ed = dynamic_cast<EnumDeclarationNode*>(decl.get());
        if (!ed || !ed->identifier || !ed->identifier->value) continue;
        std::string name = qualify(*ed->identifier->value);

        if (enumIsTagged(ed)) {
            // a payload/generic enum is a discriminated union backed by a ClassInfo.
            ClassInfo ci = buildVariantClassInfo(ed, name);
            if (ed->typeParams && !ed->typeParams->empty()) {
                // Generic enum (Optional<T>): a monomorphization TEMPLATE, kept OUT of _classes —
                // each `Optional<Arg>` becomes a specialized ClassInfo at discovery (registerGenericTypeInst).
                std::vector<std::string> ps;
                for (auto& p : *ed->typeParams) if (p) ps.push_back(*p);
                _genericTypeParams[name] = ps;
                _genericTypeBounds[name] = ed->typeBounds;
                _genericTypeCtx[name]    = _nsCtx;
                _genericTypes[name]      = ci;
            } else {
                // `@generate` serialization for a concrete tagged enum: register the Serialize/Deserialize
                // conformance + synthesized methods so the emitter emits `E__serialize`/`E__deserialize` in C
                // (externally-tagged `{"tag":…[,"value":{…}]}`). An enum can't carry a fat-pointer method, so
                // the conformance is nominal-only (retroInterfaces → static dispatch, satisfies a `<T: Serialize>`
                // bound + resolves `e.serialize()`); the body is emitted separately (emitEnumSerializeDefinition).
                bool eser = false, ede = false;
                if (ed->attributes)
                    for (auto& at : *ed->attributes) {
                        if (!at || !at->name || *at->name != "generate" || !at->args) continue;
                        for (auto& a : *at->args)
                            if (a && a->name && a->name->value && !a->expression) {
                                if      (*a->name->value == "Serialize")   eser = true;
                                else if (*a->name->value == "Deserialize") ede = true;
                                else if (*a->name->value == "noOnConstruction") { /* n/a to an enum; tolerated */ }
                                else unsupported("`@generate(...)` on an enum accepts only Serialize, Deserialize", ed->line);
                            }
                    }
                if (eser) {
                    ci.genSerialize = true;
                    ci.interfaces.push_back("Serialize"); ci.retroInterfaces.push_back("Serialize");
                    MethodInfo mi; mi.cName = name + "__serialize"; mi.visibility = Visibility::Public;
                    mi.isSynthSer = true; mi.returnType = primTypeNode(IDENTIFIER_VOID_VAL);
                    ParamSig w; w.name = "w"; w.byRef = true; w.className = "Serializer"; mi.params.push_back(w);
                    ci.methods["serialize"] = mi;
                }
                // Deserialize needs a payload-less variant as the unknown-tag fallback (else defer, as before).
                bool hasUnit = false;
                for (auto& v : ci.variants) if (v.payload.empty()) { hasUnit = true; break; }
                if (ede && hasUnit) {
                    ci.genDeserialize = true;
                    ci.interfaces.push_back("Deserialize"); ci.retroInterfaces.push_back("Deserialize");
                    MethodInfo mi; mi.cName = name + "__deserialize"; mi.visibility = Visibility::Public;
                    mi.isStatic = true; mi.isSynthDe = true; mi.returnType = ed->identifier;
                    ParamSig r; r.name = "r"; r.byRef = false; r.className = "Deserializer"; mi.params.push_back(r);
                    ci.methods["deserialize"] = mi;
                }
                _classes[name] = ci;
            }
            continue;
        }

        // Plain C-style enum — the existing lightweight path (bare integer, zero regression).
        EnumInfo ei;
        ei.name  = name;
        ei.scope = _nsCtx.scope;
        ei.usings = _nsCtx.usings;
        ei.underlyingCType = ed->underlyingType ? cType(ed->underlyingType) : "";   // `: IntType`
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
    // `enum Name : IntType` pins the value to a fixed-width integer. ISO C can't set an enum's
    // underlying type, so emit `typedef <ctype> Name;` + an anonymous enum carrying the constants.
    if (!ei.underlyingCType.empty()) {
        *_out << "typedef " << ei.underlyingCType << " " << ei.name << ";\n";
        *_out << "enum {\n";
        for (auto& m : ei.members) {
            indent(1);
            *_out << ei.name << "_" << m.name;
            if (m.value) *_out << " = " << emitExpression(m.value);
            *_out << ",\n";
        }
        *_out << "};\n\n";
        return;
    }
    *_out << "typedef enum " << ei.name << " {\n";
    for (auto& m : ei.members) {
        indent(1);
        *_out << ei.name << "_" << m.name;
        if (m.value) *_out << " = " << emitExpression(m.value);
        *_out << ",\n";
    }
    *_out << "} " << ei.name << ";\n\n";
}

// bind `_thisType` (what `This` resolves to) for a scope, restoring on exit (survives early
// returns / loop `continue`s). Used across signature collection and class/interface emission.
namespace { struct ScopedStr { std::string& s; std::string prev;
    ScopedStr(std::string& s_, const std::string& v) : s(s_), prev(s_) { s = v; }
    ~ScopedStr() { s = prev; } }; }

// Build the class table: ordered fields, methods, and the (single) constructor.
void CEmitter::collectClasses(SharedCompilationUnit unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->name || !cd->name->value) continue;

        // `type <kind> Name` — map the kind word. `contract` is registered as an interface
        // (collectInterfaces), so skip it here; a bad kind word is a clear error.
        TypeKind kind = TypeKind::Intrinsic;
        if (cd->typeKind) {
            if      (*cd->typeKind == "value")    kind = TypeKind::Value;
            else if (*cd->typeKind == "resource") kind = TypeKind::Resource;
            else if (*cd->typeKind == "contract") continue;   // handled as an interface
            else unsupported(("unknown type kind `" + *cd->typeKind
                              + "` — expected `value`, `resource`, or `contract`").c_str(), cd->line);
        }
        // A `for value|resource|both` clause gates a CONTRACT's implementers — meaningless on a value/resource.
        if (cd->forKinds && !cd->forKinds->empty())
            unsupported("a `for value|resource|both` clause applies only to a `type contract`", cd->line);

        // FFI: an `extern class`/`extern value` is an external C struct — keep its literal
        // C name (not namespace-mangled) and don't emit/own it.
        bool isExt = false;
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers)
                if (mod->value && *mod->value == "extern") isExt = true;

        ClassInfo ci;
        ci.name  = isExt ? *cd->name->value : qualify(*cd->name->value);   // scope-mangle / FFI literal name
        ci.kind  = kind;
        // A non-generic prelude type (e.g. `Chars`, the codepoint iterator) has method bodies but no
        // owning module to emit them — flag it so the header emits them static-inline.
        if (unit == _preludeUnit && (!cd->typeParams || cd->typeParams->empty())) ci.preludeStatic = true;
        ci.scope = _nsCtx.scope;
        ci.usings = _nsCtx.usings;
        ci.symbolAliases = _nsCtx.symbolAliases;
        ci.isExternStruct = isExt;
        if (isExt) _externNames.insert(ci.name);
        ci.node = cd;

        // `@generate(Serialize, Deserialize)` — opt this type into serialization codegen (pay-for-what-you-
        // use: only a marked type gets the reflective helpers). Only the type-level `@generate` is valid
        // here; per-field attributes are read in the member loop below.
        if (cd->attributes)
            for (auto& at : *cd->attributes) {
                if (!at || !at->name) continue;
                if (*at->name == "generate") {
                    if (!at->args || at->args->empty())
                        unsupported("`@generate(...)` needs at least one of Serialize, Deserialize", cd->line);
                    else for (auto& a : *at->args) {
                        // bare identifier args only (Serialize/Deserialize); a `key: value` form is invalid here
                        std::string which = (a && a->name && a->name->value && !a->expression) ? *a->name->value : "";
                        if      (which == "Serialize")   ci.genSerialize = true;
                        else if (which == "Deserialize") ci.genDeserialize = true;
                        else if (which == "noOnConstruction") ci.serNoOnConstruction = true;   // opt out of the hook rule
                        else unsupported("`@generate(...)` accepts only Serialize, Deserialize, noOnConstruction", cd->line);
                    }
                } else {
                    unsupported(("unknown type attribute `@" + *at->name + "` (expected `@generate`)").c_str(), cd->line);
                }
            }

        ScopedStr _ts(_thisType, ci.name);   // `This` -> this class in its collected method sigs

        // Single inheritance (extends). Base/interface names are RESOLVED in
        // linkBases() once every file's declarations are registered.
        if (cd->baseTypes) {
            if (cd->baseTypes->base && cd->baseTypes->base->value)
                ci.baseName = *cd->baseTypes->base->value;   // bare; resolved in linkBases
            if (cd->baseTypes->interfaces)
                for (auto& itf : *cd->baseTypes->interfaces) {
                    if (!itf || !itf->value) continue;
                    ci.interfaces.push_back(*itf->value);  // bare; resolved in linkBases
                    // Copyable is NOMINAL: `implements Copyable(bare: give|copy)` is the opt-in (a lone
                    // `copy()` no longer implies it). Recognized by name — the capability markers are
                    // compiler-owned. The `(bare: …)` parameter is the mandatory bare-hand-off default;
                    // its presence + the `copy()` method are validated after the member loop below.
                    if (*itf->value == "Copyable") {
                        ci.copyable = true; ci.bareDefault = itf->bareDefault;
                        if (itf->whenParams)   // conditional: `… when [P: B, …]` — record each gated param+bound
                            for (size_t c = 0; c < itf->whenParams->size(); ++c) {
                                auto& p = (*itf->whenParams)[c];
                                auto& b = itf->whenBounds ? (*itf->whenBounds)[c] : p;
                                ci.copyableWhenParams.push_back(p && p->value ? *p->value : "");
                                ci.copyableWhenBounds.push_back(b && b->value ? *b->value : "Copyable");
                            }
                    }
                }
        }
        // Class-level extensibility modifier: virtual | abstract | final.
        if (cd->modifiers)
            for (auto& mod : *cd->modifiers) {
                if (!mod->value) continue;
                const std::string& mv = *mod->value;
                if (mv == "abstract") ci.isAbstractClass = true;
                else if (mv == "virtual")  ci.isVirtualClass = true;
                else if (mv == "final")    ci.isFinalClass = true;
                else if (mv == "expose")
                    unsupported("`expose` is reserved (WASM/host export boundary) but not yet implemented", cd->line);
                else if (mv == "volatile")
                    unsupported("`volatile` is reserved (embedded/MMIO) but not yet implemented", cd->line);
            }
        // A plain `value`/`resource` type is sealed: only virtual/abstract/final classes may extend a base.
        // (The base must itself be extensible — checked in linkBases once names resolve.)
        if (!ci.baseName.empty() && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
            unsupported(("class '" + ci.name + "' extends '" + ci.baseName
                         + "'; only a `virtual`/`abstract`/`final class` may extend").c_str(), cd->line);
        // `virtual`/`abstract`/`final` are qualifiers on a `resource` (extensible owned
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
                            if (*mod->value == "expose")
                                unsupported("`expose` is reserved (WASM/host export boundary) but not yet implemented", fd->line);
                        }
                    // A `value` picks field visibility PER FIELD (default private, `public` allowed;
                    // `protected` belongs to an extensible `resource`). A `resource` field is always
                    // private (ownership encapsulated). An extern struct's fields are public — the
                    // FFI struct owns its layout.
                    Visibility fvis = fieldVisibility(ci, fd->modifiers, fd->line);

                    // Serialization field metadata (`@field` / `@field(name: "…")` / `@skip` / `@bits(n)`).
                    // On a `@generate`d type EVERY field must be marked `@field` or `@skip` — an unmarked
                    // field is a compile error, so adding a field always forces an explicit in/out decision.
                    bool serGen = ci.genSerialize || ci.genDeserialize;
                    bool fMarked = false, fSkip = false; std::string fName;
                    if (fd->attributes)
                        for (auto& at : *fd->attributes) {
                            if (!at || !at->name) continue;
                            const std::string& an = *at->name;
                            if (an == "skip") { fSkip = true; fMarked = true; }
                            else if (an == "bits") { fMarked = true; /* binary-only packing hint; ignored by JSON/YAML */ }
                            else if (an == "field") {
                                fMarked = true;
                                if (at->args)
                                    for (auto& a : *at->args) {
                                        if (a && a->name && a->name->value && *a->name->value == "name" && a->expression) {
                                            if (auto* sn = dynamic_cast<StringNode*>(a->expression.get()))
                                                fName = sn->value ? *sn->value : "";
                                            else unsupported("`@field(name: …)` needs a string literal", fd->line);
                                        } else {
                                            unsupported("`@field(...)` accepts only `name: \"…\"`", fd->line);
                                        }
                                    }
                            }
                            else unsupported(("unknown field attribute `@" + an + "`").c_str(), fd->line);
                        }
                    if (!serGen && fd->attributes && !fd->attributes->empty())
                        unsupported("field attributes (`@field`/`@skip`) require `@generate(...)` on the type", fd->line);
                    if (serGen && !fMarked)
                        unsupported("every field of a `@generate`d type must be marked `@field` or `@skip`", fd->line);
                    // Smart-pointer fields = object-graph mode: `Shared`/`Weak`/`Owned` serialize as integer ids
                    // into the `{root,objects}` id table and deserialize via the two-pass graph intrinsic (Phase
                    // D). `Bindable` (a bound callback) is never serializable.
                    if (serGen && !fSkip && fd->type && fd->type->value) {
                        const std::string& tn = *fd->type->value;
                        if (tn == "Bindable")
                            unsupported("a `Bindable<…>` field can't be serialized — `@skip` it and rebind in "
                                        "`onConstruction`", fd->line);
                    }

                    if (fd->declarators) {
                        for (auto& d : *fd->declarators) {
                            FieldInfo fi;
                            fi.name        = (d->name && d->name->value) ? *d->name->value : "";
                            fi.type        = fd->type;
                            fi.initializer = d->initializer;
                            fi.visibility  = fvis;
                            fi.serSkip     = fSkip;
                            fi.serName     = fName;
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
                        mi.isConst    = md->isConst;   // `const fn …`
                        if (md->whenParams)   // `fn … when [P: B, …]` — conditional method (AND of all)
                            for (size_t c = 0; c < md->whenParams->size(); ++c) {
                                auto& p = (*md->whenParams)[c];
                                auto& b = md->whenBounds ? (*md->whenBounds)[c] : p;
                                mi.whenParams.push_back(p && p->value ? *p->value : "");
                                mi.whenBounds.push_back(b && b->value ? *b->value : "Copyable");
                            }
                        mi.isPlaceReturn = md->isRef;  // `fn ref T …` — returns a place (T*), like `operator[]`
                        mi.visibility = visibilityOf(md->modifiers, Visibility::Private, md->line);
                        // `protected` belongs to a `resource` in an extensibility hierarchy
                        // (`virtual`/`abstract` declares protected members for subclasses; a `final`
                        // override still uses `protected` by NVI). It's meaningless on a `value`, a
                        // plain sealed `resource`, or a `contract` — those members are private/public.
                        if (mi.visibility == Visibility::Protected
                            && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
                            unsupported(("`protected` belongs to a `virtual`/`abstract`/`final resource` — `" + ci.name
                                         + "` is a plain `value`/`resource`, so its members are `private` or `public`").c_str(), md->line);
                        mi.isFinal    = modHas(md->modifiers, "final");
                        if (md->modifiers)
                            for (auto& mod : *md->modifiers) {
                                if (!mod->value) continue;
                                if (*mod->value == "virtual")  mi.isVirtual = true;
                                if (*mod->value == "override") { mi.isVirtual = true; mi.isOverride = true; }
                                if (*mod->value == "abstract") { mi.isVirtual = true; mi.isAbstract = true; }
                                if (*mod->value == "static")   mi.isStatic = true;   // no implicit `self`
                                if (*mod->value == "expose")
                                    unsupported("`expose` is reserved (WASM/host export boundary) but not yet implemented", md->line);
                                if (*mod->value == "volatile")
                                    unsupported("`volatile` is reserved (embedded/MMIO) but not yet implemented", md->line);
                            }
                        // a `static` method has no `this`: no vtable slot, must have a body.
                        if (mi.isStatic) {
                            if (mi.isVirtual)
                                unsupported("a `static` method has no `this` — it can't be `virtual`/`override`/`abstract`", md->line);
                            if (!md->body)
                                unsupported("a `static` method needs a body", md->line);
                        }
                        if (!md->body) mi.isAbstract = mi.isVirtual = true;   // null body => pure
                        // polymorphism rules.
                        if (mi.isVirtual) {
                            const std::string& mname = *md->name->value;
                            const char* kw = mi.isAbstract ? "abstract" : mi.isOverride ? "override" : "virtual";
                            // (3) an overridable method is written `protected` (never public/private):
                            //     a private virtual can't be overridden, a public one is the interface's job.
                            if (mi.visibility != Visibility::Protected)
                                unsupported(("overridable method '" + mname + "' must be declared `protected` (write `protected "
                                    + kw + "`); public polymorphism belongs on a contract").c_str(), md->line);
                            // (4a) the class kind must opt in to the method's polymorphism.
                            if (mi.isAbstract && !ci.isAbstractClass)
                                unsupported(("class '" + ci.name + "' declares an abstract method; declare it `abstract class`").c_str(), md->line);
                            else if (mi.isOverride && !(ci.isVirtualClass || ci.isAbstractClass || ci.isFinalClass))
                                unsupported(("class '" + ci.name + "' overrides a method; declare it `virtual`/`abstract`/`final class`").c_str(), md->line);
                            else if (!mi.isAbstract && !mi.isOverride && !ci.isVirtualClass && !ci.isAbstractClass)
                                unsupported(("class '" + ci.name + "' declares a virtual method; declare it `virtual class`").c_str(), md->line);
                        }
                        // `final` seals a virtual slot; reject the meaningless/contradictory cases.
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
                    ci.ctorVisibility = visibilityOf(cc->modifiers, Visibility::Private, cc->line);
                    if (cc->declarator) ci.ctorParams = paramSigsOf(cc->declarator->params);
                } else if (auto* dd = dynamic_cast<ClassDestructorDeclarationNode*>(mn)) {
                    // `~dtor` ⟺ `resource`. A `value` owns nothing, so a destructor makes it
                    // a resource; that disagreement is the lesson in the message.
                    if (ci.kind == TypeKind::Value)
                        unsupported("a `value` owns nothing — a `~dtor` makes it a `resource`; "
                                    "declare it `type resource`", dd->line);
                    ci.hasDtor  = true;
                    ci.dtorNode = dd;
                } else if (auto* kd = dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    // a `const` data member — a normal struct field, written
                    // ONCE in the constructor (inline init or `this.f = …`), then
                    // immutable. Enforcement is at the kama level; the C field is plain.
                    // visibility follows the same per-field rule (see fieldVisibility).
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
                } else if (auto* od = dynamic_cast<ClassOperatorDeclarationNode*>(mn)) {
                    // an operator overload registers as a method under a synthetic name
                    // (`op_add`/`op_neg`/…), disambiguated by arity: 0 params = unary-on-`this`,
                    // 1 = binary method (`this`+rhs), 2 = binary free form (both operands explicit).
                    auto* d = od->operatorDeclarator.get();
                    int arity = (d->param1Type ? 1 : 0) + (d->param2Type ? 1 : 0);
                    std::string opName = operatorName(d->opToken, arity, d->param1Type, ci.name);   // type-suffixed
                    if (opName.empty())
                        unsupported((std::string("operator '") + binaryOperator(d->opToken)
                                     + "' has no " + (arity == 0 ? "unary" : "binary") + " form").c_str(), od->line);
                    if (!od->body)
                        unsupported("an operator needs a body", od->line);
                    // No overloading BY ARITY (a stated non-goal). Operators MAY distinguish operand types
                    // (`mat*vec` vs `mat*mat` — the sanctioned exception, since `a*b` can't take named args),
                    // so a duplicate is only two operators with the same symbol AND operand type (same name).
                    if (ci.methods.count(opName))
                        unsupported((std::string("duplicate operator '") + binaryOperator(d->opToken)
                                     + "' on '" + ci.name + "' for the same operand type").c_str(), od->line);
                    MethodInfo mi;
                    mi.cName      = ci.name + "__" + opName;
                    mi.returnType = d->returnType;
                    mi.params     = paramSigsOf(operatorParamList(d));
                    mi.node       = nullptr;                 // an operator is NOT a ClassMethodDeclarationNode
                    mi.opDecl     = od;
                    mi.isOperator = true;
                    mi.arity      = arity;
                    mi.isStatic   = (arity == 2);            // the free form takes no `this`
                    mi.isPlaceReturn = d->refReturn;         // `ref T operator[]` — the result is a place (T*)
                    mi.visibility = visibilityOf(od->modifiers, Visibility::Public, od->line);   // operators are public by nature
                    ci.methods[opName] = mi;
                } else if (auto* fg = dynamic_cast<FriendGrantNode*>(mn)) {
                    // capture the grant raw; the accessor is resolved (against the
                    // full function/class tables) in resolveFriends() once all units load.
                    RawFriendGrant rg; rg.accessor = fg->accessor; rg.line = fg->line;
                    if (fg->members)                                  // null => `[...]` (all privates)
                        for (auto& m : *fg->members)
                            if (m && m->value) rg.members.insert(*m->value);
                    ci.friendGrantsRaw.push_back(rg);
                }
            }
        }
        // a `virtual`/`abstract class` must actually declare an overridable method
        // (else the qualifier is a lie); the reverse of rule 4a.
        if (ci.isVirtualClass || ci.isAbstractClass) {
            bool hasOverridable = false;
            for (auto& kv : ci.methods) if (kv.second.isVirtual) { hasOverridable = true; break; }
            if (!hasOverridable)
                unsupported(("`" + std::string(ci.isAbstractClass ? "abstract" : "virtual") + " class` '"
                             + ci.name + "' declares no overridable (virtual/abstract) method").c_str(), cd->line);
        }
        // `implements Copyable(bare: give|copy)` is the nominal opt-in. It obliges a resource to supply
        // BOTH the `copy()` method (a public nullary `fn This copy()`) AND the `bare:` contract
        // parameter (the bare-hand-off default). A value is copyable implicitly (bitwise) — it needs
        // neither, and its bare-default is a copy.
        if (ci.copyable && ci.kind != TypeKind::Value) {
            auto cm = ci.methods.find("copy");
            if (cm == ci.methods.end() || cm->second.visibility != Visibility::Public || !cm->second.params.empty())
                unsupported(("`" + ci.name + "` implements `Copyable` but has no public nullary `copy()` method "
                             "(the contract is `fn This copy()`)").c_str(), cd->line);
            if (ci.bareDefault == 0)
                unsupported(("`" + ci.name + "` implements `Copyable` but doesn't declare its bare-hand-off default "
                             "— write `implements Copyable(bare: give)` or `Copyable(bare: copy)`").c_str(), cd->line);
        }
        // A `@generate(Deserialize)` type has its constructor BYPASSED during deserialization (the struct is
        // zero-initialized and its fields populated in place). So any "runs on every construction" logic must
        // live in an `onConstruction()` hook — the compiler calls it at the end of the constructor AND after a
        // deserialize field-set. To keep that decision explicit, such a type MUST either define
        // `fn void onConstruction()` or opt out with `@generate(Deserialize, noOnConstruction)`.
        if (ci.genDeserialize && !ci.serNoOnConstruction && !ci.methods.count("onConstruction"))
            unsupported(("`" + ci.name + "` is `@generate(Deserialize)`: define `fn void onConstruction()` (runs on "
                         "every construction, incl. deserialize) or opt out with `@generate(Deserialize, noOnConstruction)`").c_str(),
                        cd->line);
        // By-value serialization intrinsic (Phase C): synthesize `serialize`/`deserialize` in C for a
        // `@generate` tree struct that supplies NEITHER a hand-written impl NOR a driver-generated graph
        // impl — both land a method in `ci.methods`, so the `!count` guard makes hand-written / graph win.
        // Concrete product types only (a generic template specializes per instance; enums are driver-side).
        // Registers the conformance exactly like `implements Serialize` (interfaces + method) so the normal
        // vtable/dispatch machinery works; only the BODY is emitted specially (isSynthSer/isSynthDe).
        if (!(cd->typeParams && !cd->typeParams->empty()) && !ci.isVariant) {
            if (ci.genSerialize && !ci.methods.count("serialize")) {
                ci.interfaces.push_back("Serialize");
                MethodInfo mi; mi.cName = ci.name + "__serialize"; mi.visibility = Visibility::Public;
                mi.isSynthSer = true; mi.returnType = primTypeNode(IDENTIFIER_VOID_VAL);
                ParamSig w; w.name = "w"; w.byRef = true; w.className = "Serializer"; mi.params.push_back(w);
                ci.methods["serialize"] = mi;   // `fn void serialize(ref Serializer w)`
            }
            if (ci.genDeserialize && !ci.methods.count("deserialize")) {
                ci.interfaces.push_back("Deserialize");
                MethodInfo mi; mi.cName = ci.name + "__deserialize"; mi.visibility = Visibility::Public;
                mi.isStatic = true; mi.isSynthDe = true; mi.returnType = cd->name;   // returns the type itself
                ParamSig r; r.name = "r"; r.byRef = false; r.className = "Deserializer"; mi.params.push_back(r);
                ci.methods["deserialize"] = mi;   // `static fn This deserialize(Deserializer r)`
            }
        }
        // a generic TYPE template (`type value Box<T>`) is kept OUT of _classes — it is
        // specialized per concrete `Box<Arg>` at discovery. Its ClassInfo shape (T-typed fields/
        // methods) is parked in _genericTypes; the specialized instances are the real classes.
        if (cd->typeParams && !cd->typeParams->empty()) {
            std::vector<std::string> ps;
            for (auto& p : *cd->typeParams) if (p) ps.push_back(*p);   // [A, B, …]
            _genericTypeParams[ci.name] = ps;
            _genericTypeBounds[ci.name] = cd->typeBounds;   // per-param contract bounds
            _genericTypeCtx[ci.name]    = _nsCtx;
            _genericTypes[ci.name]      = ci;
            if (cd->name && cd->name->value) {              // capture the prelude triad's template keys (Phase D)
                const std::string& sn = *cd->name->value;
                if      (sn == "Shared") _sharedTmpl = ci.name;
                else if (sn == "Owned")  _ownedTmpl  = ci.name;
                else if (sn == "Weak")   _weakTmpl   = ci.name;
            }
        } else {
            _classes[ci.name] = ci;
        }
    }
}

// ---- Collections -----------------------------------------------------

// ---- Const generics --------------------------------------------------
// The compile-time integer value of a const generic argument expression: an integer literal (any
// width), or a const-param identifier bound to a value in the current instantiation (`N` -> 4). This
// is the value half of the monomorphization — the parallel of resolving a bound type-param.
bool CEmitter::constValue(SharedExpression e, int64_t& out)
{
    if (!e) return false;
    ASTNode* n = e.get();
    if (auto* v = dynamic_cast<Int8Node*>(n))   { out = v->value; return true; }
    if (auto* v = dynamic_cast<Int16Node*>(n))  { out = v->value; return true; }
    if (auto* v = dynamic_cast<Int32Node*>(n))  { out = v->value; return true; }
    if (auto* v = dynamic_cast<Int64Node*>(n))  { out = (int64_t)v->value; return true; }
    if (auto* v = dynamic_cast<UInt8Node*>(n))  { out = v->value; return true; }
    if (auto* v = dynamic_cast<UInt16Node*>(n)) { out = v->value; return true; }
    if (auto* v = dynamic_cast<UInt32Node*>(n)) { out = v->value; return true; }
    if (auto* v = dynamic_cast<UInt64Node*>(n)) { out = (int64_t)v->value; return true; }
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) return constArgN(std::static_pointer_cast<IdentifierNode>(e), out);
    return false;
}

// The const value carried by a generic ARGUMENT node: a literal wrapped by the grammar
// (`constArgValue`, as in `Fixed<T,4>`), or a bare const-param identifier bound in this
// instantiation (`Fixed<T,N>`). Returns false for a type argument (no const value).
bool CEmitter::constArgN(SharedIdentifier arg, int64_t& out)
{
    if (!arg) return false;
    if (arg->constArgValue) return constValue(arg->constArgValue, out);
    if (arg->value && !arg->genericArg) {
        auto it = _constSubst.find(*arg->value);
        if (it != _constSubst.end()) { out = it->second; return true; }
    }
    return false;
}

bool CEmitter::isFixedColl(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isIntrinsicColl && it->second.collKind == CollKind::Fixed;
}

// The mangling suffix for an element type: primitives use a short stable
// spelling; a class/enum uses its own name. Array<int32> -> "int32".
std::string CEmitter::mangleElem(SharedIdentifier elem)
{
    if (!elem) return "void";
    // const generic ARGUMENT: a literal value (`Fixed<T,4>`) or a const-param identifier bound in
    // this instantiation (`Fixed<T,N>` with N=4) mangles to the integer itself (`_4`), symmetric to
    // a type arg's name. Consulted before the type-param path since a const arg has no `value`.
    { int64_t v; if (constArgN(elem, v)) return std::to_string(v); }
    // substitute a bound type-param before mangling (mirrors cType).
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
            // recurse into a nested generic arg so `Shared<Circle>` mangles to
            // `Shared_Circle` (not just `Shared`) — fixes the `List<Shared<Circle>>` collision.
            if (elem->genericArgs) for (auto& a : *elem->genericArgs) base += "_" + mangleElem(a);
            else if (elem->genericArg) base += "_" + mangleElem(elem->genericArg);
            return base;
        }
    }
}

// the mangled struct name for `Pair<A, B, …>` — the template's scoped name + one "_<mangle>"
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
    if (t->builtInVal == IDENTIFIER_STRING_VAL) return true;          // string
    // List, Array, and the smart pointers are library generic types (std::collections / std::memory),
    // not intrinsic collections; they route through registerGenericTypeInst. `Fixed<T,N>` (the
    // const-generic value array) stays intrinsic.
    return t->genericArg && t->value &&
           (*t->value == "BindableFunctionPtr" || *t->value == "Fixed");
}

// Discover a used Coll<T> instantiation: register a CollectionInfo (drives the
// macro emission) and a synthetic ClassInfo (so dispatch/RAII/decl reuse works).
void CEmitter::registerCollection(SharedIdentifier collType)
{
    if (!isCollectionType(collType)) return;

    // Smart pointers (Owned/Shared/Weak) are library generic types now; they never reach here — only
    // their polymorphic `<Contract>` instances become smart-ptr collections, registered directly via
    // registerSmartPtr from registerGenericTypeInst's interface-owner divert.
    bool isStr    = collType->builtInVal == IDENTIFIER_STRING_VAL;
    CollKind kind = isStr ? CollKind::String
                  : (*collType->value == "List") ? CollKind::List : CollKind::Array;
    // A string's element is a raw byte (`s[i]` -> uint8), so byte iteration/indexing routes through the
    // collection machinery; codepoints come from `.chars()` (a separate iterator).
    SharedIdentifier elem = isStr ? primTypeNode(IDENTIFIER_UINT8_VAL) : collType->genericArg;
    std::string elemCType  = isStr ? "uint8_t" : cType(elem);
    std::string elemMangle = isStr ? "" : mangleElem(elem);
    std::string elemClass  = (!isStr && isClass(elemCType)) ? elemCType : "";

    // BindableFunctionPtr<Sig> — element is a function SIGNATURE, not a class.
    if (!isStr && collType->value && *collType->value == "BindableFunctionPtr") {
        registerBindable(elem);
        return;
    }

    // Fixed<T, N> — the const-generic value array (a distinct shape: two args, value semantics).
    if (!isStr && collType->value && *collType->value == "Fixed") {
        registerFixed(collType);
        return;
    }

    // an interface borrows its object — it can't be a BARE collection element (a
    // `List`/`Array` of interface fat pointers would dangle). Own the object: store an
    // `Owned<I>`/`Shared<I>` (a `List<Shared<I>>`) instead — the smart-ptr-over-interface.
    if (isInterface(elemCType)) {
        std::string nm = (elem && elem->value) ? *elem->value : elemCType;
        unsupported(("a contract (`" + nm + "`) borrows its object, so it can't be a collection "
                     "element — it would dangle; store an owning `Shared<" + nm + ">` instead").c_str(),
                    collType->line);
        return;
    }

    std::string cName = isStr ? "kama_string"
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
    ci.isIntrinsicColl = true;
    ci.collKind = kind;
    ci.collElemClass = elemClass;
    ci.destructible = true;                    // owns heap -> RAII frees
    ci.copyable = isStr;                        // `string` is deep-copyable (kama_string__copy) -> satisfies
                                               // `Copyable` as a type arg, so `List<string>` gets its
                                               // `when T: Copyable` methods (copy / by-value foreach).
    if (isStr) {                                // `string` IS `Equatable` — it has a real `equals`. Record it
        ci.interfaces.push_back("Equatable");   // NOMINALLY so `when T: Equatable` gating (satisfiesBound, which
        ci.retroInterfaces.push_back("Equatable"); // reads `interfaces`) keeps `List<string>.contains` etc.;
    }                                           // retroInterfaces => static dispatch only, no fat-pointer vtable.
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
        mi.visibility = Visibility::Public;   // an intrinsic (string/List/Array op) IS that type's public API —
                                              // string's `equals` is real, so its nominal Equatable (recorded
                                              // in `interfaces`) resolves to it for a `<K: Equatable>` bound
        ci.methods[mname] = mi;
    };
    if (kind == CollKind::String) {
        addMethod("length", {}, SharedIdentifier());
        addMethod("equals", { ParamSig{"other", false, ""} }, SharedIdentifier());
        addMethod("concat", { ParamSig{"other", false, ""} }, collType);   // returns a string
        addMethod("cstr",   {}, SharedIdentifier());                        // FFI: const char*
        addMethod("get",    { ParamSig{"index", false, ""} }, elem);        // `s[i]` -> the i-th byte (uint8)
        if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<generic>"));
        addMethod("chars",  {}, std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Chars")));   // codepoint iterator
        // Phase 3 ergonomics. Bool-returning methods pass a NULL returnType (the `equals` pattern — the
        // emitter emits the raw C call and the C `bool` return governs). `substring`/`trim`/`replace`/case
        // return `collType` (a `string`), so their owned result is RAII-freed exactly like `.concat()`.
        addMethod("substring", { ParamSig{"start", false, ""}, ParamSig{"end", false, ""} }, collType);
        addMethod("contains",   { ParamSig{"substring", false, ""} }, SharedIdentifier());
        addMethod("startsWith", { ParamSig{"prefix", false, ""} },    SharedIdentifier());
        addMethod("endsWith",   { ParamSig{"suffix", false, ""} },    SharedIdentifier());
        addMethod("isEmpty",    {}, SharedIdentifier());
        addMethod("trim",       {}, collType);
        addMethod("trimStart",  {}, collType);
        addMethod("trimEnd",    {}, collType);
        addMethod("replace",    { ParamSig{"old", false, ""}, ParamSig{"with", false, ""} }, collType);  // `with:` (`new` is reserved)
        addMethod("toLower",    {}, collType);
        addMethod("toUpper",    {}, collType);
        // `find` -> Optional<usize> (null-safe byte offset). Register the Optional<usize> instance so its C
        // struct + Some/None variants exist, and give `find` that return type; the emitter emits a wrapper
        // (emitStringFind) that builds the Optional. Mirrors Weak.tryUpgrade / registerOptionalOfShared.
        if (_genericTypeParams.count("Optional")) {
            auto usizeArg = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("usize"));
            auto optArgs = std::make_shared<IdentifierList>();
            optArgs->push_back(usizeArg);
            registerGenericTypeInst("Optional", optArgs);
            auto optRet = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Optional"));
            optRet->genericArg  = usizeArg;
            optRet->genericArgs = std::make_shared<IdentifierList>();
            optRet->genericArgs->push_back(usizeArg);
            addMethod("find", { ParamSig{"substring", false, ""} }, optRet);
        }
        // `split(separator:)` -> a lazy `Split` iterator (prelude value type; see the `.split()`
        // special-case in emitMethodCall). No collections import: pieces come out one at a time.
        addMethod("split", { ParamSig{"separator", false, ""} },
                  std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Split")));
    } else {
        if (kind == CollKind::List)
            addMethod("add", { ParamSig{"item", false, elemClass} }, SharedIdentifier());
        addMethod("get",    { ParamSig{"index", false, ""} }, elem);
        addMethod("set",    { ParamSig{"index", false, ""}, ParamSig{"value", false, elemClass} }, SharedIdentifier());
        addMethod("length", {}, SharedIdentifier());
        addMethod("dataPtr", {}, SharedIdentifier());   // FFI bridge: Ptr<T> to the buffer
        addMethod("byteLen", {}, SharedIdentifier());   // len * sizeof(T)
    }

    _classes[cName] = ci;
}

// Register a `Fixed<T, N>` instance — the const-generic safe array. Unlike the heap collections it
// is a VALUE type (`struct { T v[N]; }`): it owns no heap, copies by value, and has no destructor,
// so it is a `CollKind::Fixed` carved out of the ownership machinery. It reuses the collection path
// only for bounds-checked indexing + `foreach`. The element must be a `value` (own nothing) and N a
// positive compile-time integer (a literal, or a bound `const N: int` param).
void CEmitter::registerFixed(SharedIdentifier fixedType)
{
    SharedIdentifierList args = fixedType->genericArgs;
    if (!args || args->size() != 2) {
        unsupported("`Fixed<T, N>` takes exactly two arguments — an element type and a const size "
                    "(`Fixed<float32, 4>`)", fixedType->line);
        return;
    }
    SharedIdentifier elem = (*args)[0];
    SharedIdentifier nArg = (*args)[1];
    int64_t n;
    // An unbound `N` (a `const N: int` param referenced inside a generic template, no binding yet)
    // isn't a concrete type — skip it silently; the concrete `Fixed<T, 4>` instance registers at the
    // use site (a caller's typed local / a bound instantiation). A concrete non-const size resolves here.
    if (!constArgN(nArg, n)) return;
    if (n <= 0) {
        unsupported("the size of a `Fixed<T, N>` must be a positive integer", fixedType->line);
        return;
    }

    std::string elemCType  = cType(elem);
    std::string elemMangle = mangleElem(elem);
    std::string elemClass  = isClass(elemCType) ? elemCType : "";

    // The element must OWN NOTHING (a `value` or primitive): a `Fixed` is a plain value with no
    // per-element dtor, so a `resource`/interface/destructible element would leak or dangle.
    if (isInterface(elemCType) || (!elemClass.empty() && isMoveOnlyValue(elemClass))) {
        std::string nm = (elem && elem->value) ? *elem->value : elemCType;
        unsupported(("a `Fixed<T, N>` element must be a `value` (it owns nothing) — `" + nm +
                     "` is a `resource`/contract, which would leak; wrap it in an owning `Array`/"
                     "`List` instead").c_str(), fixedType->line);
        return;
    }

    std::string cName = "Fixed_" + elemMangle + "_" + std::to_string(n);
    if (_collections.count(cName)) return;    // dedup

    CollectionInfo info;
    info.kind = CollKind::Fixed; info.cName = cName;
    info.elemCType = elemCType; info.elemMangle = elemMangle; info.elemClass = elemClass;
    info.constValue = n;
    _collections[cName] = info;
    _collectionOrder.push_back(cName);

    // Synthetic ClassInfo: a value struct (NOT destructible, no ctor) with intrinsic get/set/length.
    // Indexing (`v[i]`) and `foreach` go through collectionElemAccess/the foreach path directly;
    // `length` is the one method resolved by name (`v.length()`).
    ClassInfo ci;
    ci.name = cName;
    ci.isIntrinsicColl = true;
    ci.collKind = CollKind::Fixed;
    ci.collElemClass = elemClass;
    ci.destructible = false;                   // a value — owns no heap
    ci.hasCtor = false;                        // built from an array literal, not a ctor call
    auto addMethod = [&](const std::string& mname, std::vector<ParamSig> params, SharedIdentifier ret) {
        MethodInfo mi; mi.cName = cName + "__" + mname;
        mi.params = std::move(params); mi.returnType = ret; mi.isIntrinsic = true;
        ci.methods[mname] = mi;
    };
    addMethod("get",    { ParamSig{"index", false, ""} }, elem);
    addMethod("set",    { ParamSig{"index", false, ""}, ParamSig{"value", false, elemClass} }, SharedIdentifier());
    addMethod("length", {}, SharedIdentifier());
    _classes[cName] = ci;
}

// Register a smart pointer (Owned/Shared/Weak) as a synthetic ClassInfo backed by
// a runtime macro. Smart pointers expose a T* `ptr` (auto-deref) and have no real
// ctor (construction is inline). Shared/Weak carry a few intrinsic methods.
void CEmitter::registerSmartPtr(CollKind kind, SharedIdentifier elem, const std::string& customName)
{
    std::string elemCType  = cType(elem);
    std::string elemMangle = mangleElem(elem);
    bool elemIface = isInterface(elemCType);                          // fat-element variant
    std::string elemClass  = (isClass(elemCType) || elemIface) ? elemCType : "";
    if (elemClass.empty()) return;              // caller diagnosed
    // a library `Box<Contract>` is routed here under its own mangled name (Rust's `Box<dyn Trait>`);
    // otherwise the built-in Owned/Shared/Weak prefix.
    std::string cName = !customName.empty() ? customName
                      : (kind == CollKind::Owned  ? "Owned_"
                       : kind == CollKind::Shared ? "Shared_" : "Weak_") + elemMangle;
    if (_collections.count(cName)) return;      // dedup

    CollectionInfo info;
    info.kind = kind; info.cName = cName;
    info.elemCType = elemCType; info.elemMangle = elemMangle; info.elemClass = elemClass;
    info.elemIsInterface = elemIface;
    // An interface element's concrete object is dropped via its vtable's __dtor slot, not the
    // collection's ELEM_DTOR machinery, so elemDestructible stays false here.
    info.elemDestructible = !elemIface && _classes.count(elemClass) && _classes[elemClass].destructible;
    // The paired instance across Shared<->Weak (conventional prefix by default; a library `Rc`/`RcWeak`
    // pair overrides this after registration). A Weak upgrades to `Shared_<elem>`; a Shared downgrades to
    // `Weak_<elem>`.
    if (kind == CollKind::Weak)        info.ifacePartner = "Shared_" + elemMangle;
    else if (kind == CollKind::Shared) info.ifacePartner = "Weak_" + elemMangle;
    _collections[cName] = info;
    _collectionOrder.push_back(cName);

    ClassInfo ci;
    ci.name = cName; ci.isIntrinsicColl = true; ci.collKind = kind;
    ci.collElemClass = elemClass; ci.destructible = true; ci.hasCtor = false;
    auto addM = [&](const std::string& m, std::vector<ParamSig> p) {
        MethodInfo mi; mi.cName = cName + "__" + m; mi.params = std::move(p);
        mi.isIntrinsic = true; ci.methods[m] = mi;
    };
    // Intrinsics (not auto-deref forwarded): Owned has none; Shared has valid();
    // Weak has tryUpgrade() (-> Optional<Shared<T>>) and expired().
    if (kind == CollKind::Shared) addM("valid", {});
    if (kind == CollKind::Weak) {
        addM("tryUpgrade", {}); addM("expired", {});
        // record that tryUpgrade returns Optional<Shared<elem>> so exprClass can class a
        // `w.tryUpgrade()` call result — enabling `match(w.tryUpgrade())` without a local binding.
        if (_genericTypeParams.count("Optional")) {
            if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
            auto mk = [&](const char* nm, SharedIdentifier arg) {
                auto n = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>(nm));
                n->genericArg = arg; n->genericArgs = std::make_shared<IdentifierList>(); n->genericArgs->push_back(arg);
                return n;
            };
            ci.methods["tryUpgrade"].returnType = mk("Optional", mk("Shared", elem));
        }
    }
    _classes[cName] = ci;
}

// register `Optional<Shared<elem>>` for a `Weak<elem>` — the type `tryUpgrade()` returns.
// Synthesizes the `Shared<elem>` argument node and drives the normal generic-type monomorphization.
void CEmitter::registerOptionalOfShared(SharedIdentifier elem)
{
    if (!elem || !_genericTypeParams.count("Optional")) return;   // no prelude Optional -> skip
    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
    auto sh = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Shared"));
    sh->genericArg  = elem;
    sh->genericArgs = std::make_shared<IdentifierList>();
    sh->genericArgs->push_back(elem);
    auto optArgs = std::make_shared<IdentifierList>();
    optArgs->push_back(sh);
    registerGenericTypeInst("Optional", optArgs);
}

// register `Optional<sharedName>` where `sharedName` is an already-registered smart-ptr INSTANCE (a
// library `Rc_<elem>`) — a bare type node naming it drives the monomorphization. For `RcWeak.tryUpgrade`.
void CEmitter::registerOptionalOfName(const std::string& sharedName)
{
    if (sharedName.empty() || !_genericTypeParams.count("Optional")) return;
    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
    auto sh = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>(sharedName));
    auto optArgs = std::make_shared<IdentifierList>();
    optArgs->push_back(sh);
    registerGenericTypeInst("Optional", optArgs);
}

// Register a BindableFunctionPtr<Sig> — a callable that may own a bound
// receiver. Element is a signature type (from `fnptr`), not a class. Backed by the
// fully type-erased KAMA_BINDABLE_DEFINE struct; the sig drives only the invoke.
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
    ci.name = cName; ci.isIntrinsicColl = true; ci.collKind = CollKind::Bindable;
    ci.collElemClass = sigCName;       // reused at invoke: the bound signature's cName
    ci.destructible = true;            // owns heap (when bound) -> RAII drop
    ci.hasCtor = false;                // constructed via the dedicated bind path, not a ctor
    _classes[cName] = ci;
}

bool CEmitter::isBindableClass(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isIntrinsicColl && it->second.collKind == CollKind::Bindable;
}

void CEmitter::scanTypeForCollections(SharedIdentifier t)
{
    if (!t) return;
    // Inner-first: register the element's collections / generic instances BEFORE the enclosing
    // type, so the outer's elemClass/elemDestructible resolve against an already-registered inner
    // (`List<Shared<IShape>>` must see `Shared_IShape` in _classes to drop each element; likewise
    // `List<List<T>>`, `List<Box<T>>`). Recurse ALL type args so a collection/generic in
    // a 2nd+ position (`Pair<int, List<int>>`) is discovered — not just the first arg.
    if (t->genericArgs) for (auto& a : *t->genericArgs) scanTypeForCollections(a);
    else if (t->genericArg) scanTypeForCollections(t->genericArg);
    scanTypeForGenericTypes(t);                                 // also discover Pair<A,B> here
    scanTypeForGenericContracts(t);                             // and Iterator<int32> (value-position use)
    if (isCollectionType(t)) registerCollection(t);
}

// register the specialized instance for a user generic-type reference `Pair<A, B>`. The
// recursion into the args is driven by scanTypeForCollections (which calls this at each type node).
void CEmitter::scanTypeForGenericTypes(SharedIdentifier t)
{
    if (!t || !t->value || !t->genericArg) return;             // genericArg mirrors genericArgs[0]
    std::string tmpl = resolveUserName(*t->value, t->qualifier);
    if (_genericTypes.count(tmpl)) registerGenericTypeInst(tmpl, t->genericArgs);
}

// Deep-substitute a type node under the active _typeSubst (see the header). A bare param -> its
// (already-concrete) binding; a nested generic clones with each arg substituted; anything else is
// returned unchanged. Bindings are kept fully concrete, so recursion terminates.
SharedIdentifier CEmitter::deepSubstType(SharedIdentifier t)
{
    if (!t) return t;
    if (!_typeSubst.empty() && t->value && !t->genericArg) {   // a bare type-param
        auto s = _typeSubst.find(*t->value);
        if (s != _typeSubst.end()) return s->second;           // its binding is already deep
    }
    if (t->genericArgs && !t->genericArgs->empty()) {          // a generic type — substitute its args
        auto clone = std::make_shared<IdentifierNode>(*t);     // shallow copy (context + scalar fields)
        clone->genericArgs = std::make_shared<IdentifierList>();
        for (auto& a : *t->genericArgs) clone->genericArgs->push_back(deepSubstType(a));
        clone->genericArg = clone->genericArgs->empty() ? SharedIdentifier() : (*clone->genericArgs)[0];
        return clone;
    }
    return t;
}

// Rebind a type node to its use-site-resolved absolute name so it mangles identically in any ctx.
// Called on a generic instance's concrete args (under the use-site _nsCtx) so that when the template's
// own ctx later drives interface/member scanning, a cross-module arg (`Counter` from another file)
// keeps its home mangle instead of being resolved to a bare name against the template's scope.
SharedIdentifier CEmitter::absolutizeType(SharedIdentifier t)
{
    if (!t || !t->value) return t;
    auto clone = std::make_shared<IdentifierNode>(*t);
    // A user class/enum/contract NAME is rebound to its use-site mangle (qualifier dropped); primitives,
    // `Ptr`, `This`, usize/isize keep their spelling (they resolve context-free). Generic args recurse
    // either way (`Ptr<Counter>`, `List<Counter>`, `Owned<Counter>`).
    bool contextFree = (t->builtInVal != 0) || *t->value == "Ptr" || *t->value == "This"
                     || *t->value == "usize" || *t->value == "isize";
    if (!contextFree) {
        clone->value = std::make_shared<std::string>(resolveUserName(*t->value, t->qualifier));
        clone->qualifier = SharedStringList();                   // now an absolute name — no qualifier
    }
    if (t->genericArgs) {
        clone->genericArgs = std::make_shared<IdentifierList>();
        for (auto& a : *t->genericArgs) clone->genericArgs->push_back(absolutizeType(a));
        clone->genericArg = clone->genericArgs->empty() ? SharedIdentifier() : (*clone->genericArgs)[0];
    } else if (t->genericArg) {
        clone->genericArg = absolutizeType(t->genericArg);
    }
    return clone;
}

// True iff `a` (already run through deepSubstType/absolutizeType) still names an UNBOUND type-parameter:
// a bare identifier that resolves to no known type and isn't a primitive / reserved name. Recurses into a
// generic's args so `Result<List<V>, …>` is caught via its inner `List<V>`.
bool CEmitter::argCarriesUnboundParam(const SharedIdentifier& a)
{
    if (!a || !a->value) return false;
    if (a->genericArgs && !a->genericArgs->empty()) {          // a generic type — check its args
        for (auto& sub : *a->genericArgs) if (argCarriesUnboundParam(sub)) return true;
        return false;
    }
    if (a->builtInVal != 0) return false;                       // a primitive
    const std::string& v = *a->value;
    if (v == "This" || v == "Ptr" || v == "usize" || v == "isize") return false;
    std::string r = resolveUserName(v, a->qualifier);
    return !_classes.count(r) && !_enums.count(r) && !_genericTypes.count(r)
        && !_interfaces.count(r) && !_genericContracts.count(r);
}

// Build one synthetic specialized ClassInfo per `Box<Arg>` (mirrors registerCollection): copy the
// template shape, rewrite identity (struct name + method cNames), re-derive param signatures under
// _typeSubst, register in _classes, and transitively scan its substituted member types so a
// `Box<T>` holding `List<T>` registers `List_int32`. Deduped by the mangled name.
void CEmitter::registerGenericTypeInst(const std::string& tmpl, SharedIdentifierList args)
{
    if (!args || args->empty()) return;
    const std::vector<std::string>& params = _genericTypeParams[tmpl];

    // Resolve each arg to its concrete binding — DEEPLY (a nested `Rc<T>` -> `Rc<Counter>`), so a
    // generic arg carrying a type-param is never stored raw in _typeSubst (which would self-reference
    // and loop mangleElem for a mutually-recursive generic type).
    std::vector<SharedIdentifier> concrete;
    for (auto& a : *args) concrete.push_back(absolutizeType(deepSubstType(a)));
    // Arity: N type arguments must match the template's N type parameters.
    if (concrete.size() != params.size()) {
        unsupported(("wrong number of type arguments for generic type `" + tmpl + "` (expected "
                     + std::to_string(params.size()) + ", got " + std::to_string(concrete.size()) + ")").c_str(),
                    args->front() ? args->front()->line : 0);
        return;
    }

    // Defer if a substituted arg still carries an unbound type-param — this only happens when scanning a
    // generic FUNCTION's signature before instantiation (its `V` isn't in _typeSubst yet, so deepSubstType
    // left it raw). The real instance registers at monomorphization (with concrete args). Registering now
    // would emit a struct with a raw `V` field (invalid C), or self-loop mangleElem/cType if the param name
    // clashes with the template's. (Inside a generic-TYPE instantiation the args are already substituted to
    // concrete, so nothing is skipped there — `ListIter<int32>` etc. are unaffected. Mirrors `Fixed<T,N>`'s
    // unbound-`N` skip.)
    for (auto& c : concrete) if (argCarriesUnboundParam(c)) return;

    std::string mangled = tmpl;
    for (auto& c : concrete) mangled += "_" + mangleElem(c);
    if (_genericTypeInsts.count(mangled)) return;               // dedup

    // A library heap owner (`… implements HeapOwner<T>`) over a CONTRACT element is Rust's
    // `Box<dyn Trait>`: the type-erased `{obj,vtbl}` fat pointer + vtable dispatch/drop can't be safe
    // library code, so route this instance through the intrinsic interface-owner path (a smart pointer
    // under the specialized name) — every downstream `isSmartPtrClass` path then applies. A concrete
    // element falls through to the normal library-class instantiation below.
    // Kind: a **Copyable** heap owner retains-on-copy = reference-counted (`Shared` IFACE, with a ctrl
    // block); a move-only one is unique (`Owned` IFACE). (`Rc<T>` has `copy()`; `Box<T>` does not.)
    if (!_heapOwnerContract.empty() && !concrete.empty() && concrete[0] && isInterface(cType(concrete[0]))) {
        bool implementsHeapOwner = false;
        auto ti = _genericTypes.find(tmpl);
        if (ti != _genericTypes.end())
            for (auto& ifn : ti->second.interfaces)
                if (resolveUserName(ifn, nullptr) == _heapOwnerContract) { implementsHeapOwner = true; break; }
        if (implementsHeapOwner) {
            CollKind k = ti->second.copyable ? CollKind::Shared : CollKind::Owned;   // Copyable => refcounted
            registerSmartPtr(k, concrete[0], mangled);
            // A refcounted (Shared) owner may have a WEAK partner: a method (`downgrade`) returning a
            // DIFFERENT generic resource `RcWeak<T>` (not the owner itself). Route the weak to the Weak
            // IFACE + wire `downgrade`/`tryUpgrade` so the library API works on the type-erased pair.
            if (k == CollKind::Shared) {
                std::string downName, weakTmpl;
                for (auto& kv : ti->second.methods) {   // find the downgrade: returns a non-self generic type
                    SharedIdentifier rt = kv.second.returnType;
                    if (!rt || !rt->value || !rt->genericArg) continue;
                    std::string rn = resolveUserName(*rt->value, rt->qualifier);
                    if (rn != tmpl && _genericTypes.count(rn)) { downName = kv.first; weakTmpl = rn; break; }
                }
                if (!weakTmpl.empty()) {
                    std::string weakMangled = weakTmpl + "_" + mangleElem(concrete[0]);
                    registerSmartPtr(CollKind::Weak, concrete[0], weakMangled);
                    _collections[mangled].ifacePartner    = weakMangled;   // Rc_Shape downgrades to RcWeak_Shape
                    _collections[mangled].downgradeName   = downName;
                    _collections[weakMangled].ifacePartner = mangled;      // RcWeak_Shape upgrades to Rc_Shape
                    registerOptionalOfName(mangled);                       // Optional<Rc_Shape> for tryUpgrade
                    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
                    auto bare = [&](const std::string& n){ return std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>(n)); };
                    // downgrade intrinsic on Rc_Shape (-> RcWeak_Shape); dispatched via _classes[cls].methods
                    MethodInfo dm; dm.cName = mangled + "__" + downName; dm.isIntrinsic = true; dm.returnType = bare(weakMangled);
                    _classes[mangled].methods[downName] = dm;
                    // RcWeak_Shape.tryUpgrade returns Optional<Rc_Shape> (not the default Optional<Shared<elem>>)
                    auto opt = bare("Optional");
                    opt->genericArg = bare(mangled); opt->genericArgs = std::make_shared<IdentifierList>();
                    opt->genericArgs->push_back(opt->genericArg);
                    if (_classes[weakMangled].methods.count("tryUpgrade"))
                        _classes[weakMangled].methods["tryUpgrade"].returnType = opt;
                }
            }
            return;
        }
    }

    // each concrete type argument must satisfy its parameter's contract bounds. Runs once per
    // unique instance (after dedup); with _typeSubst still at the caller's binding so a nested arg is
    // already the resolved `concrete[i]`.
    SharedBoundsList bounds = _genericTypeBounds.count(tmpl) ? _genericTypeBounds[tmpl] : SharedBoundsList();
    if (bounds) for (size_t i = 0; i < params.size() && i < bounds->size(); ++i)
        checkBounds(params[i], concrete[i], (*bounds)[i], args->front() ? args->front()->line : 0);

    // Register the KEY first so the transitive scan below can't recurse into this same instance.
    _genericTypeInsts[mangled] = { tmpl, mangled, concrete };
    _genericTypeInstOf[mangled] = tmpl;
    _genericTypeInstOrder.push_back(mangled);
    // Emit the instance's members under the TEMPLATE's ctx, so a name the template body references in
    // its own scope (a sibling generic `Weak<T>`, a module-local helper `Ctrl`) resolves. The concrete
    // args were `absolutizeType`d to their use-site mangle, so they resolve context-free here too — no
    // need to fall back to the use-site ctx. For a same-scope user generic the two ctxs coincide.
    _genericTypeInstCtx[mangled] = _genericTypeCtx[tmpl];

    NsCtx savedCtx = _nsCtx;
    std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
    _nsCtx = _genericTypeCtx[tmpl];
    _typeSubst.clear();
    for (size_t i = 0; i < params.size(); ++i) _typeSubst[params[i]] = concrete[i];   // zip params -> args

    ClassInfo ci = _genericTypes[tmpl];                         // copy the template shape
    ci.name = mangled;
    ci.isGenericInst = true;
    // Conditional Copyable (`implements Copyable(bare: …) when T: Bound`): this instance is Copyable
    // only when its gated type-param satisfies the bound. If not, drop the capability AND its `copy()`
    // method so it is never emitted for this instance — this is what lets `List<Owned>` compile even
    // though its element isn't copyable, while `copy list<int32>` still works.
    bool copyableActive = ci.copyable;
    if (!ci.copyableWhenParams.empty()) {
        copyableActive = whenConditionsHold(ci.copyableWhenParams, ci.copyableWhenBounds, params, concrete);
        if (!copyableActive) { ci.copyable = false; ci.methods.erase("copy"); }
    }
    // Conditional METHODS (`fn … when T: Bound`): drop any whose bound fails for this instance, so it's
    // never emitted (the value `iterator()` needs a Copyable element — a `List<Owned>` simply lacks it).
    for (auto it = ci.methods.begin(); it != ci.methods.end(); ) {
        const MethodInfo& mi = it->second;
        bool drop = !mi.whenParams.empty()
                 && !whenConditionsHold(mi.whenParams, mi.whenBounds, params, concrete);
        if (drop) it = ci.methods.erase(it); else ++it;
    }
    for (auto& kv : ci.methods) kv.second.cName = mangled + "__" + kv.first;
    // Re-derive ParamSig under substitution so call-site arg typing is concrete (not a stale "T").
    if (ci.ctorNode && ci.ctorNode->declarator)
        ci.ctorParams = paramSigsOf(ci.ctorNode->declarator->params);
    for (auto& kv : ci.methods) {
        if (kv.second.node) kv.second.params = paramSigsOf(kv.second.node->params);
        else if (kv.second.isOperator && kv.second.opDecl)   // an operator has no `node`
            kv.second.params = paramSigsOf(operatorParamList(kv.second.opDecl->operatorDeclarator.get()));
    }
    // Resolve the `implements` list under THIS instance's subst (linkBases skips generic instances). A
    // generic contract implemented with the class's own param (`Box<T> implements Deref<T>`) mangles to the
    // concrete instance (`Deref_Point`) and that instance is registered; plain contracts just resolve.
    if (ci.node && ci.node->baseTypes && ci.node->baseTypes->interfaces) {
        ci.interfaces.clear();
        for (auto& itf : *ci.node->baseTypes->interfaces) {
            if (!itf || !itf->value) continue;
            // A conditional interface (`Copyable(bare:) when […]`, `Iterable<T> when […]`) is present only
            // when its when-conditions hold for this instance — drop it otherwise (generalizes the old
            // Copyable-only gate; the methods it fronts are dropped by the method gate above).
            if (itf->whenParams && !itf->whenParams->empty()) {
                std::vector<std::string> wp, wb;
                for (size_t c = 0; c < itf->whenParams->size(); ++c) {
                    auto& p = (*itf->whenParams)[c];
                    auto& b = itf->whenBounds ? (*itf->whenBounds)[c] : p;
                    wp.push_back(p && p->value ? *p->value : "");
                    wb.push_back(b && b->value ? *b->value : "Copyable");
                }
                if (!whenConditionsHold(wp, wb, params, concrete)) continue;
            }
            std::string base = resolveUserName(*itf->value, itf->qualifier);
            if (itf->genericArg && _genericContracts.count(base)) {
                scanTypeForGenericContracts(itf);                     // register `Deref_Point` under subst
                base = genericTypeMangle(base, itf->genericArgs);     // mangleElem resolves the class param
            }
            ci.interfaces.push_back(base);
        }
    }
    _classes[mangled] = ci;

    // Transitive close: register any collection / generic type the substituted members use.
    for (auto& f : ci.fields) scanTypeForCollections(f.type);
    for (auto& kv : ci.methods) {
        scanTypeForCollections(kv.second.returnType);
        if (kv.second.node) for (auto& p : *kv.second.node->params) if (p) scanTypeForCollections(p->type);
    }
    if (ci.ctorNode && ci.ctorNode->declarator)
        for (auto& p : *ci.ctorNode->declarator->params) if (p) scanTypeForCollections(p->type);
    // a generic tagged union (Optional<Shared<T>>) — scan each variant's substituted payload so
    // the inner `Shared_int32` etc. registers (inner-first) before this instance's dtor references it.
    for (auto& v : ci.variants) for (auto& f : v.payload) scanTypeForCollections(f.type);

    _typeSubst = savedSubst;
    _nsCtx = savedCtx;
}

// A contract's method-prototype list — from _interfaces (a concrete contract or a specialized
// generic-contract instance) or _genericContracts (a template). Bound-checking matches by method
// NAME (type-parameter-independent), so it reads either table through this one accessor.
const std::vector<InterfaceMethod>* CEmitter::contractMethods(const std::string& name)
{
    auto it = _interfaces.find(name);
    if (it != _interfaces.end()) return &it->second.methods;
    auto gt = _genericContracts.find(name);
    if (gt != _genericContracts.end()) return &gt->second.methods;
    return nullptr;
}

// register the specialized instance for a generic-contract reference `Iterator<Arg>` (mirrors
// scanTypeForGenericTypes). Driven by scanTypeForCollections at each type node.
void CEmitter::scanTypeForGenericContracts(SharedIdentifier t)
{
    if (!t || !t->value || !t->genericArg) return;
    std::string tmpl = resolveUserName(*t->value, t->qualifier);
    if (_genericContracts.count(tmpl)) registerGenericContractInst(tmpl, t->genericArgs);
}

// Build one specialized InterfaceInfo per `Iterator<Arg>` (the exact parallel of
// registerGenericTypeInst, minus fields/ctor/dtor): resolve each arg through the active _typeSubst
// (the nested/transitive case), mangle, dedup, copy the template shape into _interfaces under the
// specialized name, then transitively scan the substituted method sigs so an inner `Optional<T>`
// registers `Optional_int32` before this instance's vtable references it.
void CEmitter::registerGenericContractInst(const std::string& tmpl, SharedIdentifierList args)
{
    if (!args || args->empty()) return;
    const std::vector<std::string>& params = _genericContractParams[tmpl];

    std::vector<SharedIdentifier> concrete;
    for (auto& a : *args) {
        SharedIdentifier c = a;
        if (a && !_typeSubst.empty() && a->value && !a->genericArg) {
            auto s = _typeSubst.find(*a->value);
            if (s != _typeSubst.end()) c = s->second;
        }
        concrete.push_back(c);
    }
    if (concrete.size() != params.size()) {
        unsupported(("wrong number of type arguments for generic contract `" + tmpl + "` (expected "
                     + std::to_string(params.size()) + ", got " + std::to_string(concrete.size()) + ")").c_str(),
                    args->front() ? args->front()->line : 0);
        return;
    }

    std::string mangled = tmpl;
    for (auto& c : concrete) mangled += "_" + mangleElem(c);
    if (!_genericContractInsts.insert(mangled).second) return;    // dedup (also stops self-recursion)

    NsCtx savedCtx = _nsCtx;
    _genericContractInstCtx[mangled] = savedCtx;                  // use-site ctx: a user-type arg resolves here
    std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
    _nsCtx = _genericContractCtx[tmpl];
    _typeSubst.clear();
    for (size_t i = 0; i < params.size(); ++i) _typeSubst[params[i]] = concrete[i];   // zip params -> args

    InterfaceInfo ii = _genericContracts[tmpl];                   // copy the template (methods keep `T`)
    ii.name = mangled;
    ii.isGenericInst = true;
    ii.templateKey = tmpl;
    ii.typeArgs = concrete;
    _interfaces[mangled] = ii;                                    // the emit loops pick it up from here

    // transitive close: register any collection / generic type the substituted method sigs use, so
    // `Optional<T>` -> `Optional_int32` exists (as a complete typedef) before this vtable slot names it.
    for (auto& m : ii.methods) {
        scanTypeForCollections(m.returnType);
        if (m.params) for (auto& p : *m.params) if (p) scanTypeForCollections(p->type);
    }

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
    } else if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) {
        if (al->elements) for (auto& x : *al->elements) scanExprForCollections(x);
        scanExprForCollections(al->fillValue);
    } else if (auto* mm = dynamic_cast<MatchNode*>(n)) {
        // recurse into a `match` — the subject and each arm (a single expression OR a block),
        // so a type used ONLY inside an arm (e.g. a block-local `Shared<T>`) is still registered.
        scanExprForCollections(mm->subject);
        if (mm->arms) for (auto& a : *mm->arms) if (a) {
            scanExprForCollections(a->body);
            scanStmtForCollections(a->block);
        }
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
    } else if (auto* av = dynamic_cast<ArmValueNode*>(n)) {
        scanExprForCollections(av->value);
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
            // skip a generic TYPE template's members — their types name the raw type params
            // (`List<T>` would register a bogus `List_T`). Each concrete `Wrap<int32>` reference in
            // the program drives registerGenericTypeInst, which re-scans the specialized members
            // under _typeSubst (so `List<T>` -> `List_int32`).
            if (cd->typeParams && !cd->typeParams->empty()) continue;
            // `class C implements Iterator<int32>` — register the specialized contract instance so its
            // vtable emits (a value/dynamic use of C needs `C__as_Iterator_int32`).
            if (cd->baseTypes && cd->baseTypes->interfaces)
                for (auto& itf : *cd->baseTypes->interfaces) scanTypeForGenericContracts(itf);
            if (cd->members) for (auto& m : *cd->members) {
                ASTNode* mn = m.get();
                if (auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(mn)) {
                    scanTypeForCollections(fd->type);
                } else if (auto* kd = dynamic_cast<ClassConstDeclarationNode*>(mn)) {
                    scanTypeForCollections(kd->type);   // const field of a collection type
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

// ---- Generics ------------------------------------------------------
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

// The concrete type node of an argument expression (null if undeterminable). Inputs:
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
    if (dynamic_cast<CharNode*>(n))    return primTypeNode(IDENTIFIER_CHAR_VAL);
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

// A type node usable as a generic type argument: a primitive, or a known
// class/enum/contract. A bare type-parameter (its name resolves to none of these) and a
// collection/smart-pointer type argument (a `List<…>` etc.) are NOT concrete here.
bool CEmitter::isConcreteTypeArg(SharedIdentifier t)
{
    if (!t) return false;
    if (t->builtInVal != IDENTIFIER_NONE_VAL) return true;   // primitive
    if (t->genericArg) return false;                          // List<…>/Shared<…> arg
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
    std::set<std::string> cps;
    if (tmpl->constParams) for (auto& cp : *tmpl->constParams) if (cp) cps.insert(*cp);

    // synthesize a const generic ARGUMENT node carrying an integer value (so a bound const param
    // travels through mangleElem / GenericInst::typeArgs uniformly with a type argument).
    auto constArgNode = [&](int64_t v) -> SharedIdentifier {
        if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
        auto id = std::make_shared<IdentifierNode>(*_synthCtx, SharedString());
        id->constArgValue = std::make_shared<Int64Node>(*_synthCtx, v);
        return id;
    };

    std::map<std::string, SharedIdentifier> bind;
    if (tmpl->parameters) for (auto& p : *tmpl->parameters) {
        if (!p || !p->type || !p->type->value) continue;
        const std::string& pty = *p->type->value;
        // A `Fixed<ElemT, K>` parameter — infer any type-param element AND the const size K from the
        // argument's concrete `Fixed<int32, 4>` type. This is the const-generic half of inference.
        if (pty == "Fixed" && p->type->genericArgs && p->type->genericArgs->size() == 2) {
            std::string pname = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            auto ai = byName.find(pname);
            if (ai == byName.end()) continue;   // missing arg — emitReorderedCall reports it precisely
            SharedIdentifier at = exprTypeNode(ai->second, localTys);
            if (!at || !at->value || *at->value != "Fixed" || !at->genericArgs || at->genericArgs->size() != 2) {
                unsupported(("cannot infer the generic parameters of `Fixed<…>` — argument '" + pname +
                             "' is not a `Fixed<…>` value").c_str(), line);
                return false;
            }
            SharedIdentifier pElem = (*p->type->genericArgs)[0], pN = (*p->type->genericArgs)[1];
            SharedIdentifier aElem = (*at->genericArgs)[0],       aN = (*at->genericArgs)[1];
            // element type parameter (`Fixed<T, N>` — bind T), if it is a bare type-param
            if (pElem && pElem->value && !pElem->genericArg && tps.count(*pElem->value)) {
                if (!isConcreteTypeArg(aElem)) {
                    unsupported(("cannot infer type parameter '" + *pElem->value + "' from argument '" + pname + "'").c_str(), line);
                    return false;
                }
                auto b = bind.find(*pElem->value);
                if (b != bind.end() && mangleElem(b->second) != mangleElem(aElem)) {
                    unsupported(("cannot unify type parameter '" + *pElem->value + "'").c_str(), line);
                    return false;
                }
                bind[*pElem->value] = aElem;
            }
            // const size parameter (`Fixed<T, N>` — bind N to the argument's size)
            if (pN && pN->value && cps.count(*pN->value)) {
                int64_t v;
                if (!constArgN(aN, v)) {
                    unsupported(("cannot infer const parameter '" + *pN->value + "' — argument '" + pname +
                                 "' has no statically-known size").c_str(), line);
                    return false;
                }
                auto b = bind.find(*pN->value);
                if (b != bind.end()) { int64_t pv; if (constArgN(b->second, pv) && pv != v) {
                    unsupported(("cannot unify const parameter '" + *pN->value + "' (" + std::to_string(pv) +
                                 " vs " + std::to_string(v) + ")").c_str(), line);
                    return false; } }
                bind[*pN->value] = constArgNode(v);
            }
            continue;
        }
        if (p->type->genericArg || !tps.count(pty)) continue;   // not a bare type-param (List<T> etc. is a generic type)
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
                         "— pass it explicitly with turbofish, e.g. `f::<T>(...)`").c_str(), line);
            return false;
        }
    }

    // each inferred concrete type must satisfy its parameter's contract bounds (points at this call).
    if (tmpl->typeBounds)
        for (size_t i = 0; i < tmpl->typeParams->size() && i < tmpl->typeBounds->size(); ++i)
            if ((*tmpl->typeParams)[i])
                checkBounds(*(*tmpl->typeParams)[i], bind[*(*tmpl->typeParams)[i]], (*tmpl->typeBounds)[i], line);

    out.templateKey = key;
    out.typeArgs.clear();
    std::string mangled = key;
    for (auto& tp : *tmpl->typeParams) {
        SharedIdentifier a = absolutizeType(bind[*tp]);   // use-site mangle — see explicitGenericInst
        out.typeArgs.push_back(a);
        mangled += "__" + mangleElem(a);
    }
    out.mangledName = mangled;
    return true;
}

// Turbofish `f::<A, B>(…)` — bind each type param from the explicit type args in order (no inference).
// The concrete args are already resolved type nodes from the grammar; check arity + contract bounds.
bool CEmitter::explicitGenericInst(FunctionDeclarationNode* tmpl, const std::string& key,
                                   SharedIdentifierList typeArgs, int line, GenericInst& out)
{
    size_t np = tmpl->typeParams ? tmpl->typeParams->size() : 0;
    size_t na = typeArgs ? typeArgs->size() : 0;
    if (na != np) {
        unsupported(("generic function '" + key + "' takes " + std::to_string(np) + " type argument(s), but "
                     + std::to_string(na) + " were given in `::<…>`").c_str(), line);
        return false;
    }
    if (tmpl->typeBounds)
        for (size_t i = 0; i < np && i < tmpl->typeBounds->size(); ++i)
            if ((*tmpl->typeParams)[i])
                checkBounds(*(*tmpl->typeParams)[i], (*typeArgs)[i], (*tmpl->typeBounds)[i], line);

    out.templateKey = key;
    out.typeArgs.clear();
    std::string mangled = key;
    for (size_t i = 0; i < np; ++i) {
        // Absolutize the arg to its use-site (call-site) mangled name, so it resolves context-free when the
        // instance is later emitted under the TEMPLATE's home namespace (a `tryParse::<Point>` in module A
        // must carry `Point`'s home mangle, not resolve `Point` against json's scope). Mirrors
        // registerGenericTypeInst's `absolutizeType`; `_callInst`+`_genericInsts` both key off this name.
        SharedIdentifier a = absolutizeType((*typeArgs)[i]);
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
                SharedIdentifierList tfArgs = inv->identifier->genericArgs;   // turbofish `f::<…>` type args
                if (tfArgs) for (auto& ta : *tfArgs) scanTypeForCollections(ta);   // register List<…>/etc. args
                bool ok = tfArgs ? explicitGenericInst(git->second, k, tfArgs, inv->line, gi)
                                 : inferGenericInst(git->second, k, inv->args, localTys, inv->line, gi);
                if (ok) {
                    if (!_genericInsts.count(gi.mangledName)) _genericInsts[gi.mangledName] = gi;
                    _callInst[inv] = gi.mangledName;   // one call node -> one instantiation
                }
            }
            // Turbofish on a non-generic function is rejected at emit (emitInvocation), where it is a
            // hard build error — a bare `::<…>` that resolves to no generic template.
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
            // Flat name->type map, no scope-pop — correct without shadowing.
            if (v->name && v->name->value && d->type) localTys[*v->name->value] = d->type;
        }
    } else if (auto* cd = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        if (cd->variables) for (auto& v : *cd->variables) if (v) {
            scanExprForGenerics(v->initializer, localTys);
            if (v->name && v->name->value && cd->type) localTys[*v->name->value] = cd->type;
        }
    } else if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        scanExprForGenerics(r->expression, localTys);
    } else if (auto* av = dynamic_cast<ArmValueNode*>(n)) {
        scanExprForGenerics(av->value, localTys);
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

    // Bind each parameter to its argument: a const param (`const N: int`) binds a VALUE in
    // _constSubst (so a `Fixed<T,N>` param type resolves to `Fixed_T_4`); a type param binds a type
    // in _typeSubst. Both are cleared identically at the end.
    std::set<std::string> cps;
    if (tmpl->constParams) for (auto& cp : *tmpl->constParams) if (cp) cps.insert(*cp);
    _typeSubst.clear();
    _constSubst.clear();
    for (size_t i = 0; i < tmpl->typeParams->size() && i < gi.typeArgs.size(); ++i) {
        if (!(*tmpl->typeParams)[i]) continue;
        const std::string& pn = *(*tmpl->typeParams)[i];
        int64_t v;
        if (cps.count(pn) && constArgN(gi.typeArgs[i], v)) _constSubst[pn] = v;
        else _typeSubst[pn] = gi.typeArgs[i];
    }

    if (prototypeOnly) emitFunctionPrototype(tmpl, &gi.mangledName);   // emits `static` via nameOverride
    else               emitFunction(tmpl, &gi.mangledName);

    _typeSubst.clear();
    _constSubst.clear();
    _nsCtx = savedCtx;
}

// `Weak<T>.tryUpgrade() -> Optional<Shared<T>>` — the type-safe replacement for the empty-
// Shared sentinel. Wraps the runtime macro's internal `__upgrade` (which does the strong++/empty):
// a live Shared (ctrl != NULL) becomes `Some`, a dead one `None`. Emitted after the Weak macro,
// where both the Shared and the Optional<Shared<T>> instance structs are already complete.
// A library `Rc<Shape>`'s `downgrade()` (Shared IFACE -> its Weak partner): field-copy the fat pointer +
// bump `weak`. Mirrors the intrinsic Shared->Weak assignment, as a named `static inline` method so
// `rc.downgrade()` dispatches to it (registered as an intrinsic on the Shared instance).
void CEmitter::emitSharedToWeakDowngrade(const CollectionInfo& info)
{
    const std::string& wk = info.ifacePartner;           // the Weak partner (RcWeak_<elem>)
    *_out << "static inline " << wk << " " << info.cName << "__" << info.downgradeName
          << "(" << info.cName << "* self) {\n"
          << "    " << wk << " w;\n"
          << "    w.obj = self->obj; w.vtbl = self->vtbl; w.ctrl = self->ctrl;\n"
          << "    if (w.ctrl) w.ctrl->weak++;\n"
          << "    return w;\n"
          << "}\n";
}

void CEmitter::emitWeakTryUpgrade(const CollectionInfo& info)
{
    std::string sh  = info.ifacePartner;                 // the Shared it upgrades to (Shared_<elem> or a library `Rc_<elem>`)
    std::string opt = "Optional_" + sh;                 // Optional_<shared>
    if (!_genericTypeInsts.count(opt)) return;          // prelude Optional unavailable -> skip (upgrade stays)
    *_out << "static inline " << opt << " " << info.cName << "__tryUpgrade(" << info.cName << "* self) {\n"
          << "    " << sh << " s = " << info.cName << "__upgrade(self);\n"
          << "    if (s.ctrl) return (" << opt << "){ .tag = " << opt << "_Some, .u.Some = { .value = s } };\n"
          << "    return (" << opt << "){ .tag = " << opt << "_None };\n"
          << "}\n";
}

// `.find(substring:)` -> Optional<usize>: wrap the raw byte-scan helper so an absent match is `None`,
// never a sentinel. Mirrors emitWeakTryUpgrade — pure ISO C compound literals (no statement-expression),
// emitted in the collection FUNCS phase where the Optional<usize> instance struct is already complete.
void CEmitter::emitStringFind(const CollectionInfo& info)
{
    const std::string opt = "Optional_usize";
    if (!_genericTypeInsts.count(opt)) return;   // Optional<usize> unavailable -> skip (find unregistered)
    *_out << "static inline " << opt << " " << info.cName << "__find(" << info.cName
          << "* self, kama_string needle) {\n"
          << "    size_t off;\n"
          << "    if (kama_string__find_raw(self, needle, &off))\n"
          << "        return (" << opt << "){ .tag = " << opt << "_Some, .u.Some = { .value = off } };\n"
          << "    return (" << opt << "){ .tag = " << opt << "_None };\n"
          << "}\n";
}

// Emit the KAMA_*_{TYPE,FUNCS}(...) macro line per registered instantiation.
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
        std::string elemDtor = info.elemDestructible ? (info.elemClass + "__dtor") : "KAMA_ELEM_NODTOR";
        std::string tail = typesOnly ? ")\n"                          // _TYPE(T, NAME)
                                     : (", " + elemDtor + ")\n");     // _FUNCS(T, NAME, ELEM_DTOR)
        // Array/List `__copy` deep-copies each element — a `Copyable` resource via its own
        // `Elem__copy`, else a memberwise (bitwise) copy. (Only these two kinds have `__copy`.)
        std::string elemCopy = info.elemCopyable ? (info.elemClass + "__copy") : "KAMA_ELEM_MEMBERWISE";
        std::string collTail = typesOnly ? ")\n" : (", " + elemDtor + ", " + elemCopy + ")\n");
        if (info.kind == CollKind::Array)
            *_out << "KAMA_ARRAY_" << suf << "(" << info.elemCType << ", " << info.cName << collTail;
        else if (info.kind == CollKind::List)
            *_out << "KAMA_LIST_" << suf << "(" << info.elemCType << ", " << info.cName << collTail;
        else if (info.kind == CollKind::Owned && info.elemIsInterface)
            // fat-element `Owned<I>` — TYPE takes the vtbl type, FUNCS drops via the vtbl slot.
            *_out << "KAMA_OWNED_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : ")\n");
        else if (info.kind == CollKind::Owned)
            *_out << "KAMA_OWNED_" << suf << "(" << info.elemCType << ", " << info.cName << tail;
        else if (info.kind == CollKind::Shared && info.elemIsInterface) {
            *_out << "KAMA_SHARED_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : ")\n");
            // a library `Rc<Shape>` (Shared IFACE with a weak partner) gets a `downgrade()` that
            // field-copies {obj,vtbl,ctrl} into its Weak partner + bumps `weak`.
            if (!typesOnly && !info.downgradeName.empty()) emitSharedToWeakDowngrade(info);
        }
        else if (info.kind == CollKind::Shared)
            *_out << "KAMA_SHARED_" << suf << "(" << info.elemCType << ", " << info.cName << tail;
        else if (info.kind == CollKind::Weak && info.elemIsInterface) {
            // The C `__upgrade` (-> the Shared partner) stays an internal helper; `tryUpgrade` wraps it.
            *_out << "KAMA_WEAK_IFACE_" << suf << "(" << info.cName
                 << (typesOnly ? (", " + info.elemClass + "_vtbl)\n") : (", " + info.ifacePartner + ")\n"));
            if (!typesOnly) emitWeakTryUpgrade(info);
        }
        else if (info.kind == CollKind::Weak) {
            *_out << "KAMA_WEAK_" << suf << "(" << info.elemCType << ", " << info.cName
                 << (typesOnly ? ")\n" : (", " + info.ifacePartner + ")\n"));
            if (!typesOnly) emitWeakTryUpgrade(info);
        }
        else if (info.kind == CollKind::Bindable)
            // Fully type-erased — the signature drives only the invoke, not the layout.
            *_out << "KAMA_BINDABLE_" << suf << "(" << info.cName << ")\n";
        else if (info.kind == CollKind::Fixed) {
            // Value array: `struct { T v[N]; }` + bounds-checked get/set/at/length/fill. It embeds T
            // by value, so its `_TYPE` is emitted in the by-value struct-body order (emitHeaderContent),
            // NOT in this early types pass — only the `_FUNCS` half comes from here.
            if (!typesOnly)
                *_out << "KAMA_FIXED_FUNCS(" << info.elemCType << ", " << info.constValue
                     << ", " << info.cName << ")\n";
        }
        else if (info.kind == CollKind::String) {
            // kama_string itself is predefined in the runtime header; only the `.find()` Optional wrapper
            // (which needs the program-specific Optional_usize struct) is emitted here, in the FUNCS phase.
            if (!typesOnly) emitStringFind(info);
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
    if (cls.empty() || !_classes.count(cls) || !_classes[cls].isIntrinsicColl) return false;
    coll     = _classes[cls].name;
    // A `Fixed<T,N>` knows its size at compile time, so a CONSTANT out-of-range index is a
    // compile-time error, not just a runtime trap (the safe-array payoff).
    SharedExpression idxExpr = (ea->expressionlist && !ea->expressionlist->empty())
                                   ? (*ea->expressionlist)[0] : SharedExpression();
    if (isFixedColl(cls) && idxExpr) {
        int64_t iv;
        if (constValue(idxExpr, iv) && (iv < 0 || iv >= _collections[cls].constValue))
            unsupported(("index " + std::to_string(iv) + " is out of bounds for `" + cls + "` (length "
                         + std::to_string(_collections[cls].constValue) + ")").c_str(), ea->line);
    }
    // The receiver is emitted as a PLACE (an lvalue): a plain name stays itself, but a nested index
    // (`m[i]` in `m[i][j]`) becomes `(*Outer__at(&m, i))` so `&recvExpr` is a real `T*`, not the
    // address of a by-value `__get` rvalue. This is what makes chained/field-write indexing valid C.
    recvExpr = emitPlace(recv);
    // `this` inside a retro `implements C for <collection>` body is `self` — ALREADY a pointer, not a
    // by-value lvalue. Every caller takes `&(recvExpr)`, so hand back the place `(*self)` → `&(*self)` == self
    // (without this, `this[i]` emits `__get(&self, i)`, indexing the pointer's own address — a string-key
    // `hash`/`equals` would read struct bytes, not content, and Map lookups would miss).
    if (dynamic_cast<ThisAccessNode*>(recv.get()))
        recvExpr = "(*" + recvExpr + ")";
    idx      = idxExpr ? emitExpression(idxExpr) : "0";
    return true;
}

// A C lvalue (place) for `e`: an indexed element lowers to `(*NAME__at(&recv, i))` via the
// bounds-checked place intrinsic (recursing through the receiver so nested `a[i][j]` chains stay
// valid — `&(*__at(...))` folds back to the `T*`). Everything else is already an lvalue.
std::string CEmitter::emitPlace(SharedExpression e)
{
    if (auto* ea = dynamic_cast<ElementAccessNode*>(e.get())) {
        std::string coll, recvExpr, idx;
        if (collectionElemAccess(ea, coll, recvExpr, idx))   // recvExpr is itself a place (recursive)
            return "(*" + coll + "__at(&(" + recvExpr + "), " + idx + "))";
        // A user place-returning `operator[]`: `a[i]` -> `(*Class__op_index(&(place of a), i))`.
        // The receiver is emitted as a place too, so a nested `m[i][j]` chains cleanly.
        SharedExpression recv = ea->expression ? ea->expression
                                               : std::static_pointer_cast<ExpressionNode>(ea->identifier);
        if (recv) if (MethodInfo* op = userIndexOp(exprClass(recv))) {
            std::string idxE = (ea->expressionlist && !ea->expressionlist->empty())
                                   ? emitExpression((*ea->expressionlist)[0]) : "0";
            // `this` is already `self` (a pointer) — pass it directly; any other lvalue's address is `&place`.
            std::string recvAddr = dynamic_cast<ThisAccessNode*>(recv.get())
                                       ? emitExpression(recv) : ("&(" + emitPlace(recv) + ")");
            return "(*" + op->cName + "(" + recvAddr + ", " + idxE + "))";
        }
    }
    return emitExpression(e);
}

// The place-returning `operator[]` on `cls` or an ancestor (else null). `op_index` is registered with
// `isPlaceReturn` only for `ref T operator[]`.
MethodInfo* CEmitter::userIndexOp(const std::string& cls)
{
    if (cls.empty() || !_classes.count(cls)) return nullptr;
    ClassInfo* owner = nullptr;
    MethodInfo* mi = findMethod(&_classes[cls], "op_index", &owner);
    return (mi && mi->isOperator && mi->isPlaceReturn) ? mi : nullptr;
}

bool CEmitter::indexesUserOp(ElementAccessNode* ea)
{
    if (!ea) return false;
    SharedExpression recv = ea->expression ? ea->expression
                                           : std::static_pointer_cast<ExpressionNode>(ea->identifier);
    return recv && userIndexOp(exprClass(recv)) != nullptr;
}

// cType(typeNode) resolved in the type-substitution context of a generic-instance class `inCls` — so a
// method's `T`-typed return (e.g. `iterator()` -> `VecIter<T>`, `next()` -> `Optional<T>`) resolves to
// the concrete instance (`VecIter_int32` / `Optional_int32`). Mirrors computeDestructible's binding.
std::string CEmitter::cTypeInInstance(const std::string& inCls, SharedIdentifier typeNode)
{
    if (!_genericTypeInsts.count(inCls)) return cType(typeNode);   // non-generic: plain
    auto savedSubst = _typeSubst; NsCtx savedCtx = _nsCtx;
    const GenericTypeInst& gi = _genericTypeInsts[inCls];
    _nsCtx = _genericTypeInstCtx.count(inCls) ? _genericTypeInstCtx[inCls] : _genericTypeCtx[gi.templateKey];
    _typeSubst.clear();
    const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
    for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
    std::string r = cType(typeNode);
    _typeSubst = savedSubst; _nsCtx = savedCtx;
    return r;
}

// `foreach` over a user type via the ITERATOR PROTOCOL (structural, zero-cost — direct monomorphized
// calls, no vtable). VALUE (`foreach (T x in v)`): `v.iterator()` yields an iterator with
// `next() -> Optional<T>` (or `v` itself is the iterator); loop while `next()` returns `Some`. MUTABLE
// (`foreach (ref T x in v)`): `v.iterMut()` yields an iterator with `hasNext()` + a place-returning
// `next()`; loop while `hasNext()`, binding `x` to the place. A borrowing iterator holds a `Ptr` cursor
// (its own unsafe internals); the `foreach` surface stays safe.
void CEmitter::emitForeachIterator(ForEachNode* fe, const std::string& container, int depth)
{
    ClassInfo* cc = &_classes[container];
    std::string nm = (fe->name && fe->name->value) ? *fe->name->value : "__x";
    std::string elemTy = cType(fe->type);
    std::string elemClass = isClass(elemTy) ? elemTy : "";   // for `x.method()` resolution
    int id = _tempCounter++;
    std::string it = "__it" + std::to_string(id);
    std::string ot = "__o"  + std::to_string(id);
    // Emit the iterable expression WITH a hoist slot: an owned rvalue operand of a compiler iterator (the
    // receiver of `s.trim().chars()`, the separator of `x.split(a + b)`) materializes into a scope-dtor'd
    // temp that must live for the whole loop. `outerPre` marks the enclosing scope's locals before that
    // materialization so we can relocate those temps into the loop wrapper (declared before the iterator,
    // dropped after the loop) below — the A4 receiver-materialize pattern.
    size_t outerPre = _scopes.empty() ? 0 : _scopes.back().locals.size();
    auto emitIterable = [&]() -> std::string {
        bool ph = _hoistOK; _hoistOK = true;
        std::string r = emitExpression(fe->expression);
        _hoistOK = ph;
        return r;
    };

    // Resolve the iterator + method C-names (structural). `iterCall`/`nextCall`/`hasNextCall` are the
    // full direct calls; `iterCType`/`optC` the concrete types.
    std::string iterCType, iterInit, nextCall, hasNextCall, optC;
    if (fe->isRef) {
        MethodInfo* iterMi = findMethod(cc, "iterMut", nullptr);
        if (!iterMi || !iterMi->params.empty()) {
            unsupported(("`foreach (ref …)` over `" + container + "` needs a nullary `iterMut()` "
                         "(a mutable iterator)").c_str(), fe->line); *_out << "\n"; return;
        }
        if (!implementsContractTemplate(cc, "IterableMut")) {   // nominal: the container must declare it
            unsupported(("`" + container + "` must `implements IterableMut<T>` to be used in a "
                         "`foreach (ref …)`").c_str(), fe->line); *_out << "\n"; return;
        }
        iterCType = cTypeInInstance(container, iterMi->returnType);
        ClassInfo* ic = _classes.count(iterCType) ? &_classes[iterCType] : nullptr;
        MethodInfo* hasNextMi = ic ? findMethod(ic, "hasNext", nullptr) : nullptr;
        MethodInfo* nextMi    = ic ? findMethod(ic, "next", nullptr) : nullptr;
        if (!hasNextMi || !nextMi || !nextMi->isPlaceReturn) {
            unsupported(("a mutable iterator (`" + iterCType + "`) needs `hasNext()` and a "
                         "place-returning `ref T next()`").c_str(), fe->line); *_out << "\n"; return;
        }
        if (!implementsContractTemplate(ic, "IteratorMut")) {   // nominal: it must declare the protocol
            unsupported(("the mutable iterator `" + iterCType + "` must `implements IteratorMut<T>` "
                         "to be used in a `foreach (ref …)`").c_str(), fe->line); *_out << "\n"; return;
        }
        iterInit    = iterMi->cName + "(&(" + emitIterable() + "))";
        hasNextCall = hasNextMi->cName + "(&" + it + ")";
        nextCall    = nextMi->cName + "(&" + it + ")";
    } else {
        MethodInfo* iterMi = findMethod(cc, "iterator", nullptr);
        if (iterMi && !iterMi->params.empty()) iterMi = nullptr;
        ClassInfo* ic = nullptr;
        if (iterMi) { iterCType = cTypeInInstance(container, iterMi->returnType);
                      ic = _classes.count(iterCType) ? &_classes[iterCType] : nullptr;
                      iterInit = iterMi->cName + "(&(" + emitIterable() + "))";
                      if (!implementsContractTemplate(cc, "Iterable")) {   // nominal: the container declares it
                          unsupported(("`" + container + "` must `implements Iterable<T>` to be used in a "
                                       "`foreach`").c_str(), fe->line); *_out << "\n"; return;
                      } }
        else        { iterCType = container; ic = cc;                       // the container IS the iterator
                      iterInit = emitIterable(); }                          // (needs only Iterator<T>, below)
        MethodInfo* nextMi = ic ? findMethod(ic, "next", nullptr) : nullptr;
        if (!nextMi || !nextMi->params.empty()) {
            unsupported(("`foreach` over `" + container + "` needs a nullary `iterator()`, or a nullary "
                         "`next()` returning `Optional<T>`").c_str(), fe->line); *_out << "\n"; return;
        }
        if (!implementsContractTemplate(ic, "Iterator")) {   // nominal: it must declare the protocol
            unsupported(("the iterator `" + iterCType + "` must `implements Iterator<T>` to be used "
                         "in a `foreach`").c_str(), fe->line); *_out << "\n"; return;
        }
        optC     = cTypeInInstance(iterCType, nextMi->returnType);
        nextCall = nextMi->cName + "(&" + it + ")";
    }

    // Outer wrapper: the iterator, then the loop. All calls are direct (monomorphized), no vtable.
    *_out << "{\n";
    // Relocate any owned-iterable operand temps materialized by emitIterable (e.g. `s.trim()` for
    // `.chars()`) into a WRAPPER scope: declared before the iterator (they back its borrowed bytes) and
    // dropped AFTER the loop (the iterator borrows them for every iteration). Recorded in the enclosing
    // scope by hoistStringTemp; move the delta here so declaration + drop share the wrapper block.
    Scope wrap;
    if (!_scopes.empty() && _scopes.back().locals.size() > outerPre) {
        wrap.locals.assign(_scopes.back().locals.begin() + outerPre, _scopes.back().locals.end());
        _scopes.back().locals.resize(outerPre);
    }
    _scopes.push_back(wrap);
    flushHoisted(depth + 1);   // declare the relocated iterable temps before the iterator line
    indent(depth + 1); *_out << iterCType << " " << it << " = " << iterInit << ";\n";
    indent(depth + 1); *_out << "while (" << (fe->isRef ? hasNextCall : std::string("1")) << ") {\n";

    Scope sc; sc.isLoopBoundary = true;
    _scopes.push_back(sc);
    bool hadType = _localTypes.count(nm);
    std::string prevType = hadType ? _localTypes[nm] : std::string();
    _localTypes[nm] = elemClass;
    bool hadRef = _refParams.count(nm);
    if (fe->isRef) {
        _refParams.insert(nm);   // reads/writes deref the place, like a `ref` param / built-in `foreach ref`
        indent(depth + 2); *_out << elemTy << "* " << nm << " = " << nextCall << ";\n";
    } else {
        indent(depth + 2); *_out << optC << " " << ot << " = " << nextCall << ";\n";
        indent(depth + 2); *_out << "if (" << ot << ".tag == " << optC << "_None) break;\n";
        indent(depth + 2); *_out << elemTy << " " << nm << " = " << ot << ".u.Some.value;\n";
        // A destructible by-value element that the binding OWNS must RAII-drop each iteration, else it
        // leaks — but only if `next()` yields a FRESH owned value. The compiler-provided `Split` iterator
        // does. `Iterator<T>` yields BY VALUE (a copy — SPEC), so the binding OWNS a destructible element
        // and must drop it each iteration. This is sound because a `next()` returning an owning value into
        // its `Optional<T>` must hand it off with `give`/`copy` (a bare named payload is rejected at variant
        // construction), so the yield is always a fresh/owned value, never an alias the iterator still owns.
        if (_classes.count(elemTy) && _classes[elemTy].destructible)
            recordDestructibleLocal(nm, elemTy);
    }
    // Mutation guard: mutating the container mid-loop is the author's concern for a user iterator (its
    // `Ptr` cursor would dangle) — the built-in `add`-reject can't see into user methods. Still push the
    // root so a mix of a user container + a built-in field-collection `add` inside is caught.
    std::string iterRoot = rootBinding(fe->expression);
    if (!iterRoot.empty()) _foreachColls.push_back(iterRoot);

    SharedStatement last;
    if (auto* b = dynamic_cast<BlockNode*>(fe->body.get())) {
        if (b->statements) for (auto& st : *b->statements) { emitStatement(st, depth + 2); last = st; }
    } else if (fe->body) {
        emitStatement(fe->body, depth + 2); last = fe->body;
    }
    if (!(last && stmtIsJump(last))) emitScopeCleanup(_scopes.back(), depth + 2);

    if (!iterRoot.empty()) _foreachColls.pop_back();
    if (hadType) _localTypes[nm] = prevType; else _localTypes.erase(nm);
    if (fe->isRef && !hadRef) _refParams.erase(nm);
    _scopes.pop_back();

    indent(depth + 1); *_out << "}\n";   // close while
    emitScopeCleanup(_scopes.back(), depth + 1);   // drop relocated iterable temps (e.g. `s.trim()`) after the loop
    _scopes.pop_back();                            // wrapper scope
    indent(depth);     *_out << "}\n";   // close wrapper
}

// A fixed-array value literal (`[a, b, c]` or `[v; N]`) initializing a `Fixed<T,N>`. Its type comes
// from the enclosing typed position (a Fixed local, return, or assignment, threaded via the same
// target-type context as `match`/variant construction). List form -> a C99 compound literal over the
// backing array (`(NAME){ .v = { … } }`, count checked == N); fill form -> the runtime `NAME__fill(v)`.
std::string CEmitter::emitArrayLiteral(ArrayLiteralNode* al)
{
    if (!al) return "0";
    std::string ty = _variantTargetType;
    if (!isFixedColl(ty)) ty = _matchTargetCType;
    if (!isFixedColl(ty)) {
        unsupported("an array literal `[…]` initializes a `Fixed<T, N>` — its type must be known from "
                    "context (a typed local, return, or assignment)", al->line);
        return "0";
    }
    int64_t n = _collections[ty].constValue;
    // Each element's target type is the Fixed's element type — so a NESTED array literal
    // (`Fixed<Fixed<int32,2>,2> = [[1,2],[3,4]]`) resolves each inner `[…]` to the element Fixed.
    std::string elemCType = _collections[ty].elemCType;
    ScopedStr _tm(_matchTargetCType, elemCType), _tv(_variantTargetType, elemCType);
    if (al->elements) {
        size_t count = al->elements->size();
        if ((int64_t)count != n) {
            unsupported(("this array literal has " + std::to_string(count) + " element(s) but `" + ty
                         + "` holds " + std::to_string(n)).c_str(), al->line);
            return "0";
        }
        std::string s = "(" + ty + "){ .v = {";
        // Each element by value — an inline constructor element (`[Point(x:1,y:2), …]`) is
        // materialized into a hoisted temp (ISO C, no statement-expression), like an operator operand.
        for (size_t i = 0; i < al->elements->size(); ++i)
            s += (i ? ", " : " ") + emitOperandByValue((*al->elements)[i]);
        s += " } }";
        return s;
    }
    // fill form `[v; count]` — the count must be a constant equal to N.
    int64_t fc;
    if (!constValue(al->fillCount, fc)) {
        unsupported("the count in a fill literal `[v; N]` must be an integer constant", al->line);
        return "0";
    }
    if (fc != n) {
        unsupported(("this fill literal repeats " + std::to_string(fc) + " time(s) but `" + ty
                     + "` holds " + std::to_string(n)).c_str(), al->line);
        return "0";
    }
    return ty + "__fill(" + emitOperandByValue(al->fillValue) + ")";
}

// ---- Smart pointers (Owned, Shared) ---------------------------------------

bool CEmitter::isSmartPtrClass(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isIntrinsicColl &&
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
    if (dynamic_cast<MemberAccessNode*>(e) || dynamic_cast<ElementAccessNode*>(e)
        || dynamic_cast<BaseAccessNode*>(e)) return true;
    if (auto* id = dynamic_cast<IdentifierNode*>(e)) {
        // a `::`-scope-resolved enum member / variant construction (`Color::Blue`, `Box::Empty`)
        // is a FRESH rvalue, not a movable named lvalue. Distinguish it from an object access
        // `obj.field` (also a qualified IdentifierNode) by the qualifier head: a local/current-class
        // field head is an object; a type/namespace head is scope resolution.
        if (id->qualifier && !id->qualifier->empty()) {
            const std::string& head = *(*id->qualifier)[0];
            if (_localTypes.count(head)) return true;                        // obj.field…
            if (_currentClass && findFieldOwner(_currentClass, head)) return true;
            auto q = std::make_shared<StringList>();
            for (size_t i = 0; i + 1 < id->qualifier->size(); ++i) q->push_back((*id->qualifier)[i]);
            std::string en = resolveUserName(*id->qualifier->back(), q);
            if (_enums.count(en)) return false;                              // enum member
            auto c = _classes.find(en);
            if (c != _classes.end() && c->second.isVariant) return false;    // variant construction
            if (_genericTypeParams.count(en) || _genericTypes.count(en))     // generic union: Optional::None
                return false;
        }
        return true;
    }
    return false;
}

// an interface value borrows its object, so it's a second-class view — it may
// be a parameter or local, but it can't be STORED beyond the call that produced it
// (a field, a return, a collection element) without dangling. Reject the bare-interface
// case with guidance toward owning the object (`Shared<I>` — enables that).
void CEmitter::rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line)
{
    if (!ty || !isInterface(cType(ty))) return;
    std::string nm = (ty->value && !ty->value->empty()) ? *ty->value : cType(ty);
    unsupported(("a contract (`" + nm + "`) borrows its object, so it can't be " + whereClause
                 + " — it would dangle; own the object instead (e.g. `Shared<" + nm + ">`)").c_str(), line);
}

// A plain transferable lvalue: a bare identifier naming a smart-pointer local/
// param. A `new ...<T>(...)` initializer is NOT an lvalue (no source to touch).
bool CEmitter::isSmartPtrLValue(SharedExpression e)
{
    auto* id = dynamic_cast<IdentifierNode*>(e.get());
    return id && id->value && isSmartPtrExpr(e);
}

// Is `src` a concrete-element owning handle being widened into the contract-element
// intrinsic `dstTy`? True iff dst is an intrinsic smart-ptr over an INTERFACE, src is a
// named library heap-owner (`Shared`/`Owned`) whose concrete pointee nominally implements
// that interface, and the ownership KIND matches (library `Shared`(copyable)↔intrinsic
// `Shared`; library `Owned`↔intrinsic `Owned`). A `Weak` or a contract→contract source
// (not a library heap-owner) is not handled here.
bool CEmitter::isSmartPtrUpcast(const std::string& dstTy, SharedExpression src)
{
    if (!isSmartPtrClass(dstTy) || !isInterface(_classes[dstTy].collElemClass)) return false;
    if (!src || !isNamedValue(src.get())) return false;
    std::string libT = heapOwnerTarget(exprClass(src));         // concrete pointee ("" if not a library owner)
    if (libT.empty()) return false;
    auto it = _classes.find(libT);
    if (it == _classes.end()) return false;
    const std::string& dstElem = _classes[dstTy].collElemClass; // interface C name
    bool implementsIt = false;
    for (auto& i : it->second.interfaces) if (i == dstElem) { implementsIt = true; break; }
    if (!implementsIt) return false;
    // kind must agree: a copyable library owner is a `Shared`, a move-only one an `Owned`.
    CollKind dk = smartKind(dstTy);
    return isCopyable(exprClass(src)) ? (dk == CollKind::Shared) : (dk == CollKind::Owned);
}

// Emit the upcast field-bridge into the already-declared intrinsic handle `nm`. Mirrors the
// library→intrinsic bridge in emitBindableBind: the pointee via `deref()`, the concrete's
// `__as_<Contract>` vtable, and (Shared/Weak) the shared `kama_ctrl` block. Retains a
// `Shared` (strong++) or moves an `Owned` (suppress the library source's dtor). Precondition:
// isSmartPtrUpcast(dstTy, src).
void CEmitter::emitSmartPtrUpcast(const std::string& nm, const std::string& dstTy,
                                  SharedExpression src, int handoff, int depth, int line)
{
    std::string srcCls  = exprClass(src);
    std::string libT    = heapOwnerTarget(srcCls);              // concrete pointee C name
    const std::string& dstElem = _classes[dstTy].collElemClass; // interface C name
    CollKind dk = smartKind(dstTy);
    // give/copy on an upcast follows the same matrix as a same-type hand-off: `give` MOVES the
    // handle (every kind is movable), `copy` RETAINS (an `Owned` can't be copied — it's unique),
    // bare defaults to the source's nature (a `Shared`/`Weak` retains, an `Owned` moves).
    bool copyable = isCopyable(srcCls);
    bool retain = (handoff == 1) ? false : (handoff == 2 ? true : copyable);
    if (handoff == 2 && !copyable)
        unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", line);
    std::string srcE = emitExpression(src);
    // pointee: a library owner exposes it via `deref()` (T*); the concrete's per-contract vtable
    // fattens it. The refcount block is the library `.c` (kama_ctrl-compatible), shared with the source.
    indent(depth); *_out << nm << ".obj = (void*)" << srcCls << "__deref(&(" << srcE << "));\n";
    indent(depth); *_out << nm << ".vtbl = &" << libT << "__as_" << dstElem << ";\n";
    if (dk != CollKind::Owned) {                                // intrinsic Owned<I> is {obj, vtbl} — no ctrl
        indent(depth); *_out << nm << ".ctrl = (kama_ctrl*)(" << srcE << ").c;\n";
        if (retain) { indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->"
                                            << (dk == CollKind::Weak ? "weak" : "strong") << "++;\n"; }
    }
    // move (bare `Owned`, or `give`): consume the source at compile time so its dtor is skipped —
    // the destination handle now owns the ref (it shares the same ctrl without an increment).
    if (!retain) { std::string mv = moveOnlySource(src, line); if (!mv.empty()) markMoved(mv); }
}

// Is `src` a derived-class handle being widened into the base-class handle `dstTy`? Both are
// thin library `Shared`/`Owned` structs; the pointee is a `virtual class` (so it has a virtual
// destructor). True iff both are library owners of the same kind and dst's pointee is a base of
// src's pointee.
bool CEmitter::isSmartPtrBaseUpcast(const std::string& dstTy, SharedExpression src)
{
    if (!src || !isNamedValue(src.get())) return false;
    std::string baseT = heapOwnerTarget(dstTy);                // dst pointee ("" if not a library owner)
    if (baseT.empty()) return false;
    std::string derivedT = heapOwnerTarget(exprClass(src));    // src pointee
    if (derivedT.empty() || baseT == derivedT) return false;
    if (!isBaseOf(baseT, derivedT)) return false;
    return isCopyable(dstTy) == isCopyable(exprClass(src));     // both Shared, or both Owned
}

// Emit the base upcast into the already-declared handle `nm`. Builds it through the library's own
// ctor/`adopt` (so the dest fields aren't poked directly), with the pointer adjusted to the base
// subobject via the `__base` chain (offset 0 in Kama's single-vptr model). A `Shared` retains
// (shares the ctrl, strong++); an `Owned` moves (source consumed). Precondition: isSmartPtrBaseUpcast.
void CEmitter::emitSmartPtrBaseUpcast(const std::string& nm, const std::string& dstTy,
                                      SharedExpression src, int handoff, int depth, int line)
{
    std::string srcCls   = exprClass(src);
    std::string baseT    = heapOwnerTarget(dstTy);
    std::string derivedT = heapOwnerTarget(srcCls);
    // Same give/copy matrix as a same-type hand-off: `give` MOVES, `copy` RETAINS (`Owned` can't
    // be copied), bare defaults to the source's nature (`Shared`/`Weak` retain, `Owned` move).
    bool copyable = isCopyable(srcCls);
    bool retain = (handoff == 1) ? false : (handoff == 2 ? true : copyable);
    if (handoff == 2 && !copyable)
        unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", line);
    std::string srcE = emitExpression(src);
    // base-subobject pointer: the derived pointee (via deref()) walked down the `__base` chain.
    std::string bp = basePathTo(&_classes[derivedT], &_classes[baseT]);
    if (!bp.empty()) bp.pop_back();                            // drop trailing '.'
    std::string basePtr = "&((" + srcCls + "__deref(&(" + srcE + ")))->" + bp + ")";
    if (copyable) {
        // Shared/Weak: build the base handle sharing the source's ctrl; retain bumps the count,
        // a move transfers the ref (no bump — the source's dtor is suppressed below).
        indent(depth); *_out << dstTy << "__ctor(&" << nm << ", " << basePtr << ", (" << srcE << ").c);\n";
        if (retain) { indent(depth); *_out << "(" << srcE << ").c->strong++;\n"; }
    } else {
        // Owned: adopt the base subobject (no ctrl); always a move.
        indent(depth); *_out << nm << " = " << dstTy << "__adopt(" << basePtr << ");\n";
    }
    if (!retain) { std::string mv = moveOnlySource(src, line); if (!mv.empty()) markMoved(mv); }
}

std::string CEmitter::ownerElem(const std::string& cls)
{
    std::string t = heapOwnerTarget(cls);                      // library Shared/Owned
    if (!t.empty()) return t;
    if (isSmartPtrClass(cls)) return _classes[cls].collElemClass;   // intrinsic (contract handle, Weak)
    return "";
}

bool CEmitter::isSmartPtrHandoffMismatch(const std::string& dstTy, SharedExpression src)
{
    if (!src || !isNamedValue(src.get())) return false;
    std::string de = ownerElem(dstTy), se = ownerElem(exprClass(src));
    if (de.empty() || se.empty() || de == se) return false;    // both must own; a same-element conv is handled elsewhere
    return !isSmartPtrUpcast(dstTy, src) && !isSmartPtrBaseUpcast(dstTy, src);
}

// Invalidate a moved-from smart pointer: null the field its dtor guards on, so
// the source's drop becomes a no-op (the ref/ownership transfers to the dest).
std::string CEmitter::smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem)
{
    // an owned INTERFACE handle's value field is the fat pointer `.obj`, not `.ptr`.
    if (kind == CollKind::Owned) return expr + (ifaceElem ? ".obj = NULL;" : ".ptr = NULL;");
    return expr + ".ctrl = NULL; " + expr + (ifaceElem ? ".obj = NULL;" : ".ptr = NULL;");  // Shared/Weak guard on ctrl
}

// ---- Resource-value move analysis -----------------------------------------

// A move-only VALUE: a destructible class value that isn't a smart-ptr / collection / extern
// struct. It moves on hand-off (its dtor is suppressed) and is never silently copied. (Trigger
// = destructibility — the proxy for a `resource`.)
bool CEmitter::isMoveOnlyValue(const std::string& cls) const
{
    auto it = _classes.find(cls);
    if (it == _classes.end() || it->second.isIntrinsicColl || it->second.isExternStruct || isSmartPtrClass(cls))
        return false;
    const ClassInfo& ci = it->second;
    // Move-only-ness is the declared kind: a `resource` moves even if it owns nothing (an empty
    // resource is a move-only identity/token); a `value` copies. An `Intrinsic` (a tagged-union enum;
    // collections/smart-ptrs already returned above) moves iff it owns a resource (is destructible).
    if (ci.kind == TypeKind::Resource) return true;
    if (ci.kind == TypeKind::Value)    return false;
    return ci.destructible;
}

// A by-value class value the SLOT/CALLEE OWNS: a move-only `resource`, or a heap-owning collection/string
// (List/Array/string — NOT a `Fixed<T,N>`, a bitwise value; NOT a smart pointer, which has its own
// give/copy-with-refcount path). The gate for "a NAMED hand-off here needs `give` (move) / `copy` (deep)".
// Disjoint from isMoveOnlyValue (which excludes collections), so every existing isMoveOnlyValue(x) store
// site reads ownsByValue(x) with no change for a non-collection type.
bool CEmitter::ownsByValue(const std::string& cls) const
{
    if (cls.empty() || isSmartPtrClass(cls)) return false;
    if (isMoveOnlyValue(cls)) return true;
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.isIntrinsicColl && !isFixedColl(cls);
}

// has this type opted into the `Copyable` contract? (Detected structurally at collection
// time — a public nullary `copy` returning the own type; see collectClasses.) Only ever consulted
// for a move-only value, where it flips the marker from "silent move" to "mandatory give/copy".
bool CEmitter::isCopyable(const std::string& cls) const
{
    auto it = _classes.find(cls);
    return it != _classes.end() && it->second.copyable;
}

// Does concrete C-type `t` satisfy the contract `bound`? For `Copyable` (the conditional-implements
// container case): a primitive or a `value` is copyable (bitwise), a `resource` only if it implements
// Copyable. For any other contract: the concrete class must nominally implement it.
bool CEmitter::satisfiesBound(const std::string& t, const std::string& bound) const
{
    auto it = _classes.find(t);
    if (bound == "Copyable") {
        if (it == _classes.end()) return true;                 // primitive C type → bitwise-copyable
        if (it->second.kind == TypeKind::Value) return true;   // a value → bitwise-copyable
        return it->second.copyable;                            // a resource → only if `implements Copyable`
    }
    // A retroactive `implements <bound> for t` also satisfies it — including a PRIMITIVE target (`int32`),
    // whose conformance lives in _primConformances (NOT _classes). Consult the pre-scan so a `when
    // [T: Equatable]` gate on `List<int32>` sees int32's retro `Equatable` (matched on the raw source name).
    auto rc = _retroConformances.find(t);
    if (rc != _retroConformances.end() && rc->second.count(bound)) return true;
    if (it == _classes.end()) return false;
    for (auto& itf : it->second.interfaces) if (itf == bound) return true;
    return false;
}


void CEmitter::markMoved(const std::string& cVar)
{
    //  (Increment 3): moving a local declared OUTSIDE the nearest enclosing loop would move
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
// moved-from value (use Optional<T> for the field-move case).
std::string CEmitter::moveOnlySource(SharedExpression e, int line)
{
    if (auto* id = dynamic_cast<IdentifierNode*>(e.get())) {
        std::string nm = id->value ? *id->value : "";
        if ((!id->qualifier || id->qualifier->empty()) && _moveState.count(nm)) return nm;
    }
    unsupported("cannot `give` out of a field/element — it would leave the owner holding a "
                "moved-from value; move a local instead, or use Optional<T>", line);
    return "";
}

std::string CEmitter::emitSmartPtrCall(const std::string& cls, const std::string& recvExpr,
                                       const std::string& method, SharedArgumentList args, int srcLine)
{
    // An intrinsic on the pointer itself (lock/expired/valid)?
    if (_classes[cls].methods.count(method))
        return emitDispatch(cls, "&(" + recvExpr + ")", method, args, srcLine);
    // an owned INTERFACE handle dispatches polymorphically through its own {obj, vtbl}.
    if (isInterface(_classes[cls].collElemClass) && smartKind(cls) != CollKind::Weak)
        return emitInterfaceDispatch(recvExpr, _classes[cls].collElemClass, method, args, srcLine, cls);
    // Otherwise auto-deref to the pointee T (Owned/Shared expose a T* ptr).
    if (smartKind(cls) != CollKind::Weak)
        return emitDispatch(_classes[cls].collElemClass, "(" + recvExpr + ").ptr", method, args, srcLine);
    unsupported(("Weak<T> has no member '" + method + "'; call .tryUpgrade()").c_str(), srcLine);
    return "0";
}

// Resolve `extends` names to ClassInfo pointers; error on unknown/cycle.
// Merge each contract's refined-parent methods into its own `methods`, transitively (a refined parent may
// itself refine), so a refining contract's vtable struct, conformance table, and dispatch all see the full
// slot set — parent slots first, then own (a same-named own method wins). Cycle-safe via a visited set.
void CEmitter::linkContracts()
{
    std::set<std::string> done;
    std::function<void(const std::string&)> merge = [&](const std::string& name) {
        if (!done.insert(name).second) return;                 // already merged
        auto it = _interfaces.find(name);
        if (it == _interfaces.end() || it->second.refines.empty()) return;
        InterfaceInfo& ii = it->second;
        std::vector<InterfaceMethod> merged;
        std::set<std::string> seen;
        for (auto& own : ii.methods) seen.insert(own.name);    // own methods take precedence over inherited
        for (auto& parent : ii.refines) {
            merge(parent);                                     // fully merge the parent first (transitive)
            auto pit = _interfaces.find(parent);
            if (pit == _interfaces.end()) {
                unsupported(("contract `" + name + "` refines unknown contract `" + parent + "`").c_str(), 0);
                continue;
            }
            for (auto& pm : pit->second.methods)
                if (seen.insert(pm.name).second) merged.push_back(pm);   // inherited (dedup)
        }
        for (auto& own : ii.methods) merged.push_back(own);    // then own, after the inherited slots
        ii.methods = std::move(merged);
    };
    for (auto& kv : _interfaces) merge(kv.first);
}

void CEmitter::linkBases()
{
    // Now every file's declarations are registered: resolve each class's base +
    // interface references (bare/qualified) to their mangled names, in the
    // class's own namespace context.
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.isIntrinsicColl) continue;
        // a generic INSTANCE resolved its base/interfaces under its own subst in registerGenericTypeInst
        // (its `node` is the template's, whose refs still name the raw param `T`) — don't re-resolve here.
        if (ci.isGenericInst) continue;
        _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
        if (ci.node && ci.node->baseTypes && ci.node->baseTypes->base && ci.node->baseTypes->base->value)
            ci.baseName = resolveUserName(*ci.node->baseTypes->base->value, ci.node->baseTypes->base->qualifier);
        // resolve each interface name; a generic-contract `implements Iterator<int32>` resolves to the
        // specialized instance name (`Iterator_int32`) — its genericArgs live on the AST node, which
        // ci.interfaces (a plain name list) dropped, so read them back in parallel.
        SharedIdentifierList ifaceNodes = (ci.node && ci.node->baseTypes) ? ci.node->baseTypes->interfaces
                                                                          : SharedIdentifierList();
        for (size_t i = 0; i < ci.interfaces.size(); ++i) {
            std::string base = resolveUserName(ci.interfaces[i], nullptr);
            SharedIdentifier itfNode = (ifaceNodes && i < ifaceNodes->size()) ? (*ifaceNodes)[i] : nullptr;
            if (itfNode && itfNode->genericArg && _genericContracts.count(base))
                base = genericTypeMangle(base, itfNode->genericArgs);   // Iterator -> Iterator_int32
            ci.interfaces[i] = base;
        }
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
            // The base must be extensible: a `virtual`/`abstract class`, never a
            // plain/`final` (sealed) class.
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
    // specialized generic-type instances are emitted by a dedicated pass under _typeSubst,
    // not the normal class loops — exclude them here (their only consumer, header emission).
    for (auto& kv : _classes) if (!kv.second.isGenericInst) visit(&kv.second);
    return out;
}

// order EVERY laid-out struct (normal classes + generic instances + tagged unions) so a
// by-value dependency is always emitted first — base-before-derived AND held-value-before-holder.
// Unlike topoOrderClasses this INCLUDES generic instances and adds by-value field/payload edges, so
// `Box<Rock>`/`class Holder{Rock r;}`/`enum Event{Resize(Vec2)}` lay out correctly. A collection /
// smart-ptr field stores `T*` (a forward decl suffices) — not an edge; a by-value user struct is.
// A back-edge (self / mutual by-value) is a genuine infinite-size type and is reported.
std::vector<ClassInfo*> CEmitter::unifiedStructOrder()
{
    std::vector<ClassInfo*> out;
    std::set<ClassInfo*> done;        // fully ordered
    std::set<ClassInfo*> visiting;    // on the current DFS stack — a re-entry is an infinite-size cycle
    NsCtx savedCtxOuter = _nsCtx;
    std::map<std::string, SharedIdentifier> savedSubstOuter = _typeSubst;

    std::function<void(ClassInfo*)> visit = [&](ClassInfo* ci) {
        if (!ci || done.count(ci)) return;
        if (visiting.count(ci)) {
            unsupported(("type '" + ci->name + "' contains itself by value (infinite size) — hold a "
                         "member behind Owned<...>, Shared<...>, or List<...>").c_str(),
                        ci->node ? ci->node->line : 0);
            return;   // stop unwinding this cycle; the driver aborts on the recorded error
        }
        visiting.insert(ci);

        // Resolve this node's field types concretely: a generic instance binds its type args +
        // use-site scope (Box<Rock> -> Rock is an edge; Box<int32> -> none). Mirrors computeDestructible.
        bool inst = ci->isGenericInst && _genericTypeInsts.count(ci->name);
        if (inst) {
            const GenericTypeInst& gi = _genericTypeInsts[ci->name];
            _nsCtx = _genericTypeInstCtx.count(ci->name) ? _genericTypeInstCtx[ci->name]
                                                         : _genericTypeCtx[gi.templateKey];
            _typeSubst.clear();
            const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
            for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
        } else {
            _nsCtx = NsCtx{}; _nsCtx.scope = ci->scope; _nsCtx.usings = ci->usings; _nsCtx.symbolAliases = ci->symbolAliases;
            _typeSubst = savedSubstOuter;   // (typically empty here)
        }

        // A by-value dependency: a field/payload whose C type is a laid-out struct (not a T*-holding
        // collection, not an extern header struct). A `Fixed<T,N>` DOES embed its element by value,
        // so it is a real dependency — both as a field's type and as the Fixed struct's own element.
        auto dep = [&](SharedIdentifier ty) -> ClassInfo* {
            auto it = _classes.find(cType(ty));
            if (it == _classes.end() || it->second.isExternStruct) return nullptr;
            if (it->second.isIntrinsicColl && it->second.collKind != CollKind::Fixed) return nullptr;  // pointer storage
            return &it->second;
        };
        std::vector<ClassInfo*> deps;
        if (ci->base) deps.push_back(ci->base);
        for (auto& f : ci->fields) if (ClassInfo* d = dep(f.type)) deps.push_back(d);
        for (auto& v : ci->variants) for (auto& f : v.payload) if (ClassInfo* d = dep(f.type)) deps.push_back(d);
        // a `Fixed<T,N>` struct (`{ T v[N]; }`) embeds T by value -> its layout needs T's.
        if (ci->isIntrinsicColl && ci->collKind == CollKind::Fixed && !ci->collElemClass.empty()) {
            auto it = _classes.find(ci->collElemClass);
            if (it != _classes.end() && !it->second.isExternStruct &&
                !(it->second.isIntrinsicColl && it->second.collKind != CollKind::Fixed))
                deps.push_back(&it->second);
        }

        // restore the outer context BEFORE recursing, so each recursed node sets up its own binding.
        _nsCtx = savedCtxOuter; _typeSubst = savedSubstOuter;
        for (ClassInfo* d : deps) visit(d);

        visiting.erase(ci);
        done.insert(ci);
        out.push_back(ci);
    };

    for (auto& kv : _classes)
        if ((!kv.second.isIntrinsicColl && !kv.second.isExternStruct) ||
            (kv.second.isIntrinsicColl && kv.second.collKind == CollKind::Fixed))   // Fixed lays out by value
            visit(&kv.second);
    _nsCtx = savedCtxOuter; _typeSubst = savedSubstOuter;
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
            // a `final` slot may not be re-overridden by any subclass.
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
        if (ci->hasVtable && !ci->hasCtor && !ci->isIntrinsicColl && !ci->isExternStruct) {
            ci->synthCtor = true;
            ci->hasCtor   = true;   // `new` now calls the ctor; prototype gets emitted
        }
    }
    // Whole-program override index for devirtualization: a virtual slot must stay an indirect call
    // iff some class overrides it. A never-overridden slot (and every method of a `final` class /
    // any `final` method) has a unique target, so emitDispatch can lower it to a direct call.
    for (auto& kv : _classes)
        for (auto& mkv : kv.second.methods)
            if (mkv.second.isOverride)
                _overriddenSlots.insert(std::make_pair(kv.second.vtableRoot, mkv.first));
}

// A class is destructible if it declares a dtor, has a destructible field, OR
// its base is destructible (transitive). Fixed-point — cycle-safe.
void CEmitter::computeDestructible()
{
    // Seed: an explicit `~dtor` OR a collection/smart-ptr (always owns heap → RAII-dropped; its
    // ClassInfo carries destructible=true, which this reset must preserve — collections are
    // registered before this pass now).
    // A `Fixed<T,N>` is a value (owns no heap), so — unlike the heap collections — it is NOT
    // destructible; its element is a `value`, so there is nothing to drop.
    for (auto& kv : _classes)
        kv.second.destructible = kv.second.hasDtor ||
            (kv.second.isIntrinsicColl && kv.second.collKind != CollKind::Fixed);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& kv : _classes) {
            ClassInfo& ci = kv.second;
            if (ci.destructible || ci.isExternStruct) continue;   // kama doesn't own external structs
            // a specialized instance's fields are typed in `T` — resolve them under its binding
            // (and the template's scope) so `Box<Resource>` correctly sees the owned resource.
            bool inst = ci.isGenericInst && _genericTypeInsts.count(ci.name);
            if (inst) {
                const GenericTypeInst& gi = _genericTypeInsts[ci.name];
                _nsCtx = _genericTypeInstCtx.count(ci.name) ? _genericTypeInstCtx[ci.name]   // use-site ctx
                                                            : _genericTypeCtx[gi.templateKey];
                _typeSubst.clear();
                const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
                for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
            } else {
                _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;   // resolve field types in ci's scope
            }
            bool d = (ci.base && ci.base->destructible);
            if (!d)
                for (auto& f : ci.fields) {
                    auto it = _classes.find(cType(f.type));
                    if (it != _classes.end() && it->second.destructible) { d = true; break; }
                }
            // a tagged union is destructible if any variant's payload owns a resource.
            if (!d)
                for (auto& v : ci.variants) {
                    for (auto& f : v.payload) {
                        auto it = _classes.find(cType(f.type));
                        if (it != _classes.end() && it->second.destructible) { d = true; break; }
                    }
                    if (d) break;
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
        info.elemCopyable     = known && it->second.copyable;   // deep-copy each element
    }
    // A `value` owns nothing. `destructible` (computed above, transitively over base + owned fields +
    // collections + smart-ptrs) is exactly "owns something to drop", so a destructible `value` is a
    // design/field disagreement: declare it a `resource`. (A raw `Ptr`/borrowed contract confers no
    // ownership → not destructible → correctly still a value.) Only user `Value` types are checked —
    // compiler-built `Intrinsic` types (collections/smart-ptrs/variants) own by their own machinery.
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.kind == TypeKind::Value && ci.destructible && !ci.isExternStruct)
            unsupported(("a `value` owns nothing, but `" + ci.name + "` transitively owns a resource "
                         "— declare it `type resource`").c_str(), ci.node ? ci.node->line : 0);
    }
}

// Serialization mode gate: does T transitively REACH a Shared/Weak/Owned pointer? The tighter sibling of
// computeDestructible — same cycle-safe fixpoint (base + fields + variant payloads, generic instances
// resolved under their binding), with two differences: it STOPS at a pointer (a smart-ptr field makes the
// owner graph-mode; it does NOT recurse through the pointee), and a plain heap collection (List/Array/
// string) or a Fixed<T,N> contributes nothing unless its ELEMENT reaches a pointer (whereas every heap
// collection is destructible). false => by-value/tree serialization; true => object-graph (Shared<T>).
// Consumed by the serialization lowering (Phase C+); inert until then.
void CEmitter::computeReachesPointer()
{
    // Seed: only the smart pointers ARE pointers. Everything else (scalars, strings, enums, other
    // collections, user types) starts false and earns `true` only by transitively holding one.
    for (auto& kv : _classes) {
        bool intrinsicPtr = kv.second.isIntrinsicColl &&
            (kv.second.collKind == CollKind::Owned || kv.second.collKind == CollKind::Shared ||
             kv.second.collKind == CollKind::Weak);
        // A concrete-element triad instance (`Shared<Leaf>`) is a library generic instance, not intrinsic —
        // recognize it by its template key.
        bool triadPtr = false;
        auto g = _genericTypeInstOf.find(kv.first);
        if (g != _genericTypeInstOf.end())
            triadPtr = (g->second == _sharedTmpl || g->second == _ownedTmpl || g->second == _weakTmpl);
        kv.second.reachesPointer = intrinsicPtr || triadPtr;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& kv : _classes) {
            ClassInfo& ci = kv.second;
            if (ci.reachesPointer || ci.isExternStruct) continue;
            // a specialized instance's fields are typed in `T` — resolve them under its binding
            // (and the template's scope), mirroring computeDestructible.
            bool inst = ci.isGenericInst && _genericTypeInsts.count(ci.name);
            if (inst) {
                const GenericTypeInst& gi = _genericTypeInsts[ci.name];
                _nsCtx = _genericTypeInstCtx.count(ci.name) ? _genericTypeInstCtx[ci.name]   // use-site ctx
                                                            : _genericTypeCtx[gi.templateKey];
                _typeSubst.clear();
                const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
                for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
            } else {
                _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
            }
            bool r = (ci.base && ci.base->reachesPointer);
            if (!r)
                for (auto& f : ci.fields) {
                    auto it = _classes.find(cType(f.type));
                    if (it != _classes.end() && it->second.reachesPointer) { r = true; break; }
                }
            if (!r)
                for (auto& v : ci.variants) {
                    for (auto& f : v.payload) {
                        auto it = _classes.find(cType(f.type));
                        if (it != _classes.end() && it->second.reachesPointer) { r = true; break; }
                    }
                    if (r) break;
                }
            // An intrinsic collection (List/Array/string/Fixed) reaches a pointer iff its ELEMENT does —
            // the element type isn't a walkable field, so consult the collection's elemClass directly.
            if (!r && ci.isIntrinsicColl) {
                auto cit = _collections.find(ci.name);
                if (cit != _collections.end()) {
                    auto e = _classes.find(cit->second.elemClass);
                    if (e != _classes.end() && e->second.reachesPointer) r = true;
                }
            }
            if (inst) _typeSubst.clear();
            if (r) { ci.reachesPointer = true; changed = true; }
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

// a concrete type satisfies a contract BOUND when it has every one of the contract's methods,
// public (structural — this is exactly what makes the monomorphized call resolve to a static
// `Concrete__m(&x)`; nominal `implements` is not required, matching the codegen reality).
// The ClassInfo carrying a retroactive impl's injected methods for target C-type `tkey`: a collection's
// `_classes` entry (`kama_string`), or a primitive's `_primConformances` entry (scalar receiver). nullptr
// for a user-type target (which emits through the normal class machinery, not the retro emission path).
ClassInfo* CEmitter::retroTargetInfo(const std::string& tkey)
{
    auto ti = _classes.find(tkey);
    // An intrinsic collection (`string`→`kama_string`) OR a tagged-union enum: both emit their retro-impl
    // method bodies through the dedicated retro path (emitModuleContent / the prelude pass), NOT the normal
    // per-class machinery — a `ClassDeclarationNode` class emits via emitClassDefinitions and returns nullptr
    // here, but an `EnumDeclarationNode` never reaches that path, so an `implements Serialize for MyEnum`
    // body would otherwise be declared-but-undefined.
    if (ti != _classes.end() && (ti->second.isIntrinsicColl || ti->second.isVariant)) return &ti->second;
    auto pi = _primConformances.find(tkey);
    if (pi != _primConformances.end()) return &pi->second;
    return nullptr;
}

// A `when [P1: B1, …]` gate holds iff EVERY condition holds: the concrete arg bound to each gated param
// satisfies its required contract. A condition naming a param that isn't a type-param of this template
// fails conservatively (as the old single-param gate did — it left the capability off).
bool CEmitter::whenConditionsHold(const std::vector<std::string>& whenParams,
                                  const std::vector<std::string>& whenBounds,
                                  const std::vector<std::string>& params,
                                  const std::vector<SharedIdentifier>& concrete)
{
    for (size_t c = 0; c < whenParams.size() && c < whenBounds.size(); ++c) {
        bool held = false;
        for (size_t i = 0; i < params.size() && i < concrete.size(); ++i)
            if (params[i] == whenParams[c]) { held = satisfiesBound(cType(concrete[i]), whenBounds[c]); break; }
        if (!held) return false;
    }
    return true;
}

bool CEmitter::classSatisfiesBound(ClassInfo* ci, const std::string& contract)
{
    if (!ci) return false;
    // NOMINAL: the type must DECLARE `implements <contract>` — the keyword is load-bearing for bounds
    // exactly as it is for `foreach`/`when`; a coincidental set of matching public method names is not
    // enough (explicit over implicit). `implementsContractTemplate` matches a plain contract by name AND a
    // generic-contract bound against its TEMPLATE (`Iterator<T>` — no instance needed to constrain a param).
    // `Copyable` keeps its one structural rule: a `value` is bitwise-copyable; a `resource` only by declaring
    // it. (A primitive / retroactive `implements` target is handled by the caller before we're reached.)
    if (contract == "Copyable") {
        if (ci->kind == TypeKind::Value) return true;
        return ci->copyable;
    }
    return implementsContractTemplate(ci, contract);
}

bool CEmitter::implementsContractTemplate(ClassInfo* ci, const std::string& tmpl)
{
    if (!ci) return false;
    for (auto& itf : ci->interfaces) {
        if (itf == tmpl) return true;                              // a plain (non-generic) contract
        auto it = _interfaces.find(itf);                          // a specialized generic-contract instance
        if (it != _interfaces.end() && it->second.templateKey == tmpl) return true;
    }
    return false;
}

// If `cls` implements the prelude `Deref<T>` contract, return the pointee class `T` (the auto-deref
// target); "" otherwise. Nominal: keyed on `implements Deref<…>` (the contract is the opt-in gate), not
// on any compiler-blessed smart-pointer list. The pointee is the concrete `ref T` return of the class's
// own `deref()` (resolved under the instance's type-subst for a generic wrapper). Inert (always "") when
// no `Deref` is in scope, so the whole feature is zero-cost when unused.
std::string CEmitter::derefTarget(const std::string& cls)
{
    if (_derefContract.empty()) return "";
    auto it = _classes.find(cls);
    if (it == _classes.end()) return "";
    for (auto& ifn : it->second.interfaces) {
        auto ii = _interfaces.find(ifn);
        if (ii == _interfaces.end() || !ii->second.isGenericInst || ii->second.templateKey != _derefContract)
            continue;
        ClassInfo* oc = nullptr;
        MethodInfo* mi = findMethod(&it->second, "deref", &oc);
        if (!mi || !mi->returnType) return "";
        std::string t = cTypeInInstance(cls, mi->returnType);   // resolves `ref T` under a generic wrapper's subst
        return isClass(t) ? t : "";
    }
    return "";
}

// If `e` is `this.field[i]` (or `obj.field[i]`) where `field` is a raw `Ptr<T>`, return the element's
// concrete C-type (resolving `T` under the current instance subst); else "". This is a raw pointer
// slot (unsafe manual memory) — a container's own buffer — distinct from a collection / user operator[].
std::string CEmitter::ptrElemType(SharedExpression e)
{
    auto* ea = dynamic_cast<ElementAccessNode*>(e.get());
    if (!ea) return "";
    SharedExpression recv = ea->expression ? ea->expression
                                           : std::static_pointer_cast<ExpressionNode>(ea->identifier);
    auto* ma = dynamic_cast<MemberAccessNode*>(recv.get());
    if (!ma || !ma->identifier || !ma->identifier->value || !_currentClass) return "";
    ClassInfo* owner = findFieldOwner(_currentClass, *ma->identifier->value);
    if (!owner) return "";
    for (auto& f : owner->fields)
        if (f.name == *ma->identifier->value && f.type && f.type->value
            && *f.type->value == "Ptr" && f.type->genericArg)
            return cType(f.type->genericArg);
    return "";
}

// Is `base` reachable by walking `derived`'s single-inheritance chain (inclusive)? Used to admit a
// `Derived -> ref Base` upcast while rejecting an unrelated `ref` (e.g. borrowing through a `Weak`).
bool CEmitter::isBaseOf(const std::string& base, const std::string& derived) const
{
    auto it = _classes.find(derived);
    for (const ClassInfo* c = (it != _classes.end() ? &it->second : nullptr); c; c = c->base)
        if (c->name == base) return true;
    return false;
}

// If `cls` implements the prelude `HeapOwner<T>` contract, return the owned element `T` (so `new T(args)`
// placement-constructs into `cls` via `cls::adopt(Ptr<T>)`); "" otherwise. The element is the contract
// instance's type arg. Inert (always "") when no `HeapOwner` is in scope.
std::string CEmitter::heapOwnerTarget(const std::string& cls)
{
    if (_heapOwnerContract.empty()) return "";
    auto it = _classes.find(cls);
    if (it == _classes.end()) return "";
    for (auto& ifn : it->second.interfaces) {
        auto ii = _interfaces.find(ifn);
        if (ii == _interfaces.end() || !ii->second.isGenericInst || ii->second.templateKey != _heapOwnerContract)
            continue;
        if (ii->second.typeArgs.empty() || !ii->second.typeArgs[0]) return "";
        // the element is the contract instance's type arg, resolved in its use-site ctx (a user `Point`).
        NsCtx saved = _nsCtx;
        if (_genericContractInstCtx.count(ifn)) _nsCtx = _genericContractInstCtx[ifn];
        std::string t = cType(ii->second.typeArgs[0]);
        _nsCtx = saved;
        return t;
    }
    return "";
}

// at each monomorphization, verify the concrete type argument bound to `paramName` satisfies
// every contract on it (`+` = AND); a clean diagnostic instead of a downstream "class missing method".
void CEmitter::checkBounds(const std::string& paramName, SharedIdentifier concreteArg,
                           SharedIdentifierList bounds, int line)
{
    if (!bounds || bounds->empty()) return;
    std::string cls = cType(concreteArg);                 // concrete class key (or a primitive C type)
    std::string clsName = (concreteArg && concreteArg->value) ? *concreteArg->value : cls;   // source-level
    ClassInfo* ci = _classes.count(cls) ? &_classes[cls] : nullptr;
    for (auto& b : *bounds) {
        if (!b || !b->value) continue;
        std::string contract = resolveUserName(*b->value, b->qualifier);
        if (!contractMethods(contract)) {   // a plain contract OR a generic-contract template (`Iterator<T>`)
            unsupported(("unknown contract `" + *b->value + "` in a bound on type parameter `"
                         + paramName + "`").c_str(), line);
            continue;
        }
        // A retroactive `implements <bound> for <this type>` also satisfies it. Consult the pre-scan so a
        // bound check that runs during collection (before applyRetroactive injects the methods) still sees
        // it — matched on the raw source name, the same key both the pre-scan and applyRetroactive use.
        bool retro = _retroConformances.count(cls) && _retroConformances[cls].count(*b->value);
        if (!retro && (!ci || !classSatisfiesBound(ci, contract)))
            unsupported(("type argument `" + clsName + "` for type parameter `" + paramName
                         + "` does not satisfy bound `" + *b->value + "`").c_str(), line);
    }
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
// The C type of a primitive LITERAL rvalue ("" if not a literal). Used to materialize a bare literal
// into a temp when it's passed to a `ref` parameter (which needs an addressable lvalue).
static std::string litRvalueCType(ASTNode* n)
{
    if (dynamic_cast<Int8Node*>(n))    return "int8_t";
    if (dynamic_cast<Int16Node*>(n))   return "int16_t";
    if (dynamic_cast<Int32Node*>(n))   return "int32_t";
    if (dynamic_cast<Int64Node*>(n))   return "int64_t";
    if (dynamic_cast<UInt8Node*>(n))   return "uint8_t";
    if (dynamic_cast<UInt16Node*>(n))  return "uint16_t";
    if (dynamic_cast<UInt32Node*>(n))  return "uint32_t";
    if (dynamic_cast<UInt64Node*>(n))  return "uint64_t";
    if (dynamic_cast<CharNode*>(n))    return "uint32_t";   // `char` = a uint32-backed codepoint
    if (dynamic_cast<Float32Node*>(n)) return "float";
    if (dynamic_cast<Float64Node*>(n)) return "double";
    if (dynamic_cast<BooleanNode*>(n)) return "bool";
    return "";
}

std::string CEmitter::emitReorderedCall(const std::string& cName, const std::string& leadArg,
                                        const std::vector<ParamSig>& params,
                                        SharedArgumentList args, int srcLine)
{
    std::map<std::string, ArgumentNode*> byName;
    size_t named = 0;
    if (args)
        for (auto& a : *args)
            if (a->name && a->name->value) { byName[*a->name->value] = a.get(); named++; }

    // Step 5: named arguments ARE kama's calling convention — validate them so a typo or
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
        // unwrap a give/copy hand-off marker. The inner value is what we emit;
        // the marker (give=move / copy=retain) only matters for a smart pointer passed
        // BY VALUE (ownership transfer) — it's meaningless on a borrow.
        SharedExpression argExpr = f->second->expression;
        int handoff = 0;   // 0 none, 1 give, 2 copy
        if (auto* h = dynamic_cast<HandoffNode*>(argExpr.get())) { handoff = h->isGive ? 1 : 2; argExpr = h->value; }
        // inline constructor in argument position — `f(x: Counter(start: 5))`. A ctor lowers to
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
            // `f(b: Box(v: 7))` — the ctor names template `Box`; the target param is instance `Box_int32`.
            else { auto g = _genericTypeInstOf.find(p.className);
                   if (g != _genericTypeInstOf.end() && g->second == rn) ctorCls = p.className; }
        }
        // A value-producing RHS in argument position that needs its type from context: an inline variant
        // construction (`f(o: Optional::Some(…))` / `…::None`) or a value-producing `match` (`f(x: match(…))`).
        // Thread the PARAM's C type as the target so the union instance / match result resolves, exactly as
        // the initializer / assignment / return / arm positions do. (An ordinary arg leaves these unset.)
        bool argIsVariant = false;
        {
            SharedStringList vq;
            if (ctorIv) { if (ctorIv->identifier) vq = ctorIv->identifier->qualifier; }
            else if (auto* id = dynamic_cast<IdentifierNode*>(argExpr.get())) vq = id->qualifier;
            if (vq && !vq->empty()) {
                auto q = std::make_shared<StringList>();
                for (size_t i = 0; i + 1 < vq->size(); ++i) q->push_back((*vq)[i]);
                std::string en = resolveUserName(*vq->back(), q);
                argIsVariant = (_classes.count(en) && _classes[en].isVariant) || _genericTypeParams.count(en);
            }
        }
        bool argIsMatch = dynamic_cast<MatchNode*>(argExpr.get()) != nullptr;
        // An inline ctor is a temporary rvalue. To an INTERFACE `ref`/`out` param it can't be a fat pointer
        // inline — reject (bind first). To a concrete-class param (by value OR `ref`) it's materialized into
        // a hoisted temp below, so `f(Point(1, 2))` / `m.get(key: Point(1, 2))` work.
        if (!ctorCls.empty() && !_classes[ctorCls].isIntrinsicColl && p.byRef && isInterface(p.className))
            unsupported("cannot pass an inline constructor to a contract `ref`/`out` parameter — "
                        "bind it to a local first, then pass that", srcLine);
        // A marker on a FRESH inline ctor (`f(x: give Box(…))`) is meaningless — a fresh rvalue is consumed
        // in place. Reject cleanly, matching the init/assignment positions (without this it skips the
        // materialization below and falls to a confusing "unknown function" reject).
        if (handoff != 0 && !ctorCls.empty())
            unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call result needs "
                        "no marker", srcLine);
        // Does the inline ctor's concrete class implement the (by-value) contract param? (A3 — pass it as a
        // fat-pointer borrow of a materialized temp.)
        bool ctorImplementsParam = false;
        if (!ctorCls.empty() && isInterface(p.className))
            for (auto& i : _classes[ctorCls].interfaces) if (i == p.className) { ctorImplementsParam = true; break; }
        if (_hoistOK && handoff == 0 && !ctorCls.empty() && ctorCls == p.className
            && !isInterface(p.className) && !_classes[ctorCls].isIntrinsicColl) {
            std::string t = "__ctorarg" + std::to_string(_tempCounter++);
            std::string ctor = emitCtorCall(t, _classes[ctorCls], ctorIv->args, srcLine);
            _hoisted.push_back(ctorCls + " " + t + "; " + ctor + ";");
            // A `ref` borrow keeps the temp alive through the call; if it owns anything, drop it at scope end.
            if (p.byRef && _classes[ctorCls].destructible) recordDestructibleLocal(t, ctorCls);
            val = t;
        } else if (_hoistOK && handoff == 0 && ctorImplementsParam && !p.byRef
                   && !_classes[ctorCls].isIntrinsicColl) {
            // A3 — an inline STACK ctor of a concrete that implements a BY-VALUE contract parameter
            // (`useShape(a: Square(3))` where `useShape(Shape a)`). Materialize it into a temp; the by-value
            // contract param is a BORROW (fat pointer {obj, vtbl}), so the CALLER owns the temp and drops it
            // at scope end — a single drop, sound for value and resource concretes alike. The `isInterface`
            // path below wraps `val` as the fat pointer. (`new` into a contract borrow stays a rule — it
            // would leak; `ref`/`out` needs a real interface lvalue to reseat — both handled elsewhere.)
            std::string t = "__ctorarg" + std::to_string(_tempCounter++);
            std::string ctor = emitCtorCall(t, _classes[ctorCls], ctorIv->args, srcLine);
            _hoisted.push_back(ctorCls + " " + t + "; " + ctor + ";");
            if (_classes[ctorCls].destructible) recordDestructibleLocal(t, ctorCls);
            val = t;
        } else if (dynamic_cast<ObjectCreationNode*>(argExpr.get())) {
            // an inline `new T(...)` argument — box it into a hoisted temp (malloc + ctor + adopt/vtbl),
            // passed BY VALUE so the callee owns and drops it (a fresh unaliased box, consumed once — not
            // registered destructible here). `give`/`copy` are meaningless on a fresh rvalue; a `ref`/`out`
            // or contract borrow has no stable address to reseat — bind to a local first (the language rule).
            if (handoff != 0)
                unsupported("`give`/`copy` apply to a named value — a fresh `new`/constructor/call result "
                            "needs no marker", srcLine);
            if (p.byRef || isInterface(p.className)) {
                unsupported("cannot pass an inline `new` to a `ref`/`out` or contract-borrow parameter — "
                            "bind it to a local first, then pass that", srcLine);
                val = "0";   // rejected — throwaway (compilation already failed); don't re-diagnose via the gate
            } else {
                std::string t = tryHoistInlineNew(argExpr, p.className, srcLine);
                val = t.empty() ? emitExpression(argExpr) : t;   // "" => not an owning target; emitExpression diagnoses
            }
        } else if (p.byRef && dynamic_cast<ElementAccessNode*>(argExpr.get())) {
            // `ref a[i]` borrows the ELEMENT: emit it as a place (`*NAME__at(...)`) so the `&(...)`
            // below is the bounds-checked `T*` (index evaluated once), not the address of a by-value
            // `__get` rvalue. Folds to `NAME__at(&a, i)`.
            val = emitPlace(argExpr);
        } else {
            // An owned-string rvalue argument to a string INTRINSIC (`kama_string__*`) is borrowed by the
            // callee (read-only — none of concat/equals/contains/... store it), so hoist it into a
            // scope-dtor'd temp and free it instead of leaking. Scoped to the borrow-only string
            // intrinsics: a general call may CONSUME a by-value string arg (e.g. `List.add` relocates it),
            // where freeing the caller's temp would double-free — there, bind to a local first.
            std::string st = (cName.rfind("kama_string__", 0) == 0) ? hoistStringTemp(argExpr) : std::string();
            // A `ref string` borrow of a string RVALUE has no address to take: `&(rvalue)` is illegal C
            // ("cannot take the address of an rvalue"). Materialize it into a scope-dtor'd temp so the
            // `&temp` below is legal — makes `f("literal")` / `f(a + b)` work for a `ref string` param.
            // The borrow is read-only and the temp frees at scope end; an addressable lvalue (a named
            // var / field / `this`) keeps its direct `&`. hoistStringTemp handles concat/call rvalues but
            // keeps a bare LITERAL on its normal path, so materialize the literal here too.
            if (st.empty() && p.byRef && _hoistOK && exprIsString(argExpr)) {
                st = hoistStringTemp(argExpr);
                if (st.empty() && dynamic_cast<StringNode*>(argExpr.get())) {
                    st = "__strtmp" + std::to_string(_tempCounter++);
                    _hoisted.push_back("kama_string " + st + " = " + emitExpression(argExpr) + ";");
                    recordDestructibleLocal(st, "kama_string");
                }
            }
            // A bare primitive LITERAL to a `ref`-primitive param (`m.get(key: 5)`) has no address —
            // materialize it into a temp of the literal's C type so the `&temp` below is legal. No drop
            // (a primitive owns nothing).
            if (st.empty() && p.byRef && _hoistOK) {
                std::string lct = litRvalueCType(argExpr.get());
                if (!lct.empty()) {
                    st = "__primtmp" + std::to_string(_tempCounter++);
                    _hoisted.push_back(lct + " " + st + " = " + emitExpression(argExpr) + ";");
                }
            }
            if (st.empty() && (argIsVariant || argIsMatch) && !p.className.empty()) {
                // target-type the union instance / match result to the param's type, then emit
                std::string pmt = _matchTargetCType, pvt = _variantTargetType;
                _matchTargetCType = _variantTargetType = p.className;
                val = emitExpression(argExpr);
                _matchTargetCType = pmt; _variantTargetType = pvt;
            } else {
                val = st.empty() ? emitExpression(argExpr) : st;
            }
        }
        if (handoff && (p.byRef || isInterface(p.className)))
            unsupported("`give`/`copy` transfer ownership by value — they don't apply to a `ref`/`out` "
                        "or contract borrow", srcLine);
        if (isInterface(p.className)) {
            std::string c = exprClass(argExpr);
            if (p.byRef) {
                // `ref`/`out` interface: the callee may reseat the caller's handle, so the
                // argument must be an actual interface variable (pass its address). A
                // concrete class would need a throwaway temp — reject it; bind first.
                if (!c.empty() && isClass(c))
                    unsupported(("cannot pass '" + c + "' by `ref`/`out` to contract parameter '" + p.name
                                 + "'; bind it to an `" + p.className + "` first "
                                 "(`" + p.className + " s = …; … ref s`)").c_str(), srcLine);
                if (!p.isConst) checkConstWrite(argExpr, srcLine);
                s += "&(" + val + ")";
            } else {
                // by value: wrap a concrete object as an interface fat pointer (the borrow);
                // pass an existing interface value straight through.
                std::string dt = derefTarget(c);   // Owned/Shared<T> pointee via Deref ("" if not, incl. Weak)
                bool dtImpl = false;
                if (!dt.empty()) {
                    auto eit = _classes.find(dt);
                    if (eit != _classes.end())
                        for (auto& i : eit->second.interfaces) if (i == p.className) { dtImpl = true; break; }
                }
                if (dtImpl) {
                    // `encode(v: sharedRoot)`: a `Shared`/`Owned<T>` whose pointee `T` implements the contract
                    // — deref to the pointee (`T*`) and wrap THAT as the fat pointer, not the handle struct.
                    s += "(" + p.className + "){ (void*)" + c + "__deref(&(" + val + ")), &"
                       + dt + "__as_" + p.className + " }";
                } else if (!c.empty() && isClass(c)) {
                    // `this` already IS the object pointer (`self`), so wrap it without taking its
                    // address — `&self` would be a `C**` (same reason a by-ref `this` passes `self`).
                    if (dynamic_cast<ThisAccessNode*>(argExpr.get()))
                        s += "(" + p.className + "){ (void*)(" + val + "), &" + c + "__as_" + p.className + " }";
                    else
                        s += fatPointer(p.className, c, val);
                } else s += val;
            }
        } else if (p.byRef) {
            // Soundness: a non-const `ref`/`out` param can MUTATE its argument, so a
            // const binding (or a const field outside its ctor) may not be passed to one
            // — that would silently launder away const. (A `const ref` borrow is fine.)
            if (!p.isConst) checkConstWrite(argExpr, srcLine);
            // a borrow names the OBJECT (`ref T`). If the argument is a smart pointer
            // holding a T, auto-deref to its T* — `ref p` borrows the heap object, uniformly
            // with `ref stackValue`. (Weak can't be borrowed — it may be dead; tryUpgrade.)
            std::string argCls = exprClass(argExpr);
            std::string dt = derefTarget(argCls);   // library Deref<T> pointee ("" if not a Deref type)
            if (isSmartPtrClass(argCls) && _classes[argCls].collElemClass == p.className) {
                if (smartKind(argCls) == CollKind::Weak)
                    unsupported(("cannot borrow through a `Weak<" + p.className
                                 + ">` (it may be dead) — `tryUpgrade` to a `Shared` first").c_str(), srcLine);
                s += "(" + val + ").ptr";
            } else if (!dt.empty() && dt == p.className) {
                // a library heap-owner (Owned/Shared) borrows its pointee via the Deref contract's
                // `deref()` (-> T*), uniformly with `ref stackValue` — no `.ptr` field is assumed.
                s += argCls + "__deref(&(" + val + "))";
            } else if (dynamic_cast<ThisAccessNode*>(argExpr.get())) {
                // `this` lowers to `self`, which is ALREADY a `T*` (the receiver pointer). Pass it straight
                // to a `ref T` param — `&(self)` would hand over the address of the param slot (a `T**`), so
                // a whole-`this` borrow (`w.writeString(v: this)` in a retro-impl on `string`) would read the
                // pointer's own bytes, not the value. (Cast for a `ref Base` upcast.)
                s += (isClass(p.className) && argCls != p.className) ? ("(" + p.className + "*)" + val) : val;
            } else {
                // A `ref T` arg must BE a T, a subclass of T (upcast to `ref Base`), or a
                // smart-ptr / Deref<T> owner of T (both handled above). An unrelated class — most
                // notably borrowing through a `Weak<T>` (no Deref) — is a type error, caught here
                // instead of emitting a bad reinterpreting cast.
                if (isClass(p.className) && isClass(argCls) && argCls != p.className
                    && !isBaseOf(p.className, argCls))
                    unsupported(("cannot borrow a `" + argCls + "` as `ref " + p.className
                                 + "` — it is not that object (a `Weak` must `tryUpgrade` to a `Shared` first)").c_str(), srcLine);
                s += isClass(p.className) ? ("(" + p.className + "*)&(" + val + ")")  // upcast for ref Base
                                          : ("&(" + val + ")");
            }
        } else {
            // By value. A *named* smart pointer TRANSFERS into the param, which the
            // callee owns and drops at fn-end. The retain (copy) / invalidate (give) is a
            // statement, materialized as a HOISTED temp (pure ISO C).
            std::string argCls = exprClass(argExpr);
            if (isSmartPtrClass(argCls) && isNamedValue(argExpr.get())) {
                CollKind k = smartKind(argCls);
                // Default the natural op: Owned -> give (move; copy illegal), Shared/Weak -> copy
                // (retain). A marker overrides (e.g. `give Shared` moves the handle).
                bool doGive = (handoff == 1) || (handoff == 0 && k == CollKind::Owned);
                if (handoff == 2 && k == CollKind::Owned)
                    unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", srcLine);
                std::string t = "__kama_arg" + std::to_string(_tempCounter++);
                std::string side = doGive ? smartPtrInvalidate("(" + val + ")", k, isInterface(_classes[argCls].collElemClass))
                                          : ("(" + val + ").ctrl->" + (k == CollKind::Weak ? "weak" : "strong") + "++;");
                // Hoist `T t = (x); <retain/invalidate>` as an ordinary statement (ISO C)
                // everywhere a temp can precede its statement (call/init/return/ctor sites). The
                // `({ … })` form is used ONLY for a by-value smart-ptr hand-off inside a loop/branch
                // CONDITION (no preceding-statement slot) — where it is also semantically REQUIRED:
                // hoisting a per-iteration retain out of a `while (...)` would run it once, not each
                // time. No fixture reaches it (the ISO `-pedantic-errors` gate confirms zero GNU
                // extensions across the whole suite).
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
            } else if (ownsByValue(p.className) && _classes.count(p.className)
                       && _classes[p.className].isIntrinsicColl && isNamedValue(argExpr.get())) {
                // A collection/`string` passed BY VALUE to an OWNING param transfers ownership: the callee
                // owns it and drops it at fn-end (param-drop registration). `give` MOVES it in (relocate
                // struct + null source + markMoved); `copy` deep-copies (`__copy`); a BARE named arg is an
                // error. Gated on the PARAM's declared type — the `kama_string__*` read-only intrinsics
                // register their string args with `className == ""`, so this is skipped for them and they
                // keep the borrow-hoist above (Phase-3 ergonomics unchanged). A fresh rvalue / literal arg
                // (not isNamedValue) moves in bare via the `else` below.
                if (handoff == 0)
                    unsupported("passing a collection/`string` by value transfers ownership — say `give` "
                                "(move) or `copy` (deep), or pass by `ref` to borrow", srcLine);
                else if (handoff == 2) {                              // copy = deep
                    auto ci = _collections.find(p.className);
                    if (ci != _collections.end() && ci->second.elemDestructible && !ci->second.elemCopyable)
                        unsupported(("`copy` of a `" + p.className + "` needs copyable elements — its elements "
                                     "own resources but aren't `Copyable`; use `give` to move").c_str(), srcLine);
                    s += p.className + "__copy(&(" + val + "))";
                } else {                                             // give = move: relocate + null source
                    std::string t = "__kama_carg" + std::to_string(_tempCounter++);
                    std::string blit = p.className + " " + t + " = (" + val + "); ("
                                     + val + ").data = NULL; (" + val + ").len = 0;";
                    if (_hoistOK) { _hoisted.push_back(blit); s += t; }
                    else          { s += "({ " + blit + " " + t + "; })"; }
                    std::string mv = moveOnlySource(argExpr, srcLine); if (!mv.empty()) markMoved(mv);
                }
            } else if (isMoveOnlyValue(argCls) && isNamedValue(argExpr.get())) {
                // A `resource` VALUE passed by value → the callee owns it and drops it at fn-end. `give`
                // MOVES it in (mark the source moved); `copy` passes a fresh copy (source survives); a
                // BARE arg follows the type's `bare:` default. A non-Copyable resource → move.
                bool cpy = isCopyable(argCls);
                bool doCopy;
                if (handoff == 2) {
                    if (!cpy) unsupported(("`" + argCls + "` has no `copy` method — add `implements "
                                           "Copyable(bare: …)`, or use `give` to move it").c_str(), srcLine);
                    doCopy = cpy;
                } else if (handoff == 1) doCopy = false;
                else doCopy = cpy && _classes[argCls].bareDefault == COPY;
                if (doCopy) s += argCls + "__copy(&(" + val + "))";
                else { std::string mv = moveOnlySource(argExpr, srcLine); if (!mv.empty()) markMoved(mv); s += val; }
            } else if (dynamic_cast<ThisAccessNode*>(argExpr.get()) && !p.className.empty()
                       && _classes.count(p.className) && _classes[p.className].kind == TypeKind::Value) {
                // A bare `this` is `self` (a `T*` — the receiver pointer). Passed to a by-value value-type
                // parameter it must be dereferenced: `*self` is the value. (Field args are MemberAccessNodes,
                // never ThisAccessNode, and a primitive param's className is empty — so this only fires for a
                // whole-`this` value arg, e.g. `this.dot(r: this)` in `length()`.)
                s += "*(" + val + ")";
            } else {
                s += val;   // plain value copy (primitive / value / collection borrow); `give` on a value is just that copy
            }
        }
    }
    return s + ")";
}

// does a free function match a FunctionPtr signature? Positional, by C type.
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

// the root identifier a write ultimately targets, for deep-const checks.
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

// is a write/call root `const`? A const local/param, `this` inside a const
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

// is `target` a write to a `const` data member? (`this.f`, bare `f`, or `obj.f`
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

// writing TO or THROUGH a `const` binding is a hard error (deep const, so
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

// a non-const method may not be invoked on a const receiver (it could mutate).
bool CEmitter::isConstReceiver(SharedExpression receiver) const
{
    return receiver && rootIsConst(rootBinding(receiver));
}

// --- Never-null definite assignment for `Owned`/`Shared` fields (Stage 1) ------------------------------
// `Owned<T>`/`Shared<T>` are never-null: every such field must be assigned before the constructor returns,
// and must not be read before it is assigned. `Weak<T>` (nullable, `tryUpgrade`) is exempt. v1 tracks
// unconditional (top-level) assignment — the common straight-line ctor; a field assigned only inside a
// branch is conservatively treated as unassigned (path-sensitive analysis is a follow-up).

// If `e` is `this.<field>` or a bare `<field>` (not shadowed by a local) naming a data member of `owner`,
// return the field name; else "".
std::string CEmitter::ctorFieldRef(SharedExpression e, ClassInfo& owner, const std::set<std::string>& locals)
{
    if (!e) return "";
    ASTNode* n = e.get();
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        if (ma->expression && dynamic_cast<ThisAccessNode*>(ma->expression.get())
            && ma->identifier && ma->identifier->value && owner.fieldNames.count(*ma->identifier->value))
            return *ma->identifier->value;
    } else if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (id->value && (!id->qualifier || id->qualifier->empty())
            && !locals.count(*id->value) && owner.fieldNames.count(*id->value))
            return *id->value;
    }
    return "";
}

// Recursively record (first hit wins, in `bad`/`badLine`) a READ of an owning field not yet in `assigned`.
void CEmitter::scanOwningReads(SharedExpression e, ClassInfo& owner, const std::set<std::string>& owning,
                               const std::set<std::string>& assigned, const std::set<std::string>& locals,
                               std::string& bad, int& badLine)
{
    if (!e || !bad.empty()) return;
    ASTNode* n = e.get();
    std::string f = ctorFieldRef(e, owner, locals);
    if (!f.empty()) { if (owning.count(f) && !assigned.count(f)) { bad = f; badLine = e->line; } return; }
    auto rec = [&](SharedExpression x){ scanOwningReads(x, owner, owning, assigned, locals, bad, badLine); };
    if (auto* oc = dynamic_cast<ObjectCreationNode*>(n)) {
        if (oc->args) for (auto& a : *oc->args) if (a) rec(a->expression);
    } else if (auto* c = dynamic_cast<CastNode*>(n)) { rec(c->unaryExpression);
    } else if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) { rec(b->LHS); rec(b->RHS);
    } else if (auto* l = dynamic_cast<LogicalAndOrNode*>(n)) { rec(l->LHS); rec(l->RHS);
    } else if (auto* tn = dynamic_cast<TernaryExpressionNode*>(n)) { rec(tn->condition); rec(tn->LHS); rec(tn->RHS);
    } else if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        // the LHS of a direct `this.f = …` is a WRITE, not a read — skip it; else scan (write-through reads f)
        if (ctorFieldRef(as->unaryExpression, owner, locals).empty()) rec(as->unaryExpression);
        rec(as->expression);
    } else if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        rec(inv->expression);
        if (inv->args) for (auto& a : *inv->args) if (a) rec(a->expression);
    } else if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        rec(ea->expression);
        if (ea->expressionlist) for (auto& x : *ea->expressionlist) rec(x);
    } else if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) { rec(ma->expression);
    } else if (auto* pe = dynamic_cast<PreIncrDecrNode*>(n)) { rec(pe->expression);
    } else if (auto* po = dynamic_cast<PostIncrDecrNode*>(n)) { rec(po->expression);
    } else if (auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(n)) { rec(su->expression);
    } else if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) {
        if (al->elements) for (auto& x : *al->elements) rec(x);
        rec(al->fillValue);
    }
}

// Walk one ctor statement: check its reads against `assigned`, record a top-level `this.f = …`, track locals.
void CEmitter::analyzeCtorStmt(SharedStatement st, ClassInfo& owner, const std::set<std::string>& owning,
                               std::set<std::string>& assigned, std::set<std::string>& locals, bool topLevel)
{
    if (!st) return;
    ASTNode* n = st.get();
    auto checkE = [&](SharedExpression e){
        std::string bad; int line = 0;
        scanOwningReads(e, owner, owning, assigned, locals, bad, line);
        if (!bad.empty())
            unsupported(("'" + bad + "' is used before it is assigned in the constructor "
                         "(`Owned`/`Shared` are never-null)").c_str(), line ? line : st->line);
    };
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        std::string tgt = ctorFieldRef(as->unaryExpression, owner, locals);
        if (tgt.empty()) checkE(as->unaryExpression);   // a write-through LHS may itself read a field
        checkE(as->expression);
        if (!tgt.empty() && topLevel) assigned.insert(tgt);
    } else if (auto* lv = dynamic_cast<LocalVariableDeclaration*>(n)) {
        if (lv->variables) for (auto& v : *lv->variables) if (v) {
            checkE(v->initializer);
            if (v->name && v->name->value) locals.insert(*v->name->value);
        }
    } else if (auto* iff = dynamic_cast<IfNode*>(n)) {
        checkE(iff->booleanExpression);
        analyzeCtorStmt(iff->ifStatement, owner, owning, assigned, locals, false);
        analyzeCtorStmt(iff->elseStatement, owner, owning, assigned, locals, false);
    } else if (auto* wh = dynamic_cast<WhileNode*>(n)) {
        checkE(wh->booleanExpression);
        analyzeCtorStmt(wh->whileStatement, owner, owning, assigned, locals, false);
    } else if (auto* blk = dynamic_cast<BlockNode*>(n)) {
        if (blk->statements) for (auto& s : *blk->statements)
            analyzeCtorStmt(s, owner, owning, assigned, locals, topLevel);
    } else if (dynamic_cast<ExpressionStatementNode*>(n)) {
        checkE(std::dynamic_pointer_cast<ExpressionNode>(st));   // Invocation / incr / etc. — scan its reads
    }
    // (for/foreach/match/early-return: not modeled in v1 — the ctor-end assignment check still holds)
}

// Enforce never-null on a constructor: every `Owned`/`Shared` field assigned by ctor-end, none read before.
void CEmitter::checkCtorNeverNull(ClassInfo& owner, SharedBlock body)
{
    std::set<std::string> owning;
    for (auto& f : owner.fields)
        if (!heapOwnerTarget(cType(f.type)).empty()) owning.insert(f.name);   // Owned/Shared (not Weak)
    if (owning.empty()) return;
    std::set<std::string> assigned, locals;
    for (auto& f : owner.fields) if (f.initializer) assigned.insert(f.name);   // a field-initializer pre-assigns
    if (body && body->statements)
        for (auto& st : *body->statements)
            analyzeCtorStmt(st, owner, owning, assigned, locals, true);
    for (auto& f : owner.fields)
        if (owning.count(f.name) && !assigned.count(f.name))
            unsupported(("'" + f.name + "' must be set before the constructor returns "
                         "(`Owned`/`Shared` are never-null)").c_str(),
                        owner.ctorNode ? owner.ctorNode->line : (owner.node ? owner.node->line : 0));
}

// access control --------------------------------------------------------
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

// A field's visibility. A `value` picks it per field (default private, `public` allowed, `protected`
// rejected — protected belongs to an extensible resource). A `resource` field is always private
// (ownership encapsulated). An extern struct is public (the FFI struct owns its layout).
Visibility CEmitter::fieldVisibility(const ClassInfo& ci, SharedModifierList mods, int line)
{
    if (ci.isExternStruct) return Visibility::Public;
    if (ci.kind == TypeKind::Value) {
        if (modHas(mods, "protected"))
            unsupported("a `value` field can't be `protected` — protected belongs to an extensible `resource`", line);
        return visibilityOf(mods, Visibility::Private, line);
    }
    // A `resource` field is always private — ownership is encapsulated; expose behavior through methods.
    if (modHas(mods, "public") || modHas(mods, "protected") || modHas(mods, "private"))
        unsupported("a `resource` field is always private — expose behavior through methods", line);
    return Visibility::Private;
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
        // an owner-granted `friend` may touch the named members. The accessing
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

// resolve each class's raw `friend` grants to match keys, once every unit's
// functions/classes are registered. An accessor is a class (matched vs _currentClass),
// a free function, or a `Class::method` (both matched vs _currentFunc's C-name).
void CEmitter::resolveFriends()
{
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.friendGrantsRaw.empty()) continue;
        _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
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

// bind a value to a FunctionPtr<Sig> local — a free function name (resolve +
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
            // `Type::method` — an unbound method reference. The method lowers
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

// `new BindableFunctionPtr<Sig>(obj: x, method: T::m)` — bind an object + a
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

    // The object: an owning smart-pointer lvalue — a library `Owned`/`Shared` (heap-owner over a
    // concrete class) or a polymorphic IFACE owner (still intrinsic). A `Weak` doesn't keep the
    // object alive, so it can't be bound. `retain` = shared ownership (bump the count); else MOVE.
    std::string objCls = exprClass(objArg);
    std::string libT   = heapOwnerTarget(objCls);               // library heap-owner pointee ("" otherwise)
    bool isLibOwner    = !libT.empty();
    bool isIntrinOwner = isSmartPtrClass(objCls) && smartKind(objCls) != CollKind::Weak;
    if (!isLibOwner && !isIntrinOwner) {
        unsupported("BindableFunctionPtr obj: must be an Owned<T> or Shared<T>", ln); return;
    }
    bool retain = isLibOwner ? isCopyable(objCls) : (smartKind(objCls) == CollKind::Shared);
    std::string T = isLibOwner ? libT : _classes[objCls].collElemClass;
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
    // The receiver pointer: a library owner exposes it via `deref()` (T*); an intrinsic owner's
    // struct carries it in `.ptr`. The refcount block: library `.c`, intrinsic `.ctrl` (both a
    // `kama_ctrl`-compatible layout — the library `Ctrl` uses `usize` counts for this).
    if (isLibOwner) { indent(depth); *_out << nm << ".obj = (void*)" << objCls << "__deref(&(" << objE << "));\n"; }
    else            { indent(depth); *_out << nm << ".obj = (void*)(" << objE << ").ptr;\n"; }
    if (retain) {
        indent(depth); *_out << nm << ".ctrl = (kama_ctrl*)(" << objE << ")." << (isLibOwner ? "c" : "ctrl") << ";\n";
    }
    indent(depth); *_out << nm << ".fn = (void (*)(void))" << mi->cName << ";\n";
    indent(depth); *_out << nm << ".elemdtor = "
                         << (destr ? ("(void (*)(void*))" + T + "__dtor") : "0") << ";\n";
    // Ownership transfer: a shared owner RETAINS (bump strong); a unique owner MOVES — for a library
    // Owned, consume the source at COMPILE time so its dtor is skipped (the bindable now owns and frees
    // the pointee); an intrinsic Owned nulls its `.ptr` at runtime (its dtor guards a null pointer).
    if (retain) { indent(depth); *_out << "if (" << nm << ".ctrl) " << nm << ".ctrl->strong++;\n"; }
    else if (isLibOwner) { std::string mv = moveOnlySource(objArg, ln); if (!mv.empty()) markMoved(mv); }
    else { indent(depth); *_out << "(" << objE << ").ptr = NULL;\n"; }
}

// `BindableFunctionPtr<Sig> b = <free fn | another bindable>;` — promote a free
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

// invoke a bindable — branch on obj (bound: pass it first; free: call directly).
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
// the switch body shared by both `match` positions. Borrows the subject (a pointer, never a
// copy — a move-only union must not be shallow-copied), checks exhaustiveness, binds each arm's
// payload into a fresh scope, and either assigns the arm value to `resultTemp` (expression position)
// or emits it as a side-effect statement (statement position). Writes to the current `_out`.
// Materialize `value` into the already-declared lvalue `dst` (of type `dstCType`), honoring an owned
// hand-off: unwrap a give/copy marker, resolve an inline ctor/`new`/bare-generic-ctor from `dstCType`, then
// apply the give/copy matrix (smart-ptr / resource / collection / bindable — move consumes the source, copy
// duplicates). Mirrors the `return`-value hand-off, shared with value-producing `match` arms.
void CEmitter::emitOwnedValueInto(const std::string& dst, const std::string& dstCType,
                                  SharedExpression value, int line, int depth)
{
    int handoff = 0;   // 0 none, 1 give, 2 copy
    SharedExpression v = value;
    if (v) if (auto* h = dynamic_cast<HandoffNode*>(v.get())) { handoff = h->isGive ? 1 : 2; v = h->value; }
    bool ph = _hoistOK; _hoistOK = true;                       // inline-ctor hoisting
    std::string pmt = _matchTargetCType; _matchTargetCType = dstCType;   // `:= match(…)` / bare generic ctor
    std::string pvt = _variantTargetType; _variantTargetType = dstCType; // `:= Optional::Some(…)`
    std::string rv = tryHoistInlineCtor(v, dstCType, line);    // `:= Point(…)` / `:= List()`
    if (rv.empty()) rv = tryHoistInlineNew(v, dstCType, line); // `:= new T(…)`
    if (rv.empty()) rv = emitExpression(v);
    _matchTargetCType = pmt; _variantTargetType = pvt; _hoistOK = ph;
    flushHoisted(depth);
    indent(depth); *_out << dst << " = " << rv << ";\n";
    std::string rc = exprClass(v);
    // Smart pointer: give (or a bare dying local) MOVES out (invalidate the source); copy RETAINS.
    if (isSmartPtrClass(rc) && isNamedValue(v.get())) {
        CollKind k = smartKind(rc);
        bool doGive;
        if (handoff == 1)      doGive = true;
        else if (handoff == 2) { if (k == CollKind::Owned)
                                     unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", line);
                                 doGive = false; }
        else if (isSmartPtrLValue(v)) doGive = true;           // bare local/param dies here: move it out
        else { unsupported("handing out a smart-pointer field/element needs `give` (move it out) or "
                           "`copy` (retain — the source stays valid)", line); doGive = true; }
        indent(depth);
        if (doGive) *_out << smartPtrInvalidate(emitExpression(v), k, isInterface(_classes[rc].collElemClass)) << "\n";
        else        *_out << "(" << emitExpression(v) << ").ctrl->" << (k == CollKind::Weak ? "weak" : "strong") << "++;\n";
    }
    // Destructible `resource` VALUE: give MOVES (mark source moved), copy duplicates via copy().
    else if (isMoveOnlyValue(rc) && isNamedValue(v.get())) {
        bool cpy = isCopyable(rc);
        bool doCopy;
        if (handoff == 2) { if (!cpy) unsupported(("`" + rc + "` has no `copy` method — add `implements "
                                                   "Copyable(bare: …)`, or use `give` to move it").c_str(), line);
                            doCopy = cpy; }
        else if (handoff == 1) doCopy = false;
        else doCopy = cpy && _classes[rc].bareDefault == COPY;
        if (doCopy) { indent(depth); *_out << dst << " = " << rc << "__copy(&(" << emitExpression(v) << "));\n"; }
        else { std::string mv = moveOnlySource(v, line); if (!mv.empty()) markMoved(mv); }
    }
    // Named collection/`string` VALUE (exprClass is "" — key on dstCType): give/bare-dying moves, copy deep-copies.
    else if (ownsByValue(dstCType) && _classes.count(dstCType) && _classes[dstCType].isIntrinsicColl
             && isNamedValue(v.get())) {
        if (handoff == 2) {
            auto ci = _collections.find(dstCType);
            if (ci != _collections.end() && ci->second.elemDestructible && !ci->second.elemCopyable)
                unsupported(("`copy` of a `" + dstCType + "` needs copyable elements — use `give` to move it").c_str(), line);
            else { indent(depth); *_out << dst << " = " << dstCType << "__copy(&(" << emitExpression(v) << "));\n"; }
        } else { std::string mv = moveOnlySource(v, line); if (!mv.empty()) markMoved(mv); }
    }
    // A named BindableFunctionPtr may own its bound object: null the source so its scope-drop no-ops.
    else if (auto* rid = dynamic_cast<IdentifierNode*>(v.get())) {
        if (rid->value && isBindableClass(exprClass(v))) {
            std::string e = emitExpression(v);
            indent(depth);
            *_out << "(" << e << ").obj = NULL; (" << e << ").ctrl = NULL; ("
                  << e << ").fn = NULL; (" << e << ").elemdtor = NULL;\n";
        }
    }
}

void CEmitter::emitMatchSwitch(MatchNode* m, const std::string* resultTemp, int depth)
{
    std::string subjCls = exprClass(m->subject);
    auto cit = _classes.find(subjCls);
    if (cit == _classes.end() || !cit->second.isVariant) {
        std::string enumTy = exprEnumType(m->subject);
        if (!enumTy.empty()) { emitMatchPlainEnum(m, enumTy, resultTemp, depth); return; }
        unsupported("`match` requires an enum subject (a tagged union, or a plain enum)", m->line);
        return;
    }
    ClassInfo& ci = cit->second;

    // a specialized generic union (Optional_int32) stores its variant payloads in the template's
    // `T`; bind the instance's type args so payload binding types resolve concretely (mirrors
    // computeDestructible). Restored at the end; arm bodies contain no `T`, so a whole-switch scope is safe.
    std::map<std::string, SharedIdentifier> savedSubst;
    bool instSubst = ci.isGenericInst && _genericTypeInsts.count(subjCls);
    if (instSubst) {
        savedSubst = _typeSubst;
        _typeSubst.clear();
        const GenericTypeInst& gi = _genericTypeInsts[subjCls];
        const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
        for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
    }

    // Exhaustiveness (compile-time): every variant handled exactly once, unless a `_` wildcard is present.
    bool hasWildcard = false;
    std::set<std::string> covered;
    for (auto& a : *m->arms) {
        if (a->isWildcard()) { hasWildcard = true; continue; }
        std::string vn = a->variantName ? *a->variantName : "";
        bool found = false;
        for (auto& v : ci.variants) if (v.name == vn) { found = true; break; }
        if (!found)          unsupported(("`match` arm names unknown variant '" + vn + "' of '" + subjCls + "'").c_str(), a->line);
        if (covered.count(vn)) unsupported(("duplicate `match` arm for variant '" + vn + "'").c_str(), a->line);
        covered.insert(vn);
    }
    if (!hasWildcard)
        for (auto& v : ci.variants)
            if (!covered.count(v.name))
                unsupported(("`match` on '" + subjCls + "' is not exhaustive: variant '" + v.name
                             + "' is unhandled — add `case " + v.name + ":` or `case _:`").c_str(), m->line);

    // The subject is borrowed by pointer. A plain lvalue is addressed in place; a non-lvalue
    // (a call / construction result) can't be `&`-taken, so materialize an OWNING temp
    // first and drop it after the switch (arm payload bindings borrow it, like a foreach element).
    std::string sp = "__msub" + std::to_string(_tempCounter++);
    bool subjLvalue = dynamic_cast<IdentifierNode*>(m->subject.get())
                   || dynamic_cast<MemberAccessNode*>(m->subject.get())
                   || dynamic_cast<ThisAccessNode*>(m->subject.get())
                   || dynamic_cast<ElementAccessNode*>(m->subject.get());
    std::string subjOwner;
    // Evaluate the subject FIRST, then flush any temps it hoisted (e.g. a `string` literal materialized for
    // a `ref string` param — `match (map.get("k"))`), so their decls land BEFORE the subject line, not
    // inside the first arm body (which would leave them undeclared at the point of use).
    std::string subjExpr = emitExpression(m->subject);
    flushHoisted(depth);
    if (subjLvalue) {
        indent(depth); *_out << subjCls << "* " << sp << " = &(" << subjExpr << ");\n";
    } else {
        subjOwner = "__msubj" + std::to_string(_tempCounter++);
        indent(depth); *_out << subjCls << " " << subjOwner << " = " << subjExpr << ";\n";
        indent(depth); *_out << subjCls << "* " << sp << " = &" << subjOwner << ";\n";
    }
    indent(depth); *_out << "switch (" << sp << "->tag) {\n";
    auto beforeMove = _moveState;                            // each arm branches from the same pre-match state
    std::vector<std::map<std::string, MoveState>> armEnds;
    std::vector<bool> armDivs;
    for (auto& a : *m->arms) {
        _moveState = beforeMove;
        indent(depth + 1);
        if (a->isWildcard()) *_out << "default: {\n";
        else                 *_out << "case " << subjCls << "_" << *a->variantName << ": {\n";

        const VariantCase* vc = nullptr;
        if (!a->isWildcard()) for (auto& v : ci.variants) if (v.name == *a->variantName) { vc = &v; break; }

        // Fresh arm scope; bind the payload fields (borrowed copies — same as a foreach element).
        Scope sc; _scopes.push_back(sc);
        struct Saved { std::string name; bool had; std::string prev; };
        std::vector<Saved> savedTypes;
        if (a->bindings && !a->bindings->empty()) {
            if (!vc || a->bindings->size() != vc->payload.size())
                unsupported(("`match` arm for '" + (a->variantName ? *a->variantName : std::string("_"))
                             + "' binds " + std::to_string(a->bindings->size()) + " field(s) but the variant has "
                             + std::to_string(vc ? vc->payload.size() : 0)).c_str(), a->line);
            for (size_t i = 0; vc && i < a->bindings->size() && i < vc->payload.size(); ++i) {
                std::string bn = *(*a->bindings)[i];
                const FieldInfo& pf = vc->payload[i];
                std::string bcty = cType(pf.type);
                indent(depth + 2);
                *_out << bcty << " " << bn << " = " << sp << "->u." << *a->variantName << "." << pf.name << ";\n";
                savedTypes.push_back({bn, (bool)_localTypes.count(bn), _localTypes.count(bn) ? _localTypes[bn] : std::string()});
                _localTypes[bn] = (isClass(bcty) || isInterface(bcty) || isSigType(bcty)) ? bcty : "";
            }
        }

        // Arm body: a single expression, OR a block. Hoist temps INSIDE the arm braces.
        if (a->block) {
            // block arm — emit its statements in the arm scope. For a value-producing match the
            // block's LAST statement must yield the value (an expression-statement); earlier statements
            // (locals, side effects) run normally, RAII-dropped by the arm's scope cleanup.
            SharedStatementList stmts = a->block->statements;
            size_t nstmt = stmts ? stmts->size() : 0;
            for (size_t i = 0; i < nstmt; ++i) {
                SharedStatement st = (*stmts)[i];
                auto armv = std::dynamic_pointer_cast<ArmValueNode>(st);   // `:= expr;` — the arm's value
                if (armv && i + 1 != nstmt) {
                    unsupported("`:=` must be the final statement of a match arm", armv->line);
                    continue;
                }
                if (resultTemp && i + 1 == nstmt) {
                    // A value-producing arm's block states its value with `:= expr;` as the final statement.
                    // The value may be an owned hand-off (`:= give x`) or a bare generic ctor (`:= List()`).
                    if (armv) emitOwnedValueInto(*resultTemp, _matchTargetCType, armv->value, armv->line, depth + 2);
                    else unsupported("a value-producing `match` arm block must end in `:= <expr>;`", a->line);
                } else {
                    emitStatement(st, depth + 2);
                }
            }
            emitScopeCleanup(_scopes.back(), depth + 2);
        } else if (resultTemp) {
            emitOwnedValueInto(*resultTemp, _matchTargetCType, a->body, a->line, depth + 2);   // `case X: give x;`
            emitScopeCleanup(_scopes.back(), depth + 2);   // drop owned sub-expr temps (e.g. concat intermediates)
        } else {
            bool ph = _hoistOK; _hoistOK = true;
            std::string av = emitExpression(a->body);
            _hoistOK = ph;
            flushHoisted(depth + 2);
            indent(depth + 2); *_out << av << ";\n";
            emitScopeCleanup(_scopes.back(), depth + 2);   // drop owned sub-expr temps of a statement-position arm
        }
        indent(depth + 2); *_out << "break;\n";

        for (auto& sv : savedTypes) { if (sv.had) _localTypes[sv.name] = sv.prev; else _localTypes.erase(sv.name); }
        armEnds.push_back(_moveState);
        armDivs.push_back(a->block ? bodyDiverges(std::static_pointer_cast<StatementNode>(a->block)) : false);
        _scopes.pop_back();
        indent(depth + 1); *_out << "}\n";
    }
    mergeMatchMoveStates(beforeMove, armEnds, armDivs);
    if (!hasWildcard) { indent(depth + 1); *_out << "default: break;\n"; }   // exhaustive; keeps the C switch total
    indent(depth); *_out << "}\n";
    // a materialized owning subject (a call/construction result) is dropped once after the
    // switch — bindings only borrowed it, so this releases its owned resource (no leak, no double-free).
    if (!subjOwner.empty() && _classes.count(subjCls) && _classes[subjCls].destructible) {
        indent(depth); *_out << subjCls << "__dtor(&" << subjOwner << ");\n";
    }
    if (instSubst) _typeSubst = savedSubst;
}

// a value-producing `match` in expression position. Lifts to a result temp + a switch, hoisted
// before the enclosing statement — strict ISO C11, no GNU statement-expression.
std::string CEmitter::emitMatch(MatchNode* m)
{
    if (!_hoistOK) {
        unsupported("a value-producing `match` here needs a statement slot — bind it to a local first", m->line);
        return "0";
    }
    if (_matchTargetCType.empty()) {
        unsupported("a value-producing `match` must appear in a typed position "
                    "(a local-variable initializer or a `return`)", m->line);
        return "0";
    }
    std::string t  = "__match" + std::to_string(_tempCounter++);
    std::string rt = _matchTargetCType;
    // Build the lowering into a buffer so it can be hoisted as one block; the arms flush their own
    // temps inside their braces (so nested lifting can't escape the arm).
    std::ostringstream buf;
    std::ostream* savedOut = _out; _out = &buf;
    std::vector<std::string> savedHoist; savedHoist.swap(_hoisted);
    *_out << rt << " " << t << ";\n";
    emitMatchSwitch(m, &t, 0);
    savedHoist.swap(_hoisted);
    _out = savedOut;
    _hoisted.push_back(buf.str());
    return t;
}

void CEmitter::emitMatchStatement(MatchNode* m, int depth)
{
    line(m->line);
    emitMatchSwitch(m, nullptr, depth);
}

void CEmitter::mergeMatchMoveStates(const std::map<std::string, MoveState>& before,
                                    const std::vector<std::map<std::string, MoveState>>& armEnds,
                                    const std::vector<bool>& armDivs)
{
    _moveState = before;
    for (auto& kv : before) {
        bool any = false, allMoved = true, allNot = true;
        for (size_t i = 0; i < armEnds.size(); ++i) {
            if (armDivs[i]) continue;                        // a diverging arm never reaches the join
            any = true;
            MoveState s = armEnds[i].count(kv.first) ? armEnds[i].at(kv.first) : kv.second;
            if (s != MoveState::Moved)    allMoved = false;
            if (s != MoveState::NotMoved) allNot   = false;
        }
        if (!any) { _moveState[kv.first] = kv.second; continue; }   // every arm diverges -> join unreachable
        _moveState[kv.first] = allMoved ? MoveState::Moved
                             : (allNot  ? MoveState::NotMoved : MoveState::MaybeMoved);
    }
}

// Resolve the plain (payload-less) enum type of a `match` subject; "" if it is not a plain enum.
// A tagged union resolves through exprClass instead — this only sees bare-integer enums.
std::string CEmitter::exprEnumType(SharedExpression e)
{
    if (!e) return "";
    auto asEnum = [&](const std::string& ty) -> std::string {
        return (!ty.empty() && _enums.count(ty)) ? ty : std::string();
    };
    if (auto* id = dynamic_cast<IdentifierNode*>(e.get())) {
        if (!id->value) return "";
        auto it = _localCTypes.find(*id->value);              // a local / param of enum type
        if (it != _localCTypes.end()) { std::string r = asEnum(it->second); if (!r.empty()) return r; }
        if (_currentClass) {                                  // a bare field reference inside a method
            ClassInfo* owner = findFieldOwner(_currentClass, *id->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *id->value && f.type) { std::string r = asEnum(cType(f.type)); if (!r.empty()) return r; }
        }
        return "";
    }
    if (auto* ma = dynamic_cast<MemberAccessNode*>(e.get())) {   // `obj.field` of enum type
        std::string recv = exprClass(ma->expression);
        if (!recv.empty() && ma->identifier && ma->identifier->value && _classes.count(recv)) {
            ClassInfo* owner = findFieldOwner(&_classes[recv], *ma->identifier->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *ma->identifier->value && f.type) { std::string r = asEnum(cType(f.type)); if (!r.empty()) return r; }
        }
        return "";
    }
    return "";
}

// Lower a `match` over a plain enum to a C `switch` on the integer value. Same compile-time
// exhaustiveness + `_` wildcard as the tagged-union path, but no payload binding.
void CEmitter::emitMatchPlainEnum(MatchNode* m, const std::string& enumTy, const std::string* resultTemp, int depth)
{
    EnumInfo& ei = _enums[enumTy];

    bool hasWildcard = false;
    std::set<std::string> covered;
    for (auto& a : *m->arms) {
        if (a->isWildcard()) { hasWildcard = true; continue; }
        std::string vn = a->variantName ? *a->variantName : "";
        bool found = false;
        for (auto& mem : ei.members) if (mem.name == vn) { found = true; break; }
        if (!found)            unsupported(("`match` arm names unknown case '" + vn + "' of enum '" + enumTy + "'").c_str(), a->line);
        if (a->bindings && !a->bindings->empty())
                               unsupported(("enum case '" + vn + "' carries no payload to bind").c_str(), a->line);
        if (covered.count(vn)) unsupported(("duplicate `match` arm for case '" + vn + "'").c_str(), a->line);
        covered.insert(vn);
    }
    if (!hasWildcard)
        for (auto& mem : ei.members)
            if (!covered.count(mem.name))
                unsupported(("`match` on '" + enumTy + "' is not exhaustive: case '" + mem.name
                             + "' is unhandled — add `case " + mem.name + ":` or `case _:`").c_str(), m->line);

    indent(depth); *_out << "switch (" << emitExpression(m->subject) << ") {\n";
    auto beforeMove = _moveState;
    std::vector<std::map<std::string, MoveState>> armEnds;
    std::vector<bool> armDivs;
    for (auto& a : *m->arms) {
        _moveState = beforeMove;
        indent(depth + 1);
        if (a->isWildcard()) *_out << "default: {\n";
        else                 *_out << "case " << enumTy << "_" << *a->variantName << ": {\n";

        Scope sc; _scopes.push_back(sc);
        if (a->block) {
            SharedStatementList stmts = a->block->statements;
            size_t nstmt = stmts ? stmts->size() : 0;
            for (size_t i = 0; i < nstmt; ++i) {
                SharedStatement st = (*stmts)[i];
                auto armv = std::dynamic_pointer_cast<ArmValueNode>(st);   // `:= expr;` — the arm's value
                if (armv && i + 1 != nstmt) {
                    unsupported("`:=` must be the final statement of a match arm", armv->line);
                    continue;
                }
                if (resultTemp && i + 1 == nstmt) {
                    // A value-producing arm's block states its value with `:= expr;` as the final statement.
                    // The value may be an owned hand-off (`:= give x`) or a bare generic ctor (`:= List()`).
                    if (armv) emitOwnedValueInto(*resultTemp, _matchTargetCType, armv->value, armv->line, depth + 2);
                    else unsupported("a value-producing `match` arm block must end in `:= <expr>;`", a->line);
                } else {
                    emitStatement(st, depth + 2);
                }
            }
            emitScopeCleanup(_scopes.back(), depth + 2);
        } else if (resultTemp) {
            emitOwnedValueInto(*resultTemp, _matchTargetCType, a->body, a->line, depth + 2);   // `case X: give x;`
            emitScopeCleanup(_scopes.back(), depth + 2);   // drop owned sub-expr temps (e.g. concat intermediates)
        } else {
            bool ph = _hoistOK; _hoistOK = true;
            std::string av = emitExpression(a->body);
            _hoistOK = ph;
            flushHoisted(depth + 2);
            indent(depth + 2); *_out << av << ";\n";
            emitScopeCleanup(_scopes.back(), depth + 2);   // drop owned sub-expr temps of a statement-position arm
        }
        indent(depth + 2); *_out << "break;\n";
        armEnds.push_back(_moveState);
        armDivs.push_back(a->block ? bodyDiverges(std::static_pointer_cast<StatementNode>(a->block)) : false);
        _scopes.pop_back();
        indent(depth + 1); *_out << "}\n";
    }
    mergeMatchMoveStates(beforeMove, armEnds, armDivs);
    if (!hasWildcard) { indent(depth + 1); *_out << "default: break;\n"; }   // exhaustive; keeps the C switch total
    indent(depth); *_out << "}\n";
}

// an inline constructor `Cls(args)` as a general rvalue (return / variant payload). A ctor
// lowers to `Cls__ctor(&dest, …)` which needs an lvalue destination, so materialize a HOISTED temp
// (declared before the leaf statement — pure ISO C, no `({…})`) and return its name. "" if `e` is not
// an inline ctor for exactly `targetCType`, or no hoist slot. Mirrors the arg-position recognizer.
std::string CEmitter::tryHoistInlineCtor(SharedExpression e, const std::string& targetCType, int srcLine)
{
    if (!_hoistOK || targetCType.empty()) return "";
    auto* iv = dynamic_cast<InvocationNode*>(e.get());
    if (!iv || !iv->identifier || !iv->identifier->value) return "";
    std::string rn = resolveUserName(*iv->identifier->value, iv->identifier->qualifier);
    std::string ctorCls;
    if (isClass(rn) && _classes.count(rn)) ctorCls = rn;
    else { auto g = _genericTypeInstOf.find(targetCType);   // ctor names template `Box`; target is `Box_int32`
           if (g != _genericTypeInstOf.end() && g->second == rn) ctorCls = targetCType; }
    if (ctorCls.empty() || ctorCls != targetCType || _classes[ctorCls].isIntrinsicColl) return "";
    std::string t = "__ctorarg" + std::to_string(_tempCounter++);
    std::string ctor = emitCtorCall(t, _classes[ctorCls], iv->args, srcLine);
    _hoisted.push_back(ctorCls + " " + t + "; " + ctor + ";");
    return t;
}

// an inline `new T(...)` as a general rvalue whose target is an owning-pointer type (Owned/Shared, or a
// library HeapOwner) — box it into a HOISTED temp (declared before the leaf statement, pure ISO C, no
// `({…})`) and return its name. Ownership transfers to the CONSUMER (a callee param, or a return `__ret`
// temp), which is the sole party registered to drop it — the box is NOT recordDestructibleLocal'd here.
// The emitted C mirrors the (ASan-clean) local-init boxing at emitLocalVariableDeclaration; keep in sync.
//   returns "" when not applicable (not a `new`, or the target isn't an owning pointer / the element
//   plainly mismatches) so the CALLER diagnoses; on a specific semantic error (Weak/abstract/non-impl/no
//   `adopt`) it emits ONE precise diagnostic and returns a declared degenerate temp (compilation already
//   failed, so the throwaway C is never built) — this suppresses the caller's generic fallback gate.
std::string CEmitter::tryHoistInlineNew(SharedExpression e, const std::string& targetCType, int srcLine)
{
    if (!_hoistOK || targetCType.empty()) return "";
    auto* oc = dynamic_cast<ObjectCreationNode*>(e.get());
    if (!oc) return "";
    auto cit = _classes.find(targetCType);
    if (cit == _classes.end()) return "";
    std::string octy = cType(oc->type);
    // one precise diagnostic + a declared degenerate temp (see header) — suppresses the caller's gate.
    auto reject = [&](const std::string& msg) -> std::string {
        unsupported(msg.c_str(), srcLine);
        std::string t = "__newarg" + std::to_string(_tempCounter++);
        _hoisted.push_back(targetCType + " " + t + " = {0};");
        return t;
    };

    // ---- LIBRARY HeapOwner, concrete element: `T* hp = malloc; C__ctor(hp,…); box = Owner__adopt(hp)` ----
    // (mirror emitLocalVariableDeclaration ~1265-1300, incl. the derived→base upcast; `adopt` itself
    // allocates the Shared ctrl, so — unlike the intrinsic paths below — we do NOT emit kama_ctrl_new().)
    if (!isSmartPtrClass(targetCType)) {
        std::string T = heapOwnerTarget(targetCType);
        if (T.empty()) return "";                              // not an owning pointer — caller diagnoses
        bool upcastNew = (octy != T) && isClass(octy) && isBaseOf(T, octy);
        std::string C = upcastNew ? octy : T;                  // the concrete actually built
        if (octy != T && !upcastNew)
            return reject("`" + targetCType + "` owns `" + T + "`, but got `new " + octy
                          + "(...)` — name the element type or a derived of it, not the owner");
        if (isClass(C) && _classes[C].isAbstractClass)
            return reject("cannot instantiate abstract class '" + C + "'");
        ClassInfo* ao = nullptr;
        MethodInfo* adoptM = findMethod(&_classes[targetCType], "adopt", &ao);
        if (!adoptM) return reject("`" + targetCType + "` implements HeapOwner but has no `adopt` method");
        std::string hp = "__heap"   + std::to_string(_tempCounter++);
        std::string t  = "__newarg" + std::to_string(_tempCounter++);
        std::string box = C + "* " + hp + " = (" + C + "*)malloc(sizeof(" + C + "));";
        if (isClass(C) && _classes[C].hasCtor)
            box += " " + emitReorderedCall(C + "__ctor", hp, _classes[C].ctorParams, oc->args, srcLine) + ";";
        std::string adoptArg = hp;                             // adopt the base subobject when widening (offset-0)
        if (upcastNew) {
            std::string bp = basePathTo(&_classes[C], &_classes[T]);
            if (!bp.empty()) bp.pop_back();
            adoptArg = "(" + T + "*)&(" + hp + "->" + bp + ")";
        }
        box += " " + targetCType + " " + t + " = " + adoptM->cName + "(" + adoptArg + ");";
        _hoisted.push_back(box);
        return t;
    }

    // ---- INTRINSIC owning pointer (registered fat/thin box) ----
    std::string T = cit->second.collElemClass;
    if (smartKind(targetCType) == CollKind::Weak)              // a Weak is non-owning — it can't own a `new`
        return reject("`new` allocates on the heap — a `Weak` can't own it; wrap it in an `Owned<" + T
                      + ">`/`Shared<" + T + ">` (a `Weak` is a non-owning observer)");

    if (isInterface(T)) {   // `new Concrete` into a Shared/Owned<Interface> — fat box {obj, vtbl[, ctrl]}
        auto oit = _classes.find(octy);                       // (mirror emitLocalVariableDeclaration ~1213-1237)
        bool implementsT = false;
        if (oit != _classes.end())
            for (auto& i : oit->second.interfaces) if (i == T) { implementsT = true; break; }
        if (!implementsT)
            return reject("`new " + octy + "` does not implement `" + T + "` — `" + targetCType
                          + "` owns a class that satisfies the contract");
        if (_classes[octy].isAbstractClass)
            return reject("cannot instantiate abstract class '" + octy + "'");
        std::string t = "__newarg" + std::to_string(_tempCounter++);
        std::string box = targetCType + " " + t + " = {0}; " + t + ".obj = malloc(sizeof(" + octy + "));";
        if (_classes[octy].hasCtor)
            box += " " + emitReorderedCall(octy + "__ctor", "(" + octy + "*)" + t + ".obj",
                                           _classes[octy].ctorParams, oc->args, srcLine) + ";";
        box += " " + t + ".vtbl = &" + octy + "__as_" + T + ";";
        if (smartKind(targetCType) == CollKind::Shared) box += " " + t + ".ctrl = kama_ctrl_new();";
        _hoisted.push_back(box);
        return t;
    }

    if (octy != T) return "";                                  // element mismatch — let the caller diagnose
    if (isClass(T) && _classes[T].isAbstractClass)
        return reject("cannot instantiate abstract class '" + T + "'");
    std::string t = "__newarg" + std::to_string(_tempCounter++);
    std::string box = targetCType + " " + t + " = {0}; " + t + ".ptr = (" + T + "*)malloc(sizeof(" + T + "));";
    if (isClass(T) && _classes[T].hasCtor)
        box += " " + emitReorderedCall(T + "__ctor", t + ".ptr", _classes[T].ctorParams, oc->args, srcLine) + ";";
    if (smartKind(targetCType) == CollKind::Shared) box += " " + t + ".ctrl = kama_ctrl_new();";
    _hoisted.push_back(box);
    return t;
}

// resolve the variant type a `::` qualifier names. A non-generic union is in _classes
// directly; a generic union names its bare template (`Optional`, in _genericTypes) — resolve it to
// the target instance set by the enclosing typed position (`_variantTargetType`, e.g. Optional_int32).
ClassInfo* CEmitter::resolveVariantType(const std::string& qualResolved)
{
    auto direct = _classes.find(qualResolved);
    if (direct != _classes.end() && direct->second.isVariant) return &direct->second;
    if (_genericTypeParams.count(qualResolved) && !_variantTargetType.empty()) {
        auto of = _genericTypeInstOf.find(_variantTargetType);
        if (of != _genericTypeInstOf.end() && of->second == qualResolved) {
            auto inst = _classes.find(_variantTargetType);
            if (inst != _classes.end() && inst->second.isVariant) return &inst->second;
        }
    }
    return nullptr;
}

// `Union::Variant(field: value, …)` -> a C99 compound literal
//   (Shape){ .tag = Shape_Circle, .u.Circle = { .radius = 2.0 } }
// A no-payload variant omits the union member. The compound literal is an rvalue; stored in a local
// it is recorded destructible (if the union owns a resource) and drops via the switch-on-tag dtor.
std::string CEmitter::emitVariantConstruction(ClassInfo& ci, const std::string& variant,
                                              SharedArgumentList args, int srcLine)
{
    const VariantCase* vc = nullptr;
    for (auto& v : ci.variants) if (v.name == variant) { vc = &v; break; }
    if (!vc) { unsupported(("'" + ci.name + "' has no variant '" + variant + "'").c_str(), srcLine); return "0"; }

    // a specialized generic union (Optional_int32) stores payloads in the template's `T`; bind the
    // instance's type args so each payload field's C type resolves concretely (in the caller's scope).
    std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
    bool instSubst = ci.isGenericInst && _genericTypeInsts.count(ci.name);
    if (instSubst) {
        _typeSubst.clear();
        const GenericTypeInst& gi = _genericTypeInsts[ci.name];
        const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
        for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
    }

    std::map<std::string, ArgumentNode*> byName;
    if (args) for (auto& a : *args) if (a->name && a->name->value) byName[*a->name->value] = a.get();
    size_t argc = args ? args->size() : 0;
    if (argc != vc->payload.size())
        unsupported(("variant '" + ci.name + "::" + variant + "' takes " + std::to_string(vc->payload.size())
                     + " payload field(s), got " + std::to_string(argc)).c_str(), srcLine);

    std::string s = "(" + ci.name + "){ .tag = " + ci.name + "_" + variant;
    if (!vc->payload.empty()) {
        s += ", .u." + variant + " = {";
        bool first = true;
        for (auto& f : vc->payload) {
            auto ai = byName.find(f.name);
            if (ai == byName.end()) {
                unsupported(("missing payload field '" + f.name + "' for '" + ci.name + "::" + variant + "'").c_str(), srcLine);
                continue;
            }
            std::string fcls = cType(f.type);            // the payload field's C type (Shared_Probe / int32_t / …)
            SharedExpression argExpr = ai->second->expression;
            int handoff = 0;                             // 0 none, 1 give, 2 copy
            if (auto* h = dynamic_cast<HandoffNode*>(argExpr.get())) { handoff = h->isGive ? 1 : 2; argExpr = h->value; }
            // an inline construction as the payload (inline `new`, an inline generic-instance ctor,
            // or a nested `Some(Some(…))` / value-producing `match`) materializes against the field type;
            // propagate the target so the nested variant/match resolves.
            std::string pmt = _matchTargetCType, pvt = _variantTargetType;
            _matchTargetCType = _variantTargetType = fcls;
            std::string val = tryHoistInlineNew(argExpr, fcls, srcLine);
            if (val.empty()) val = tryHoistInlineCtor(argExpr, fcls, srcLine);
            if (val.empty()) val = emitExpression(argExpr);
            _matchTargetCType = pmt; _variantTargetType = pvt;
            std::string argCls = exprClass(argExpr);
            std::string field;
            if (isSmartPtrClass(argCls) && isNamedValue(argExpr.get())) {
                // A named smart pointer MOVES/RETAINS into the union (which now owns it, dropped by the
                // switch-on-tag dtor). Same hand-off as a by-value call arg, hoisted (ISO C).
                CollKind k = smartKind(argCls);
                bool doGive = (handoff == 1) || (handoff == 0 && k == CollKind::Owned);
                if (handoff == 2 && k == CollKind::Owned)
                    unsupported("an `Owned` is unique — it can't be `copy`'d; use `give` to move it", srcLine);
                if (!_hoistOK)
                    unsupported("moving a smart pointer into a variant here needs a statement slot — bind the "
                                "constructed value to a local first", srcLine);
                std::string t = "__kama_varg" + std::to_string(_tempCounter++);
                std::string side = doGive ? smartPtrInvalidate("(" + val + ")", k, isInterface(_classes[argCls].collElemClass))
                                          : ("(" + val + ").ctrl->" + (k == CollKind::Weak ? "weak" : "strong") + "++;");
                _hoisted.push_back(fcls + " " + t + " = (" + val + "); " + side);
                field = t;
            } else if (isMoveOnlyValue(argCls) && isNamedValue(argExpr.get())) {
                // A named `resource` value handed into a union field. `give` MOVES it (mark the source
                // moved, dtor suppressed); `copy` deep-copies (source survives); a BARE arg follows the
                // type's `bare:` default. A non-Copyable resource → move.
                bool cpy = isCopyable(argCls);
                bool doCopy;
                if (handoff == 2) {
                    if (!cpy) unsupported(("`" + argCls + "` has no `copy` method — use `give` to move it").c_str(), srcLine);
                    doCopy = cpy;
                } else if (handoff == 1) doCopy = false;
                else doCopy = cpy && _classes[argCls].bareDefault == COPY;
                if (doCopy) field = argCls + "__copy(&(" + val + "))";
                else { std::string mv = moveOnlySource(argExpr, srcLine); if (!mv.empty()) markMoved(mv); field = val; }
            } else if (!argCls.empty() && _classes.count(argCls) && _classes[argCls].isIntrinsicColl && handoff) {
                // Hand a collection/`string` into the union: `copy` deep-copies (`__copy`; the union owns
                // the clone, the source survives), `give` transfers the struct (buffer) and nulls the
                // source so its scope-drop is a no-op (the union now owns it, dropped by the tag dtor).
                if (handoff == 2) {                                  // copy = deep copy into the payload
                    auto ci = _collections.find(argCls);
                    if (ci != _collections.end() && ci->second.elemDestructible && !ci->second.elemCopyable)
                        unsupported(("`copy` of a `" + argCls + "` needs copyable elements — use `give` to "
                                     "move it").c_str(), srcLine);
                    else field = argCls + "__copy(&(" + val + "))";
                }
                else if (!_hoistOK)
                    unsupported("moving a collection into a variant here needs a statement slot — bind the "
                                "constructed value to a local first", srcLine);
                else {
                    std::string t = "__varg" + std::to_string(_tempCounter++);
                    _hoisted.push_back(fcls + " " + t + " = (" + val + "); ("
                                       + val + ").data = NULL; (" + val + ").len = 0;");
                    field = t;
                }
            } else if (!argCls.empty() && _classes.count(argCls) && _classes[argCls].isIntrinsicColl
                       && isNamedValue(argExpr.get())) {
                // A bare NAMED collection/`string` into a variant would alias (the union owns it AND the
                // source frees it → double-free / dangle). Ownership transfer must be explicit — `give`
                // (move) or `copy` (deep). A fresh rvalue payload (a call result / literal) needs no marker.
                unsupported("a collection/`string` into a variant transfers ownership — say `give` (move) "
                            "or `copy` (deep)", srcLine);
                field = val;
            } else {
                // Primitive / plain value / fresh smart-ptr rvalue (factory result): consumed in place.
                // A `give`/`copy` on a plain value (primitive or `value` struct) is a no-op — a value is
                // always copied — so only a stray marker on a fell-through resource rvalue is an error.
                bool plainValue = argCls.empty()
                    || (_classes.count(argCls) && _classes[argCls].kind == TypeKind::Value);
                // A tagged-union (enum) value accepts `give` into the outer variant — the tag+payload copies
                // bitwise (a no-op move for a non-owning enum like `Result::Ok(value: give myEnum)` in
                // `decode<T>`). An owning-payload enum move is a deferred follow-up (it would need the source
                // dtor suppressed here). A plain value's `give` is likewise a no-op copy.
                bool variantVal = !argCls.empty() && _classes.count(argCls) && _classes[argCls].isVariant;
                if (handoff && !plainValue && !variantVal && !isSmartPtrClass(argCls))
                    unsupported("`give`/`copy` apply to a named smart pointer / resource value", srcLine);
                field = val;
            }
            s += (first ? " ." : ", .");
            s += f.name + " = " + field;
            first = false;
        }
        s += " }";
    }
    if (instSubst) _typeSubst = savedSubst;
    return s + " }";
}

// a place-returning free/static call lowers to `T*`; deref it so the call is an lvalue EVERYWHERE
// (read copies out; `f(…) = x` writes through; `ref f(…)` / `f(…).field` / nesting all compose) —
// the same treatment a place-returning method call gets. `&(*…)` folds, so chains stay clean ISO C.
static inline std::string placeWrap(const std::string& c, bool isPlace)
{
    return isPlace ? ("(*" + c + ")") : c;
}

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

    // a call to a generic function was resolved to a concrete instantiation at discovery.
    // Route it to that specialized C name; reorder named args off the template's param list.
    {
        auto ci = _callInst.find(call);
        if (ci != _callInst.end()) {
            const GenericInst& gi = _genericInsts[ci->second];
            const FuncSig& tmpl = _funcs[gi.templateKey];
            return placeWrap(emitReorderedCall(gi.mangledName, "", tmpl.params, call->args, call->line),
                             tmpl.isPlaceReturn);
        }
    }

    // Turbofish `f::<…>` that didn't resolve to a generic instantiation -> the target isn't a generic
    // function. Reject rather than silently drop the type arguments.
    if (call->identifier->genericArgs) {
        unsupported(("`" + name + "::<…>` — turbofish type arguments are only valid on a generic function").c_str(), call->line);
        return "0";
    }

    // FunctionPtr invoke: a bare local whose type is a signature → an indirect
    // call `c(reordered args)` (c IS the function pointer). Named-arg reorder off the sig.
    if ((!call->identifier->qualifier || call->identifier->qualifier->empty())
        && _localTypes.count(name) && isSigType(_localTypes[name])) {
        const SigInfo& sig = _sigs.at(_localTypes[name]);
        std::string callee = _refParams.count(name) ? ("(*" + name + ")") : name;
        return emitReorderedCall(callee, "", sig.params, call->args, call->line);
    }

    // BindableFunctionPtr invoke: a bare local of bindable type → branch on the
    // bound object (call the method with it, or the free fn directly).
    if ((!call->identifier->qualifier || call->identifier->qualifier->empty())
        && _localTypes.count(name) && isBindableClass(_localTypes[name])) {
        return emitBindableInvoke(name, _localTypes[name], call->args, call->line);
    }

    // FFI: `addr(x)` is a builtin — the address of a local/value (`&(x)`),
    // for out-params and passing a descriptor by pointer. A controlled operation
    // (it addresses a real value), so it needs no `unsafe`.
    if (name == "addr" && (!call->identifier->qualifier || call->identifier->qualifier->empty())
        && call->args && call->args->size() == 1)
        return "&(" + emitExpression((*call->args)[0]->expression) + ")";

    // `panic(msg: s)` and `assert(cond: c)` — builtins that trap cleanly (abort with a message), the
    // user-facing form of the runtime bounds trap. Let a user collection bounds-check itself without
    // dropping to FFI `abort()`. `panic` aborts unconditionally; `assert` aborts iff the condition is
    // false. Recoverable errors use `Result<T,E>` — `panic` is for "this is a bug that can't continue".
    bool bareCall = (!call->identifier->qualifier || call->identifier->qualifier->empty());
    if (name == "panic" && bareCall && call->args && call->args->size() == 1)
        return "kama_panic(" + emitExpression((*call->args)[0]->expression) + ")";
    if (name == "assert" && bareCall && call->args && call->args->size() == 1)
        return "((" + emitExpression((*call->args)[0]->expression)
             + ") ? (void)0 : kama_panic(kama_string_lit(\"assertion failed\", 16)))";
    // `drop(place)` — run the destructor of a place's value (for a library owner over `Ptr<T>` to drop
    // its heap pointee before `free`). A no-op when the value's type isn't destructible. The type is
    // resolved via exprClass (so `drop(this.deref())` reaches the pointee `T` through a `ref T` return).
    if (name == "drop" && bareCall && call->args && call->args->size() == 1) {
        SharedExpression a = (*call->args)[0]->expression;
        std::string cls = exprClass(a);
        if (!cls.empty() && _classes.count(cls)) {
            // A polymorphic type drops through its vtable (`__vdrop`): a base handle owning a
            // derived must run the derived's dtor, and a derived can own resources even when the
            // base doesn't — so the runtime vtable, not the static type, decides.
            if (_classes[cls].hasVtable) return cls + "__vdrop(&(" + emitExpression(a) + "))";
            if (_classes[cls].destructible) return cls + "__dtor(&(" + emitExpression(a) + "))";
        }
        return "(void)0";   // nothing to drop (a value / non-destructible type)
    }
    // `addr(of: place)` — the address of a PLACE (a field/local/element) as a `Ptr<T>`. Taking an
    // address is safe (a `Ptr` is safe to hold); dereferencing it stays `unsafe`. Lets a library type
    // keep a live back-pointer to another's field — e.g. an iterator to its container's mutation counter.
    if (name == "addr" && bareCall && call->args && call->args->size() == 1) {
        SharedExpression a = (*call->args)[0]->expression;
        if (!isNamedValue(a.get()))
            unsupported("`addr(of: …)` needs a place — a field, local, or element (not a temporary)", call->line);
        return "(&(" + emitPlace(a) + "))";
    }

    // (A bare function name is a value — its C function pointer — so `FunctionPtr<Sig> c = fn;`
    // and passing `fn` directly bind a callable; there is no separate `funcptr(of: fn)` builtin.)

    // A `::`-qualified callee is **scope resolution**: `Namespace::fn(...)`. The head of a `::`
    // is always a type/namespace — never an object (object
    // access is `.`/member_access) — so the old namespace-vs-object precedence hack
    // is gone. `resolveFunc` handles namespace + `using` + alias resolution.
    SharedStringList qual = call->identifier->qualifier;
    if (qual && !qual->empty()) {
        // `Union::Variant(args)` — construct a discriminated-union value with a payload
        // (`Optional<int32>::Some` resolves via the target-type context).
        auto tq = std::make_shared<StringList>();
        for (size_t i = 0; i + 1 < qual->size(); ++i) tq->push_back((*qual)[i]);
        if (ClassInfo* vt = resolveVariantType(resolveUserName(*qual->back(), tq)))
            for (auto& v : vt->variants)
                if (v.name == name)
                    return emitVariantConstruction(*vt, name, call->args, call->line);
        // `Type::method(args)` — a static method (no implicit `self`). The qualifier head
        // resolves to a class; the named method must be `static`.
        std::string typeName = resolveUserName(*qual->back(), tq);
        // Fallback: a bound type-parameter head (`T::make` in a generic `fn f<T: C>()`) substitutes to its
        // monomorphized concrete type, so a static contract method on a type-param resolves (e.g.
        // `json::decode<T: Deserialize>` calling `T::deserialize(...)`, or a collection's `T::deserialize`).
        // The concrete type may be a PRIMITIVE whose static conformance lives in `_primConformances`
        // (`int32::deserialize` -> `r.readI32()`), so consider both tables.
        if (!_classes.count(typeName) && !_primConformances.count(typeName)) {
            auto sit = _typeSubst.find(*qual->back());
            if (sit != _typeSubst.end() && sit->second) {
                std::string concrete = cType(sit->second);
                if (_classes.count(concrete) || _primConformances.count(concrete)) typeName = concrete;
            }
        }
        // A user type resolves in `_classes`; a primitive/intrinsic-collection static (a retro-impl
        // conformance) resolves via `retroTargetInfo` (`_primConformances`).
        ClassInfo* stci = _classes.count(typeName) ? &_classes[typeName] : retroTargetInfo(typeName);
        if (stci) {
            ClassInfo* owner = nullptr;
            MethodInfo* mi = findMethod(stci, name, &owner);
            if (mi && mi->isStatic) {
                canAccess(owner, mi->visibility, name, call->line);
                // a place-returning `static fn ref T` returns a `T*` — deref like a free fn (no self)
                return placeWrap(emitReorderedCall(mi->cName, "", mi->params, call->args, call->line),
                                 mi->isPlaceReturn);
            }
            if (mi && !mi->isStatic)
                unsupported(("`" + typeName + "::" + name + "` names a non-static method — call it on an instance (`obj." + name + "(...)`)").c_str(), call->line);
        }
        auto fit = _funcs.find(resolveFunc(name, qual));
        if (fit != _funcs.end())
            return placeWrap(emitReorderedCall(fit->second.cName, "", fit->second.params, call->args, call->line),
                             fit->second.isPlaceReturn);
        unsupported("scope-qualified call resolves to no known function", call->line);
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
    return placeWrap(emitReorderedCall(it->second.cName, "", it->second.params, call->args, call->line),
                     it->second.isPlaceReturn);
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
    // A retroactively-conformed PRIMITIVE (`implements Hashable for int32`) takes `this` (= `self`) as the
    // SCALAR by value — `int32_t self`, not `int32_t* self`.
    if (selfType) { s += std::string(selfType) + (_primConformances.count(selfType) ? " self" : "* self"); first = false; }
    if (params) {
        for (auto& p : *params) {
            if (!first) s += ", ";
            first = false;
            std::string nm = (p->identifier && p->identifier->value) ? *p->identifier->value : "";
            // `const Ptr<T>`/`const Ptr` emits `const T*`/`const void*` (FFI const
            // pointers — to match C const callback/API signatures). Only pointer types:
            // a `const ref <class>` stays plain (its methods take a non-const `self`).
            bool constPtr = p->isConst && p->type && p->type->value && *p->type->value == "Ptr";
            // a `ref`/`const ref` parameter may not name a smart pointer — you borrow
            // the OBJECT (`ref T`), or transfer ownership by value (`give`/`copy`). Borrowing
            // the handle never makes sense (and would make `ref p` ambiguous). `out` producing
            // a handle (tryUpgrade / factory-out) stays legal.
            std::string pct = cType(p->type);
            std::string libElem = heapOwnerTarget(pct);   // library Owned/Shared pointee ("" otherwise)
            if (paramByRef(p.get()) && p->modifier && p->modifier->value && *p->modifier->value == "ref"
                && (isSmartPtrClass(pct) || !libElem.empty()))
                unsupported(("a `ref` parameter may not name a smart pointer ('" + pct
                             + "') — borrow the object with `ref "
                             + (libElem.empty() ? _classes[pct].collElemClass : libElem)
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
    // a place-returning `fn ref T f(…)` emits `T* f(…)` (the place); its `return e` addresses it.
    *_out << (nameOverride ? "static " : "") << cType(fn->returnType) << (fn->isRef ? "*" : "") << " " << name
          << "(" << paramListC(fn->parameters, nullptr) << ");\n";
}

void CEmitter::emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride)
{
    bool isEntry = false;
    std::string name = nameOverride ? *nameOverride : mangledFunctionName(fn, isEntry);

    // Track by-ref params (deref on read) and param classes (for member calls).
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _moveState.clear();   // per-function move analysis
    _pendingParamDtors.clear();
    _currentClass = nullptr;
    _currentFunc  = name;   // a free function may be a `friend` accessor
    if (fn->parameters) {
        for (auto& p : *fn->parameters) {
            if (!p->identifier || !p->identifier->value) continue;
            const std::string& pn = *p->identifier->value;
            if (paramByRef(p.get())) _refParams.insert(pn);
            if (p->isConst) _constLocals.insert(pn);   // const param is immutable
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = (isClass(pty) || isInterface(pty) || isSigType(pty)) ? pty : "";   // record (incl. fnptr params)
            _localCTypes[pn] = pty;                    // full C type (incl. enums/primitives) — e.g. a plain-enum `match` subject
            // a by-value smart-ptr OR owning collection/`string` param is OWNED by the callee — drop it at
            // fn-end (a `ref` param is a borrow — never). The function-root scope is created later
            // (emitBlockScoped); stash it there. Sound because the arg path now forces the caller to
            // `give`/`copy` a named owned collection/string (it can't pass a live-owned one bare).
            if (!paramByRef(p.get()) && (isSmartPtrClass(pty) || ownsByValue(pty))) {
                _pendingParamDtors.push_back({pn, pty});
                if (ownsByValue(pty)) _moveState[pn] = MoveState::NotMoved;   // track move-only / collection / string param
            }
        }
    }

    _currentReturnCType = cType(fn->returnType);
    _tempCounter = 0;
    _scopes.clear();

    line(fn->line);
    // a place-returning `fn ref T f(…)` emits `T* f(…)`; its `return e` addresses the place (the
    // ReturnNode path, gated on `_returnIsPlace`) — same as a `fn ref T` method. The escape check
    // there requires the place to borrow a `ref`/`out` param (a free fn has no `this`), so it can't
    // dangle. `_currentReturnCType` stays the base `T` (the place path never consults it).
    *_out << (nameOverride ? "static " : "") << cType(fn->returnType) << (fn->isRef ? "*" : "") << " " << name
          << "(" << paramListC(fn->parameters, nullptr) << ")\n";

    _returnIsPlace = fn->isRef;
    if (fn->block) {
        emitBlockScoped(fn->block.get(), 0, /*loopBoundary=*/false, /*functionRoot=*/true);
    } else {
        *_out << "{\n}";
    }
    _returnIsPlace = false;
    *_out << "\n\n";

    _refParams.clear();

    if (isEntry) {
        // Synthesized portable entry point. Argument marshaling (List<String>)
        // arrives once collections land; for now args are ignored.
        *_out << "int main(int argc, char** argv) {\n"
             << "    (void)argc; (void)argv;\n"
             << "    return (int)kama_main();\n"
             << "}\n\n";
    }
}

// ---------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------

void CEmitter::emitStruct(ClassInfo& ci)
{
    ScopedStr _ts(_thisType, ci.name);   // `This` -> this class while emitting its struct
    // a tagged union — a discriminant tag + a union of per-variant payloads.
    if (ci.isVariant) { emitVariantStruct(ci); return; }
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

// `struct Name { <tag> tag; union { struct {…} <Variant>; … } u; };` — a discriminated union.
// The tag enum carries symbolic case constants (`Name_Circle`); only payload-carrying variants
// contribute a union member. `: IntType` pins the tag field to a fixed-width integer.
void CEmitter::emitVariantStruct(ClassInfo& ci)
{
    std::string tagTy = ci.tagCType.empty() ? (ci.name + "_Tag") : ci.tagCType;
    // Tag constants: a named `Name_Tag` enum by default, else an anonymous enum (the struct tag
    // field is the pinned integer type, but the constants are still needed for `switch` labels).
    if (ci.tagCType.empty()) *_out << "typedef enum " << ci.name << "_Tag {\n";
    else                     *_out << "enum {\n";
    for (auto& v : ci.variants) { indent(1); *_out << ci.name << "_" << v.name << ",\n"; }
    if (ci.tagCType.empty()) *_out << "} " << ci.name << "_Tag;\n";
    else                     *_out << "};\n";

    *_out << "struct " << ci.name << " {\n";
    indent(1); *_out << tagTy << " tag;\n";
    bool anyPayload = false;
    for (auto& v : ci.variants) if (!v.payload.empty()) { anyPayload = true; break; }
    if (anyPayload) {
        indent(1); *_out << "union {\n";
        for (auto& v : ci.variants) {
            if (v.payload.empty()) continue;
            indent(2); *_out << "struct {\n";
            for (auto& f : v.payload) {
                // A by-value user value/resource payload is legal — the unified struct order
                // lays out the payload type first; a self/mutual by-value cycle is caught (infinite size)
                // by unifiedStructOrder.
                indent(3); *_out << cType(f.type) << " " << f.name << ";\n";
            }
            indent(2); *_out << "} " << v.name << ";\n";
        }
        indent(1); *_out << "} u;\n";
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
    // Virtual-destructor slot: destroying a derived through a base handle (`Shared<Base>`
    // owning a `Derived`) dispatches here, so the MOST-DERIVED dtor runs (no slicing). NULL
    // for a non-destructible impl — a base whose derived owns nothing.
    indent(1); *_out << "void (*__dtor)(void*);\n";
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
    // this class's own destructor drives polymorphic drop (`__vdrop`); NULL if it frees nothing.
    if (ci.destructible)
        *_out << "    .__dtor = (void(*)(void*))&" << ci.name << "__dtor,\n";
    *_out << "};\n\n";
    // Virtual drop: read the runtime vtable off the object's vptr and call its `__dtor`. A base
    // handle (`Shared<Base>`) drops through this so a Derived's full chain runs even though the
    // static type is Base. The vptr already names the most-derived vtable (set at construction).
    *_out << "static inline void " << ci.name << "__vdrop(" << ci.name << "* self) {\n";
    indent(1); *_out << "const " << ci.vtableRoot << "_vtable* __vt = self->" << vptrPrefix(&ci) << "__vptr;\n";
    indent(1); *_out << "if (__vt && __vt->__dtor) __vt->__dtor(self);\n";
    *_out << "}\n\n";
}

// ---- Interfaces -----------------------------------------------------

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
CEmitter::ContractSubst::ContractSubst(CEmitter& e_, const InterfaceInfo& ii)
    : e(e_), savedCtx(e_._nsCtx), savedSubst(e_._typeSubst), active(ii.isGenericInst)
{
    if (!active) return;
    // emit under the USE-SITE ctx (where the instance's type args — e.g. a user `Point` — resolve),
    // falling back to the template's home ctx. Mirrors emitGenericTypeInst's _genericTypeInstCtx.
    e._nsCtx = e._genericContractInstCtx.count(ii.name) ? e._genericContractInstCtx[ii.name]
             : e._genericContractCtx.count(ii.templateKey) ? e._genericContractCtx[ii.templateKey] : e._nsCtx;
    e._typeSubst.clear();
    const std::vector<std::string>& ps = e._genericContractParams[ii.templateKey];
    for (size_t i = 0; i < ps.size() && i < ii.typeArgs.size(); ++i) e._typeSubst[ps[i]] = ii.typeArgs[i];
}
CEmitter::ContractSubst::~ContractSubst()
{
    if (!active) return;
    e._typeSubst = savedSubst;
    e._nsCtx = savedCtx;
}

void CEmitter::emitInterfaceTypes(InterfaceInfo& ii)
{
    // in the type-erased vtbl slot, `This` is the interface type itself (a contract's `This`-typed
    // method is dispatched STATICALLY via a bound; the vtbl slot is dead for that use but must be valid C).
    ScopedStr _ts(_thisType, ii.name);
    ContractSubst _cs(*this, ii);   // bind T->int32 for a generic-contract instance (`Iterator_int32`)
    *_out << "struct " << ii.name << "_vtbl {\n";
    for (auto& m : ii.methods) {
        indent(1);
        // a place-returning contract method (`fn ref T m()`) is lowered to a `T*`-returning slot.
        *_out << cType(m.returnType) << (m.isPlaceReturn ? "*" : "") << " (*" << m.name << ")"
              << ifaceSlotSig(m.params) << ";\n";
    }
    // a virtual-destructor slot so an OWNED interface (`Owned`/`Shared<I>`) can drop its
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
        // A retroactively-implemented contract dispatches statically (monomorphized) — no fat-pointer vtable.
        bool retro = false;
        for (auto& r : ci.retroInterfaces) if (r == ifn) { retro = true; break; }
        if (retro) continue;
        auto it = _interfaces.find(ifn);
        if (it == _interfaces.end()) { unsupported("unknown contract in implements", ci.node->line); continue; }
        InterfaceInfo& ii = it->second;
        // the slot casts must MATCH the vtbl struct's slot types EXACTLY, so render the contract's method
        // sigs under the CONTRACT's own name-resolution scope (its imports) — not the implementing unit's.
        // A non-generic contract carries that scope in `ii`; a generic-contract instance gets it (with T
        // bound) from ContractSubst below, so only reseat here for the non-generic case.
        NsCtx _savedNs = _nsCtx;
        if (!ii.isGenericInst) { _nsCtx.scope = ii.scope; _nsCtx.usings = ii.usings; _nsCtx.symbolAliases = ii.symbolAliases; }
        // the slot casts must match the vtbl struct's erased signature -> `This` = the interface.
        ScopedStr _ts(_thisType, ii.name);
        ContractSubst _cs(*this, ii);   // bind T->int32 so a generic-contract slot's sig matches its vtbl
        // EXTERNAL linkage (not `static`) + a forward decl in the shared header, so a value can be bound to
        // this contract across module boundaries (e.g. a generic `json::parse<T>`/`toString<T>` in one unit
        // instantiated with a type whose vtable is defined in another). Defined once, in the owning unit.
        // A generic-type INSTANCE (`List<int32>`) is header-inline (`_emitStaticClass`), so ITS vtable is
        // `static` — defined in every TU that includes the header, no extern decl (see the skip above).
        *_out << (_emitStaticClass ? "static const " : "const ")
              << ii.name << "_vtbl " << ci.name << "__as_" << ii.name << " = {\n";
        for (auto& m : ii.methods) {
            ClassInfo* owner = nullptr;
            MethodInfo* mi = findMethod(&ci, m.name, &owner);
            if (!mi) { unsupported(("class missing contract method '" + m.name + "'").c_str(), ci.node->line); continue; }
            // an interface is a PUBLIC contract — a method that satisfies it must be
            // public too (else it's reachable through the interface but not by name: a leak).
            if (mi->visibility != Visibility::Public)
                unsupported(("method '" + m.name + "' implements contract '" + ii.name
                             + "' and must be declared `public`").c_str(),
                            mi->node ? mi->node->line : ci.node->line);
            indent(1);
            *_out << "." << m.name << " = (" << cType(m.returnType) << (m.isPlaceReturn ? "*" : "")
                 << "(*)" << ifaceSlotSig(m.params) << ")&" << mi->cName << ",\n";
        }
        // the virtual-destructor slot — the concrete dtor (cast to the erased signature),
        // or NULL when this impl owns nothing to free.
        indent(1);
        if (ci.destructible) *_out << ".__dtor = (void(*)(void*))&" << ci.name << "__dtor,\n";
        else                 *_out << ".__dtor = (void(*)(void*))0,\n";
        *_out << "};\n\n";
        _nsCtx = _savedNs;   // restore (the non-generic reseat above; ContractSubst restores its own)
    }
}

// (I){ (void*)&(lvalue), &C__as_I } — wrap a concrete class lvalue as interface I.
std::string CEmitter::fatPointer(const std::string& iface, const std::string& concrete, const std::string& lvalue)
{
    return "(" + iface + "){ (void*)&(" + lvalue + "), &" + concrete + "__as_" + iface + " }";
}

// True for a bare C identifier (a local/param) — cheap and side-effect-free to read more than
// once, so it needs no hoisting. Anything with a call/index/member/deref is not "simple".
static bool isSimpleIdent(const std::string& s)
{
    if (s.empty()) return false;
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

// s.m(args) where s is an interface value -> (s).vtbl->m((s).obj, <reordered args>).
// A fat-pointer dispatch reads the receiver twice (vtbl + obj), so a non-trivial receiver (a
// bounds-checked index, a call) would run twice — hoist it into one temp (perf + side-effect
// safety). Needs a hoistable statement context; a bare identifier is free to re-read as-is.
std::string CEmitter::emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                            const std::string& method, SharedArgumentList args, int srcLine,
                                            const std::string& recvCType)
{
    auto it = _interfaces.find(iface);
    if (it == _interfaces.end()) { unsupported("dispatch on unknown contract", srcLine); return "0"; }
    std::string recv = fatExpr;
    if (_hoistOK && !isSimpleIdent(fatExpr)) {
        // The receiver's own C type — the plain interface `iface`, or a smart-ptr-of-interface
        // (`Owned/Shared<I>`) when called through a handle; all expose `.obj`/`.vtbl`.
        std::string ty = recvCType.empty() ? iface : recvCType;
        std::string t = "__ifacerecv" + std::to_string(_tempCounter++);
        _hoisted.push_back(ty + " " + t + " = " + fatExpr + ";");
        recv = t;
    }
    for (auto& m : it->second.methods) {
        if (m.name != method) continue;
        std::vector<ParamSig> params = paramSigsOf(m.params);
        return emitReorderedCall("(" + recv + ").vtbl->" + method, "(" + recv + ").obj",
                                 params, args, srcLine);
    }
    unsupported("unknown contract method", srcLine);
    return "0";
}

void CEmitter::emitClassPrototypes(ClassInfo& ci)
{
    if (ci.isIntrinsicColl || ci.isExternStruct) return;   // macro / header provides these
    ScopedStr _ts(_thisType, ci.name);                  // `This` -> this class in method prototypes
    const char* stat = _emitStaticClass ? "static inline " : "";   // specialized instances are header-static inline
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator)
        *_out << stat << "void " << ci.name << "__ctor("
             << paramListC(ci.ctorNode->declarator->params, ci.name.c_str()) << ");\n";
    else if (ci.synthCtor)                                // synthesized default ctor
        *_out << stat << "void " << ci.name << "__ctor(" << paramListC(nullptr, ci.name.c_str()) << ");\n";
    if (ci.destructible)
        *_out << stat << "void " << ci.name << "__dtor(" << ci.name << "* self);\n";
    if (ci.hasVtable)   // polymorphic drop dispatcher (defined with the vtable instance)
        *_out << "static inline void " << ci.name << "__vdrop(" << ci.name << "* self);\n";
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        if (mi.isAbstract) continue;   // pure: no definition, no prototype
        if (mi.isSynthSer) { *_out << stat << "void " << ci.name << "__serialize(" << ci.name << "* self, Serializer* w);\n"; continue; }
        if (mi.isSynthDe)  { *_out << stat << cType(mi.returnType) << " " << ci.name << "__deserialize(Deserializer r);\n"; continue; }   // graph: Shared<T>; by-value: T
        rejectStoredInterface(mi.returnType, "returned from a method",
                              mi.node ? mi.node->line : (ci.node ? ci.node->line : 0));
        // an operator has no `node`; emit its prototype from `opDecl` (free form: no self).
        SharedParameterList plist = mi.isOperator ? operatorParamList(mi.opDecl->operatorDeclarator.get())
                                                  : mi.node->params;
        // a place-returning `ref T operator[]` returns a `T*` (the place); everything else by value.
        std::string retC = cType(mi.returnType) + (mi.isPlaceReturn ? "*" : "");
        *_out << stat << retC << " " << mi.cName << "("
             << paramListC(plist, mi.isStatic ? nullptr : ci.name.c_str()) << ");\n";   // static/free: no self
    }
    if (ci.isGraphNode)
        emitGraphNodeHelperProtos(ci);   // graph node-helper prototypes (Phase D)
}

// void Name__dtor(Name* self): user body first, then destructible fields in
// reverse declaration order. (Early return inside a dtor body is unsupported.)
void CEmitter::emitDtorDefinition(ClassInfo& ci)
{
    line(ci.dtorNode ? ci.dtorNode->line : (ci.node ? ci.node->line : 0));
    _currentClass = &ci;
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _currentReturnCType = "void";
    _tempCounter = 0;
    _scopes.clear();
    Scope root; root.isFunctionRoot = true;
    _scopes.push_back(root);

    *_out << (_emitStaticClass ? "static inline " : "") << "void " << ci.name << "__dtor(" << ci.name << "* self)\n{\n";

    // a discriminated union drops ONLY the active variant's owning payload fields (switch on tag).
    if (ci.isVariant) {
        indent(1); *_out << "switch (self->tag) {\n";
        for (auto& v : ci.variants) {
            bool anyDrop = false;
            for (auto& f : v.payload) {
                auto cit = _classes.find(cType(f.type));
                if (cit != _classes.end() && cit->second.destructible) { anyDrop = true; break; }
            }
            if (!anyDrop) continue;
            indent(2); *_out << "case " << ci.name << "_" << v.name << ":\n";
            for (auto it = v.payload.rbegin(); it != v.payload.rend(); ++it) {   // reverse declaration order
                auto cit = _classes.find(cType(it->type));
                if (cit != _classes.end() && cit->second.destructible) {
                    indent(3);
                    *_out << cit->second.name << "__dtor(&self->u." << v.name << "." << it->name << ");\n";
                }
            }
            indent(3); *_out << "break;\n";
        }
        indent(2); *_out << "default: break;\n";
        indent(1); *_out << "}\n";
        *_out << "}\n\n";
        _scopes.clear();
        _currentClass = nullptr;
        return;
    }

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
                                    ClassInfo& owner, bool isCtor, bool isConstMethod, bool isStatic)
{
    _currentClass = &owner;
    _currentFunc  = cName;   // a method may be a `Class::method` friend accessor
    _inStaticMethod = isStatic;   // a static body has no `self`/`this`
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _moveState.clear();   // per-method move analysis
    _inCtor = isCtor;   // const fields are writable only here
    if (isConstMethod) _constLocals.insert("this");   // `this` is immutable (deep)
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
            if (p->isConst) _constLocals.insert(pn);   // const param is immutable
            std::string pty = p->type ? cType(p->type) : "";
            _localTypes[pn] = (isClass(pty) || isInterface(pty) || isSigType(pty)) ? pty : "";   // record (incl. fnptr params)
            _localCTypes[pn] = pty;                    // full C type (incl. enums/primitives) — e.g. a plain-enum `match` subject
            // a by-value smart-ptr OR owning collection/`string` param is owned by the callee — drop it at
            // fn-end (a `ref` is a borrow — never). The root scope is already on the stack, so record it
            // directly (dropped last). recordDestructibleLocal also move-tracks an ownsByValue param.
            if (!paramByRef(p.get()) && (isSmartPtrClass(pty) || ownsByValue(pty))) recordDestructibleLocal(pn, pty);
        }
    }

    *_out << (_emitStaticClass ? "static inline " : "") << retType << " " << cName
          << "(" << paramListC(params, isStatic ? nullptr : owner.name.c_str()) << ")\n{\n";   // static: no self

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
                // (`.add(...)`) and its RAII drop would touch garbage. This also covers a library
                // owner (Owned/Shared/Weak) held as a field: the first `this.f = x` RELEASES the old
                // value, which must be a null (no-op) handle — the library dtors guard a null pointer.
                // (Plain class-value fields still need their own ctor — a separate, deferred gap.)
                std::string fct = cType(f.type);
                // Also a destructible `resource` field (e.g. a `Map`/`Set` member, or any user resource
                // that owns a buffer): zero it so the FIRST `this.f = give x` releases a valid empty value
                // (cap=0 / NULL → its dtor is a no-op), not uninitialized garbage — a garbage free is UB
                // that aborts only when the stack happens to be non-null.
                if (_classes.count(fct) && (_classes[fct].isIntrinsicColl
                        || _classes[fct].copyable || !heapOwnerTarget(fct).empty()
                        || (_classes[fct].destructible && _classes[fct].kind == TypeKind::Resource))) {
                    indent(1);
                    *_out << "self->" << f.name << " = (" << fct << "){0};\n";
                }
            }
        }
    }
    // Stage 1: never-null — every `Owned`/`Shared` field must be assigned by ctor-end and not read before.
    if (isCtor) checkCtorNeverNull(owner, body);
    SharedStatement last;
    if (body && body->statements) {
        for (auto& st : *body->statements) { emitStatement(st, 1); last = st; }
    }
    if (!(last && stmtIsJump(last))) {
        // `onConstruction()` lifecycle hook: for a `@generate(Deserialize)` type that defines it, the compiler
        // calls it at the END of every constructor (and the deserialize codegen calls it after field-set), so
        // "runs on every construction" logic lives in one place despite deserialize bypassing the ctor.
        if (isCtor && owner.genDeserialize && owner.methods.count("onConstruction")) {
            indent(1);
            *_out << owner.name << "__onConstruction(self);\n";
        }
        emitScopeCleanup(_scopes.back(), 1);
    }
    *_out << "}\n\n";

    _scopes.clear();
    _currentClass = nullptr;
    _refParams.clear();
    _localTypes.clear(); _constLocals.clear(); _inCtor = false;
    _inStaticMethod = false;
}

void CEmitter::emitClassDefinitions(ClassInfo& ci)
{
    if (ci.isIntrinsicColl || ci.isExternStruct) return;   // macro / header provides these
    ScopedStr _ts(_thisType, ci.name);                  // `This` -> this class in method bodies/sigs
    if (ci.hasCtor && ci.ctorNode && ci.ctorNode->declarator) {
        line(ci.ctorNode->line);
        emitMethodOrCtorBody(ci.name + "__ctor", "void",
                             ci.ctorNode->declarator->params, ci.ctorNode->body, ci, true);
    } else if (ci.synthCtor) {                            // emit the synthesized default ctor
        emitMethodOrCtorBody(ci.name + "__ctor", "void",
                             SharedParameterList(), SharedBlock(), ci, true);
    }
    for (auto& kv : ci.methods) {
        MethodInfo& mi = kv.second;
        if (mi.isAbstract) continue;   // pure: no body to emit
        if (mi.isSynthSer) { ci.reachesPointer   ? emitGraphSerializeDefinition(ci)   : emitSerializeDefinition(ci); continue; }
        if (mi.isSynthDe)  { ci.graphDeserialize ? emitGraphDeserializeDefinition(ci) : emitDeserializeDefinition(ci); continue; }
        // an operator has no `node`; emit its body from `opDecl` (free form: no self).
        if (mi.isOperator) {
            auto* d = mi.opDecl->operatorDeclarator.get();
            line(mi.opDecl->line);
            // a place-returning `ref T operator[]` emits `T* Class__op_index(Class* self, …)`; its
            // `return e` addresses the place (see the ReturnNode path, gated on `_returnIsPlace`).
            std::string ret = cType(mi.returnType) + (mi.isPlaceReturn ? "*" : "");
            _returnIsPlace = mi.isPlaceReturn;
            emitMethodOrCtorBody(mi.cName, ret.c_str(), operatorParamList(d), mi.opDecl->body, ci, false, false, mi.arity == 2);
            _returnIsPlace = false;
            continue;
        }
        line(mi.node->line);
        // a place-returning `fn ref T m(…)` emits `T* Class__m(Class* self, …)`; its `return e`
        // addresses the place (the ReturnNode path, gated on `_returnIsPlace`) — same as `operator[]`.
        std::string ret = cType(mi.returnType) + (mi.isPlaceReturn ? "*" : "");
        _returnIsPlace = mi.isPlaceReturn;
        emitMethodOrCtorBody(mi.cName, ret.c_str(), mi.node->params, mi.node->body, ci, false, mi.isConst, mi.isStatic);
        _returnIsPlace = false;
    }
    if (ci.destructible)
        emitDtorDefinition(ci);
    if (ci.isGraphNode)
        emitGraphNodeHelpers(ci);   // T__serializeNode / T__allocShell / T__wireShell bodies (Phase D)
}

// ---- By-value (tree) serialization intrinsic (Phase C) --------------------
// Direct C emission of `serialize`/`deserialize` for a `@generate` product (value/tree) type, replacing
// the generated-kama path. Reproduces the exact JSON wire the old synthesis produced. Delegation: a
// scalar/string writes/reads via the Serializer/Deserializer contract directly; a nested struct / enum /
// collection field calls its own `<CType>__serialize`/`__deserialize` (emitted per-instance elsewhere);
// `Optional<E>` is inlined. (`Fixed<T,N>` is unused by any `@generate` type today — deferred.)

// A `kama_string_lit("…", n)` for a wire name (same escaping the StringNode lowering uses).
static std::string kamaStrLit(const std::string& s)
{
    std::ostringstream os;
    os << "kama_string_lit(\"";
    for (char c : s)
        switch (c) {
            case '\\': os << "\\\\"; break;  case '"': os << "\\\""; break;
            case '\n': os << "\\n";  break;  case '\t': os << "\\t"; break;
            case '\r': os << "\\r";  break;  default:  os << c;      break;
        }
    os << "\", " << s.size() << ")";
    return os.str();
}

// The Serializer/Deserializer scalar method suffix for a builtin ("I32"/"U8"/"F64"/"Bool"/"Char"), "" otherwise.
static const char* serScalarSuffix(int builtInVal)
{
    switch (builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "I8";
        case IDENTIFIER_INT16_VAL:   return "I16";
        case IDENTIFIER_INT32_VAL:   return "I32";
        case IDENTIFIER_INT64_VAL:   return "I64";
        case IDENTIFIER_UINT8_VAL:   return "U8";
        case IDENTIFIER_UINT16_VAL:  return "U16";
        case IDENTIFIER_UINT32_VAL:  return "U32";
        case IDENTIFIER_UINT64_VAL:  return "U64";
        case IDENTIFIER_FLOAT32_VAL: return "F32";
        case IDENTIFIER_FLOAT64_VAL: return "F64";
        case IDENTIFIER_BOOL_VAL:    return "Bool";
        case IDENTIFIER_CHAR_VAL:    return "Char";
        default:                     return "";
    }
}

// Emit the write for one field value (`access`) into the Serializer `w` (a `Serializer*`).
void CEmitter::emitSerFieldWrite(SharedIdentifier ty, const std::string& access, int depth)
{
    if (ty && ty->value && *ty->value == "Optional" && ty->genericArg) {   // Some -> inner value, None -> null
        std::string oc = cType(ty);
        indent(depth); *_out << "switch ((" << access << ").tag) {\n";
        indent(depth); *_out << "case " << oc << "_Some: {\n";
        emitSerFieldWrite(ty->genericArg, "(" + access + ").u.Some.value", depth + 1);
        indent(depth + 1); *_out << "break;\n";
        indent(depth); *_out << "}\n";
        indent(depth); *_out << "case " << oc << "_None: { w->vtbl->writeNull(w->obj); break; }\n";
        indent(depth); *_out << "default: break;\n";
        indent(depth); *_out << "}\n";
        return;
    }
    if (ty && ty->builtInVal == IDENTIFIER_STRING_VAL) {
        indent(depth); *_out << "w->vtbl->writeString(w->obj, &(" << access << "));\n";
        return;
    }
    const char* suf = serScalarSuffix(ty ? ty->builtInVal : 0);
    if (*suf) {
        indent(depth); *_out << "w->vtbl->write" << suf << "(w->obj, " << access << ");\n";
        return;
    }
    // enum / nested struct / collection -> its own serialize (self by pointer, Serializer* through).
    indent(depth); *_out << cType(ty) << "__serialize(&(" << access << "), w);\n";
}

void CEmitter::emitSerializeDefinition(ClassInfo& ci)
{
    *_out << (_emitStaticClass ? "static inline " : "") << "void " << ci.name
          << "__serialize(" << ci.name << "* self, Serializer* w)\n{\n";
    indent(1); *_out << "w->vtbl->beginObject(w->obj);\n";
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        const std::string& wire = f.serName.empty() ? f.name : f.serName;
        indent(1); *_out << "w->vtbl->fieldName(w->obj, " << kamaStrLit(wire) << ");\n";
        emitSerFieldWrite(f.type, "self->" + f.name, 1);
    }
    indent(1); *_out << "w->vtbl->endObject(w->obj);\n";
    *_out << "}\n\n";
}

// The `Deserializer r` read EXPRESSION for a field type (scalar/string direct, else `<CType>__deserialize`).
std::string CEmitter::deReadExpr(SharedIdentifier ty)
{
    if (ty && ty->builtInVal == IDENTIFIER_STRING_VAL) return "r.vtbl->readString(r.obj)";
    const char* suf = serScalarSuffix(ty ? ty->builtInVal : 0);
    if (*suf) return std::string("r.vtbl->read") + suf + "(r.obj)";
    return cType(ty) + "__deserialize(r)";
}

// Emit the read of one field into `dst` from the Deserializer `r` (a `Deserializer` value).
void CEmitter::emitDeFieldRead(SharedIdentifier ty, const std::string& dst, int depth)
{
    if (ty && ty->value && *ty->value == "Optional" && ty->genericArg) {
        std::string oc = cType(ty);
        indent(depth); *_out << "if (r.vtbl->readNull(r.obj)) { " << dst << " = (" << oc << "){ .tag = " << oc << "_None }; }\n";
        indent(depth); *_out << "else { " << dst << " = (" << oc << "){ .tag = " << oc << "_Some, .u.Some = { .value = "
                             << deReadExpr(ty->genericArg) << " } }; }\n";
        return;
    }
    indent(depth); *_out << dst << " = " << deReadExpr(ty) << ";\n";
}

void CEmitter::emitDeserializeDefinition(ClassInfo& ci)
{
    *_out << (_emitStaticClass ? "static inline " : "") << ci.name << " " << ci.name
          << "__deserialize(Deserializer r)\n{\n";
    indent(1); *_out << ci.name << " result = (" << ci.name << "){0};\n";   // bypass-ctor zero-init
    // Zero-init makes an Optional field `Some(zeroed)` (tag 0) — reset every one to None first.
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        if (f.type && f.type->value && *f.type->value == "Optional") {
            std::string oc = cType(f.type);
            indent(1); *_out << "result." << f.name << " = (" << oc << "){ .tag = " << oc << "_None };\n";
        }
    }
    indent(1); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(1); *_out << "while (r.vtbl->moreFields(r.obj)) {\n";
    indent(2); *_out << "kama_string __key = r.vtbl->fieldName(r.obj);\n";
    bool first = true;
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        const std::string& wire = f.serName.empty() ? f.name : f.serName;
        indent(2); *_out << (first ? "if" : "else if") << " (kama_string__equals(&__key, " << kamaStrLit(wire) << ")) {\n";
        emitDeFieldRead(f.type, "result." + f.name, 3);
        indent(2); *_out << "}\n";
        first = false;
    }
    indent(2); *_out << (first ? "" : "else ") << "{ r.vtbl->skipValue(r.obj); }\n";
    indent(2); *_out << "kama_string__dtor(&__key);\n";   // the field-name string is owned — free each iteration
    indent(1); *_out << "}\n";
    if (!ci.serNoOnConstruction && ci.methods.count("onConstruction")) {
        indent(1); *_out << ci.name << "__onConstruction(&result);\n";   // hook runs after a bypass-ctor field-set
    }
    indent(1); *_out << "return result;\n";   // by-value return moves a resource out (no dtor on `result`)
    *_out << "}\n\n";
}

// Externally-tagged enum serialize: `{"tag":"V"}` (unit) / `{"tag":"V","value":{fields…}}` (payload).
void CEmitter::emitEnumSerializeDefinition(ClassInfo& ci)
{
    *_out << "void " << ci.name << "__serialize(" << ci.name << "* self, Serializer* w)\n{\n";
    indent(1); *_out << "w->vtbl->beginObject(w->obj);\n";
    indent(1); *_out << "switch (self->tag) {\n";
    for (auto& v : ci.variants) {
        indent(1); *_out << "case " << ci.name << "_" << v.name << ": {\n";
        indent(2); *_out << "w->vtbl->fieldName(w->obj, " << kamaStrLit("tag") << ");\n";
        indent(2); *_out << "kama_string __tv = " << kamaStrLit(v.name) << "; w->vtbl->writeString(w->obj, &__tv);\n";
        if (!v.payload.empty()) {
            indent(2); *_out << "w->vtbl->fieldName(w->obj, " << kamaStrLit("value") << ");\n";
            indent(2); *_out << "w->vtbl->beginObject(w->obj);\n";
            for (auto& f : v.payload) {
                indent(2); *_out << "w->vtbl->fieldName(w->obj, " << kamaStrLit(f.name) << ");\n";
                emitSerFieldWrite(f.type, "self->u." + v.name + "." + f.name, 2);
            }
            indent(2); *_out << "w->vtbl->endObject(w->obj);\n";
        }
        indent(2); *_out << "break;\n";
        indent(1); *_out << "}\n";
    }
    indent(1); *_out << "default: break;\n";
    indent(1); *_out << "}\n";
    indent(1); *_out << "w->vtbl->endObject(w->obj);\n";
    *_out << "}\n\n";
}

// Enum deserialize: read `tag`, dispatch, read the `value` object positionally, construct the variant. An
// unknown tag flags the reader (`fail`) and returns a benign payload-less variant (the guaranteed fallback).
void CEmitter::emitEnumDeserializeDefinition(ClassInfo& ci)
{
    std::string dflt;
    for (auto& v : ci.variants) if (v.payload.empty()) { dflt = v.name; break; }
    *_out << ci.name << " " << ci.name << "__deserialize(Deserializer r)\n{\n";
    indent(1); *_out << ci.name << " __result = (" << ci.name << "){0};\n";
    indent(1); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(1); *_out << "r.vtbl->moreFields(r.obj);\n";
    indent(1); *_out << "kama_string __k = r.vtbl->fieldName(r.obj); kama_string__dtor(&__k);\n";   // the "tag" key
    indent(1); *_out << "kama_string __tag = r.vtbl->readString(r.obj);\n";
    bool first = true;
    for (auto& v : ci.variants) {
        indent(1); *_out << (first ? "if" : "else if") << " (kama_string__equals(&__tag, " << kamaStrLit(v.name) << ")) {\n";
        if (!v.payload.empty()) {
            indent(2); *_out << "r.vtbl->moreFields(r.obj);\n";
            indent(2); *_out << "kama_string __kv = r.vtbl->fieldName(r.obj); kama_string__dtor(&__kv);\n";   // the "value" key
            indent(2); *_out << "r.vtbl->beginObject(r.obj);\n";
            for (size_t i = 0; i < v.payload.size(); ++i) {
                const FieldInfo& f = v.payload[i];
                std::string idx = std::to_string(i);
                indent(2); *_out << "r.vtbl->moreFields(r.obj);\n";
                indent(2); *_out << "kama_string __pk" << idx << " = r.vtbl->fieldName(r.obj); kama_string__dtor(&__pk" << idx << ");\n";
                indent(2); *_out << cType(f.type) << " __p_" << f.name << " = " << deReadExpr(f.type) << ";\n";
            }
            indent(2); *_out << "r.vtbl->moreFields(r.obj);\n";   // close the value object
            indent(2); *_out << "r.vtbl->moreFields(r.obj);\n";   // close the outer object
            indent(2); *_out << "__result = (" << ci.name << "){ .tag = " << ci.name << "_" << v.name << ", .u." << v.name << " = { ";
            for (size_t i = 0; i < v.payload.size(); ++i) { if (i) *_out << ", "; *_out << "." << v.payload[i].name << " = __p_" << v.payload[i].name; }
            *_out << " } };\n";
        } else {
            indent(2); *_out << "r.vtbl->moreFields(r.obj);\n";   // close the outer object
            indent(2); *_out << "__result = (" << ci.name << "){ .tag = " << ci.name << "_" << v.name << " };\n";
        }
        indent(1); *_out << "}\n";
        first = false;
    }
    indent(1); *_out << "else { r.vtbl->fail(r.obj); __result = (" << ci.name << "){ .tag = " << ci.name << "_" << dflt << " }; }\n";
    indent(1); *_out << "kama_string__dtor(&__tag);\n";
    indent(1); *_out << "return __result;\n";
    *_out << "}\n\n";
}

// ---- Graph (object-graph / pointer) serialization intrinsic (Phase D) ------
// A `@generate` type that transitively reaches a Shared/Weak/Owned pointer serializes as an object graph:
// every distinct object gets a stable id, pointer fields become ids, and objects are written into a flat
// `{"root":id,"objects":{id:{"__type":T,…}}}` id table. This replaces the generated-kama
// `std::serialization::graph` module + driver synthesis, lowering the id table / two-pass rebuild / ownership
// transfer straight to C over `kama_ser_graph` / `kama_de_graph` (kama_runtime.h). Reproduces the exact wire.

// One field's graph-pointer classification. kind is "" for a non-pointer field (scalar/string/nested/
// collection — those flow through the by-value helpers), else "Shared"/"Weak"/"Owned" with elemC the pointee's
// C type. `optional` = the edge is wrapped in `Optional<…>` (nullable, e.g. a list tail / cycle bootstrap).
CEmitter::GraphEdge CEmitter::graphEdgeOf(SharedIdentifier ty)
{
    GraphEdge e;
    if (!ty || !ty->value) return e;
    bool opt = (*ty->value == "Optional" && ty->genericArg);
    SharedIdentifier inner = opt ? ty->genericArg : ty;
    std::string ic = cType(inner);
    // Concrete-element triad instance (`Shared<Leaf>` etc.) — a library generic instance keyed by template.
    auto g = _genericTypeInstOf.find(ic);
    if (g != _genericTypeInstOf.end()) {
        if      (g->second == _sharedTmpl) e.kind = "Shared";
        else if (g->second == _ownedTmpl)  e.kind = "Owned";
        else if (g->second == _weakTmpl)   e.kind = "Weak";
        if (!e.kind.empty()) { e.elemC = inner->genericArg ? cType(inner->genericArg) : ""; e.elemIsContract = isInterface(e.elemC); e.optional = opt; return e; }
    }
    // Interface-erased intrinsic smart pointer (`Owned<Contract>` / `Shared<Contract>` / Box<dyn>).
    if (isSmartPtrClass(ic)) {
        CollKind k = smartKind(ic);
        e.kind  = (k == CollKind::Shared) ? "Shared" : (k == CollKind::Weak) ? "Weak" : "Owned";
        e.elemC = _classes.count(ic) ? _classes[ic].collElemClass : "";
        e.elemIsContract = isInterface(e.elemC);
        e.optional = opt;
    }
    return e;
}

// The wire `__type` tag for a graph node: the SOURCE type name (`Node`), not the mangled C name (`_F4__Node`).
// Both the writer (beginTableEntry) and the reader (dispatch) must agree, so they share this.
std::string CEmitter::graphWireName(const ClassInfo& ci)
{
    return (ci.node && ci.node->name && ci.node->name->value) ? *ci.node->name->value : ci.name;
}

// Synthesize a `Shared<elem>` type node (the graph deserialize return type: `decode::<Shared<T>>` hands back
// the owning root handle). Resolves like the user-written `Shared<Leaf>` field types (empty qualifier + the
// implicit `using std::memory`), so cType mangles it to `Shared_<elem>`.
SharedIdentifier CEmitter::sharedTypeNode(SharedIdentifier elem)
{
    if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<synth>"));
    auto node = std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Shared"),
                                                 std::make_shared<StringList>(), elem);
    node->genericArgs = std::make_shared<IdentifierList>();
    node->genericArgs->push_back(elem);
    return node;
}

// Closure (post-computeReachesPointer): a graph node is a `@generate` root (`reachesPointer`) OR a pointee
// reached via some node's Shared/Weak/Owned field (a tree type like `Leaf` that is only ever a `Shared<Leaf>`
// target). Each node emits the node helpers + a `deserialize` returning `Shared<T>` (so `Shared<T>::deserialize`
// → `T::deserialize` type-checks). Also records the node set in a stable order for the driver dispatch chains.
void CEmitter::computeGraphNodeTypes()
{
    NsCtx saved = _nsCtx;
    std::vector<std::string> work;
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.reachesPointer && (ci.genSerialize || ci.genDeserialize)) { ci.isGraphNode = true; work.push_back(kv.first); }
    }
    while (!work.empty()) {
        std::string cur = work.back(); work.pop_back();
        ClassInfo& ci = _classes[cur];
        _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
        _typeSubst.clear();
        for (auto& f : ci.fields) {
            GraphEdge e = graphEdgeOf(f.type);
            if (e.kind.empty() || e.elemC.empty()) continue;
            if (e.elemIsContract) {   // a `Shared<Contract>` edge: every @generate implementor is a graph node
                _polyContracts.insert(e.elemC);
                for (auto& kv2 : _classes) {
                    ClassInfo& c2 = kv2.second;
                    if (c2.isGraphNode) continue;
                    bool impl = std::find(c2.interfaces.begin(), c2.interfaces.end(), e.elemC) != c2.interfaces.end();
                    if (impl && (c2.genSerialize || c2.genDeserialize)) { c2.isGraphNode = true; work.push_back(kv2.first); }
                }
                continue;
            }
            auto it = _classes.find(e.elemC);
            if (it != _classes.end() && !it->second.isGraphNode) { it->second.isGraphNode = true; work.push_back(e.elemC); }
        }
    }
    _nsCtx = saved; _typeSubst.clear();
    _graphNodeOrder.clear();
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (!ci.isGraphNode) continue;
        ci.graphTypeId = (int)_graphNodeOrder.size();
        _graphNodeOrder.push_back(kv.first);
        auto it = ci.methods.find("deserialize");     // graph deserialize returns Shared<T> — but only when the
        if (it != ci.methods.end() && it->second.isSynthDe) {   // Shared<T> instance exists (root / Shared/Weak
            SharedIdentifier sh = sharedTypeNode(it->second.returnType);   // pointee). A pure Owned pointee has no
            _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
            if (_classes.count(cType(sh))) { it->second.returnType = sh; ci.graphDeserialize = true; }   // Shared<T> — stays by-value
            _nsCtx = saved;
        }
    }
}

// Prototypes for the graph node helpers (in the shared header, so cross-referencing node writers link).
void CEmitter::emitGraphNodeHelperProtos(ClassInfo& ci)
{
    const char* stat = _emitStaticClass ? "static inline " : "";
    *_out << stat << "void " << ci.name << "__serializeNode(void* __obj, void* __wv, struct kama_ser_graph* __g, uint64_t __id);\n";
    *_out << stat << "kama_de_box* " << ci.name << "__allocShell(Deserializer r, struct kama_de_arena* __arena);\n";
    *_out << stat << "void " << ci.name << "__wireShell(kama_de_box* __b, Deserializer r, struct kama_de_graph* __g);\n";
}

// Phase E: for each contract used as a graph edge element, a closed-world dispatch pair over its
// @generate implementors. `<C>__nodeWriterFor` maps a fat handle's runtime `.vtbl` to that concrete's
// serializeNode (write side); `<C>__implVtbl` maps a pointee's concrete type-id (recovered from the box)
// to its `&<K>__as_<C>` conformance vtable (read side; NULL for a non-implementor => TypeMismatch).
// Emitted once into the shared header, after the class prototypes + `extern const <K>__as_<C>` decls.
void CEmitter::emitPolyContractResolvers()
{
    for (auto& C : _polyContracts) {
        std::vector<std::string> impls;   // implementors that are graph nodes, in stable _graphNodeOrder
        for (auto& K : _graphNodeOrder) {
            ClassInfo& ci = _classes[K];
            bool impl  = std::find(ci.interfaces.begin(),      ci.interfaces.end(),      C) != ci.interfaces.end();
            bool retro = std::find(ci.retroInterfaces.begin(), ci.retroInterfaces.end(), C) != ci.retroInterfaces.end();
            if (impl && !retro) impls.push_back(K);
        }
        *_out << "static inline kama_node_writer " << C << "__nodeWriterFor(const struct " << C << "_vtbl* __vt)\n{\n";
        for (auto& K : impls) { indent(1); *_out << "if (__vt == &" << K << "__as_" << C << ") return " << K << "__serializeNode;\n"; }
        indent(1); *_out << "return 0;\n}\n";
        *_out << "static inline const struct " << C << "_vtbl* " << C << "__implVtbl(uint32_t __tid)\n{\n";
        indent(1); *_out << "switch (__tid) {\n";
        for (auto& K : impls) { indent(1); *_out << "case " << _classes[K].graphTypeId << ": return &" << K << "__as_" << C << ";\n"; }
        indent(1); *_out << "default: return 0;\n";
        indent(1); *_out << "}\n}\n\n";
    }
}

// The three per-node-type helpers: write one table entry (serializeNode), pass-1 alloc+scalar-read
// (allocShell), pass-2 pointer wiring (wireShell). Emitted for every graph node (root or pointee).
void CEmitter::emitGraphNodeHelpers(ClassInfo& ci)
{
    const char* stat = _emitStaticClass ? "static inline " : "";

    // -- serializeNode: `beginTableEntry(id,"T")` + each field (scalars via the by-value helper; pointer edges
    //    intern the pointee and write its id) + `endTableEntry`.
    // The prelude triad's concrete-element C struct fields are `p` (pointee) and `c` (control block); a
    // CONTRACT-element edge is a fat handle {obj, vtbl, ctrl} — its writer is resolved from `.vtbl` at runtime.
    auto refWrite = [&](const GraphEdge& e, const std::string& val, int d) {
        std::string pf = e.elemIsContract ? ".obj" : ".p";
        std::string cf = e.elemIsContract ? ".ctrl" : ".c";
        std::string writer = e.elemIsContract ? (e.elemC + "__nodeWriterFor((" + val + ").vtbl)")
                                              : (e.elemC + "__serializeNode");
        std::string ptr = "(" + val + ")" + pf;
        std::string intern = "kama_ser_graph_intern(__g, (uint64_t)(uintptr_t)" + ptr + ", " + ptr + ", " + writer + ")";
        if (e.kind == "Weak") {   // a Weak writes its id only while a strong handle still exists, else 0 (expired)
            indent(d); *_out << "if ((" << val << ")" << cf << " && (" << val << ")" << cf << "->strong > 0) "
                             << "w->vtbl->writeRef(w->obj, " << intern << ");\n";
            indent(d); *_out << "else w->vtbl->writeRef(w->obj, 0);\n";
        } else {   // Shared / Owned — always present
            indent(d); *_out << "w->vtbl->writeRef(w->obj, " << intern << ");\n";
        }
    };
    *_out << stat << "void " << ci.name << "__serializeNode(void* __obj, void* __wv, struct kama_ser_graph* __g, uint64_t __id)\n{\n";
    indent(1); *_out << ci.name << "* self = (" << ci.name << "*)__obj;\n";
    indent(1); *_out << "Serializer* w = (Serializer*)__wv;\n";
    indent(1); *_out << "w->vtbl->beginTableEntry(w->obj, __id, " << kamaStrLit(graphWireName(ci)) << ");\n";
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        const std::string& wire = f.serName.empty() ? f.name : f.serName;
        GraphEdge e = graphEdgeOf(f.type);
        indent(1); *_out << "w->vtbl->fieldName(w->obj, " << kamaStrLit(wire) << ");\n";
        if (e.kind.empty()) { emitSerFieldWrite(f.type, "self->" + f.name, 1); continue; }
        std::string acc = "self->" + f.name;
        if (e.optional) {
            std::string oc = cType(f.type);
            indent(1); *_out << "switch ((" << acc << ").tag) {\n";
            indent(1); *_out << "case " << oc << "_Some: {\n";
            refWrite(e, "(" + acc + ").u.Some.value", 2);
            indent(2); *_out << "break;\n";
            indent(1); *_out << "}\n";
            indent(1); *_out << "case " << oc << "_None: { w->vtbl->writeRef(w->obj, 0); break; }\n";
            indent(1); *_out << "default: break;\n";
            indent(1); *_out << "}\n";
        } else {
            refWrite(e, acc, 1);
        }
    }
    indent(1); *_out << "w->vtbl->endTableEntry(w->obj);\n";
    *_out << "}\n\n";

    // -- allocShell (pass 1): a zeroed heap pointee in a fresh box (ctrl strong=1), scalar fields read, pointer
    //    fields skipped (wired in pass 2). Registered by the driver.
    *_out << stat << "kama_de_box* " << ci.name << "__allocShell(Deserializer r, struct kama_de_arena* __arena)\n{\n";
    indent(1); *_out << "kama_de_box* __b = kama_de_arena_new(__arena);\n";
    indent(1); *_out << "__b->ptr = kama_calloc(1, sizeof(" << ci.name << "));\n";
    indent(1); *_out << ci.name << "* self = (" << ci.name << "*)__b->ptr;\n";
    for (auto& f : ci.fields) {   // reset Optional-of-scalar fields (zero-init = Some(0))
        if (f.serSkip) continue;
        if (!graphEdgeOf(f.type).kind.empty()) continue;
        if (f.type && f.type->value && *f.type->value == "Optional") {
            std::string oc = cType(f.type);
            indent(1); *_out << "self->" << f.name << " = (" << oc << "){ .tag = " << oc << "_None };\n";
        }
    }
    indent(1); *_out << "while (r.vtbl->moreFields(r.obj)) {\n";
    indent(2); *_out << "kama_string __key = r.vtbl->fieldName(r.obj);\n";
    bool firstS = true;
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        if (!graphEdgeOf(f.type).kind.empty()) continue;   // pointer edge — pass 2
        const std::string& wire = f.serName.empty() ? f.name : f.serName;
        indent(2); *_out << (firstS ? "if" : "else if") << " (kama_string__equals(&__key, " << kamaStrLit(wire) << ")) {\n";
        emitDeFieldRead(f.type, "self->" + f.name, 3);
        indent(2); *_out << "}\n";
        firstS = false;
    }
    indent(2); *_out << (firstS ? "" : "else ") << "{ r.vtbl->skipValue(r.obj); }\n";
    indent(2); *_out << "kama_string__dtor(&__key);\n";
    indent(1); *_out << "}\n";
    indent(1); *_out << "return __b;\n";
    *_out << "}\n\n";

    // -- wireShell (pass 2): re-read the entry, resolving each pointer field's id to the registered box and
    //    constructing the Shared/Weak/Owned value over (ptr, ctrl). Scalar fields are skipped (already read).
    *_out << stat << "void " << ci.name << "__wireShell(kama_de_box* __b, Deserializer r, struct kama_de_graph* __g)\n{\n";
    indent(1); *_out << ci.name << "* self = (" << ci.name << "*)__b->ptr;\n";
    indent(1); *_out << "while (r.vtbl->moreFields(r.obj)) {\n";
    indent(2); *_out << "kama_string __key = r.vtbl->fieldName(r.obj);\n";
    bool firstW = true;
    for (auto& f : ci.fields) {
        if (f.serSkip) continue;
        GraphEdge e = graphEdgeOf(f.type);
        if (e.kind.empty()) continue;   // scalar — pass 1
        const std::string& wire = f.serName.empty() ? f.name : f.serName;
        indent(2); *_out << (firstW ? "if" : "else if") << " (kama_string__equals(&__key, " << kamaStrLit(wire) << ")) {\n";
        emitGraphRefRead(f.type, e, "self->" + f.name, 3);
        indent(2); *_out << "}\n";
        firstW = false;
    }
    indent(2); *_out << (firstW ? "" : "else ") << "{ r.vtbl->skipValue(r.obj); }\n";
    indent(2); *_out << "kama_string__dtor(&__key);\n";
    indent(1); *_out << "}\n";
    *_out << "}\n\n";
}

// Emit the pass-2 wiring of ONE pointer field (`dst`) from its wire id. `e` is its classification; `ty` its
// full type (for the Optional mangling). A `kama_de_box*` for the pointee id is looked up in `__g`.
void CEmitter::emitGraphRefRead(SharedIdentifier ty, const GraphEdge& e, const std::string& dst, int d)
{
    std::string X = e.elemC;
    std::string innerC = e.optional ? cType(ty->genericArg) : cType(ty);   // Shared_X / Weak_X / Owned_X
    // the wiring of a resolved box `__t` into a smart-ptr value expression `place` of kind e.kind. A
    // concrete-element handle is the thin triad struct (`p` pointee, `c` control block); a CONTRACT-element
    // handle is the fat `{obj, vtbl, ctrl}`, whose vtable is recovered from the box's concrete type-id (a
    // non-implementor tag => TypeMismatch, leaving the calloc-zeroed handle untouched). Shared retains
    // (strong++), Weak downgrades (weak++).
    auto wireInto = [&](const std::string& place, int dd) {
        if (e.elemIsContract) {
            indent(dd); *_out << "const struct " << X << "_vtbl* __vt = " << X << "__implVtbl(__t->type_id);\n";
            indent(dd); *_out << "if (!__vt) { r.vtbl->failWith(r.obj, DeError_TypeMismatch); }\n";
            indent(dd); *_out << "else { " << place << ".obj = __t->ptr; " << place << ".vtbl = __vt; " << place
                              << ".ctrl = __t->ctrl; __t->ctrl->" << (e.kind == "Weak" ? "weak" : "strong") << "++; }\n";
        } else if (e.kind == "Shared") {
            indent(dd); *_out << place << ".p = (" << X << "*)__t->ptr; " << place << ".c = (void*)__t->ctrl; __t->ctrl->strong++;\n";
        } else if (e.kind == "Weak") {
            indent(dd); *_out << place << ".p = (" << X << "*)__t->ptr; " << place << ".c = (void*)__t->ctrl; __t->ctrl->weak++;\n";
        }
        // Owned is handled separately (give-once move), never through wireInto.
    };
    indent(d); *_out << "uint64_t __rid = r.vtbl->readRef(r.obj);\n";

    if (e.kind == "Owned") {   // give-once: exclusive transfer, claim-checked, no refcount
        std::string ownedNull = e.optional ? ("(" + cType(ty) + "){ .tag = " + cType(ty) + "_None }") : "";
        if (e.optional) { indent(d); *_out << "if (__rid == 0) { " << dst << " = " << ownedNull << "; }\n"; indent(d); *_out << "else "; }
        else            { indent(d); *_out << "if (__rid == 0) { r.vtbl->failWith(r.obj, DeError_Malformed); }\n"; indent(d); *_out << "else "; }
        *_out << "if (kama_de_graph_claim(__g, __rid)) { r.vtbl->failWith(r.obj, DeError_DuplicateId); }\n";
        indent(d); *_out << "else {\n";
        indent(d + 1); *_out << "kama_de_box* __t = (kama_de_box*)kama_de_graph_lookup(__g, __rid);\n";
        indent(d + 1); *_out << "if (__t) {\n";
        if (e.elemIsContract) {   // fat move: recover the concrete vtable, then transfer the pointee (no refcount)
            indent(d + 2); *_out << "const struct " << X << "_vtbl* __vt = " << X << "__implVtbl(__t->type_id);\n";
            indent(d + 2); *_out << "if (!__vt) { r.vtbl->failWith(r.obj, DeError_TypeMismatch); }\n";
            indent(d + 2); *_out << "else {\n";
            if (e.optional) {
                indent(d + 3); *_out << innerC << " __v = {0}; __v.obj = __t->ptr; __v.vtbl = __vt;\n";
                indent(d + 3); *_out << dst << " = (" << cType(ty) << "){ .tag = " << cType(ty) << "_Some, .u.Some = { .value = __v } };\n";
            } else {
                indent(d + 3); *_out << dst << ".obj = __t->ptr; " << dst << ".vtbl = __vt;\n";
            }
            indent(d + 3); *_out << "__t->ptr = NULL;\n";
            indent(d + 3); *_out << "if (__t->ctrl) { if (__t->ctrl->weak == 0) kama_free(__t->ctrl); __t->ctrl = NULL; }\n";
            indent(d + 2); *_out << "}\n";
        } else {
            if (e.optional) {
                indent(d + 2); *_out << innerC << " __v; __v.p = (" << X << "*)__t->ptr;\n";
                indent(d + 2); *_out << dst << " = (" << cType(ty) << "){ .tag = " << cType(ty) << "_Some, .u.Some = { .value = __v } };\n";
            } else {
                indent(d + 2); *_out << dst << ".p = (" << X << "*)__t->ptr;\n";
            }
            indent(d + 2); *_out << "__t->ptr = NULL;\n";
            indent(d + 2); *_out << "if (__t->ctrl) { if (__t->ctrl->weak == 0) kama_free(__t->ctrl); __t->ctrl = NULL; }\n";
        }
        indent(d + 1); *_out << "}\n";
        indent(d + 1); *_out << "else r.vtbl->failWith(r.obj, DeError_UnresolvedReference);\n";
        indent(d); *_out << "}\n";
        return;
    }

    // Shared / Weak. A miss is a dangling reference (UnresolvedReference); id 0 = null (None / expired).
    if (e.optional) {
        std::string oc = cType(ty);
        indent(d); *_out << "if (__rid == 0) { " << dst << " = (" << oc << "){ .tag = " << oc << "_None }; }\n";
        indent(d); *_out << "else {\n";
        indent(d + 1); *_out << "kama_de_box* __t = (kama_de_box*)kama_de_graph_lookup(__g, __rid);\n";
        indent(d + 1); *_out << "if (__t) {\n";
        indent(d + 2); *_out << innerC << " __v = {0};\n";   // zeroed: a contract TypeMismatch leaves it null (safe drop)
        wireInto("__v", d + 2);
        indent(d + 2); *_out << dst << " = (" << oc << "){ .tag = " << oc << "_Some, .u.Some = { .value = __v } };\n";
        indent(d + 1); *_out << "}\n";
        indent(d + 1); *_out << "else r.vtbl->failWith(r.obj, DeError_UnresolvedReference);\n";
        indent(d); *_out << "}\n";
    } else {
        // A bare Shared is never null; a bare Weak may be expired (id 0 → leave the zeroed handle).
        if (e.kind == "Weak") { indent(d); *_out << "if (__rid != 0) {\n"; }
        else                  { indent(d); *_out << "{\n"; }
        indent(d + 1); *_out << "kama_de_box* __t = (kama_de_box*)kama_de_graph_lookup(__g, __rid);\n";
        indent(d + 1); *_out << "if (__t) {\n";
        wireInto(dst, d + 2);
        indent(d + 1); *_out << "}\n";
        indent(d + 1); *_out << "else r.vtbl->failWith(r.obj, DeError_UnresolvedReference);\n";
        indent(d); *_out << "}\n";
    }
}

// Public graph serialize: reserve the root id from `self`'s address, write the root entry inline, drain the
// worklist (each node via its writer, in discovery == id order), close. `encode(v: x)` dispatches here (x a
// by-value root or a `Shared<T>` auto-deref'd to the pointee).
void CEmitter::emitGraphSerializeDefinition(ClassInfo& ci)
{
    const char* stat = _emitStaticClass ? "static inline " : "";
    *_out << stat << "void " << ci.name << "__serialize(" << ci.name << "* self, Serializer* w)\n{\n";
    indent(1); *_out << "struct kama_ser_graph __g; kama_ser_graph_init(&__g);\n";
    indent(1); *_out << "uint64_t __root = kama_ser_graph_reserve(&__g, (uint64_t)(uintptr_t)self);\n";
    indent(1); *_out << "w->vtbl->beginGraph(w->obj, __root);\n";
    indent(1); *_out << ci.name << "__serializeNode((void*)self, (void*)w, &__g, __root);\n";
    indent(1); *_out << "for (size_t __i = 0; __i < kama_ser_graph_count(&__g); __i++)\n";
    indent(2); *_out << "kama_ser_graph_writer(&__g, __i)(kama_ser_graph_node(&__g, __i), (void*)w, &__g, kama_ser_graph_node_id(&__g, __i));\n";
    indent(1); *_out << "w->vtbl->endGraph(w->obj);\n";
    indent(1); *_out << "kama_ser_graph_free(&__g);\n";
    *_out << "}\n\n";
}

// Public graph deserialize (two-pass): pass 1 allocates + registers every shell by id (scalars read); pass 2
// wires pointer fields; then the root is retained + returned and the per-shell construction strong is dropped
// (an unreferenced node frees, the live graph survives via its real edges + the returned root). Returns
// Shared<T>; a dangling root → UnresolvedReference.
void CEmitter::emitGraphDeserializeDefinition(ClassInfo& ci)
{
    std::string T = ci.name;
    std::string sharedT = cType(ci.methods["deserialize"].returnType);   // Shared_T
    const char* stat = _emitStaticClass ? "static inline " : "";
    *_out << stat << sharedT << " " << T << "__deserialize(Deserializer r)\n{\n";
    indent(1); *_out << "struct kama_de_graph __g; kama_de_graph_init(&__g);\n";
    for (auto& K : _graphNodeOrder) { indent(1); *_out << "struct kama_de_arena __arena_" << K << "; kama_de_arena_init(&__arena_" << K << ");\n"; }
    indent(1); *_out << "uint64_t __root = r.vtbl->beginGraph(r.obj);\n";
    // PASS 1 — alloc + register
    indent(1); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(1); *_out << "while (r.vtbl->moreFields(r.obj)) {\n";
    indent(2); *_out << "uint64_t __eid = r.vtbl->entryKey(r.obj);\n";
    indent(2); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(2); *_out << "kama_string __tk = r.vtbl->fieldName(r.obj); kama_string__dtor(&__tk);\n";
    indent(2); *_out << "kama_string __ty = r.vtbl->readString(r.obj);\n";
    bool f1 = true;
    for (auto& K : _graphNodeOrder) {
        indent(2); *_out << (f1 ? "if" : "else if") << " (kama_string__equals(&__ty, " << kamaStrLit(graphWireName(_classes[K])) << ")) { "
                         << "kama_de_box* __b = " << K << "__allocShell(r, &__arena_" << K << "); "
                         << "__b->type_id = " << _classes[K].graphTypeId << "; kama_de_graph_register(&__g, __eid, __b); }\n";
        f1 = false;
    }
    indent(2); *_out << "else { while (r.vtbl->moreFields(r.obj)) r.vtbl->skipValue(r.obj); }\n";
    indent(2); *_out << "kama_string__dtor(&__ty);\n";
    indent(1); *_out << "}\n";
    // PASS 2 — wire
    indent(1); *_out << "r.vtbl->rewindGraph(r.obj);\n";
    indent(1); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(1); *_out << "while (r.vtbl->moreFields(r.obj)) {\n";
    indent(2); *_out << "uint64_t __eid = r.vtbl->entryKey(r.obj);\n";
    indent(2); *_out << "r.vtbl->beginObject(r.obj);\n";
    indent(2); *_out << "kama_string __tk = r.vtbl->fieldName(r.obj); kama_string__dtor(&__tk);\n";
    indent(2); *_out << "kama_string __ty = r.vtbl->readString(r.obj);\n";
    bool f2 = true;
    for (auto& K : _graphNodeOrder) {
        indent(2); *_out << (f2 ? "if" : "else if") << " (kama_string__equals(&__ty, " << kamaStrLit(graphWireName(_classes[K])) << ")) { "
                         << "kama_de_box* __b = (kama_de_box*)kama_de_graph_lookup(&__g, __eid); " << K << "__wireShell(__b, r, &__g); }\n";
        f2 = false;
    }
    indent(2); *_out << "else { while (r.vtbl->moreFields(r.obj)) r.vtbl->skipValue(r.obj); }\n";
    indent(2); *_out << "kama_string__dtor(&__ty);\n";
    indent(1); *_out << "}\n";
    indent(1); *_out << "r.vtbl->endGraph(r.obj);\n";
    // ROOT — retain the looked-up shell; a dangling root id surfaces UnresolvedReference.
    indent(1); *_out << "kama_de_box* __rb = (kama_de_box*)kama_de_graph_lookup(&__g, __root);\n";
    indent(1); *_out << sharedT << " __ret = {0};\n";
    indent(1); *_out << "if (__rb) { __ret.p = (" << T << "*)__rb->ptr; __ret.c = (void*)__rb->ctrl; __rb->ctrl->strong++; }\n";
    indent(1); *_out << "else { r.vtbl->failWith(r.obj, DeError_UnresolvedReference); }\n";   // dangling root -> empty Shared
    // CLEANUP — drop each shell's construction strong; free the boxes + arenas.
    for (auto& K : _graphNodeOrder) {
        bool kd = _classes.count(K) && _classes[K].destructible;
        indent(1); *_out << "for (size_t __i = 0; __i < __arena_" << K << ".len; __i++) {\n";
        indent(2); *_out << "kama_de_box* __b = __arena_" << K << ".items[__i];\n";
        indent(2); *_out << "if (__b->ctrl) { __b->ctrl->strong--; if (__b->ctrl->strong == 0) {\n";
        indent(3); *_out << "if (__b->ptr) { " << (kd ? (K + "__dtor((" + K + "*)__b->ptr); ") : "") << "kama_free(__b->ptr); }\n";
        indent(3); *_out << "if (__b->ctrl->weak == 0) kama_free(__b->ctrl);\n";
        indent(2); *_out << "} }\n";
        indent(2); *_out << "kama_free(__b);\n";
        indent(1); *_out << "}\n";
        indent(1); *_out << "kama_de_arena_free(&__arena_" << K << ");\n";
    }
    indent(1); *_out << "kama_de_graph_free(&__g);\n";
    indent(1); *_out << "return __ret;\n";
    *_out << "}\n\n";
}

// emit one specialized generic-type instance under its binding. phase 0 = struct typedef+body,
// 1 = ctor/dtor/method prototypes, 2 = bodies. Mirrors emitGenericInst: all specialized class
// functions are header-`static` (every module includes the header), so `_emitStaticClass` is set here.
void CEmitter::emitGenericTypeInst(const GenericTypeInst& gi, int phase)
{
    auto cit = _classes.find(gi.mangledName);
    if (cit == _classes.end()) return;
    ClassInfo& ci = cit->second;
    NsCtx savedCtx = _nsCtx;
    // emit under the USE-SITE ctx (so a prelude template's user-type args resolve); for a
    // same-scope user generic this equals the template's home ctx.
    _nsCtx = _genericTypeInstCtx.count(gi.mangledName) ? _genericTypeInstCtx[gi.mangledName]
                                                       : _genericTypeCtx[gi.templateKey];
    _typeSubst.clear();
    const std::vector<std::string>& ps = _genericTypeParams[gi.templateKey];
    for (size_t i = 0; i < ps.size() && i < gi.typeArgs.size(); ++i) _typeSubst[ps[i]] = gi.typeArgs[i];
    _emitStaticClass = true;
    if      (phase == 0) { emitStruct(ci); }   // forward typedef now emitted in the phase-(a) loop
    else if (phase == 1) emitClassPrototypes(ci);
    else { emitClassDefinitions(ci); emitClassInterfaceVtables(ci); }   // + `static` C__as_I vtables (e.g. List<int32> as Serialize)
    _emitStaticClass = false;
    _typeSubst.clear();
    _nsCtx = savedCtx;
}

// True iff `e` statically has kama type `string` (lowers to `kama_string`): a string literal, a `string`
// local/param or a string-returning call (both surface via `exprClass(e) == "kama_string"`), or a nested
// `+` chain with a string operand. `exprClass`/`operatorResultClass` only class USER operators, so the
// string `+` result is invisible there — recurse here. Keeps string knowledge localized to this predicate
// + the `emitBinaryOperator` string branch, leaving `exprClass`/`operatorResultClass` string-free.
bool CEmitter::exprIsString(SharedExpression e)
{
    if (!e) return false;
    if (dynamic_cast<StringNode*>(e.get())) return true;
    if (auto* be = dynamic_cast<BinaryExpressionNode*>(e.get()))
        if (be->token == PLUS && (exprIsString(be->LHS) || exprIsString(be->RHS))) return true;
    return exprClass(e) == "kama_string";
}

// A `string`-typed RVALUE that isn't a plain lvalue or a borrowed literal — a call/operator result
// (concat, substring, trim, replace, case, find/split pieces...) — may own a heap buffer. Used as an
// operand/receiver it would otherwise be materialized into a throwaway compound-literal and LEAK. Hoist
// it into a named temp registered for RAII drop at scope exit so the buffer is freed. Returns the temp
// name, or "" for a literal / lvalue / non-string (the caller keeps its normal, leak-free path) or when
// there is no statement slot to hoist into (a rare raw `if`/`while` operand — `_hoistOK` false).
// A borrowed result (an empty piece, cap==0) is harmless: kama_string__dtor is a no-op on it.
std::string CEmitter::hoistStringTemp(SharedExpression e)
{
    if (!e || !_hoistOK) return "";                // no statement slot to hoist into (raw operand)
    ASTNode* n = e.get();
    if (dynamic_cast<StringNode*>(n) || dynamic_cast<IdentifierNode*>(n)
        || dynamic_cast<MemberAccessNode*>(n) || dynamic_cast<ThisAccessNode*>(n)) return "";
    if (!exprIsString(e)) return "";
    std::string t = "__strtmp" + std::to_string(_tempCounter++);
    _hoisted.push_back("kama_string " + t + " = " + emitExpression(e) + ";");
    recordDestructibleLocal(t, "kama_string");
    return t;
}

// The static class type of an expression ("" if primitive/unknown).
// The C type of an assignable lvalue (a local/param or a field) — like exprClass, but KEEPS
// collection/string/Fixed types (exprClass drops them via its `isClass` filter, returning ""). Used by the
// assignment handler to detect an `ownsByValue` LHS (a `string`/collection local, param, or `this.`-field).
std::string CEmitter::lvalueCType(SharedExpression e)
{
    if (!e) return "";
    ASTNode* n = e.get();
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (!id->value) return "";
        auto it = _localCTypes.find(*id->value);                     // a local / param (incl. kama_string / List_*)
        if (it != _localCTypes.end()) return it->second;
        if (_currentClass) {                                         // a bare or `this.`-qualified field of the current class
            ClassInfo* owner = findFieldOwner(_currentClass, *id->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *id->value && f.type) return cType(f.type);
        }
        return "";
    }
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {             // `obj.field` — resolve the receiver's field
        std::string recv = exprClass(ma->expression);
        if (isSmartPtrClass(recv)) recv = _classes[recv].collElemClass;
        else { std::string dt = derefTarget(recv); if (!dt.empty()) recv = dt; }
        if (!recv.empty() && ma->identifier && ma->identifier->value && _classes.count(recv)) {
            ClassInfo* owner = findFieldOwner(&_classes[recv], *ma->identifier->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *ma->identifier->value && f.type) return cType(f.type);
        }
        return "";
    }
    return "";
}

std::string CEmitter::exprClass(SharedExpression e)
{
    if (!e) return "";
    ASTNode* n = e.get();

    if (dynamic_cast<StringNode*>(n)) return "kama_string";   // a string literal is the `string` primitive

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
        else { std::string dt = derefTarget(recv); if (!dt.empty()) recv = dt; }   // user Deref<T> auto-deref
        if (!recv.empty() && ma->identifier && ma->identifier->value && _classes.count(recv)) {
            ClassInfo* owner = findFieldOwner(&_classes[recv], *ma->identifier->value);
            if (owner)
                for (auto& f : owner->fields)
                    if (f.name == *ma->identifier->value && f.type && isClass(cType(f.type)))
                        return cType(f.type);
        }
        return "";
    }

    // `list[i]` / `a[i]` resolves to the ELEMENT type, so `list[i].m()` finds the method.
    // Pure resolution (no emission) — mirrors the front of collectionElemAccess.
    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        SharedExpression recv = ea->expression ? ea->expression
                                               : std::static_pointer_cast<ExpressionNode>(ea->identifier);
        std::string cls = exprClass(recv);
        if (!cls.empty() && _classes.count(cls) && _classes[cls].isIntrinsicColl)
            return _classes[cls].collElemClass;
        // a user place-`operator[]` element resolves to the operator's element type (its `ref T`), so
        // `m[i][j]` / `m[i].field` chain. Bind `This`/the instance's type args for the return type.
        if (MethodInfo* op = userIndexOp(cls)) {
            ScopedStr _ts(_thisType, cls);
            // Bind the owning generic instance's type args so `operator[]`'s `ref T` resolves concretely
            // (`v[i]` on a `List<Probe>` -> Probe), the same as a method return.
            NsCtx savedCtx = _nsCtx;
            std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
            auto gi = _genericTypeInsts.find(cls);
            if (gi != _genericTypeInsts.end()) {
                _typeSubst.clear();
                const std::vector<std::string>& ps = _genericTypeParams[gi->second.templateKey];
                for (size_t i = 0; i < ps.size() && i < gi->second.typeArgs.size(); ++i)
                    _typeSubst[ps[i]] = gi->second.typeArgs[i];
                _nsCtx = _genericTypeInstCtx.count(cls) ? _genericTypeInstCtx[cls] : _genericTypeCtx[gi->second.templateKey];
            }
            std::string rt = cType(op->returnType);
            _typeSubst = savedSubst; _nsCtx = savedCtx;
            return isClass(rt) ? rt : "";
        }
        // A raw `Ptr<T>` field index (`this.data[i]` in unsafe container code): resolve to the pointer's
        // element type, so a `drop`/`copy`/variant hand-off of the element knows what it is.
        std::string pet = ptrElemType(e);
        if (!pet.empty()) return isClass(pet) ? pet : "";
        return "";
    }

    // a CALL RESULT's static class (pure resolution — no emission), so a call can be a
    // `match` subject / value site (`match(w.tryUpgrade())`). A class return maps to its name; a
    // primitive/void return stays "".
    if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        // Method call `recv.method(args)` — call->expression is a MemberAccessNode.
        if (auto* ma = inv->expression ? dynamic_cast<MemberAccessNode*>(inv->expression.get()) : nullptr) {
            std::string method = (ma->identifier && ma->identifier->value) ? *ma->identifier->value : "";
            std::string cls = exprClass(ma->expression);
            if (isSmartPtrClass(cls)) {   // an intrinsic (tryUpgrade/valid/…) — returnType is on the smart-ptr
                auto mit = _classes[cls].methods.find(method);
                if (mit != _classes[cls].methods.end() && mit->second.returnType) {
                    std::string rc = cType(mit->second.returnType);
                    return isClass(rc) ? rc : "";
                }
                cls = _classes[cls].collElemClass;   // else auto-deref to the pointee's method
            }
            if (!cls.empty() && _classes.count(cls)) {
                ClassInfo* owner = nullptr;
                MethodInfo* mi = findMethod(&_classes[cls], method, &owner);
                // auto-deref via a user Deref<T> contract: the method may live on the pointee T.
                if (!mi) { std::string dt = derefTarget(cls);
                           if (!dt.empty() && _classes.count(dt)) mi = findMethod(&_classes[dt], method, &owner); }
                if (mi && mi->returnType) {
                    // A method on a generic INSTANCE returns the template's unbound type
                    // (`Weak<T>.tryUpgrade() -> Optional<Shared<T>>`). Bind the owning instance's
                    // type args + its ctx so the return mangles concretely (else `match(w.tryUpgrade())`
                    // can't find the Optional variant). Mirrors emitGenericTypeInst's binding.
                    std::string ownerCls = owner ? owner->name : cls;
                    NsCtx savedCtx = _nsCtx;
                    std::map<std::string, SharedIdentifier> savedSubst = _typeSubst;
                    auto gi = _genericTypeInsts.find(ownerCls);
                    if (gi != _genericTypeInsts.end()) {
                        _typeSubst.clear();
                        const std::vector<std::string>& ps = _genericTypeParams[gi->second.templateKey];
                        for (size_t i = 0; i < ps.size() && i < gi->second.typeArgs.size(); ++i)
                            _typeSubst[ps[i]] = gi->second.typeArgs[i];
                        _nsCtx = _genericTypeInstCtx.count(ownerCls) ? _genericTypeInstCtx[ownerCls]
                                                                     : _genericTypeCtx[gi->second.templateKey];
                    }
                    std::string rc = cType(mi->returnType);
                    _typeSubst = savedSubst; _nsCtx = savedCtx;
                    return isClass(rc) ? rc : "";
                }
            }
            return "";
        }
        // Static method call `Class::method(args)` — a qualified identifier with no receiver expression
        // (`File::open(...)`). Resolve the class, find the method, return its (class) return type, so a
        // fallible factory is usable inline as a `match` subject / value site — not only after binding to
        // a typed local. Mirrors the `Type::method` resolution in emitFnPtrBind.
        if (inv->identifier && inv->identifier->value && !inv->expression
            && inv->identifier->qualifier && !inv->identifier->qualifier->empty()) {
            auto prefix = std::make_shared<StringList>();
            for (size_t i = 0; i + 1 < inv->identifier->qualifier->size(); ++i)
                prefix->push_back((*inv->identifier->qualifier)[i]);
            std::string cls = resolveUserName(*inv->identifier->qualifier->back(), prefix);
            if (_classes.count(cls)) {
                ClassInfo* owner = nullptr;
                MethodInfo* mi = findMethod(&_classes[cls], *inv->identifier->value, &owner);
                if (mi && mi->returnType) {
                    std::string rc = cType(mi->returnType);
                    if (isClass(rc)) return rc;
                }
            }
        }

        // Q3: a bare inline constructor `Vec3(x: …)` — its own class (so an inline ctor works as an
        // operator operand). Checked before _funcs since a class name is never a function.
        if (inv->identifier && inv->identifier->value && !inv->expression
            && (!inv->identifier->qualifier || inv->identifier->qualifier->empty())) {
            std::string rn = resolveUserName(*inv->identifier->value, inv->identifier->qualifier);
            if (isClass(rn) && _classes.count(rn) && !_classes[rn].isIntrinsicColl) return rn;
        }
        // Free / qualified function call — its C return type, if that names a class.
        if (inv->identifier && inv->identifier->value) {
            auto f = _funcs.find(resolveFunc(*inv->identifier->value, inv->identifier->qualifier));
            if (f != _funcs.end() && isClass(f->second.retCType)) return f->second.retCType;
        }
        return "";
    }

    // a user-operator result carries the operator's return type, so a NESTED operator
    // (`a + b + c`, `-a + b`, `(a + b) * s`) resolves and the enclosing operator can be found.
    if (auto* be = dynamic_cast<BinaryExpressionNode*>(n))
        return operatorResultClass(be->token, /*binary*/1, be->LHS, be->RHS);
    if (auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(n))
        return operatorResultClass(su->token, /*unary*/0, su->expression, nullptr);
    if (auto* pr = dynamic_cast<PreIncrDecrNode*>(n))
        return operatorResultClass(pr->token, 0, pr->expression, nullptr);
    if (auto* po = dynamic_cast<PostIncrDecrNode*>(n))
        return operatorResultClass(po->token, 0, po->expression, nullptr);
    return "";
}

// the class an operator expression evaluates to = the resolved operator's return type
// (a class), else "" (a primitive result like a comparison's `bool`, or no matching operator).
std::string CEmitter::operatorResultClass(int opToken, int arity, SharedExpression lhs, SharedExpression rhs)
{
    std::string lc = exprClass(lhs);
    ClassInfo* owner = nullptr;
    MethodInfo* mi = nullptr;
    if (arity == 0) {   // unary — `op_<sym>` on the operand type
        std::string opName = operatorMangle(opToken, 0);
        if (opName.empty() || !userOperandType(lc, _classes)) return "";
        mi = findMethod(&_classes[lc], opName, &owner);
        if (!(mi && mi->isOperator)) return "";
    } else {            // binary — resolve by operand types (mirrors the dispatch)
        mi = findBinaryOperator(opToken, lc, exprClass(rhs), &owner);
        if (!mi) return "";
    }
    ScopedStr _ts(_thisType, owner ? owner->name : lc);   // a `This` return type resolves to the operator's owner
    std::string rt = cType(mi->returnType);
    return isClass(rt) ? rt : "";
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
            unsupported("cannot access a field through Weak<T>; call .tryUpgrade()", ma->line);
            return field;
        }
        std::string T = _classes[cls].collElemClass;
        std::string basePath;
        ClassInfo* owner = findFieldOwner(&_classes[T], field);
        if (owner) { basePath = basePathTo(&_classes[T], owner); checkFieldAccess(owner, field, ma->line); }
        return "(" + emitExpression(ma->expression) + ").ptr->" + basePath + field;
    }
    std::string basePath;
    if (!cls.empty() && _classes.count(cls)) {
        ClassInfo* owner = findFieldOwner(&_classes[cls], field);
        if (owner) { basePath = basePathTo(&_classes[cls], owner); checkFieldAccess(owner, field, ma->line); }
        else {
            // the field is NOT on the wrapper itself — auto-deref via a `Deref<T>` contract to the
            // pointee: `w.field` -> `Cls__deref(&(w))->[base]field` (a T*). (Only when not on `cls`.)
            std::string dtgt = derefTarget(cls);
            ClassInfo* fo = dtgt.empty() || !_classes.count(dtgt) ? nullptr : findFieldOwner(&_classes[dtgt], field);
            if (fo) {
                ClassInfo* dc = nullptr;
                MethodInfo* dref = findMethod(&_classes[cls], "deref", &dc);
                std::string bp = basePathTo(&_classes[dtgt], fo);
                checkFieldAccess(fo, field, ma->line);
                if (dref) return dref->cName + "(&(" + emitExpression(ma->expression) + "))->" + bp + field;
            }
        }
    }
    if (dynamic_cast<ThisAccessNode*>(ma->expression.get())) {
        if (_inStaticMethod) unsupported("a `static` method has no `this`", ma->line);
        return "self->" + basePath + field;
    }
    // Emit the receiver as a PLACE: for an indexed-element receiver this is `(*NAME__at(&a,i)).field`
    // (a real lvalue), so `arr[i].field = v` is a valid write — not `(__get(...)).field = v` (assigning
    // to a member of an rvalue). `emitPlace` is identity for a name/`this`/nested member access.
    return "(" + emitPlace(ma->expression) + ")." + basePath + field;
}

// Dispatch a method call on a receiver of static class `clsName`.
std::string CEmitter::emitDispatch(const std::string& clsName, const std::string& recvPtr,
                                   const std::string& method, SharedArgumentList args, int srcLine)
{
    if (!_classes.count(clsName)) { unsupported("call on unknown class", srcLine); return "0"; }
    ClassInfo* owner = nullptr;
    MethodInfo* mi = findMethod(&_classes[clsName], method, &owner);
    if (!mi) {
        // Auto-deref fallback: `method` is not on `clsName`, but `clsName` implements `Deref<T>` and the
        // method IS on the pointee `T`. Resolve on `T` and call through `clsName__deref(recvPtr)` — a
        // `T*` — as the receiver. Recursive, so `A: Deref<B>, B: Deref<T>` chains transitively.
        std::string tgt = derefTarget(clsName);
        ClassInfo* dc = nullptr;
        MethodInfo* dref = tgt.empty() ? nullptr : findMethod(&_classes[clsName], "deref", &dc);
        if (dref && _classes.count(tgt) && findMethod(&_classes[tgt], method, nullptr)) {
            std::string derefed = dref->cName + "((" + clsName + "*)" + recvPtr + ")";
            return emitDispatch(tgt, derefed, method, args, srcLine);
        }
        unsupported("unknown method", srcLine); return "0";
    }
    if (!mi->isIntrinsic) canAccess(owner, mi->visibility, method, srcLine);

    if (mi->isVirtual) {
        // Devirtualize when the concrete target is unique for every possible dynamic type: a
        // `final` receiver class (no subclass), a `final` method (unoverridable), or a virtual
        // method no class overrides anywhere in the program (whole-program view — no LTO needed).
        // Then fall through to the direct call below. Otherwise dispatch through the vptr.
        ClassInfo& sc = _classes[clsName];
        bool monomorphic = sc.isFinalClass || mi->isFinal
                         || !_overriddenSlots.count(std::make_pair(sc.vtableRoot, method));
        if (!monomorphic) {
            // Dynamic dispatch through the vptr (at offset 0 via the vtable root).
            const std::string& root = sc.vtableRoot;
            std::string slotOwner = owner->name;
            auto rit = _rootVtables.find(root);
            if (rit != _rootVtables.end())
                for (auto& s : rit->second) if (s.name == method) { slotOwner = s.owner; break; }
            std::string self = "(" + slotOwner + "*)" + recvPtr;
            std::string vptr = "((" + root + "*)" + recvPtr + ")->__vptr";
            return emitReorderedCall(vptr + "->" + method, self, mi->params, args, srcLine);
        }
    }
    // Static call; upcast self to the declaring class (offset-0 valid). Also the devirtualized path.
    std::string self = "(" + owner->name + "*)" + recvPtr;
    return emitReorderedCall(mi->cName, self, mi->params, args, srcLine);
}

// obj.method(args) — the member-access callee form (incl. this.method()).
// A "stable" string reference: re-evaluating it is side-effect-free and yields the same bytes+length —
// a variable, a literal, `this`, or a field access rooted in one of those. Gates `.chars()`/`.split()`,
// which read their receiver (and separator) twice and BORROW the bytes for the whole loop; a call/operator
// rvalue (owned or side-effecting) would desync the two reads (OOB) and leak/dangle, so it must be bound
// to a local first.
static bool isStableStringRef(ASTNode* n)
{
    if (!n) return false;
    if (dynamic_cast<IdentifierNode*>(n) || dynamic_cast<StringNode*>(n) || dynamic_cast<ThisAccessNode*>(n))
        return true;
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) return isStableStringRef(ma->expression.get());
    return false;
}

// Does an invocation used AS A RECEIVER return a place (a `fn ref T` borrow) rather than a fresh owned
// value? A place receiver must NOT be materialized + dropped (it borrows — dropping a copy would
// double-free); a value receiver (ctor / value-returning method or free fn) must be, else its heap leaks.
// `isPlaceReturn` (on MethodInfo + FuncSig, set for `fn ref T`) is the fresh-value-vs-borrow discriminator.
bool CEmitter::invocationReturnsPlace(InvocationNode* iv)
{
    if (!iv) return false;
    if (iv->identifier && iv->identifier->value) {           // bare call: a free fn (or an inline ctor)
        auto fit = _funcs.find(resolveFunc(*iv->identifier->value, iv->identifier->qualifier));
        return fit != _funcs.end() && fit->second.isPlaceReturn;   // a ctor isn't in _funcs -> false (a value)
    }
    if (auto* ma = dynamic_cast<MemberAccessNode*>(iv->expression.get())) {   // method call `recv.m()`
        std::string rcls = exprClass(ma->expression);
        if (isSmartPtrClass(rcls)) rcls = _classes[rcls].collElemClass;       // a method lives on the pointee
        std::string m = (ma->identifier && ma->identifier->value) ? *ma->identifier->value : "";
        if (rcls.empty() || !_classes.count(rcls) || m.empty()) return false;
        ClassInfo* owner = nullptr;
        MethodInfo* mi = findMethod(&_classes[rcls], m, &owner);
        return mi && mi->isPlaceReturn;
    }
    return false;
}

std::string CEmitter::emitMethodCall(InvocationNode* call, MemberAccessNode* recv)
{
    std::string method = (recv->identifier && recv->identifier->value) ? *recv->identifier->value : "";
    SharedExpression receiver = recv->expression;
    std::string cls = exprClass(receiver);
    // A `string` receiver that `exprClass` can't name — a bare literal (`"x".trim()`) or a `+` chain
    // (`(a + b).length()`) — still classes as the `string` primitive. Localizes string knowledge to
    // `exprIsString`; the rvalue is made addressable below (addrOfOperand), like `.concat()` composes.
    if (cls.empty() && exprIsString(receiver)) cls = "kama_string";
    // Iterator invalidation: growing a collection (`add`) while a `foreach` iterates it reallocs the
    // buffer and dangles the loop's element refs (a use-after-free for a `ref` binding). Reject it — a
    // local rule (the loop already names the collection), no lifetimes; collect + append after the loop.
    if (method == "add" && !_foreachColls.empty() && !cls.empty() && _classes.count(cls)
        && _classes[cls].isIntrinsicColl) {
        std::string r = rootBinding(receiver);
        bool iterated = false;
        if (!r.empty()) for (auto& c : _foreachColls) if (c == r) { iterated = true; break; }
        if (iterated)
            unsupported(("cannot grow `" + r + "` while iterating it in a `foreach` (it would invalidate "
                         "the loop) — collect the additions and append them after the loop").c_str(), call->line);
    }
    // a non-const method may not be called on a const receiver (deep const).
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
    // A PRIMITIVE receiver with a retroactive conformance (`implements Hashable for int32`): the method's
    // `this` is the SCALAR itself, passed BY VALUE. `exprClass` is "" for a primitive, so recover the
    // receiver's C type from `_localCTypes` (params/locals record it there). Resolve on `_primConformances`;
    // the call is a plain free function `int32_t__hash(k)` (no pointer, no vtable).
    {
        std::string primTy = cls;
        if (primTy.empty())
            if (auto* rid = dynamic_cast<IdentifierNode*>(receiver.get()))
                if (rid->value && _localCTypes.count(*rid->value)) primTy = _localCTypes[*rid->value];
        auto pit = _primConformances.find(primTy);
        if (pit != _primConformances.end()) {
            ClassInfo* powner = nullptr;
            MethodInfo* pmi = findMethod(&pit->second, method, &powner);
            if (!pmi) { unsupported(("unknown method `" + method + "` on `" + primTy + "`").c_str(), call->line); return "0"; }
            return emitReorderedCall(pmi->cName, emitExpression(receiver), pmi->params, call->args, call->line);
        }
    }
    if (cls.empty() || !_classes.count(cls)) {
        unsupported("method call on unresolved receiver", call->line);
        return "0";
    }
    // `.chars()`/`.split()` build a BORROWING iterator (Chars/Split) as a compound literal that reads each
    // operand TWICE (.data/.len) and borrows its bytes for the WHOLE loop. An owned/side-effecting rvalue
    // operand (`s.trim()`, `a + b`) is materialized ONCE into a scope-dtor'd temp (stable + read-once,
    // dropped after the loop — the A2 receiver-drop pattern), so it neither desyncs the two reads nor
    // dangles; a stable var/literal/field/`this` keeps the zero-copy path. Handled BEFORE `recvPtr` so an
    // owned receiver isn't ALSO hoisted by the general string path below (which would double-evaluate it).
    if (cls == "kama_string" && (method == "chars" || method == "split")) {
        auto stableBorrow = [&](SharedExpression e, const char* what) -> std::string {
            std::string t = hoistStringTemp(e);      // owned rvalue -> scope-dtor'd temp; "" if stable OR no slot
            if (!t.empty()) return t;
            if (!isStableStringRef(e.get())) {
                unsupported((std::string("`.") + method + "()` borrows and re-reads its " + what + " across "
                             "the whole loop, so it must be a stable reference (a variable, literal, field, "
                             "or `this`) — bind an expression like `s.trim()` or `a + b` to a local first").c_str(),
                            call->line);
                return "";                            // failure sentinel (a valid ref never emits "")
            }
            return emitExpression(e);                 // stable ref -> direct (zero-copy, unchanged)
        };
        if (!_synthCtx) _synthCtx = std::make_shared<CodeGenContext>(std::make_shared<std::string>("<generic>"));
        if (method == "chars") {
            std::string sv = stableBorrow(receiver, "receiver");
            if (sv.empty()) return "0";
            std::string charsC = cType(std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Chars")));
            return "((" + charsC + "){ (uint8_t*)(" + sv + ").data, (int32_t)(" + sv + ").len, 0 })";
        }
        // split — borrows the receiver AND the separator; materialize each independently.
        SharedExpression sepExpr = (call->args && call->args->size() == 1) ? (*call->args)[0]->expression
                                                                          : SharedExpression();
        std::string sv = stableBorrow(receiver, "receiver");
        if (sv.empty()) return "0";
        std::string sep = sepExpr ? stableBorrow(sepExpr, "separator") : std::string("kama_string_lit(\"\", 0)");
        if (sep.empty()) return "0";
        std::string splitC = cType(std::make_shared<IdentifierNode>(*_synthCtx, std::make_shared<std::string>("Split")));
        return "((" + splitC + "){ (uint8_t*)(" + sv + ").data, (int32_t)(" + sv + ").len, (uint8_t*)("
             + sep + ").data, (int32_t)(" + sep + ").len, 0, false })";
    }
    std::string recvPtr;
    if (auto* ea = dynamic_cast<ElementAccessNode*>(receiver.get())) {
        // `list[i].m()` — borrow the element IN PLACE via the bounds-checked `__at`
        // (a T* into the buffer). No copy, no temp; a plain nested call, strictly ISO C.
        std::string coll, recvExpr, idx;
        if (collectionElemAccess(ea, coll, recvExpr, idx))
            recvPtr = coll + "__at(&(" + recvExpr + "), " + idx + ")";
        else
            recvPtr = "&(" + emitExpression(receiver) + ")";
    } else if (cls == "kama_string") {
        // A string receiver may be an rvalue — a chained `s.trim().toLower()`, a `"lit".method()`, or an
        // `(a + b).length()`. An OWNED rvalue is hoisted into a scope-dtor'd temp (so its buffer is freed,
        // not leaked); a literal/lvalue keeps the addressable compound-literal / `&(x)` path.
        std::string t = hoistStringTemp(receiver);
        recvPtr = t.empty() ? addrOfOperand(receiver, "kama_string", call->line) : ("&" + t);
    } else {
        // An OWNED rvalue receiver — an operator result (`(a - b).m()`) or a by-value call/ctor result
        // (`make().m()`, `R().m()`, a method chain `builder.make().use()`) — owns a heap buffer that
        // addrOfOperand's throwaway compound-literal receiver would LEAK. Materialize it into a scope-dtor'd
        // temp so RAII frees it (the general-class analogue of the string-receiver hoist above). Gated to a
        // fresh BY-VALUE rvalue: an operator result, or ANY call (free fn OR method) that returns a fresh
        // owned value. A `fn ref T` place return (a borrow — a `ref` method result, `a[i]`, or a place-
        // returning free fn) is left to addrOfOperand and NEVER dropped, so this can't double-free.
        // `invocationReturnsPlace` (via isPlaceReturn) is the discriminator — it covers method chains
        // (previously excluded → leak) and correctly excludes a `fn ref T` free fn (previously included →
        // would double-free). Pure RAII scope-drop; no lifetime analysis.
        InvocationNode* riv = dynamic_cast<InvocationNode*>(receiver.get());
        bool byValueRvalue = dynamic_cast<BinaryExpressionNode*>(receiver.get())
                             || (riv && !invocationReturnsPlace(riv));
        if (_hoistOK && byValueRvalue && _classes.count(cls) && _classes[cls].destructible) {
            std::string t = "__recv" + std::to_string(_tempCounter++);
            std::string ct = hoistCtorIfInline(receiver);                 // inline ctor -> its own temp init
            if (!ct.empty()) { recordDestructibleLocal(ct, cls); recvPtr = "&" + ct; }
            else {
                _hoisted.push_back(cls + " " + t + " = " + emitExpression(receiver) + ";");
                recordDestructibleLocal(t, cls);
                recvPtr = "&" + t;
            }
        } else {
            recvPtr = addrOfOperand(receiver, cls, call->line);
        }
    }
    std::string callStr = emitDispatch(cls, recvPtr, method, call->args, call->line);
    // a place-returning `fn ref T m(…)` returns a `T*`; deref it so the call is an lvalue EVERYWHERE
    // (read copies out; `m(…) = x` writes through; `m(…).f` / `ref m(…)` / nesting all compose via the
    // existing lvalue paths). `&(*…)` folds, so a chained `a.at(i).at(j)` stays clean ISO C.
    ClassInfo* powner = nullptr;
    MethodInfo* pmi = findMethod(&_classes[cls], method, &powner);
    return (pmi && pmi->isPlaceReturn) ? ("(*" + callStr + ")") : callStr;
}

std::string CEmitter::emitCtorCall(const std::string& cVar, ClassInfo& ci, SharedArgumentList args, int srcLine)
{
    if (ci.isAbstractClass)   // instantiating one crashes on a NULL vtable slot
        unsupported(("cannot instantiate abstract class '" + ci.name
                     + "' (it has an unimplemented method)").c_str(), srcLine);
    if (ci.hasCtor && !ci.isIntrinsicColl)   // private ctor blocks external `new` (intrinsics exempt)
        canAccess(&ci, ci.ctorVisibility, "constructor", srcLine);
    return emitReorderedCall(ci.name + "__ctor", "&" + cVar, ci.ctorParams, args, srcLine);
}

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

// Whole-program symbol table: run the collect passes for every unit (they append
// to the shared maps), then resolve inheritance/vtables/destructibility once.
void CEmitter::collectProgram(const std::vector<SharedCompilationUnit>& userUnits)
{
    // the prelude (Optional/Result) is collected FIRST, in the global namespace (empty scope),
    // so its generic templates register under bare names resolvable unqualified from every file.
    std::vector<SharedCompilationUnit> units;
    if (_preludeUnit) units.push_back(_preludeUnit);
    // The namespaced built-in modules (the smart-ptr triad, std::memory) collect right after the prelude
    // and before user code. Unlike _preludeUnit they are NOT global: ctxOf reads their `namespace`/`export`
    // (below), so they register under `std__memory` and satisfy an explicit `import std::memory` — while an
    // implicit `using std::memory` (added in ctxOf) also makes their names resolve unqualified everywhere.
    for (auto& m : _preludeModuleUnits) if (m) units.push_back(m);
    for (auto& u : userUnits) units.push_back(u);

    // Assign each file its namespace context (public namespace or _F<idx> private)
    // and register public namespaces, before any name resolution.
    for (size_t i = 0; i < units.size(); ++i) {
        if (!units[i]) continue;
        NsCtx ctx;
        if (units[i] == _preludeUnit) { ctx.isPublic = true; }   // empty scope = global (bare names)
        else                          ctx = ctxOf(units[i], (int)i);
        _unitCtx[units[i].get()] = ctx;
        if (ctx.isPublic && !ctx.scope.empty()) _namespaces.insert(ctx.scope);
    }
    // Record each module's PUBLIC SURFACE from its top-of-file `export { … };` manifest. Everything
    // unlisted is module-private and cannot be pulled in by another module's per-symbol `import`
    // (enforced below). A listed name is validated against real declarations after collect.
    for (auto& u : units) {
        if (!u || u == _preludeUnit || !u->exportList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& name : *u->exportList)
            if (name) _exported.insert(qualify(*name));
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
                    // `type contract` pre-registers as an interface name, not a class. A GENERIC
                    // contract pre-registers in _genericContracts (like a generic type below), so an
                    // early `cType(Iterator<int32>)` (collectSignatures runs first) mangles to the
                    // instance name rather than resolving the bare template stub.
                    if (cd->typeKind && *cd->typeKind == "contract") {
                        std::string n = qualify(*cd->name->value);
                        if (cd->typeParams && !cd->typeParams->empty()) _genericContracts[n].name = n;
                        else                                            _interfaces[n].name = n;
                    } else if (cd->typeParams && !cd->typeParams->empty()) {
                        // a generic TYPE template pre-registers in _genericTypes, NOT _classes
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
                if (ed->identifier && ed->identifier->value) {
                    std::string n = qualify(*ed->identifier->value);
                    // pre-register a tagged/generic enum where its real home is (a class-like
                    // type / a generic template), NOT _enums — else emitEnum would emit a bogus enum.
                    if (ed->typeParams && !ed->typeParams->empty()) _genericTypes[n].name = n;
                    else if (enumIsTagged(ed))                      _classes[n].name = n;
                    else                                            _enums[n].name = n;
                }
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
    linkContracts();    // merge refined-parent methods into each contract before vtables are built
    buildVtables();
    resolveFriends();   // after all classes/functions are registered
    // Pre-scan retroactive `implements C for T` blocks into _retroConformances (target cType -> contracts)
    // BEFORE collectCollections. A `Map<string, V>` local drives a generic-type-arg bound check DURING
    // collection, which is earlier than applyRetroactive injects `hash` into `string`; without this the
    // check would falsely reject `string: Hashable`. The real methods + coherence are still handled below.
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& decl : *u->codeDeclarationList) {
            auto* ri = dynamic_cast<RetroactiveImplNode*>(decl.get());
            if (ri && ri->contract && ri->contract->value && ri->target && ri->target->value)
                _retroConformances[cType(ri->target)].insert(*ri->contract->value);
        }
    }
    // register collections BEFORE the destructibility fixpoint, so a class whose only
    // owning member is a collection field (`List<T>` etc., no explicit `~dtor`) is correctly seen
    // as a resource (destructible + move-only). computeDestructible then re-derives each
    // collection's elemDestructible from the final class destructibility.
    for (auto& u : units)
        if (u && u->codeDeclarationList) { _nsCtx = _unitCtx[u.get()]; collectCollections(u); }
    // Retroactive contract conformance — `implements C for T { … }` blocks. Runs AFTER collectCollections
    // so a primitive/collection target (`string` → `kama_string`) already has its ClassInfo. Inject each
    // block's methods into the target's ClassInfo (mangled `Target__method`, so the existing dispatch +
    // method emission pick them up unchanged) and record the conformance. Coherence: reject a duplicate
    // impl of the same contract for the same type, and reject an impl method that clobbers an existing
    // method — under the whole-program view a duplicate/conflict is directly visible, which is how the
    // orphan rule (declare the contract or the type) is enforced here.
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& decl : *u->codeDeclarationList) {
            auto* ri = dynamic_cast<RetroactiveImplNode*>(decl.get());
            if (!ri || !ri->contract || !ri->contract->value || !ri->target || !ri->target->value) continue;
            std::string contract = *ri->contract->value;
            std::string tkey = cType(ri->target);   // string→kama_string, a user type→its mangled name
            auto ti = _classes.find(tkey);
            if (ti == _classes.end()) {
                // A PRIMITIVE target (`int32`, …): it has NO ClassInfo, and it must NOT get one — every
                // "is this a user type?" test keys on `_classes`, so an entry there would break int
                // operators/ownership. Hang the injected methods on a SEPARATE `_primConformances` registry
                // (scalar-receiver: `this` is the value itself). Method calls + bound checks consult it.
                if (ri->target->builtInVal != 0) {
                    ClassInfo& pci = _primConformances[tkey];
                    pci.name = tkey;
                    pci.kind = TypeKind::Value;
                    pci.isScalarRecv = true;
                    ti = _primConformances.find(tkey);
                } else {
                    unsupported(("`implements " + contract + " for " + *ri->target->value +
                                 "` — unknown target type").c_str(), ri->line);
                    continue;
                }
            }
            ClassInfo& tci = ti->second;
            for (auto& ex : tci.interfaces)
                if (ex == contract) { unsupported(("`" + tkey + "` already implements `" + contract
                                                   + "`").c_str(), ri->line); break; }
            if (ri->members)
                for (auto& m : *ri->members) {
                    auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get());
                    if (!md || !md->name || !md->name->value) continue;
                    std::string mname = *md->name->value;
                    if (tci.methods.count(mname)) {
                        unsupported(("retroactive `implements " + contract + " for " + tkey + "`: method `"
                                     + mname + "` conflicts with an existing method on the type").c_str(), md->line);
                        continue;
                    }
                    MethodInfo mi;
                    mi.cName        = tkey + "__" + mname;
                    mi.returnType   = md->returnType;
                    mi.params       = paramSigsOf(md->params);
                    mi.node         = md;
                    mi.isConst      = md->isConst;
                    mi.isPlaceReturn = md->isRef;
                    mi.isStatic     = modHas(md->modifiers, "static");  // a static factory (no `self`) — e.g. `deserialize`
                    mi.visibility   = Visibility::Public;   // a contract's methods are public
                    mi.isRetro      = true;                 // emitted static-inline in the header (below)
                    tci.methods[mname] = mi;
                }
            tci.interfaces.push_back(contract);
            tci.retroInterfaces.push_back(contract);   // static dispatch only — no fat-pointer vtable
            // Completeness: the impl must supply every method the contract requires.
            if (const std::vector<InterfaceMethod>* need = contractMethods(contract))
                for (auto& nm : *need)
                    if (!tci.methods.count(nm.name))
                        unsupported(("`implements " + contract + " for " + tkey + "` is missing method `"
                                     + nm.name + "` required by the contract").c_str(), ri->line);
        }
    }
    // discover generic-function instantiations after collections (a specialization may use
    // one) and before the destructibility fixpoint. Runs with _typeSubst empty (concrete mangles).
    for (auto& u : units)
        if (u && u->codeDeclarationList) { _nsCtx = _unitCtx[u.get()]; collectGenericInsts(u); }
    computeDestructible();
    computeReachesPointer();   // serialization mode gate (by-value vs. graph)
    computeGraphNodeTypes();   // graph node closure + Shared<T> deserialize return types (Phase D)

    // Contract kind-gate enforcement: a class may `implements` a contract only if its kind (value/resource)
    // is permitted by the contract's `for` clause.
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.kind != TypeKind::Value && ci.kind != TypeKind::Resource) continue;
        for (auto& base : ci.interfaces) {
            auto it = _interfaces.find(base);
            if (it == _interfaces.end()) continue;   // a base class / unresolved — not a kind-gated contract
            if (ci.kind == TypeKind::Value && !it->second.allowsValue)
                unsupported(("`" + kv.first + "` is a `value`, but contract `" + base + "` is declared "
                             "`for resource` — a value can't implement it").c_str(), ci.node ? ci.node->line : 0);
            if (ci.kind == TypeKind::Resource && !it->second.allowsResource)
                unsupported(("`" + kv.first + "` is a `resource`, but contract `" + base + "` is declared "
                             "`for value` — a resource can't implement it").c_str(), ci.node ? ci.node->line : 0);
        }
    }

    // Validate export manifests: a name in `export { … };` must be a real top-level declaration in
    // that same file (catches typos + enforces per-file surfaces for directory-modules).
    for (auto& u : units) {
        if (!u || u == _preludeUnit || !u->exportList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& name : *u->exportList) {
            if (!name) continue;
            std::string q = qualify(*name);
            bool exists = _classes.count(q) || _enums.count(q) || _interfaces.count(q) || _funcs.count(q)
                       || _genericTypes.count(q) || _genericContracts.count(q) || _sigs.count(q);
            if (!exists)
                unsupported(("export list names `" + *name + "` but there is no such top-level declaration in this module").c_str(),
                            u->nameSpace && u->nameSpace->name ? u->nameSpace->name->line : 0);
        }
    }

    // Enforce module privacy: a per-symbol `import a::b::{X}` may bind only an `export`ed X
    // (an unmarked or missing symbol is not importable — "does not export").
    for (auto& u : units) {
        if (!u || !u->importDeclarationList) continue;
        for (auto& imp : *u->importDeclarationList) {
            if (!imp || !imp->symbols || !imp->modulePath || imp->modulePath->empty()) continue;
            std::string path;
            for (auto& s : *imp->modulePath) path += (path.empty() ? "" : ".") + *s;
            std::string mod = mangleNs(path);
            for (auto& sym : *imp->symbols) {
                if (!sym || !sym->identifier || !sym->identifier->value) continue;
                if (!_exported.count(mod + "__" + *sym->identifier->value))
                    unsupported(("module `" + path + "` does not export `" + *sym->identifier->value + "`").c_str(), imp->line);
            }
        }
    }
}

// All DECLARATIONS (the shared header): typedefs, enums, struct/vtable types,
// interface types, collection/smart-pointer macros, and every prototype.
void CEmitter::emitHeaderContent(const std::vector<SharedCompilationUnit>& units)
{
    std::vector<ClassInfo*> classes = topoOrderClasses();       // base before derived (no generic instances)
    std::vector<ClassInfo*> ordered = unifiedStructOrder();     // all struct bodies, by-value-dep order

    // Forward typedefs so bodies can reference each other and any struct (incl. generic instances).
    for (ClassInfo* ci : ordered) {
        if (ci->isExternStruct) continue;
        if (ci->isIntrinsicColl) {
            // A `Fixed<T,N>` gets its full `KAMA_FIXED_TYPE` later (by-value struct order); forward-
            // declare it HERE so a pointer-storing collection (`List<Fixed<...>>`, emitted in the early
            // types pass) can name it. The later full typedef is a legal C11 redeclaration. Other
            // collections already emit a full `_TYPE` typedef in that early pass.
            if (ci->collKind == CollKind::Fixed)
                *_out << "typedef struct " << ci->name << " " << ci->name << ";\n";
            continue;
        }
        *_out << "typedef struct " << ci->name << " " << ci->name << ";\n";
        if (ci->hasVtable && ci->vtableRoot == ci->name)
            *_out << "typedef struct " << ci->name << "_vtable " << ci->name << "_vtable;\n";
    }
    for (auto& kv : _interfaces) {
        *_out << "typedef struct " << kv.first << "_vtbl " << kv.first << "_vtbl;\n";
        *_out << "typedef struct " << kv.first << " " << kv.first << ";\n";
    }
    if (!ordered.empty() || !_interfaces.empty()) *_out << "\n";

    // Function-pointer signature typedefs: `typedef ret (*Name)(params);`.
    // After the class forward-typedefs so a signature may take/return a class.
    for (auto& kv : _sigs) {
        SigInfo& si = kv.second;
        *_out << "typedef " << si.retCType << " (*" << si.cName << ")(";
        if (si.params.empty()) *_out << "void";
        for (size_t i = 0; i < si.params.size(); ++i) {
            const ParamSig& p = si.params[i];
            // const pointer params -> `const T*` (FFI). className already ends
            // in `*` for a Ptr<T>/Ptr; a const-ref class param keeps its self mutable.
            bool constPtr = p.isConst && !p.className.empty() && p.className.back() == '*';
            *_out << (i ? ", " : "") << (constPtr ? "const " : "") << p.className << (p.byRef ? "*" : "");
        }
        *_out << ");\n";
    }
    if (!_sigs.empty()) *_out << "\n";

    // Set the name-resolution scope from the type/file being emitted. `aliases` carries the
    // declaring file's per-symbol imports (a field/base typed with an imported generic needs them).
    auto scopeOf = [&](const std::string& scope, const std::vector<std::string>& usings,
                       const std::map<std::string, std::string>& aliases = std::map<std::string, std::string>()) {
        _nsCtx = NsCtx{}; _nsCtx.scope = scope; _nsCtx.usings = usings; _nsCtx.symbolAliases = aliases;
    };

    for (auto& kv : _enums) { scopeOf(kv.second.scope, kv.second.usings); emitEnum(kv.second); }

    // Collection/smart-pointer STRUCT typedefs (the `_TYPE` half) — before class struct
    // bodies, so a class may hold a collection/smart-pointer BY VALUE as a field. They
    // store only `T*`, so the element being forward-declared (above) is enough.
    emitCollectionDefs(/*typesOnly=*/true);

    // struct bodies (normal classes + generic instances + tagged unions) in ONE by-value-
    // dependency order — so any struct may hold another user struct BY VALUE (`Box<Rock>`, a union
    // carrying a `Vec2`, a class holding a class). A generic instance routes through emitGenericTypeInst
    // (which binds its _typeSubst/_nsCtx for its `T`-typed fields); a normal class/variant through
    // emitStruct. The emission ORDER places every by-value dependency before its holder.
    for (ClassInfo* ci : ordered) {
        if (ci->isExternStruct) continue;
        if (ci->isIntrinsicColl) {
            // A `Fixed<T,N>` embeds its element BY VALUE, so — unlike the pointer-storing collections
            // (whose `_TYPE` went out above) — its struct typedef must land HERE, after the element's
            // struct body (this loop is the by-value-dependency order). Other collections are skipped.
            if (ci->collKind == CollKind::Fixed) {
                CollectionInfo& info = _collections[ci->name];
                *_out << "KAMA_FIXED_TYPE(" << info.elemCType << ", " << info.constValue
                     << ", " << info.cName << ")\n";
            }
            continue;   // macro / header provides it
        }
        if (ci->isGenericInst) {
            emitGenericTypeInst(_genericTypeInsts[ci->name], /*phase=*/0);   // body-only (forward split out)
        } else {
            scopeOf(ci->scope, ci->usings, ci->symbolAliases);
            if (ci->hasVtable && ci->vtableRoot == ci->name) emitVtableType(*ci);
            emitStruct(*ci);   // dispatches to emitVariantStruct for a union
        }
    }
    for (auto& kv : _interfaces) { scopeOf(kv.second.scope, kv.second.usings, kv.second.symbolAliases); emitInterfaceTypes(kv.second); }

    // Forward-declare each class-interface vtable (`C__as_I`) — the definitions have external linkage (see
    // emitClassInterfaceVtables) so binding a concrete to a contract works across module boundaries.
    for (ClassInfo* ci : classes) {
        if (ci->isIntrinsicColl || ci->isExternStruct) continue;
        if (ci->isGenericInst) continue;   // a generic instance's vtables are emitted `static` inline (below), no extern decl
        for (auto& ifn : ci->interfaces) {
            bool retro = false;
            for (auto& r : ci->retroInterfaces) if (r == ifn) { retro = true; break; }
            if (retro) continue;
            auto it = _interfaces.find(ifn);
            if (it == _interfaces.end()) continue;
            *_out << "extern const " << it->second.name << "_vtbl " << ci->name << "__as_" << it->second.name << ";\n";
        }
    }

    // Element destructor prototypes the collection/smart-pointer macros call, then the
    // macros themselves, then class prototypes — so a method (or any class member)
    // can pass a collection/smart-pointer wrapper BY VALUE in its signature, the wrapper
    // type being complete by then. The dtor protos are re-declared (identically, harmless)
    // by emitClassPrototypes. Free-function prototypes follow (they may use a wrapper too).
    for (ClassInfo* ci : classes)
        if (!ci->isIntrinsicColl && !ci->isExternStruct && ci->destructible)
            *_out << "void " << ci->name << "__dtor(" << ci->name << "* self);\n";
    // a collection of `Copyable` elements deep-copies via the element's `copy()`, so its
    // prototype must precede the `_FUNCS` macro that calls it (re-declared identically by
    // emitClassPrototypes). The C signature is `Elem Elem__copy(Elem* self)` (nullary; paramListC).
    for (ClassInfo* ci : classes)
        if (!ci->isIntrinsicColl && !ci->isExternStruct && ci->copyable)
            *_out << ci->name << " " << ci->name << "__copy(" << ci->name << "* self);\n";
    emitCollectionDefs(/*typesOnly=*/false);   // the `_FUNCS` half (ctor/dtor/methods)
    for (ClassInfo* ci : classes) {
        if (ci->preludeStatic) continue;   // emitted static-inline below (a non-generic prelude type)
        scopeOf(ci->scope, ci->usings, ci->symbolAliases); emitClassPrototypes(*ci);
    }
    emitPolyContractResolvers();   // Phase E: per-contract graph-edge dispatch (after node-helper protos + extern vtbl decls)
    // Retroactive `implements C for T { … }` for a COLLECTION/primitive target (`string` → `kama_string`):
    // the normal per-class emitters early-out for a collection, so emit a non-static PROTOTYPE here — BEFORE
    // the generic-function instances below, which may call it (e.g. a `<K: Hashable>` body calling
    // `k.hash()` monomorphized for `string`). The body lands once in the impl's module `.c`
    // (emitModuleContent). A USER-type target emits through the normal machinery (skipped here).
    // User-unit retro-impls: non-static prototype here; body lands in that unit's `.c` (below). The PRELUDE's
    // retro-impls (collect-only, no home module) are emitted `static inline` in a dedicated pass just after.
    auto emitRetroProtos = [&](const std::vector<SharedCompilationUnit>& us, const char* stat) {
        for (auto& u : us) {
            if (!u || !u->codeDeclarationList) continue;
            _nsCtx = _unitCtx[u.get()];
            for (auto& decl : *u->codeDeclarationList) {
                auto* ri = dynamic_cast<RetroactiveImplNode*>(decl.get());
                if (!ri || !ri->target || !ri->target->value || !ri->members) continue;
                ClassInfo* tcip = retroTargetInfo(cType(ri->target));   // collection ClassInfo OR primitive conformance
                if (!tcip) continue;
                ScopedStr _ts(_thisType, tcip->name);
                for (auto& m : *ri->members) {
                    auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get());
                    if (!md || !md->name || !md->name->value) continue;
                    std::string ret = cType(md->returnType) + (md->isRef ? "*" : "");
                    // a `static` retro method has no implicit `self` receiver param
                    const char* recv = modHas(md->modifiers, "static") ? nullptr : tcip->name.c_str();
                    *_out << stat << ret << " " << tcip->name << "__" << *md->name->value << "("
                          << paramListC(md->params, recv) << ");\n";
                }
            }
        }
    };
    emitRetroProtos(units, "");                                     // user units (`units` excludes the prelude)
    if (_preludeUnit) emitRetroProtos({_preludeUnit}, "static inline ");
    // specialized generic-type instance prototypes (ctor/dtor/method), `static`.
    for (const std::string& m : _genericTypeInstOrder)
        emitGenericTypeInst(_genericTypeInsts[m], /*phase=*/1);
    // Prototypes for kama's OWN free functions. kama never emits prototypes for
    // `extern` C functions: an `extern` decl is purely kama's call signature
    // (name + named params, for lowering) — the C prototype comes from the header
    // you `extern "<…>";` (or one the runtime already includes). This keeps the
    // FFI rule a single explicit sentence and makes redeclaration conflicts
    // impossible (kama can't always spell a C type exactly, e.g. const char*).
    bool any = false;
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        _nsCtx = _unitCtx[u.get()];
        for (auto& decl : *u->codeDeclarationList)
            if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
                if (isExtern(fn) || !fn->block) continue;   // skip extern + signature types
                if (fn->typeParams && !fn->typeParams->empty()) continue;   // template — instantiated below
                emitFunctionPrototype(fn);
                any = true;
            }
    }
    if (any) *_out << "\n";

    // generic-function instantiations — one `static` C function per (template, type-args),
    // in the header so every module can call them (like the collection macros). Forward-declare
    // all, then define, so a generic that calls another (or recurses) resolves.
    if (!_genericInsts.empty()) {
        for (auto& kv : _genericInsts) emitGenericInst(kv.second, /*prototypeOnly=*/true);
        *_out << "\n";
        for (auto& kv : _genericInsts) emitGenericInst(kv.second, /*prototypeOnly=*/false);
    }

    // specialized generic-type instance BODIES (ctor/method/dtor), `static`, in the header.
    for (const std::string& m : _genericTypeInstOrder)
        emitGenericTypeInst(_genericTypeInsts[m], /*phase=*/2);

    // Non-generic prelude types (e.g. Chars): the prelude is collect-only, so no module emits their
    // bodies — emit prototype + definition static-inline here (struct went out above with the others).
    for (auto& kv : _classes) {
        if (!kv.second.preludeStatic || kv.second.methods.empty()) continue;
        scopeOf(kv.second.scope, kv.second.usings, kv.second.symbolAliases);
        _emitStaticClass = true;
        emitClassPrototypes(kv.second);
        emitClassDefinitions(kv.second);
        _emitStaticClass = false;
    }

    // Prelude retro-impl BODIES (e.g. `implements Hashable/Equatable for int32`): the prelude is collect-only,
    // so — like the prelude types above — emit their method bodies `static inline` in the header (prototype
    // already emitted above). This makes the primitive conformances UNIVERSAL: a `<T: Equatable>` bound,
    // `List<int32>.contains`, or an int-keyed `Map` resolves without importing `std::collections`.
    if (_preludeUnit && _preludeUnit->codeDeclarationList) {
        _nsCtx = _unitCtx[_preludeUnit.get()];
        _emitStaticClass = true;
        for (auto& decl : *_preludeUnit->codeDeclarationList) {
            auto* ri = dynamic_cast<RetroactiveImplNode*>(decl.get());
            if (!ri || !ri->target || !ri->target->value || !ri->members) continue;
            ClassInfo* tcip = retroTargetInfo(cType(ri->target));
            if (!tcip) continue;
            ScopedStr _ts(_thisType, tcip->name);
            for (auto& m : *ri->members) {
                auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get());
                if (!md || !md->name || !md->name->value) continue;
                std::string ret = cType(md->returnType) + (md->isRef ? "*" : "");
                line(md->line);
                _returnIsPlace = md->isRef;
                emitMethodOrCtorBody(tcip->name + "__" + *md->name->value, ret.c_str(),
                                     md->params, md->body, *tcip, false, md->isConst,
                                     modHas(md->modifiers, "static"));
                _returnIsPlace = false;
            }
        }
        _emitStaticClass = false;
    }
}

// This file's DEFINITIONS: its classes' vtable instances + interface vtables +
// method/ctor/dtor bodies, then its free-function bodies. Prototypes for anything
// referenced across files live in the shared header.
void CEmitter::emitModuleContent(SharedCompilationUnit unit)
{
    _nsCtx = _unitCtx[unit.get()];   // resolve this file's body references in its scope
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
    // Retroactive-impl method BODIES for a collection/primitive target (`string` → `kama_string`): the
    // normal class machinery early-outs for a collection, so the body lands here, non-static (its prototype
    // is in the shared header). A user-type target already emitted through emitClassDefinitions above.
    for (auto& decl : *unit->codeDeclarationList) {
        auto* ri = dynamic_cast<RetroactiveImplNode*>(decl.get());
        if (!ri || !ri->target || !ri->target->value || !ri->members) continue;
        std::string tkey = cType(ri->target);
        ClassInfo* tcip = retroTargetInfo(tkey);   // collection ClassInfo OR primitive conformance
        if (!tcip) continue;
        ClassInfo& tci = *tcip;
        ScopedStr _ts(_thisType, tci.name);
        for (auto& m : *ri->members) {
            auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get());
            if (!md || !md->name || !md->name->value) continue;
            std::string ret = cType(md->returnType) + (md->isRef ? "*" : "");
            line(md->line);
            _returnIsPlace = md->isRef;
            emitMethodOrCtorBody(tci.name + "__" + *md->name->value, ret.c_str(),
                                 md->params, md->body, tci, false, md->isConst,
                                 modHas(md->modifiers, "static"));
            _returnIsPlace = false;
        }
    }
    for (auto& decl : *unit->codeDeclarationList) {
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
            if (fn->typeParams && !fn->typeParams->empty()) continue;   // template — instantiations live in the header
            if (!isExtern(fn) && fn->block) emitFunction(fn);   // skip signature types (no body)
        } else if (dynamic_cast<ClassDeclarationNode*>(decl.get())) {
            // emitted above
        } else if (auto* ed = dynamic_cast<EnumDeclarationNode*>(decl.get())) {
            // a non-generic tagged union's dtor DEFINITION lives in its home module (its struct +
            // prototype are in the header). Generic-enum instances are emitted static-inline in the header.
            if (ed->identifier && ed->identifier->value) {
                auto it = _classes.find(qualify(*ed->identifier->value));
                if (it != _classes.end() && it->second.isVariant) {
                    ClassInfo& eci = it->second;
                    if (eci.destructible) emitDtorDefinition(eci);
                    // `@generate` enum serde — bodies land in the home module (protos are in the header via
                    // emitClassPrototypes; classOf skips enums, so emit here alongside the dtor). The unit's
                    // scope is already active (_nsCtx = _unitCtx above), so payload types resolve.
                    if (eci.methods.count("serialize")   && eci.methods["serialize"].isSynthSer)   emitEnumSerializeDefinition(eci);
                    if (eci.methods.count("deserialize") && eci.methods["deserialize"].isSynthDe)   emitEnumDeserializeDefinition(eci);
                }
            }
        } else if (dynamic_cast<IncludeNode*>(decl.get())) {
            // FFI #include — emitted in the header by emitIncludes
        } else if (dynamic_cast<RetroactiveImplNode*>(decl.get())) {
            // `implements C for T { … }` — its methods were injected into T's ClassInfo (applyRetroactive
            // pass) and emit with T's other methods; nothing to emit at this top-level site.
        } else if (decl) {
            unsupported("top-level declaration", decl->line);
            *_out << "\n";
        }
    }
}

// FFI: emit a C `#include` per `extern "<header>";` directive, deduped.
void CEmitter::emitIncludes(const std::vector<SharedCompilationUnit>& units)
{
    std::set<std::string> seen;
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        for (auto& decl : *u->codeDeclarationList)
            if (auto* inc = dynamic_cast<IncludeNode*>(decl.get())) {
                std::string h = inc->header ? *inc->header : "";
                if (h.empty()) continue;
                _externedHeaders.insert(h);   // record for the driver's link hints (e.g. <math.h> -> -lm)
                if (seen.count(h)) continue;
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
    *_out << "/* Generated by kama. Do not edit. */\n";
    *_out << "#include \"kama_runtime.h\"\n";
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

    std::string guard = "KAMA_GEN_";
    for (char c : headerName) guard += (isalnum((unsigned char)c) ? (char)toupper(c) : '_');

    _out = &header;
    header << "/* Generated by kama. Do not edit. */\n";
    header << "#ifndef " << guard << "\n#define " << guard << "\n";
    header << "#include \"kama_runtime.h\"\n";
    emitIncludes(units);        // FFI #include directives (before any type decls)
    emitHeaderContent(units);   // declarations only — no bodies, so no #line needed
    header << "#endif /* " << guard << " */\n";

    for (size_t i = 0; i < units.size(); ++i) {
        _out = moduleStreams[i];
        _sourcePath = sourcePaths[i];   // #line in this module points to its own source
        *_out << "/* Generated by kama. Do not edit. */\n";
        *_out << "#include \"" << headerName << "\"\n\n";
        emitModuleContent(units[i]);
    }
    return _unsupported;
}
