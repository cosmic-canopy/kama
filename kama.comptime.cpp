// kama.comptime.cpp — const-eval 6b-3: the compile-time function (`comptime fn`) subsystem.
//
// A `comptime fn` is a bounded, pure function the compiler RUNS at compile time to bake a `static const`
// scalar or table (a CRC/gamma/trig LUT) into the emitted C — zero runtime cost, sits in .rodata/flash.
// It is comptime-ONLY: never emitted as a C symbol, callable only from a comptime context.
//
// This translation unit is the interpreter — a sibling of the emitter that produces *values*, not C text.
// It is kept as its own TU (defining CEmitter methods) so the eval kernel stays modular: a future
// scripting/REPL backend (GOALS §6/§10) can reuse the walk + value model under a permissive policy, while
// purity/budget are the compile-time policy layered on top here. PURITY is enforced structurally: any node
// the interpreter has no case for yields a clean "unsupported in comptime fn" diagnostic (the C++ constexpr
// model), which automatically rejects new/spawn/unsafe/asm/FFI/strings/pointers/mutable-static reads.

#include "kama.cemit.h"
#include "kama.ast.h"
#include "kama.context.h"
#include "kama.parser.hpp"   // token constants (PLUS, EQEQ, LTLT, …)

#include <cstdio>
#include <sstream>

// Does `name` (with an optional scope qualifier) resolve to a registered `comptime fn`? Used at the
// call-emit sites to reject a runtime-position call with a diagnostic that points at the `comptime`
// constant form. Mirrors the common resolveFunc search (same-module, `using`, prelude) over _comptimeFns;
// an explicit `mod::f()` qualifier that misses simply falls through to the generic unknown-fn diagnostic.
bool CEmitter::isComptimeFnName(const std::string& name, SharedStringList /*qualifier*/, std::string& outKey) const
{
    auto hit = [&](const std::string& k) -> bool {
        auto i = _comptimeFns.find(k);
        if (i != _comptimeFns.end()) { outKey = k; return true; }
        return false;
    };
    if (hit(qualify(name))) return true;                                  // same-module `f()`
    for (auto& u : _nsCtx.usings) if (hit(u + "__" + name)) return true;  // imported via `using`
    return hit(name);                                                     // prelude / global namespace
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

int64_t CEmitter::ctAsI(const CTValue& v) { return v.kind == CTValue::Float ? (int64_t)v.f : v.i; }
double  CEmitter::ctAsF(const CTValue& v) { return v.kind == CTValue::Float ? v.f : (double)v.i; }

// Wrap an integer value to its declared width, sign-extending (signed) or masking (unsigned) — the
// point where narrow-type arithmetic matches the emitted C. Intermediate results compute in full int64
// (C promotes narrow operands to int); truncation happens only here, at a cast and a typed store.
void CEmitter::ctTruncate(CTValue& v)
{
    if (v.kind == CTValue::Float || v.width >= 64 || v.width <= 0) return;
    uint64_t mask = (v.width >= 64) ? ~0ULL : ((1ULL << v.width) - 1);
    uint64_t raw  = (uint64_t)v.i & mask;
    if (v.isSigned) {
        uint64_t signbit = 1ULL << (v.width - 1);
        if (raw & signbit) raw |= ~mask;   // sign-extend into the int64 lane
    }
    v.i = (int64_t)raw;
}

// Map a Kama scalar type to a CTValue "prototype" (kind/width/sign/isF32). Returns false for a type the
// interpreter does not model as a comptime scalar (arrays are handled by the caller in Stage 3).
bool CEmitter::ctTypeInfo(SharedIdentifier type, CTValue& proto)
{
    if (!type || !type->value) return false;
    const std::string& t = *type->value;
    auto setInt = [&](int w, bool s) { proto.kind = CTValue::Int; proto.width = w; proto.isSigned = s; };
    if      (t == "int8")   setInt(8,  true);
    else if (t == "int16")  setInt(16, true);
    else if (t == "int32" || t == "int") setInt(32, true);
    else if (t == "int64")  setInt(64, true);
    else if (t == "uint8")  setInt(8,  false);
    else if (t == "uint16") setInt(16, false);
    else if (t == "uint32") setInt(32, false);
    else if (t == "uint64" || t == "usize") setInt(64, false);
    else if (t == "char")   setInt(32, false);
    else if (t == "bool")   { proto.kind = CTValue::Bool; proto.width = 1; proto.isSigned = false; }
    else if (t == "float32") { proto.kind = CTValue::Float; proto.isF32 = true; }
    else if (t == "float64" || t == "double") { proto.kind = CTValue::Float; proto.isF32 = false; }
    else return false;
    return true;
}

// Coerce a computed value to a declared type (a typed store: the local decl, an assignment target, a
// param bind, a return). Integer stores truncate to the target width; a float32 store rounds through float.
void CEmitter::ctCoerce(const CTValue& proto, CTValue& v)
{
    if (proto.kind == CTValue::Float) {
        double d = ctAsF(v);
        v.kind = CTValue::Float; v.isF32 = proto.isF32;
        v.f = proto.isF32 ? (double)(float)d : d;
        return;
    }
    // Int / Bool target
    int64_t iv = ctAsI(v);
    v.kind = proto.kind; v.width = proto.width; v.isSigned = proto.isSigned; v.f = 0.0;
    v.i = iv;
    ctTruncate(v);
}

bool CEmitter::ctFail(const char* what, int line)
{
    if (!_ctFailed) {
        std::fprintf(stderr, "kama: error: comptime evaluation: %s at %s:%d\n", what, _sourcePath.c_str(), line);
        ++_unsupported;
        _ctFailed = true;
    }
    return false;
}

// Render a scalar comptime value as a C initializer. Integers bake as decimal (with a width-appropriate
// suffix avoided — the declared type on the LHS carries it); float as a literal; bool as true/false.
std::string CEmitter::ctRender(const CTValue& v) const
{
    if (v.kind == CTValue::Bool)  return v.i ? "true" : "false";
    if (v.kind == CTValue::Float) {
        std::ostringstream os;
        os.precision(17);
        os << v.f;
        std::string s = os.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
            s.find("inf") == std::string::npos && s.find("nan") == std::string::npos) s += ".0";
        if (v.isF32) s += "f";
        return s;
    }
    // Int: emit unsigned values as unsigned decimal so a uint64 top-bit value is not a negative literal.
    std::ostringstream os;
    if (!v.isSigned) os << (uint64_t)v.i << "u";
    else             os << v.i;
    return os.str();
}

// Resolve an identifier to a module/type `comptime` constant as a CTValue. Consults, in order: a baked
// comptime-fn-derived value (_comptimeConstVals), a module `comptime` int (_moduleConsts), a type-associated
// `Type::NAME` (_typeConsts / its baked value), and the const-generic/local folds shared with 6b-1/6b-2.
bool CEmitter::ctResolveConst(SharedIdentifier id, CTValue& out)
{
    if (!id || !id->value) return false;
    auto asInt = [&](int64_t val) { out = CTValue{}; out.width = 64; out.isSigned = true; out.i = val; };

    // Type-associated `Type::NAME`.
    if (id->qualifier && !id->qualifier->empty()) {
        auto tq = std::make_shared<StringList>();
        for (size_t i = 0; i + 1 < id->qualifier->size(); ++i) tq->push_back((*id->qualifier)[i]);
        std::string key = resolveUserName(*id->qualifier->back(), tq) + "::" + *id->value;
        auto cv = _comptimeConstVals.find(key);
        if (cv != _comptimeConstVals.end()) { out = cv->second; return true; }
        auto tc = _typeConsts.find(key);
        if (tc != _typeConsts.end() && tc->second.hasValue) { asInt(tc->second.value); return true; }
        return false;
    }

    // Bare name.
    auto cv = _comptimeConstVals.find(qualify(*id->value));
    if (cv != _comptimeConstVals.end()) { out = cv->second; return true; }
    auto mc = _moduleConsts.find(qualify(*id->value));
    if (mc != _moduleConsts.end()) { asInt(mc->second); return true; }
    auto cs = _constSubst.find(*id->value);
    if (cs != _constSubst.end()) { asInt(cs->second); return true; }
    auto lv = _constLocalVals.find(*id->value);
    if (lv != _constLocalVals.end()) { asInt(lv->second); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Expression evaluation
// ---------------------------------------------------------------------------

bool CEmitter::ctEvalExpr(SharedExpression e, CTEnv& env, CTValue& out)
{
    if (_ctFailed) return false;
    if (!e) return ctFail("empty expression", 0);
    if (++_ctSteps > CT_STEP_BUDGET) return ctFail("step budget exceeded — a comptime fn must terminate quickly (raise the loop bound or simplify)", e->line);
    ASTNode* n = e.get();

    // --- literals ---
    if (auto* v = dynamic_cast<Int8Node*>(n))   { out = CTValue{}; out.width = 8;  out.i = v->value; return true; }
    if (auto* v = dynamic_cast<Int16Node*>(n))  { out = CTValue{}; out.width = 16; out.i = v->value; return true; }
    if (auto* v = dynamic_cast<Int32Node*>(n))  { out = CTValue{}; out.width = 32; out.i = v->value; return true; }
    if (auto* v = dynamic_cast<Int64Node*>(n))  { out = CTValue{}; out.width = 64; out.i = (int64_t)v->value; return true; }
    if (auto* v = dynamic_cast<UInt8Node*>(n))  { out = CTValue{}; out.width = 8;  out.isSigned = false; out.i = v->value; return true; }
    if (auto* v = dynamic_cast<UInt16Node*>(n)) { out = CTValue{}; out.width = 16; out.isSigned = false; out.i = v->value; return true; }
    if (auto* v = dynamic_cast<UInt32Node*>(n)) { out = CTValue{}; out.width = 32; out.isSigned = false; out.i = v->value; return true; }
    if (auto* v = dynamic_cast<UInt64Node*>(n)) { out = CTValue{}; out.width = 64; out.isSigned = false; out.i = (int64_t)v->value; return true; }
    if (auto* v = dynamic_cast<Float32Node*>(n)) { out = CTValue{}; out.kind = CTValue::Float; out.isF32 = true;  out.f = v->value; return true; }
    if (auto* v = dynamic_cast<Float64Node*>(n)) { out = CTValue{}; out.kind = CTValue::Float; out.isF32 = false; out.f = v->value; return true; }
    if (auto* v = dynamic_cast<BooleanNode*>(n)) { out = CTValue{}; out.kind = CTValue::Bool; out.width = 1; out.isSigned = false; out.i = v->value ? 1 : 0; return true; }
    if (auto* v = dynamic_cast<CharNode*>(n))    { out = CTValue{}; out.width = 32; out.isSigned = false; out.i = (int64_t)v->value; return true; }

    // --- identifier: a local frame var, else a module/type comptime constant ---
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (id->value) {
            auto it = env.vars.find(*id->value);
            if (it != env.vars.end()) { out = it->second; return true; }
            if (ctResolveConst(std::static_pointer_cast<IdentifierNode>(e), out)) return true;
        }
        return ctFail(("unknown identifier `" + (id->value ? *id->value : std::string("?"))
                       + "` — a comptime fn reads only its params, locals, and `comptime` constants").c_str(), e->line);
    }

    // --- cast ---
    if (auto* c = dynamic_cast<CastNode*>(n)) {
        CTValue v; if (!ctEvalExpr(c->unaryExpression, env, v)) return false;
        CTValue proto;
        if (!ctTypeInfo(c->type, proto))
            return ctFail("cast target is not a comptime scalar type", e->line);
        out = v; ctCoerce(proto, out); return true;
    }

    // --- unary ---
    if (auto* u = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        CTValue v; if (!ctEvalExpr(u->expression, env, v)) return false;
        switch (u->token) {
            case PLUS:  out = v; return true;
            case MINUS:
                if (v.kind == CTValue::Float) { out = v; out.f = -v.f; return true; }
                out = v; out.i = -v.i; ctTruncate(out); return true;
            case TILDE:
                if (v.kind == CTValue::Float) return ctFail("`~` needs an integer operand", e->line);
                out = v; out.i = ~v.i; ctTruncate(out); return true;
            case EXCLAMATION:
                out = CTValue{}; out.kind = CTValue::Bool; out.width = 1; out.isSigned = false;
                out.i = (ctAsI(v) == 0) ? 1 : 0; return true;
            default: return ctFail("unsupported unary operator in comptime fn", e->line);
        }
    }

    // --- short-circuit logical `&&` / `||` ---
    if (auto* l = dynamic_cast<LogicalAndOrNode*>(n)) {
        CTValue lv; if (!ctEvalExpr(l->LHS, env, lv)) return false;
        bool lb = ctAsI(lv) != 0;
        out = CTValue{}; out.kind = CTValue::Bool; out.width = 1; out.isSigned = false;
        if (l->token == ANDAND) { if (!lb) { out.i = 0; return true; } }
        else                    { if (lb)  { out.i = 1; return true; } }   // OROR
        CTValue rv; if (!ctEvalExpr(l->RHS, env, rv)) return false;
        out.i = (ctAsI(rv) != 0) ? 1 : 0; return true;
    }

    // --- binary ---
    if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) {
        CTValue lv, rv;
        if (!ctEvalExpr(b->LHS, env, lv) || !ctEvalExpr(b->RHS, env, rv)) return false;
        bool flt = (lv.kind == CTValue::Float || rv.kind == CTValue::Float);
        auto mkInt = [&](int64_t r) { out = CTValue{}; out.width = 64; out.isSigned = true; out.i = r; };
        auto mkBool = [&](bool r) { out = CTValue{}; out.kind = CTValue::Bool; out.width = 1; out.isSigned = false; out.i = r ? 1 : 0; };
        if (flt) {
            double a = ctAsF(lv), c = ctAsF(rv);
            switch (b->token) {
                case PLUS:  out = CTValue{}; out.kind = CTValue::Float; out.f = a + c; return true;
                case MINUS: out = CTValue{}; out.kind = CTValue::Float; out.f = a - c; return true;
                case STAR:  out = CTValue{}; out.kind = CTValue::Float; out.f = a * c; return true;
                case SLASH: out = CTValue{}; out.kind = CTValue::Float; out.f = a / c; return true;
                case EQEQ:  mkBool(a == c); return true;
                case NOTEQ: mkBool(a != c); return true;
                case LT:    mkBool(a <  c); return true;
                case GT:    mkBool(a >  c); return true;
                case LEQ:   mkBool(a <= c); return true;
                case GEQ:   mkBool(a >= c); return true;
                default:    return ctFail("unsupported float operator in comptime fn", e->line);
            }
        }
        int64_t a = ctAsI(lv), c = ctAsI(rv);
        switch (b->token) {
            case PLUS:    mkInt(a + c); return true;
            case MINUS:   mkInt(a - c); return true;
            case STAR:    mkInt(a * c); return true;
            case SLASH:   if (c == 0 || (a == INT64_MIN && c == -1)) return ctFail("division by zero (or overflow) in comptime fn", e->line); mkInt(a / c); return true;
            case PERCENT: if (c == 0 || (a == INT64_MIN && c == -1)) return ctFail("remainder by zero (or overflow) in comptime fn", e->line); mkInt(a % c); return true;
            case LTLT:    if (c < 0 || c >= 64) return ctFail("shift amount out of range in comptime fn", e->line); mkInt((int64_t)((uint64_t)a << c)); return true;
            case GTGT:    if (c < 0 || c >= 64) return ctFail("shift amount out of range in comptime fn", e->line); mkInt(a >> c); return true;
            case AMP:     mkInt(a & c); return true;
            case BAR:     mkInt(a | c); return true;
            case CARET:   mkInt(a ^ c); return true;
            case EQEQ:    mkBool(a == c); return true;
            case NOTEQ:   mkBool(a != c); return true;
            case LT:      mkBool(a <  c); return true;
            case GT:      mkBool(a >  c); return true;
            case LEQ:     mkBool(a <= c); return true;
            case GEQ:     mkBool(a >= c); return true;
            default:      return ctFail("unsupported binary operator in comptime fn", e->line);
        }
    }

    // --- ternary ---
    if (auto* t = dynamic_cast<TernaryExpressionNode*>(n)) {
        CTValue cnd; if (!ctEvalExpr(t->condition, env, cnd)) return false;
        return ctEvalExpr(ctAsI(cnd) != 0 ? t->LHS : t->RHS, env, out);
    }

    // --- call to another comptime fn ---
    if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        if (!inv->identifier || !inv->identifier->value)
            return ctFail("a comptime fn may call only another `comptime fn` by name", e->line);
        std::string key;
        if (!isComptimeFnName(*inv->identifier->value, inv->identifier->qualifier, key))
            return ctFail(("a comptime fn may call only another `comptime fn` — `" + *inv->identifier->value
                           + "` is not one").c_str(), e->line);
        FunctionDeclarationNode* fn = _comptimeFns[key];
        std::vector<CTValue> args;
        if (inv->args) for (auto& a : *inv->args) {
            CTValue av; if (!a || !ctEvalExpr(a->expression, env, av)) return false;
            args.push_back(av);
        }
        return ctEvalCall(fn, args, e->line, out);
    }

    return ctFail("unsupported expression in comptime fn (no I/O, allocation, pointers, or strings)", e->line);
}

// ---------------------------------------------------------------------------
// Statement evaluation
// ---------------------------------------------------------------------------

CEmitter::CTFlow CEmitter::ctEvalStmt(SharedStatement s, CTEnv& env, CTValue& ret)
{
    if (_ctFailed) return CTFlow::Fail;
    if (!s) return CTFlow::Normal;
    if (++_ctSteps > CT_STEP_BUDGET) { ctFail("step budget exceeded — a comptime fn must terminate quickly", s->line); return CTFlow::Fail; }
    ASTNode* n = s.get();

    if (auto* blk = dynamic_cast<BlockNode*>(n)) {
        if (blk->statements) for (auto& st : *blk->statements) {
            CTFlow f = ctEvalStmt(st, env, ret);
            if (f != CTFlow::Normal) return f;
        }
        return CTFlow::Normal;
    }

    if (auto* d = dynamic_cast<LocalVariableDeclaration*>(n)) {
        CTValue proto;
        if (!ctTypeInfo(d->type, proto)) { ctFail("a comptime fn local must be a scalar type (Stage 3 adds fixed arrays)", s->line); return CTFlow::Fail; }
        if (d->variables) for (auto& v : *d->variables) {
            if (!v || !v->name || !v->name->value) continue;
            CTValue val;
            if (v->initializer) { if (!ctEvalExpr(v->initializer, env, val)) return CTFlow::Fail; }
            else                { val = proto; }   // zero-init
            ctCoerce(proto, val);
            env.vars[*v->name->value] = val;
        }
        return CTFlow::Normal;
    }

    if (auto* d = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        CTValue proto;
        if (!ctTypeInfo(d->type, proto)) { ctFail("a comptime fn const local must be a scalar type", s->line); return CTFlow::Fail; }
        if (d->variables) for (auto& v : *d->variables) {
            if (!v || !v->name || !v->name->value) continue;
            CTValue val;
            if (!v->initializer || !ctEvalExpr(v->initializer, env, val)) return CTFlow::Fail;
            ctCoerce(proto, val);
            env.vars[*v->name->value] = val;
        }
        return CTFlow::Normal;
    }

    if (auto* a = dynamic_cast<AssignmentNode*>(n)) {
        if (a->token != EQ) { ctFail("only plain `=` assignment is supported in a comptime fn (Stage 2)", s->line); return CTFlow::Fail; }
        auto* tgt = dynamic_cast<IdentifierNode*>(a->unaryExpression.get());
        if (!tgt || !tgt->value) { ctFail("comptime assignment target must be a local (Stage 3 adds array-element writes)", s->line); return CTFlow::Fail; }
        auto it = env.vars.find(*tgt->value);
        if (it == env.vars.end()) { ctFail(("assignment to unknown local `" + *tgt->value + "` in comptime fn").c_str(), s->line); return CTFlow::Fail; }
        CTValue val;
        if (!ctEvalExpr(a->expression, env, val)) return CTFlow::Fail;
        ctCoerce(it->second, val);   // store into the target's declared width/kind
        it->second = val;
        return CTFlow::Normal;
    }

    if (auto* iff = dynamic_cast<IfNode*>(n)) {
        CTValue c; if (!ctEvalExpr(iff->booleanExpression, env, c)) return CTFlow::Fail;
        if (ctAsI(c) != 0) return ctEvalStmt(iff->ifStatement, env, ret);
        if (iff->elseStatement) return ctEvalStmt(iff->elseStatement, env, ret);
        return CTFlow::Normal;
    }

    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        for (;;) {
            CTValue c; if (!ctEvalExpr(w->booleanExpression, env, c)) return CTFlow::Fail;
            if (ctAsI(c) == 0) break;
            CTFlow f = ctEvalStmt(w->whileStatement, env, ret);
            if (f == CTFlow::Return || f == CTFlow::Fail) return f;
            if (f == CTFlow::Break) break;
        }
        return CTFlow::Normal;
    }

    if (auto* w = dynamic_cast<DoWhileNode*>(n)) {
        for (;;) {
            CTFlow f = ctEvalStmt(w->doWhileStatement, env, ret);
            if (f == CTFlow::Return || f == CTFlow::Fail) return f;
            if (f == CTFlow::Break) break;
            CTValue c; if (!ctEvalExpr(w->booleanExpression, env, c)) return CTFlow::Fail;
            if (ctAsI(c) == 0) break;
        }
        return CTFlow::Normal;
    }

    if (auto* f = dynamic_cast<ForNode*>(n)) {
        if (f->initializerStatements) for (auto& is : *f->initializerStatements) {
            CTFlow r = ctEvalStmt(is, env, ret);
            if (r == CTFlow::Fail) return r;
        }
        for (;;) {
            if (f->booleanExpression) {
                CTValue c; if (!ctEvalExpr(f->booleanExpression, env, c)) return CTFlow::Fail;
                if (ctAsI(c) == 0) break;
            }
            CTFlow r = ctEvalStmt(f->body, env, ret);
            if (r == CTFlow::Return || r == CTFlow::Fail) return r;
            if (r == CTFlow::Break) break;
            if (f->iteratorStatements) for (auto& is : *f->iteratorStatements) {
                CTFlow ir = ctEvalStmt(is, env, ret);
                if (ir == CTFlow::Fail) return ir;
            }
        }
        return CTFlow::Normal;
    }

    if (dynamic_cast<BreakNode*>(n))    return CTFlow::Break;
    if (dynamic_cast<ContinueNode*>(n)) return CTFlow::Continue;

    if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        if (!r->expression) { ctFail("a comptime fn must return a value", s->line); return CTFlow::Fail; }
        if (!ctEvalExpr(r->expression, env, ret)) return CTFlow::Fail;
        return CTFlow::Return;
    }

    // A bare expression-statement (e.g. a call whose result is discarded).
    if (dynamic_cast<ExpressionStatementNode*>(n)) {
        if (auto ex = std::dynamic_pointer_cast<ExpressionNode>(s)) {
            CTValue tmp; if (!ctEvalExpr(ex, env, tmp)) return CTFlow::Fail;
            return CTFlow::Normal;
        }
    }

    ctFail("unsupported statement in comptime fn (no I/O, allocation, unsafe, or spawn)", s->line);
    return CTFlow::Fail;
}

// ---------------------------------------------------------------------------
// Call + top-level const evaluation
// ---------------------------------------------------------------------------

bool CEmitter::ctEvalCall(FunctionDeclarationNode* fn, const std::vector<CTValue>& args, int line, CTValue& out)
{
    if (!fn || !fn->block) return ctFail("comptime fn has no body", line);
    if (++_ctDepth > CT_MAX_DEPTH) { _ctDepth--; return ctFail("comptime fn recursion too deep", line); }

    CTEnv env;
    // Bind parameters (positional; a named arg binds to its own param slot). Each bind is a typed store.
    size_t np = fn->parameters ? fn->parameters->size() : 0;
    if (args.size() != np) { _ctDepth--; return ctFail("comptime fn called with the wrong number of arguments", line); }
    for (size_t i = 0; i < np; ++i) {
        auto& p = (*fn->parameters)[i];
        if (!p || !p->identifier || !p->identifier->value || !p->type) { _ctDepth--; return ctFail("malformed comptime fn parameter", line); }
        CTValue proto;
        if (!ctTypeInfo(p->type, proto)) { _ctDepth--; return ctFail("a comptime fn parameter must be a scalar type", line); }
        CTValue v = args[i]; ctCoerce(proto, v);
        env.vars[*p->identifier->value] = v;
    }

    CTValue ret;
    CTFlow f = ctEvalStmt(fn->block, env, ret);
    _ctDepth--;
    if (f == CTFlow::Fail) return false;
    if (f != CTFlow::Return) return ctFail("a comptime fn must return a value on every path", line);

    // Coerce the return value to the declared return type.
    CTValue proto;
    if (ctTypeInfo(fn->returnType, proto)) ctCoerce(proto, ret);
    out = ret;
    return true;
}

// Evaluate the module `comptime` constants whose fold needed the interpreter (a `comptime fn` call), in
// declaration order, BEFORE registerInstColls so a comptime-fn-derived scalar can drive a const-generic
// array size. Scalar results are mirrored into _moduleConsts (int sizing) and _comptimeConstVals (emit).
void CEmitter::evalComptimeConsts()
{
    NsCtx saved = _nsCtx;
    for (auto& dc : _ctDeferredConsts) {
        _nsCtx = dc.ctx;
        _ctSteps = 0; _ctDepth = 0; _ctFailed = false;
        CTEnv env; CTValue v;
        if (!ctEvalExpr(dc.init, env, v)) {
            if (!_ctFailed)
                ctFail(("a `comptime` initializer must be a compile-time constant (a literal, `sizeof`, "
                        "const arithmetic, or a `comptime fn` call) — `" + dc.cName + "`").c_str(), dc.line);
            _ctErroredConsts.insert(dc.cName);   // a precise error was emitted; suppress the emit-time duplicate
            continue;
        }
        // Coerce to the constant's declared type, then bake.
        CTValue proto;
        if (ctTypeInfo(dc.type, proto)) ctCoerce(proto, v);
        _comptimeConstVals[dc.cName] = v;
        if (v.kind == CTValue::Int || v.kind == CTValue::Bool)
            _moduleConsts[dc.cName] = v.i;   // mirror for const-generic array sizing
    }
    _nsCtx = saved;
}
