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

// Does `name` (with an optional scope qualifier) resolve to a registered `comptime fn`? The interpreter's
// callee lookup, and the call-emit sites' too, which reject a runtime-position call with a diagnostic that
// points at the `comptime` constant form. Climbs resolveFuncImpl's ladder, minus the rungs a runtime call
// leaves to checkReach (the interpreter has no reach check): what this file IMPORTS — which the import site
// already held to the exporting file's `export` and the module's `visibility` — then what it declares, then
// its `using`s. The import rung was missing, so an exported comptime fn was "not one" to its importer (KR-95).
bool CEmitter::isComptimeFnName(const std::string& name, SharedStringList /*qualifier*/, std::string& outKey) const
{
    auto hit = [&](const std::string& k) -> bool {
        auto i = _comptimeFns.find(k);
        if (i != _comptimeFns.end()) { outKey = k; return true; }
        return false;
    };
    auto sa = _nsCtx.symbolAliases.find(name);                           // `import { m::f };` (or `as g`)
    if (sa != _nsCtx.symbolAliases.end() && hit(sa->second)) return true;
    if (hit(qualify(name))) return true;                                  // this file's own `f()`
    for (auto& u : _nsCtx.usings) if (hit(u + "__" + name)) return true;  // imported via `using`
    return hit(name);                                                     // prelude / global namespace
}

// Does `owner` grant `member` to `fromType` by a `friend`? The comptime interpreter's slice of the same
// question canAccess answers, kept separate because the interpreter has no `_currentClass`/`_currentFunc`:
// its whole notion of "where am I" is `_ctCurrentOwner`, a type name. So this matches CLASS accessors only
// — including the corresponding instance of a generic one, by the same rule canAccess uses — and answers
// no for a function accessor, which cannot be the context here.
//
// The owner may be a template (`_genericTypes`) or a concrete class; a grant lives on whichever holds it.
bool CEmitter::comptimeFriendGrants(const std::string& ownerKey, const std::string& member,
                                    const std::string& fromType)
{
    if (fromType.empty()) return false;
    const ClassInfo* oc = nullptr;
    { auto c = _classes.find(ownerKey); if (c != _classes.end()) oc = &c->second;
      else { auto g = _genericTypes.find(ownerKey); if (g != _genericTypes.end()) oc = &g->second; } }
    if (!oc) return false;
    std::string ownerArgs;
    { auto of = _genericTypeInstOf.find(ownerKey);
      if (of != _genericTypeInstOf.end() && ownerKey.size() > of->second.size())
          ownerArgs = ownerKey.substr(of->second.size()); }
    for (auto& g : oc->friendGrants) {
        if (!g.members.empty() && !g.members.count(member)) continue;
        if (!g.accessorIsClass) continue;
        if (!g.accessorArgs.empty()) {
            const std::vector<SharedIdentifier>* oa = nullptr;
            auto oi = _genericTypeInsts.find(ownerKey);
            if (oi != _genericTypeInsts.end()) oa = &oi->second.typeArgs;
            std::string key = g.accessor; bool ok = true;
            for (auto& e : g.accessorArgs) {
                if (e.first < 0) { key += "_" + e.second; continue; }
                if (!oa || (size_t)e.first >= oa->size()) { ok = false; break; }
                key += "_" + mangleElem((*oa)[e.first]);
            }
            if (ok && fromType == key) return true;
            continue;
        }
        std::string acc = g.accessor;
        if (g.accessorIsTemplate) {
            if (ownerArgs.empty()) {
                auto cf = _genericTypeInstOf.find(fromType);
                std::string fromTmpl = (cf != _genericTypeInstOf.end()) ? cf->second : fromType;
                if (fromTmpl != g.accessor) continue;
                return true;
            }
            acc += ownerArgs;
            if (!g.accessorMethod.empty()) continue;   // `Type::method` names a method, not this context
        }
        if (fromType == acc) return true;
    }
    return false;
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

// Shape of an `InlineArray<T, N>` type for the interpreter: the element scalar prototype, the size N, and
// the element C type (for baking). Mirrors registerFixed's extraction. Returns false for a non-array type
// or a non-scalar / unbound element (arrays of arrays / class elements are out of scope for v1).
bool CEmitter::ctArrayInfo(SharedIdentifier type, CTValue& elemProto, int64_t& n, std::string& elemCType)
{
    if (!type || !type->value || *type->value != "InlineArray") return false;
    auto args = type->genericArgs;
    if (!args || args->size() != 2 || !(*args)[0] || !(*args)[1]) return false;
    if (!ctTypeInfo((*args)[0], elemProto)) return false;   // element must be a comptime scalar
    if (!constArgN((*args)[1], n) || n <= 0) return false;
    elemCType = cType((*args)[0]);
    return true;
}

// Coerce a computed value to a declared type (a typed store: the local decl, an assignment target, a
// param bind, a return). Integer stores truncate to the target width; a float32 store rounds through float.
void CEmitter::ctCoerce(const CTValue& proto, CTValue& v)
{
    if (proto.isStruct || proto.isArray || v.isStruct || v.isArray) return;   // an aggregate keeps its shape (KR-93)
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
    if (_ctQuiet) { _ctFailed = true; return false; }   // a pre-pass: the emission-time evaluation reports
    if (!_ctFailed) {
        ++_unsupported;
        // The structured form is the ONLY form. `kama check` and the language server decide purely from the
        // Diagnostic list (`_unsupported` is a build-path counter they never read), so a comptime failure
        // that only reached stderr made the editor call a file clean that `kama build` rejects — the same
        // "check and build disagree" defect this pass exists to close. Guarded by the check/build agreement
        // assertion in run_tests.sh. A stderr line printed here as well was the OTHER half of that defect:
        // it duplicated every comptime failure under a second wording, exactly as `unsupported` once did.
        //
        // `diagFile()`, not `_sourcePath` — the latter is "" in a multi-file build, so a comptime failure
        // inside an imported module reported no file at all.
        Diagnostic d;
        d.line = line;
        d.severity = DiagSeverity::Error;
        d.code = "comptime";
        // Inside a value type's member, say WHICH member cannot run at compile time — the construct that
        // failed is in its body, not at the constant the author is looking at (KR-93). Demangled as
        // `unsupported` demangles: a C name in a diagnostic is the C-name family leaking into the language.
        const std::string body = _ctRunning.empty() ? std::string(what)
                               : "`" + _ctRunning + "` cannot run at compile time — " + what;
        d.message = std::string("comptime evaluation: ") + demangleForDisplay(body);
        d.file = reportPath(diagFile());   // the prelude names its own file when the install has it — see reportPath
        attributeToInstSite(d.file, d.line, d.message);   // a stdlib type's assert names the author's instantiation (KR-38)
        _diagnostics.push_back(d);
        _ctFailed = true;
    }
    return false;
}

// Render a scalar comptime value as a C initializer. Integers bake as decimal (with a width-appropriate
// suffix avoided — the declared type on the LHS carries it); float as a literal; bool as true/false.
std::string CEmitter::ctRender(const CTValue& v) const
{
    // A `type value` → a designated initializer over the emitter's own C field names (KR-93).
    if (v.isStruct) {
        if (v.elems.empty()) return "{0}";
        auto it = _classes.find(v.structClass);
        std::string s = "{ ";
        for (size_t k = 0; k < v.elems.size(); ++k) {
            const std::string fn = k < v.fieldNames.size() ? v.fieldNames[k] : std::string();
            s += (k ? ", ." : ".") + (it != _classes.end() ? kMember(it->second, fn) : kName(fn)) + " = " + ctRender(v.elems[k]);
        }
        return s + " }";
    }
    // Fixed array → a C initializer for the `struct { T kama_v[N]; }` (KAMA_FIXED_TYPE) carrier: `{ .kama_v = {…} }`.
    if (v.isArray) {
        std::string s = "{ .kama_v = { ";
        for (size_t k = 0; k < v.elems.size(); ++k) { if (k) s += ", "; s += ctRender(v.elems[k]); }
        s += " } }";
        return s;
    }
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
    // A module constant only the interpreter folds, and not evaluated yet: evaluate it NOW, so a constant may
    // read one declared later — in the same file or another. One already mid-evaluation is a cycle.
    auto forceDeferred = [&](const std::string& key) -> bool {
        auto st = _ctDeferredState.find(key);
        if (st == _ctDeferredState.end() || st->second == CTConstState::Done) return true;
        if (st->second == CTConstState::Evaluating)
            return ctFail(("`" + *id->value + "` is defined in terms of itself").c_str(), id->line);
        if (!evalDeferredConst(key)) { _ctFailed = true; return false; }   // it already said why
        return true;
    };

    // Type-associated `Type::NAME`.
    if (id->qualifier && !id->qualifier->empty()) {
        auto tq = std::make_shared<StringList>();
        for (size_t i = 0; i + 1 < id->qualifier->size(); ++i) tq->push_back((*id->qualifier)[i]);
        const std::string owner = resolveUserName(*id->qualifier->back(), tq);
        std::string key = owner + "::" + *id->value;
        auto tc = _typeConsts.find(key);
        if (tc != _typeConsts.end()) {
            // Member visibility, as at run time (KR-93: this read used to skip it), and then ON DEMAND
            // evaluation — a type constant the interpreter folds may be read before its turn, as a module
            // constant may, and one mid-evaluation is a cycle.
            if (!typeConstReadable(tc->second, *id->value)) {
                reportTypeConstAccess(tc->second, *id->value, id->line);
                _ctFailed = true;
                return false;
            }
            if (!forceDeferred(key)) return false;
        }
        auto cv = _comptimeConstVals.find(key);
        if (cv != _comptimeConstVals.end()) { out = cv->second; return true; }
        if (tc != _typeConsts.end() && tc->second.hasValue) { asInt(tc->second.value); return true; }
        // A plain enum's member (`K::A`), folded ON DEMAND — so an enum initializer may name a sibling
        // declared after it, or another enum's member, whatever order the enums are collected in.
        if (EnumInfo* ei = enumInfo(owner))
            for (size_t i = 0; i < ei->members.size(); ++i)
                if (ei->members[i].name == *id->value) {
                    if (ei->isExtern)   // KR-55: named where it is USED, which is the line the author can change
                        return ctFail(("`" + ei->name + "::" + *id->value + "` is a constant of a `type extern enum`, "
                                       "whose values the C header defines — kama cannot fold one; compare against "
                                       "the constant at run time").c_str(), id->line);
                    int64_t v;
                    if (!enumMemberValue(*ei, i, v)) return false;
                    asInt(v);
                    return true;
                }
        // …and if it is not a type-associated constant, it may be a MODULE one reached by its module
        // path (`cfg::CAP`). The two spellings are indistinguishable here — both are a qualifier and a
        // name — so the type reading is tried first and this is the fallback, not a competing arm.
        const std::string qk = resolveModuleVar(*id->value, id->qualifier);
        if (!qk.empty()) {
            if (!forceDeferred(qk)) return false;
            auto qv = _comptimeConstVals.find(qk);
            if (qv != _comptimeConstVals.end()) { out = qv->second; return true; }
            auto qm = _moduleConsts.find(qk);
            if (qm != _moduleConsts.end()) { asInt(qm->second); return true; }
        }
        if (!resolveExternConst(*id->value, id->qualifier).empty())   // KR-56, the qualified spelling
            return ctFail(("`" + *id->value + "` is an `extern const`, whose value the C header defines — kama cannot "
                           "fold one; use it at run time, or state the number in a `comptime` constant of your own").c_str(),
                          id->line);
        return false;
    }

    // Bare name. Through `resolveModuleVar` so a per-symbol `import { m::cfg::CAP }` resolves to the
    // DECLARING module's key rather than this file's scope, where nothing of that name exists.
    const std::string mk = resolveModuleVar(*id->value, id->qualifier);
    if (!forceDeferred(mk.empty() ? qualify(*id->value) : mk)) return false;
    auto cv = _comptimeConstVals.find(mk.empty() ? qualify(*id->value) : mk);
    if (cv != _comptimeConstVals.end()) { out = cv->second; return true; }
    if (!mk.empty()) {
        auto mc = _moduleConsts.find(mk);
        if (mc != _moduleConsts.end()) { asInt(mc->second); return true; }
    }
    auto cs = _comptimeSubst.find(*id->value);
    if (cs != _comptimeSubst.end()) { asInt(cs->second.value); return true; }
    auto cl = _comptimeLocalVals.find(*id->value);   // a local `comptime`, whatever its type (KR-93)
    if (cl != _comptimeLocalVals.end()) { out = cl->second; return true; }
    auto lv = _constLocalVals.find(*id->value);
    if (lv != _constLocalVals.end()) { asInt(lv->second); return true; }
    if (!_localTypes.count(*id->value) && !resolveExternConst(*id->value, id->qualifier).empty())   // KR-56
        return ctFail(("`" + *id->value + "` is an `extern const`, whose value the C header defines — kama cannot "
                       "fold one; use it at run time, or state the number in a `comptime` constant of your own").c_str(),
                      id->line);
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

    // --- sizeof(T) for a fixed-width scalar (M6) ---
    // constValue already folds this on the fast path; the interpreter needs its own arm for the cases
    // that reach here instead — a `comptime fn` body, and a deferred module constant whose initializer
    // mixes `sizeof` with a comptime-fn call (`comptime int32 X = round8(sizeof(int32));`). `sizeof`
    // yields a `usize`, so the value is unsigned 64. `alignof` is deliberately not folded — see
    // CEmitter::scalarByteSize for why there is no premise to fold it against.
    if (auto* s = dynamic_cast<SizeofNode*>(n)) {
        int64_t sz;
        if (s->isAlign || !scalarByteSize(s->type, sz))
            return ctFail(s->isAlign
                          ? "`alignof` does not fold — alignment is a target ABI property, not a language "
                            "guarantee (use it in a runtime position)"
                          : "`sizeof` folds only for fixed-width scalars (`int8`..`int64`, `uint8`..`uint64`, "
                            "`char`, `float32`, `float64`) — `usize`, `bool`, `string` and user types have "
                            "target- or layout-dependent size", e->line);
        out = CTValue{}; out.width = 64; out.isSigned = false; out.i = sz;
        return true;
    }

    // --- identifier: a local frame var, else a module/type comptime constant ---
    if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
        if (id->value) {
            auto it = env.vars.find(*id->value);
            if (it != env.vars.end()) { out = it->second; return true; }
            if (ctResolveConst(std::static_pointer_cast<IdentifierNode>(e), out)) return true;
            if (_ctFailed) return false;
            // A bare field name inside a value type's member reads `this`'s field (KR-93).
            auto self = env.vars.find("this");
            if (self != env.vars.end() && self->second.isStruct && (!id->qualifier || id->qualifier->empty()))
                for (size_t k = 0; k < self->second.fieldNames.size(); ++k)
                    if (self->second.fieldNames[k] == *id->value) { out = self->second.elems[k]; return true; }
        }
        // Phrased for BOTH callers: a `comptime fn` body and (M7) a `comptime assert` predicate, which has
        // no params or locals of its own. Naming only the comptime-fn rule read as a non-sequitur there.
        return ctFail(("unknown identifier `" + (id->value ? *id->value : std::string("?"))
                       + "` — a compile-time expression reads only `comptime` constants, comptime "
                         "parameters, and (inside a `comptime fn`) that function's params and locals").c_str(), e->line);
    }

    // --- `this` and a field read `x.f` — a value type's member, running (KR-93) ---
    if (dynamic_cast<ThisAccessNode*>(n)) {
        auto it = env.vars.find("this");
        if (it == env.vars.end()) return ctFail("`this` is read only inside a value type's member", e->line);
        out = it->second;
        return true;
    }
    if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        if (!ma->identifier || !ma->identifier->value || !ma->expression)
            return ctFail("unsupported member access at compile time", e->line);
        CTValue base; if (!ctEvalExpr(ma->expression, env, base)) return false;
        const std::string& fname = *ma->identifier->value;
        if (!base.isStruct)
            return ctFail(("`." + fname + "` — only a `type value`'s fields are read at compile time").c_str(), e->line);
        ClassInfo& ci = _classes[base.structClass];
        for (size_t k = 0; k < base.fieldNames.size(); ++k)
            if (base.fieldNames[k] == fname) {
                Visibility fv = k < ci.fields.size() ? ci.fields[k].visibility : Visibility::Public;
                if (!ctMemberVisible(ci, fv, fname))
                    return ctFail(("'" + fname + "' is private in '" + ci.name + "'").c_str(), e->line);
                out = base.elems[k];
                return true;
            }
        return ctFail(("`" + demangleForDisplay(ci.name) + "` has no field `" + fname + "`").c_str(), e->line);
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
        if (v.isStruct) return ctStructOperator(u->token, v, nullptr, e->line, out);   // the type's operator (KR-93)
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
        // A value type's operand: `==`/`!=` is its equality, anything else its declared operator (KR-93).
        if (lv.isStruct || rv.isStruct) {
            if (b->token == EQEQ || b->token == NOTEQ) {
                bool eq = false;
                if (!ctStructEquals(lv, rv, e->line, eq)) return false;
                out = CTValue{}; out.kind = CTValue::Bool; out.width = 1; out.isSigned = false;
                out.i = (eq == (b->token == EQEQ)) ? 1 : 0;
                return true;
            }
            return ctStructOperator(b->token, lv, &rv, e->line, out);
        }
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

    // --- fixed-array element read `a[i]` ---
    if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        SharedExpression base = ea->expression ? ea->expression
                              : (ea->identifier ? std::static_pointer_cast<ExpressionNode>(ea->identifier) : SharedExpression());
        if (!base || !ea->expressionlist || ea->expressionlist->size() != 1)
            return ctFail("only single-index fixed-array access is supported in a comptime fn", e->line);
        CTValue arr; if (!ctEvalExpr(base, env, arr)) return false;
        if (!arr.isArray) return ctFail("indexed value is not a fixed array in comptime fn", e->line);
        CTValue idx; if (!ctEvalExpr((*ea->expressionlist)[0], env, idx)) return false;
        int64_t i = ctAsI(idx);
        if (i < 0 || (size_t)i >= arr.elems.size())
            return ctFail("comptime fixed-array index out of bounds", e->line);
        out = arr.elems[(size_t)i];
        return true;
    }

    // --- call to another comptime fn (free `f()` or type-associated `Type::name()`) ---
    if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        if (!inv->identifier) return ctEvalCallExpr(inv, env, out);   // `Type.ctor(…)` / `value.method(…)` (KR-93)
        if (!inv->identifier->value)
            return ctFail("a comptime fn may call only another `comptime fn` by name", e->line);
        std::vector<std::string> argNames;
        auto evalArgs = [&](std::vector<CTValue>& args) -> bool {
            if (inv->args) for (auto& a : *inv->args) {
                CTValue av; if (!a || !ctEvalExpr(a->expression, env, av)) return false;
                args.push_back(av);
                argNames.push_back(a->name && a->name->value ? *a->name->value : std::string());
            }
            return true;
        };
        // Type-associated `Type::name()` — resolve the owner type, honor visibility (private is callable
        // only from within the same type's comptime fns).
        if (inv->identifier->qualifier && !inv->identifier->qualifier->empty()) {
            auto tq = std::make_shared<StringList>();
            for (size_t i = 0; i + 1 < inv->identifier->qualifier->size(); ++i) tq->push_back((*inv->identifier->qualifier)[i]);
            std::string owner = resolveUserName(*inv->identifier->qualifier->back(), tq);
            std::string disp = *inv->identifier->qualifier->back() + "::" + *inv->identifier->value;   // source-written name
            auto mit = _comptimeMethods.find(owner + "::" + *inv->identifier->value);
            if (mit == _comptimeMethods.end()) {
                bool handled = false;   // a value type's `static fn` runs too (KR-93)
                bool ok = ctStaticMember(owner, *inv->identifier->value, disp, inv, env, e->line, out, handled);
                if (handled) return ok;
                return ctFail(("a compile-time call names a `comptime fn`, a value type's ctor, method or `static fn` — `"
                               + disp + "` is none of those").c_str(), e->line);
            }
            // ...or a `friend` grant names it. This is the THIRD access-check path — canAccess (emitter)
            // and visibleFrom (query) are the other two — and it was the one that knew nothing about
            // grants, so `friend P[secret]` on a `comptime fn` resolved and was then refused here anyway
            // (KR-80). SPEC says a type's `comptime` members are visibility-controlled like any other, so
            // "controlled" has to include who the owner lets in.
            //
            // The accessing context is a TYPE (`_ctCurrentOwner`, the type whose comptime fn is running),
            // never a function, so only a class-accessor grant can match — a `friend someFn[…]` cannot
            // reach here, because there is no function context at compile-time evaluation to be.
            if (mit->second.vis == Visibility::Private && _ctCurrentOwner != owner
                && !comptimeFriendGrants(owner, *inv->identifier->value, _ctCurrentOwner))
                // The hint names the owner the way the SOURCE does (the qualifier as written), never the
                // mangled key `_ctCurrentOwner` holds — a diagnostic that prints `k_Fprobe__Holder` at a
                // reader is the C-name family leaking into the language (KR-67).
                return ctFail(("`" + disp + "` is a private `comptime fn` — not accessible here; "
                               "mark it `public` to call it from another scope, or have `"
                               + *inv->identifier->qualifier->back()
                               + "` grant it to the calling type: `friend <ThatType>["
                               + *inv->identifier->value + "];`").c_str(), e->line);
            std::vector<CTValue> args; if (!evalArgs(args)) return false;
            if (!ctBindByName(mit->second.node->params, argNames, args, disp, e->line)) return false;
            std::string saved = _ctCurrentOwner; _ctCurrentOwner = owner;
            const ComptimeMethod& cm = mit->second;
            bool ok = ctInDeclaringFile(cm.ctx, [&] {
                return ctEvalBody(cm.node->params, cm.node->body, cm.node->returnType, args, e->line, out); });
            _ctCurrentOwner = saved;
            return ok;
        }
        // Free `comptime fn`.
        std::string key;
        if (!isComptimeFnName(*inv->identifier->value, inv->identifier->qualifier, key)) {
            // Say what is CALLING: a type member run by name, a comptime fn's body (depth > 0), or a
            // compile-time expression itself — a constant's initializer was told "a comptime fn may call only
            // another comptime fn", about a caller that is not one (KR-95).
            const std::string& nm = *inv->identifier->value;
            // ...unless it IS one, declared where this file cannot reach it — then say that, and why.
            for (auto& kv : _comptimeFns)
                if (kv.first.size() > nm.size() + 2
                    && kv.first.compare(kv.first.size() - nm.size() - 2, std::string::npos, "__" + nm) == 0)
                    return ctFail(("`" + nm + "` is a `comptime fn` of `" + kv.second.ctx.unitPath + "`, out of this "
                                   "file's reach — " + (_exported.count(kv.first)
                                       ? std::string("import it (`import { … };`)")
                                       : std::string("it is private to that file, and leaves it only through that "
                                                     "file's `export { … };`"))).c_str(), e->line);
            return ctFail((!_ctRunning.empty() ? "it calls `" + nm + "`, which is not a `comptime fn`"
                         : _ctDepth > 0        ? "a comptime fn may call only another `comptime fn` — `" + nm + "` is not one"
                                               : "a compile-time expression may call only a `comptime fn` — `" + nm
                                                 + "` is not one").c_str(), e->line);
        }
        std::vector<CTValue> args; if (!evalArgs(args)) return false;
        const ComptimeFn& cf = _comptimeFns[key];
        if (!ctBindByName(cf.node->parameters, argNames, args, *inv->identifier->value, e->line)) return false;
        std::string saved = _ctCurrentOwner; _ctCurrentOwner.clear();   // a free fn has no owning type
        bool ok = ctInDeclaringFile(cf.ctx, [&] { return ctEvalCall(cf.node, args, e->line, out); });
        _ctCurrentOwner = saved;
        return ok;
    }

    return ctFail("unsupported expression in comptime fn (no I/O, allocation, pointers, or strings)", e->line);
}

// Build a fixed array's element values from its initializer: `[v; N]` (fill), `[a, b, c]` (list), or
// another array expression (a var / a comptime-fn call returning an array). Each element is a typed store.
bool CEmitter::ctBuildArrayInit(SharedExpression init, CTEnv& env, const CTValue& elemProto, size_t n, std::vector<CTValue>& out)
{
    auto* al = dynamic_cast<ArrayLiteralNode*>(init.get());
    if (!al) {   // an array-valued expression
        CTValue src; if (!ctEvalExpr(init, env, src)) return false;
        if (!src.isArray || src.elems.size() != n) return ctFail("comptime array initializer shape mismatch", init->line);
        out = src.elems; return true;
    }
    if (al->fillValue && al->fillCount) {
        CTValue fv; if (!ctEvalExpr(al->fillValue, env, fv)) return false;
        int64_t fc;
        if (!constValue(al->fillCount, fc)) { CTValue c; if (!ctEvalExpr(al->fillCount, env, c)) return false; fc = ctAsI(c); }
        if (fc < 0 || (size_t)fc != n) return ctFail("comptime array fill count does not match the declared size", init->line);
        ctCoerce(elemProto, fv);
        out.assign(n, fv);
        return true;
    }
    if (al->elements) {
        if (al->elements->size() != n) return ctFail("comptime array literal length does not match the declared size", init->line);
        out.clear();
        for (auto& el : *al->elements) { CTValue ev; if (!ctEvalExpr(el, env, ev)) return false; ctCoerce(elemProto, ev); out.push_back(ev); }
        return true;
    }
    return ctFail("unsupported comptime array initializer", init->line);
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
        // Fixed-array local (`InlineArray<T,N> t = [0; N];` / `= [a, b, c];`) — the table carrier.
        CTValue elemProto; int64_t n64; std::string elemCType;
        if (ctArrayInfo(d->type, elemProto, n64, elemCType)) {
            if (d->variables) for (auto& v : *d->variables) {
                if (!v || !v->name || !v->name->value) continue;
                CTValue arr = elemProto; arr.isArray = true; arr.elemCType = elemCType;
                arr.elems.assign((size_t)n64, [&]{ CTValue z = elemProto; z.i = 0; z.f = 0.0; return z; }());
                if (v->initializer) {
                    if (!ctBuildArrayInit(v->initializer, env, elemProto, (size_t)n64, arr.elems)) return CTFlow::Fail;
                }
                env.vars[*v->name->value] = arr;
            }
            return CTFlow::Normal;
        }
        CTValue proto;
        if (!ctValueProto(d->type, proto)) {
            ctFail("a compile-time local holds a number, `bool`, `char`, fixed array, or a `type value` of those", s->line);
            return CTFlow::Fail;
        }
        if (d->variables) for (auto& v : *d->variables) {
            if (!v || !v->name || !v->name->value) continue;
            CTValue val;
            if (v->initializer) { if (!ctEvalExpr(v->initializer, env, val)) return CTFlow::Fail; }
            else                { val = proto; }   // zero-init
            ctCoerceTo(proto, val);
            env.vars[*v->name->value] = val;
        }
        return CTFlow::Normal;
    }

    if (auto* d = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        CTValue proto;
        if (!ctValueProto(d->type, proto)) {
            ctFail("a compile-time const local holds a number, `bool`, `char`, or a `type value` of those", s->line);
            return CTFlow::Fail;
        }
        if (d->variables) for (auto& v : *d->variables) {
            if (!v || !v->name || !v->name->value) continue;
            CTValue val;
            if (!v->initializer || !ctEvalExpr(v->initializer, env, val)) return CTFlow::Fail;
            ctCoerceTo(proto, val);
            env.vars[*v->name->value] = val;
        }
        return CTFlow::Normal;
    }

    if (auto* a = dynamic_cast<AssignmentNode*>(n)) {
        if (a->token != EQ) { ctFail("only plain `=` assignment is supported in a comptime fn", s->line); return CTFlow::Fail; }
        // Fixed-array element write `t[i] = v` — the table-fill primitive.
        if (auto* ea = dynamic_cast<ElementAccessNode*>(a->unaryExpression.get())) {
            SharedExpression base = ea->expression ? ea->expression
                                  : (ea->identifier ? std::static_pointer_cast<ExpressionNode>(ea->identifier) : SharedExpression());
            auto* bid = base ? dynamic_cast<IdentifierNode*>(base.get()) : nullptr;
            if (!bid || !bid->value) { ctFail("a comptime array-element write targets a local fixed array", s->line); return CTFlow::Fail; }
            auto it = env.vars.find(*bid->value);
            if (it == env.vars.end() || !it->second.isArray) { ctFail(("`" + (bid->value ? *bid->value : std::string("?")) + "` is not a fixed-array local in comptime fn").c_str(), s->line); return CTFlow::Fail; }
            if (!ea->expressionlist || ea->expressionlist->size() != 1) { ctFail("only single-index array writes are supported in a comptime fn", s->line); return CTFlow::Fail; }
            CTValue idx; if (!ctEvalExpr((*ea->expressionlist)[0], env, idx)) return CTFlow::Fail;
            int64_t i = ctAsI(idx);
            if (i < 0 || (size_t)i >= it->second.elems.size()) { ctFail("comptime fixed-array write index out of bounds", s->line); return CTFlow::Fail; }
            CTValue val; if (!ctEvalExpr(a->expression, env, val)) return CTFlow::Fail;
            CTValue elemProto = it->second; elemProto.isArray = false; elemProto.elems.clear();   // element scalar proto
            ctCoerce(elemProto, val);
            it->second.elems[(size_t)i] = val;
            return CTFlow::Normal;
        }
        // A field write `this.f = v` / `p.f = v` / `this.a.f = v` — a value type's member, running (KR-93).
        if (dynamic_cast<MemberAccessNode*>(a->unaryExpression.get())) {
            std::vector<std::string> path;
            SharedExpression cur = a->unaryExpression;
            while (auto* m = dynamic_cast<MemberAccessNode*>(cur.get())) {
                if (!m->identifier || !m->identifier->value) { ctFail("unsupported field write at compile time", s->line); return CTFlow::Fail; }
                path.insert(path.begin(), *m->identifier->value);
                cur = m->expression;
            }
            std::string root;
            if (dynamic_cast<ThisAccessNode*>(cur.get())) root = "this";
            else if (auto* rid = dynamic_cast<IdentifierNode*>(cur.get()))
                if (rid->value && (!rid->qualifier || rid->qualifier->empty())) root = *rid->value;
            auto it = root.empty() ? env.vars.end() : env.vars.find(root);
            if (it == env.vars.end()) { ctFail("a compile-time field write targets `this` or a local value", s->line); return CTFlow::Fail; }
            CTValue val; if (!ctEvalExpr(a->expression, env, val)) return CTFlow::Fail;
            CTValue* slot = &it->second;
            for (auto& fname : path) {
                if (!slot->isStruct) { ctFail(("`." + fname + "` is not a field of a value").c_str(), s->line); return CTFlow::Fail; }
                size_t k = 0;
                while (k < slot->fieldNames.size() && slot->fieldNames[k] != fname) ++k;
                if (k == slot->fieldNames.size()) { ctFail(("no field `" + fname + "`").c_str(), s->line); return CTFlow::Fail; }
                slot = &slot->elems[k];
            }
            ctCoerceTo(*slot, val);   // store into the field's declared width/kind
            *slot = val;
            return CTFlow::Normal;
        }
        auto* tgt = dynamic_cast<IdentifierNode*>(a->unaryExpression.get());
        if (!tgt || !tgt->value) { ctFail("comptime assignment target must be a local or an element of a local array (`a[i] = …`)", s->line); return CTFlow::Fail; }
        auto it = env.vars.find(*tgt->value);
        // A bare field of `this` inside a value type's member (KR-93).
        if (it == env.vars.end()) {
            auto self = env.vars.find("this");
            if (self != env.vars.end() && self->second.isStruct)
                for (size_t k = 0; k < self->second.fieldNames.size(); ++k)
                    if (self->second.fieldNames[k] == *tgt->value) {
                        CTValue val; if (!ctEvalExpr(a->expression, env, val)) return CTFlow::Fail;
                        ctCoerceTo(self->second.elems[k], val);
                        self->second.elems[k] = val;
                        return CTFlow::Normal;
                    }
        }
        if (it == env.vars.end()) { ctFail(("assignment to unknown local `" + *tgt->value + "` in comptime fn").c_str(), s->line); return CTFlow::Fail; }
        CTValue val;
        if (!ctEvalExpr(a->expression, env, val)) return CTFlow::Fail;
        ctCoerceTo(it->second, val);   // store into the target's declared width/kind
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

    if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        if (fe->isRef) { ctFail("`foreach (ref …)` write-back is not supported in a comptime fn — use indexed `a[i] = …`", s->line); return CTFlow::Fail; }
        CTValue arr; if (!ctEvalExpr(fe->expression, env, arr)) return CTFlow::Fail;
        if (!arr.isArray) { ctFail("`foreach` in a comptime fn iterates a fixed array only", s->line); return CTFlow::Fail; }
        if (!fe->name || !fe->name->value) return CTFlow::Normal;
        const std::string& bind = *fe->name->value;
        for (auto& elv : arr.elems) {
            env.vars[bind] = elv;   // by value (a fresh copy per iteration)
            CTFlow f = ctEvalStmt(fe->body, env, ret);
            if (f == CTFlow::Return || f == CTFlow::Fail) return f;
            if (f == CTFlow::Break) break;
        }
        return CTFlow::Normal;
    }

    if (dynamic_cast<BreakNode*>(n))    return CTFlow::Break;
    if (dynamic_cast<ContinueNode*>(n)) return CTFlow::Continue;

    if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        if (!r->expression) { ret = CTValue{}; ret.isVoid = true; return CTFlow::Return; }   // a `void` member's `return;`
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

// Put a call's arguments in PARAMETER order. kama arguments are named and may be written in any order, and the
// interpreter bound them by POSITION, so `sub(b: 1, a: 10)` computed `1 - 10` — a silent miscompile of every
// comptime call whose arguments were not written in declaration order (KR-93). A comptime-only call is never
// emitted, so the emitter's named-argument checks never saw it either: an unknown, missing or repeated label
// is refused here.
bool CEmitter::ctBindByName(SharedParameterList params, const std::vector<std::string>& names,
                            std::vector<CTValue>& args, const std::string& callee, int line)
{
    const size_t np = params ? params->size() : 0;
    std::vector<CTValue> ordered(np);
    std::vector<bool> seen(np, false);
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& nm = k < names.size() ? names[k] : std::string();
        size_t at = np;
        for (size_t i = 0; i < np; ++i)
            if ((*params)[i] && (*params)[i]->identifier && (*params)[i]->identifier->value
                && *(*params)[i]->identifier->value == nm) { at = i; break; }
        if (at == np)
            return ctFail(("`" + callee + "` has no parameter `" + nm + "`").c_str(), line);
        if (seen[at])
            return ctFail(("`" + callee + "` is given `" + nm + "` twice").c_str(), line);
        seen[at] = true;
        ordered[at] = args[k];
    }
    for (size_t i = 0; i < np; ++i)
        if (!seen[i])
            return ctFail(("`" + callee + "` is missing the argument `"
                           + (params && (*params)[i] && (*params)[i]->identifier && (*params)[i]->identifier->value
                                  ? *(*params)[i]->identifier->value : std::string("?")) + ":`").c_str(), line);
    args.swap(ordered);
    return true;
}

// Run a comptime fn's body where it was WRITTEN: its file's scope, imports and file rung, and that file named
// by a diagnostic. The caller's context is the wrong one the moment the two files differ — an exported fn
// calling its own private helper resolved the helper in the IMPORTER's file, and a failure in the body was
// blamed on the importer's file at the body's line (KR-95). Argument values are the caller's, evaluated first.
bool CEmitter::ctInDeclaringFile(const NsCtx& ctx, const std::function<bool()>& run)
{
    NsCtx saved = _nsCtx; std::string savedUnit = _collectingUnitPath;
    _nsCtx = ctx;
    if (!ctx.unitPath.empty()) _collectingUnitPath = ctx.unitPath;
    bool ok = run();
    _nsCtx = saved; _collectingUnitPath = savedUnit;
    return ok;
}

bool CEmitter::ctEvalCall(FunctionDeclarationNode* fn, const std::vector<CTValue>& args, int line, CTValue& out)
{
    if (!fn) return ctFail("comptime fn has no body", line);
    return ctEvalBody(fn->parameters, fn->block, fn->returnType, args, line, out);
}

// Shared call core: bind args to params (each a typed store), walk the body, coerce the return value.
// Used by both free `comptime fn`s and type-associated ones (whose params/body/returnType have the same shape).
bool CEmitter::ctEvalBody(SharedParameterList params, SharedBlock body, SharedIdentifier retType,
                          const std::vector<CTValue>& args, int line, CTValue& out)
{
    if (!body) return ctFail("comptime fn has no body", line);
    if (++_ctDepth > CT_MAX_DEPTH) { _ctDepth--; return ctFail("comptime fn recursion too deep", line); }

    CTEnv env;
    size_t np = params ? params->size() : 0;
    if (args.size() != np) { _ctDepth--; return ctFail("comptime fn called with the wrong number of arguments", line); }
    for (size_t i = 0; i < np; ++i) {
        auto& p = (*params)[i];
        if (!p || !p->identifier || !p->identifier->value || !p->type) { _ctDepth--; return ctFail("malformed comptime fn parameter", line); }
        CTValue proto;
        if (!ctValueProto(p->type, proto)) {
            _ctDepth--;
            return ctFail("a comptime fn parameter holds a number, `bool`, `char`, fixed array, or a `type value` of those", line);
        }
        CTValue v = args[i]; ctCoerceTo(proto, v);
        env.vars[*p->identifier->value] = v;
    }

    CTValue ret;
    CTFlow f = ctEvalStmt(body, env, ret);
    _ctDepth--;
    if (f == CTFlow::Fail) return false;
    if (f != CTFlow::Return) return ctFail("a comptime fn must return a value on every path", line);
    if (ret.isVoid) return ctFail("a comptime fn must return a value", line);

    CTValue proto;
    if (ctValueProto(retType, proto)) ctCoerceTo(proto, ret);   // scalar return coercion (aggregates pass through)
    out = ret;
    return true;
}

// ---------------------------------------------------------------------------
// KR-93: a `type value` at compile time
//
// A `comptime` may hold a small value type — `public comptime Permissions OwnerRead = Permissions.fromBits(bits:
// 0o400);` — so the interpreter runs a value type's ctor, and its methods and operators when a compile-time
// expression calls them: the dual-use widening SPEC reserved when `comptime fn` was made comptime-only. There
// is no marker on the ctor — the `comptime` at the use site is the explicit part — and a body that leaves the
// comptime subset (a print, an allocation, a call into C) is refused at the constant, naming the construct.
// ---------------------------------------------------------------------------

// A primitive C type's scalar prototype — what a generic instance's field (`T v` in `Box<int32>`) resolves to.
bool CEmitter::ctProtoOfCType(const std::string& ct, CTValue& proto)
{
    auto setInt = [&](int w, bool sgn) { proto = CTValue{}; proto.kind = CTValue::Int; proto.width = w; proto.isSigned = sgn; };
    if      (ct == "int8_t")    setInt(8, true);
    else if (ct == "int16_t")   setInt(16, true);
    else if (ct == "int32_t")   setInt(32, true);
    else if (ct == "int64_t")   setInt(64, true);
    else if (ct == "uint8_t")   setInt(8, false);
    else if (ct == "uint16_t")  setInt(16, false);
    else if (ct == "uint32_t")  setInt(32, false);
    else if (ct == "uint64_t")  setInt(64, false);
    else if (ct == "kama_char") setInt(32, false);
    else if (ct == "bool")      { proto = CTValue{}; proto.kind = CTValue::Bool; proto.width = 1; proto.isSigned = false; }
    else if (ct == "float")     { proto = CTValue{}; proto.kind = CTValue::Float; proto.isF32 = true; }
    else if (ct == "double")    { proto = CTValue{}; proto.kind = CTValue::Float; proto.isF32 = false; }
    else return false;
    return true;
}

// What a compile-time local, parameter or return of `type` holds: a scalar, a fixed array, a payload-less
// enum (its integer), or a `type value` whose every field is one of those. Silent — a caller says why.
bool CEmitter::ctValueProto(SharedIdentifier type, CTValue& proto)
{
    if (!type) return false;
    if (ctTypeInfo(type, proto)) return true;
    CTValue ep; int64_t n = 0; std::string ec;
    if (ctArrayInfo(type, ep, n, ec)) {
        proto = CTValue{}; proto.isArray = true; proto.elemCType = ec;
        CTValue z = ep; z.i = 0; z.f = 0.0;
        proto.elems.assign((size_t)n, z);
        return true;
    }
    const std::string ct = cType(type);
    if (ctProtoOfCType(ct, proto)) return true;
    if (isEnum(ct)) { proto = CTValue{}; proto.width = 32; return true; }
    std::string why;
    return _classes.count(ct) && ctStructProto(ct, proto, why);
}

// The zeroed shape of a `type value` a compile-time value can hold. `why` names the first thing it cannot.
bool CEmitter::ctStructProto(const std::string& cls, CTValue& proto, std::string& why)
{
    auto it = _classes.find(cls);
    const std::string shown = demangleForDisplay(cls);
    if (it == _classes.end()) { why = "`" + shown + "` is not a type a compile-time value can hold"; return false; }
    ClassInfo& ci = it->second;
    if (ci.kind != TypeKind::Value) why = "`" + shown + "` is not a `type value`, and a compile-time value owns nothing";
    else if (ci.isBorrow)           why = "`" + shown + "` is a `view`";
    else if (ci.isExternStruct)     why = "`" + shown + "` has a layout a C header owns";
    else if (ci.isIntrinsicColl)    why = "`" + shown + "` is a collection";
    else if (ci.isVariant)          why = "`" + shown + "` is an enum with payloads";
    if (!why.empty()) return false;
    proto = CTValue{}; proto.isStruct = true; proto.structClass = cls;
    for (auto& f : ci.fields) {
        CTValue fv;
        bool ok = ctTypeInfo(f.type, fv);
        if (!ok) {
            CTValue ep; int64_t n = 0; std::string ec;
            if (ctArrayInfo(f.type, ep, n, ec)) {
                fv = CTValue{}; fv.isArray = true; fv.elemCType = ec;
                CTValue z = ep; z.i = 0; z.f = 0.0; fv.elems.assign((size_t)n, z);
                ok = true;
            }
        }
        if (!ok) {
            const std::string fct = fieldCType(ci.name, f);   // resolved where the TYPE was declared
            auto fc = _classes.find(fct);
            if (ctProtoOfCType(fct, fv)) ok = true;
            else if (isEnum(fct)) { fv = CTValue{}; fv.width = 32; ok = true; }
            else if (fc != _classes.end() && fc->second.kind == TypeKind::Value && !fc->second.isIntrinsicColl
                     && !fc->second.isBorrow && !fc->second.isExternStruct && !fc->second.isVariant) {
                std::string inner;
                if (!ctStructProto(fct, fv, inner)) { why = "`" + shown + "`'s field `" + f.name + "`: " + inner; return false; }
                ok = true;
            }
        }
        if (!ok) {
            why = "`" + shown + "`'s field `" + f.name + "` is a `"
                + (f.type && f.type->value ? *f.type->value : std::string("?"))
                + "`, which a compile-time value cannot hold (a number, `bool`, `char`, fixed array, or a `type value` "
                  "of those)";
            return false;
        }
        proto.fieldNames.push_back(f.name);
        proto.elems.push_back(fv);
    }
    return true;
}

void CEmitter::ctCoerceTo(const CTValue& proto, CTValue& v)
{
    if (proto.isStruct || proto.isArray || v.isStruct || v.isArray) return;   // the emitter typed the aggregate
    ctCoerce(proto, v);
}

// May the code the interpreter is running reach `ci`'s member? The reader is the type whose body is running,
// else the type being collected; with neither, a function body is reading and the run-time rule answers.
bool CEmitter::ctMemberVisible(const ClassInfo& ci, Visibility vis, const std::string& member)
{
    if (vis == Visibility::Public) return true;
    const std::string& reader = !_ctCurrentOwner.empty() ? _ctCurrentOwner : _constReaderType;
    if (!reader.empty()) {
        if (reader == ci.name) return true;
        if (vis == Visibility::Protected) {
            auto rc = _classes.find(reader);
            for (ClassInfo* c = rc != _classes.end() ? rc->second.base : nullptr; c; c = c->base)
                if (c->name == ci.name) return true;
        }
        return comptimeFriendGrants(ci.name, member, reader);
    }
    return accessAllowed(const_cast<ClassInfo*>(&ci), vis, member);
}

// A call's arguments, evaluated in the CALLER's frame, with the labels they were written with.
bool CEmitter::ctCallArgs(InvocationNode* inv, CTEnv& env, std::vector<CTValue>& args, std::vector<std::string>& names)
{
    if (inv->args) for (auto& a : *inv->args) {
        CTValue av; if (!a || !ctEvalExpr(a->expression, env, av)) return false;
        args.push_back(av);
        names.push_back(a->name && a->name->value ? *a->name->value : std::string());
    }
    return true;
}

// Run a member's body AS its type — its own privates in reach — and in its type's scope, where the names in
// the body were written. `self` is the receiver (a ctor's value under construction), written back after.
bool CEmitter::ctRunMember(ClassInfo& ci, std::vector<CTArg> bound, SharedStatement body, SharedIdentifier retType,
                           CTValue* self, const std::string& what, int line, CTValue& out)
{
    if (!body) return ctFail(("`" + what + "` has no body to run at compile time").c_str(), line);
    if (++_ctDepth > CT_MAX_DEPTH) { _ctDepth--; return ctFail("comptime evaluation recursion too deep", line); }
    std::string savedOwner = _ctCurrentOwner; _ctCurrentOwner = ci.name;
    std::string savedRunning = _ctRunning; _ctRunning = what;
    NsCtx savedNs = _nsCtx;
    _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
    restoreFileRung(_nsCtx, ci.declFile);
    CTEnv env;
    for (auto& b : bound) {
        CTValue proto;
        if (b.type && ctValueProto(b.type, proto)) ctCoerceTo(proto, b.value);
        env.vars[b.name] = b.value;
    }
    if (self) env.vars["this"] = *self;
    CTValue ret;
    CTFlow f = ctEvalStmt(body, env, ret);
    _ctDepth--;
    _nsCtx = savedNs;
    _ctCurrentOwner = savedOwner;
    _ctRunning = savedRunning;
    if (f == CTFlow::Fail) return false;
    if (self) { auto t = env.vars.find("this"); if (t != env.vars.end()) *self = t->second; }
    out = CTValue{};
    if (f == CTFlow::Return && !ret.isVoid) {
        CTValue proto;
        if (retType && ctValueProto(retType, proto)) ctCoerceTo(proto, ret);
        out = ret;
    }
    return true;
}

// `Type.ctor(…)` at compile time: the value type's shape, zeroed, its field initializers, then the ctor body
// with `this` bound. A ctor's value is what it returned (`return this;`, a delegation `return T.other(…)`),
// else `this`. `@generate(of)` and `@generate(zero)` have no body and are built directly.
bool CEmitter::ctConstruct(ClassInfo& ci, const std::string& ctorName, InvocationNode* inv, CTEnv& env, int line,
                           CTValue& out)
{
    const std::string shown = demangleForDisplay(ci.name) + "." + ctorName;
    CTValue self; std::string why;
    if (!ctStructProto(ci.name, self, why))
        return ctFail((why + " — so `" + shown + "` cannot build a compile-time value").c_str(), line);
    std::vector<CTValue> args; std::vector<std::string> names;
    if (!ctCallArgs(inv, env, args, names)) return false;
    auto mit = ci.methods.find(ctorName);
    const bool bodiless = mit == ci.methods.end() || !mit->second.node;
    if (ctorName == "of" && ci.genOf && bodiless) {
        std::vector<bool> seen(self.elems.size(), false);
        for (size_t k = 0; k < args.size(); ++k) {
            size_t at = self.fieldNames.size();
            for (size_t i = 0; i < self.fieldNames.size(); ++i) if (self.fieldNames[i] == names[k]) { at = i; break; }
            if (at == self.fieldNames.size())
                return ctFail(("`" + shown + "` has no parameter `" + names[k] + "`").c_str(), line);
            if (seen[at]) return ctFail(("`" + shown + "` is given `" + names[k] + "` twice").c_str(), line);
            seen[at] = true;
            ctCoerceTo(self.elems[at], args[k]);
            self.elems[at] = args[k];
        }
        for (size_t i = 0; i < seen.size(); ++i)
            if (!seen[i]) return ctFail(("`" + shown + "` is missing the argument `" + self.fieldNames[i] + ":`").c_str(), line);
        out = self;
        return true;
    }
    if (ctorName == "zero" && ci.genZero && bodiless) {
        if (!args.empty()) return ctFail(("`" + shown + "` takes no arguments").c_str(), line);
        out = self;
        return true;
    }
    if (mit == ci.methods.end() || !mit->second.isCtor)
        return ctFail(("`" + demangleForDisplay(ci.name) + "` has no ctor `" + ctorName + "`").c_str(), line);
    MethodInfo& mi = mit->second;
    if (!ctMemberVisible(ci, mi.visibility, ctorName))
        return ctFail(("'" + ctorName + "' is " + (mi.visibility == Visibility::Protected ? "protected" : "private")
                       + " in '" + ci.name + "'").c_str(), line);
    auto cit = ci.ctors.find(ctorName);
    if (cit != ci.ctors.end() && cit->second.isFallible)
        return ctFail(("`" + shown + "` returns a `Result`, and a compile-time value is built by a ctor that cannot "
                       "fail").c_str(), line);
    if (!mi.node || !mi.node->body) return ctFail(("`" + shown + "` has no body to run at compile time").c_str(), line);
    if (!ctBindByName(mi.node->params, names, args, shown, line)) return false;
    // The field initializers (`int32 fd = -1;`), which hold on entry to every ctor — evaluated as the type, in
    // the type's scope, where they were written.
    for (size_t i = 0; i < ci.fields.size() && i < self.elems.size(); ++i)
        if (ci.fields[i].initializer) {
            std::string savedOwner = _ctCurrentOwner; _ctCurrentOwner = ci.name;
            NsCtx savedNs = _nsCtx;
            _nsCtx = NsCtx{}; _nsCtx.scope = ci.scope; _nsCtx.usings = ci.usings; _nsCtx.symbolAliases = ci.symbolAliases;
            restoreFileRung(_nsCtx, ci.declFile);
            CTEnv fenv; CTValue fv;
            bool ok = ctEvalExpr(ci.fields[i].initializer, fenv, fv);
            _nsCtx = savedNs; _ctCurrentOwner = savedOwner;
            if (!ok) return false;
            ctCoerceTo(self.elems[i], fv);
            self.elems[i] = fv;
        }
    std::vector<CTArg> bound;
    for (size_t i = 0; i < args.size(); ++i) {
        auto& p = (*mi.node->params)[i];
        bound.push_back({ *p->identifier->value, p->type, args[i] });
    }
    CTValue result;
    if (!ctRunMember(ci, bound, mi.node->body, SharedIdentifier(), &self, shown, line, result)) return false;
    out = result.isStruct ? result : self;
    if (!out.isStruct || out.structClass != ci.name)
        return ctFail(("`" + shown + "` must produce a `" + demangleForDisplay(ci.name) + "`").c_str(), line);
    return true;
}

// A call through `.`: `Type.ctor(…)` constructs; `value.method(…)` runs a method of the value's own type, and a
// method that is not `const fn` writes its receiver back when the receiver is a local or `this`.
bool CEmitter::ctEvalCallExpr(InvocationNode* inv, CTEnv& env, CTValue& out)
{
    auto* ma = inv ? dynamic_cast<MemberAccessNode*>(inv->expression.get()) : nullptr;
    const int line = inv ? inv->line : 0;
    if (!ma || !ma->identifier || !ma->identifier->value || !ma->expression)
        return ctFail("a compile-time call names a `comptime fn`, a value type's ctor, or a method of a value", line);
    const std::string& member = *ma->identifier->value;
    // `Type.ctor(…)` — the receiver names a TYPE (a binding can never be named like one in reach).
    if (auto* rid = dynamic_cast<IdentifierNode*>(ma->expression.get()))
        if (rid->value && !env.vars.count(*rid->value)) {
            std::string cls;
            if (rid->genericArgs && !rid->genericArgs->empty()) cls = cType(std::static_pointer_cast<IdentifierNode>(ma->expression));
            else cls = resolveUserName(*rid->value, rid->qualifier);
            auto cit = _classes.find(cls);
            if (cit != _classes.end() && !_typeConsts.count(cls + "::" + member))
                return ctConstruct(cit->second, member, inv, env, line, out);
        }
    CTValue recv; if (!ctEvalExpr(ma->expression, env, recv)) return false;
    if (!recv.isStruct)
        return ctFail(("`" + member + "` — only a `type value`'s methods run at compile time").c_str(), line);
    ClassInfo& ci = _classes[recv.structClass];
    const std::string shown = demangleForDisplay(ci.name) + "." + member;
    auto mit = ci.methods.find(member);
    if (mit == ci.methods.end() || mit->second.isCtor || mit->second.isStatic || mit->second.isOperator)
        return ctFail(("`" + demangleForDisplay(ci.name) + "` has no method `" + member + "`").c_str(), line);
    MethodInfo& mi = mit->second;
    if (!ctMemberVisible(ci, mi.visibility, member))
        return ctFail(("'" + member + "' is " + (mi.visibility == Visibility::Protected ? "protected" : "private")
                       + " in '" + ci.name + "'").c_str(), line);
    if (!mi.node || !mi.node->body) return ctFail(("`" + shown + "` has no body to run at compile time").c_str(), line);
    std::vector<CTValue> args; std::vector<std::string> names;
    if (!ctCallArgs(inv, env, args, names)) return false;
    if (!ctBindByName(mi.node->params, names, args, shown, line)) return false;
    std::vector<CTArg> bound;
    for (size_t i = 0; i < args.size(); ++i) {
        auto& p = (*mi.node->params)[i];
        bound.push_back({ *p->identifier->value, p->type, args[i] });
    }
    CTValue self = recv;
    if (!ctRunMember(ci, bound, mi.node->body, mi.returnType, &self, shown, line, out)) return false;
    if (!mi.isConst) {
        if (auto* rid = dynamic_cast<IdentifierNode*>(ma->expression.get())) {
            if (rid->value && (!rid->qualifier || rid->qualifier->empty())) {
                auto it = env.vars.find(*rid->value);
                if (it != env.vars.end()) it->second = self;
            }
        } else if (dynamic_cast<ThisAccessNode*>(ma->expression.get())) {
            env.vars["this"] = self;
        }
    }
    return true;
}

// `Type::name(…)` naming a value type's `static fn` — the dual-use widening reaches it too. `handled` is false
// when `owner` has no such static, so the caller keeps its own diagnostic.
bool CEmitter::ctStaticMember(const std::string& owner, const std::string& name, const std::string& shown,
                              InvocationNode* inv, CTEnv& env, int line, CTValue& out, bool& handled)
{
    handled = false;
    auto oc = _classes.find(owner);
    if (oc == _classes.end()) return false;
    ClassInfo& ci = oc->second;
    auto mit = ci.methods.find(name);
    if (mit == ci.methods.end() || !mit->second.isStatic || mit->second.isCtor || mit->second.isOperator || !mit->second.node)
        return false;
    handled = true;
    MethodInfo& mi = mit->second;
    if (!ctMemberVisible(ci, mi.visibility, name))
        return ctFail(("'" + name + "' is private in '" + ci.name + "'").c_str(), line);
    std::vector<CTValue> args; std::vector<std::string> names;
    if (!ctCallArgs(inv, env, args, names)) return false;
    if (!ctBindByName(mi.node->params, names, args, shown, line)) return false;
    std::vector<CTArg> bound;
    for (size_t i = 0; i < args.size(); ++i) {
        auto& p = (*mi.node->params)[i];
        bound.push_back({ *p->identifier->value, p->type, args[i] });
    }
    return ctRunMember(ci, bound, mi.node->body, mi.returnType, nullptr, shown, line, out);
}

// An operator on a value at compile time — found as the emitter finds it: the method form (arity 1, or 0 for a
// unary) on the left operand's type, else the free form (arity 2) on either operand's type — matched by the
// token and the operand types, then its body run.
bool CEmitter::ctStructOperator(int token, const CTValue& lv, const CTValue* rv, int line, CTValue& out)
{
    const bool unary = rv == nullptr;
    // Does a declared parameter type accept this operand? A class must be the operand's own; a scalar
    // parameter takes a scalar.
    auto accepts = [&](ClassInfo& owner, SharedIdentifier pt, const CTValue& v) -> bool {
        if (!pt) return false;
        NsCtx saved = _nsCtx;
        _nsCtx = NsCtx{}; _nsCtx.scope = owner.scope; _nsCtx.usings = owner.usings; _nsCtx.symbolAliases = owner.symbolAliases;
        restoreFileRung(_nsCtx, owner.declFile);
        const std::string pc = (pt->value && *pt->value == "This") ? owner.name : cType(pt);
        _nsCtx = saved;
        if (v.isStruct) return pc == v.structClass;
        return !_classes.count(pc) || _classes[pc].isScalarEnum();   // a scalar operand takes a scalar parameter
    };
    std::vector<ClassInfo*> owners;
    if (lv.isStruct) owners.push_back(&_classes[lv.structClass]);
    if (rv && rv->isStruct && (!lv.isStruct || rv->structClass != lv.structClass)) owners.push_back(&_classes[rv->structClass]);
    for (int pass = 0; pass < 2; ++pass)
        for (ClassInfo* ci : owners)
            for (auto& kv : ci->methods) {
                MethodInfo& mi = kv.second;
                if (!mi.isOperator || !mi.opDecl || !mi.opDecl->operatorDeclarator) continue;
                auto* d = mi.opDecl->operatorDeclarator.get();
                if (d->opToken != token) continue;
                std::vector<CTArg> bound;
                CTValue self; CTValue* selfp = nullptr;
                if (pass == 0) {
                    // the method form, on the left operand's type only
                    if (ci != (lv.isStruct ? &_classes[lv.structClass] : nullptr)) continue;
                    if (unary ? mi.arity != 0 : (mi.arity != 1 || !accepts(*ci, d->param1Type, *rv))) continue;
                    if (!unary) bound.push_back({ d->param1Name && d->param1Name->value ? *d->param1Name->value : std::string(),
                                                  d->param1Type, *rv });
                    self = lv; selfp = &self;
                } else {
                    if (unary || mi.arity != 2) continue;
                    if (!accepts(*ci, d->param1Type, lv) || !accepts(*ci, d->param2Type, *rv)) continue;
                    bound.push_back({ d->param1Name && d->param1Name->value ? *d->param1Name->value : std::string(), d->param1Type, lv });
                    bound.push_back({ d->param2Name && d->param2Name->value ? *d->param2Name->value : std::string(), d->param2Type, *rv });
                }
                const std::string shown = demangleForDisplay(ci->name) + " operator" + binaryOperator(token);
                if (!ctMemberVisible(*ci, mi.visibility, kv.first))
                    return ctFail(("`" + shown + "` is private in '" + ci->name + "'").c_str(), line);
                return ctRunMember(*ci, bound, mi.opDecl->body, d->returnType, selfp, shown, line, out);
            }
    return ctFail((std::string("no `operator") + binaryOperator(token) + "` for these operands at compile time").c_str(), line);
}

// `==` on two values of one type: a hand-written `equals` (Equatable) runs; `@generate(Equatable)` compares
// memberwise, as the generated C does. A type with neither has no equality, as at run time.
bool CEmitter::ctStructEquals(const CTValue& a, const CTValue& b, int line, bool& eq)
{
    if (!a.isStruct || !b.isStruct || a.structClass != b.structClass)
        return ctFail("`==` compares two values of one type", line);
    ClassInfo& ci = _classes[a.structClass];
    auto mit = ci.methods.find("equals");
    if (mit != ci.methods.end() && mit->second.node && mit->second.node->body && mit->second.node->params
        && mit->second.node->params->size() == 1) {
        auto& p = (*mit->second.node->params)[0];
        CTValue self = a, r;
        if (!ctRunMember(ci, { { *p->identifier->value, p->type, b } }, mit->second.node->body,
                         mit->second.returnType, &self, demangleForDisplay(ci.name) + ".equals", line, r)) return false;
        eq = ctAsI(r) != 0;
        return true;
    }
    if (!ci.genEquatable)
        return ctFail(("`" + demangleForDisplay(ci.name) + "` has no equality — implement `Equatable`, or "
                       "`@generate(Equatable)`").c_str(), line);
    std::function<bool(const CTValue&, const CTValue&)> same = [&](const CTValue& x, const CTValue& y) -> bool {
        if (x.isStruct || x.isArray) {
            if (x.elems.size() != y.elems.size()) return false;
            for (size_t i = 0; i < x.elems.size(); ++i) if (!same(x.elems[i], y.elems[i])) return false;
            return true;
        }
        return x.kind == CTValue::Float ? ctAsF(x) == ctAsF(y) : ctAsI(x) == ctAsI(y);
    };
    eq = same(a, b);
    return true;
}

// Evaluate the module `comptime` constants whose fold needed the interpreter (a `comptime fn` call), BEFORE
// collectCollections so a comptime-fn-derived scalar can size an array. Each is evaluated ON DEMAND — a
// constant that reads one not evaluated yet evaluates that one first (ctResolveConst) — so declaration order
// and file order do not matter, and a cycle is caught as one. Scalar results are mirrored into _moduleConsts
// (int sizing) and _comptimeConstVals (emit).
void CEmitter::evalComptimeConsts()
{
    for (auto& dc : _ctDeferredConsts) _ctDeferredState[dc.cName] = CTConstState::Pending;
    for (auto& dc : _ctDeferredConsts) evalDeferredConst(dc.cName);
}

bool CEmitter::evalDeferredConst(const std::string& cName)
{
    auto st = _ctDeferredState.find(cName);
    if (st == _ctDeferredState.end()) return true;                  // not a deferred constant
    if (st->second == CTConstState::Done) return !_ctErroredConsts.count(cName);
    CTDeferredConst* found = nullptr;
    for (auto& d : _ctDeferredConsts) if (d.cName == cName) { found = &d; break; }
    if (!found) return true;
    CTDeferredConst& dc = *found;
    st->second = CTConstState::Evaluating;
    // Its own evaluation, with its own budget and file; the reader that forced it resumes where it was.
    NsCtx saved = _nsCtx;
    long savedSteps = _ctSteps; int savedDepth = _ctDepth; bool savedFailed = _ctFailed;
    std::string savedOwner = _ctCurrentOwner;
    std::string savedUnit = _collectingUnitPath;
    _nsCtx = dc.ctx;
    // A type constant evaluates AS its type — its own private members in reach, exactly as its comptime fns
    // are — and a module constant as no type at all (KR-93).
    _ctSteps = 0; _ctDepth = 0; _ctFailed = false; _ctCurrentOwner = dc.owner;
    if (!dc.ctx.unitPath.empty()) _collectingUnitPath = dc.ctx.unitPath;   // a failure names the DECLARING file
    // The name a diagnostic shows: a type constant as the source spells it (`Palette::SIZE`), never the
    // mangled owner key it is filed under.
    const std::string shown = dc.owner.empty() ? dc.cName
                            : demangleForDisplay(dc.owner) + dc.cName.substr(dc.owner.size());
    bool good = [&]() -> bool {
        CTEnv env; CTValue v;
        // An ARRAY-typed constant may be initialized by an array literal (`[1, 2, 3]`, `[v; N]`) as well as
        // by a `comptime fn` call: the literal goes through `ctBuildArrayInit`, the same arm a comptime
        // LOCAL's initializer takes, and bakes through the same `ctRender`. It used to be refused as
        // "unsupported expression in comptime fn" — the general evaluator has no array-literal case, on
        // purpose (an array is not a scalar value) — so the first external package built a 64-entry table
        // by assignment inside a `comptime fn`. The consumer-driven audit (0.9.221) admitted the literal.
        CTValue elemProto; int64_t an; std::string aelem;
        const bool isArrayConst = ctArrayInfo(dc.type, elemProto, an, aelem);
        bool ok;
        if (isArrayConst && dynamic_cast<ArrayLiteralNode*>(dc.init.get())) {
            v.isArray = true;
            ok = ctBuildArrayInit(dc.init, env, elemProto, (size_t)an, v.elems);
        } else {
            ok = ctEvalExpr(dc.init, env, v);
        }
        if (!ok) {
            if (!_ctFailed)
                ctFail(("a `comptime` initializer must be a compile-time constant (a literal, an array literal, "
                        "`sizeof` of a fixed-width scalar, const arithmetic, another `comptime`, or a `comptime fn` "
                        "call) — `" + shown + "`").c_str(), dc.line);
            _ctErroredConsts.insert(dc.cName);   // a precise error was emitted; suppress the emit-time duplicate
            return false;
        }
        // Coerce/validate against the constant's declared type, then bake.
        if (isArrayConst) {
            if (!v.isArray || (int64_t)v.elems.size() != an) {
                ctFail(("a `comptime` array constant's initializer must return an `InlineArray` of the "
                        "declared size — `" + shown + "`").c_str(), dc.line);
                _ctErroredConsts.insert(dc.cName);
                return false;
            }
            v.elemCType = aelem;   // bake with the constant's declared element C type
            _comptimeConstVals[dc.cName] = v;
            return true;
        }
        // A constant's initializer is never emitted, so no run-time type check sees it: a value-type constant
        // must be given a value of its own type, and a scalar one a scalar (KR-93).
        CTValue shape;
        const bool shaped = ctValueProto(dc.type, shape);
        if ((shaped && shape.isStruct) != v.isStruct || (shape.isStruct && v.structClass != shape.structClass)) {
            ctFail(("`" + shown + "` is declared a `" + (dc.type && dc.type->value ? *dc.type->value : std::string("?"))
                    + "`, and its initializer builds " + (v.isStruct ? "a `" + demangleForDisplay(v.structClass) + "`"
                                                                     : std::string("a number")).c_str()
                    ).c_str(), dc.line);
            _ctErroredConsts.insert(dc.cName);
            return false;
        }
        CTValue proto;
        if (ctTypeInfo(dc.type, proto)) ctCoerce(proto, v);
        _comptimeConstVals[dc.cName] = v;
        if (v.kind == CTValue::Int || v.kind == CTValue::Bool) {
            // Mirror an integer for const-generic array sizing, into the table that size reads: a module
            // constant's, or the type constant's own record (constArgN reads `hasValue`).
            if (dc.owner.empty()) _moduleConsts[dc.cName] = v.i;
            else {
                auto tc = _typeConsts.find(dc.cName);
                if (tc != _typeConsts.end()) { tc->second.hasValue = true; tc->second.value = v.i; }
            }
        }
        return true;
    }();
    _nsCtx = saved;
    _ctSteps = savedSteps; _ctDepth = savedDepth; _ctFailed = savedFailed;
    _ctCurrentOwner = savedOwner;
    _collectingUnitPath = savedUnit;
    _ctDeferredState[cName] = CTConstState::Done;
    return good;
}

// KR-93. A LOCAL `comptime`, evaluated by the same interpreter a module or type constant runs — so one keyword
// takes one set of initializers at every scope (a `comptime fn` call, a float, a table, another constant) — with
// the enclosing type's private members in reach, as its methods have them. It used to be checked only for
// "C can compute this", which accepted a struct's `sizeof` (a value the target's ABI decides) and refused a
// `comptime fn` call while the second diagnostic told the author to assign the call to a `comptime` constant.
bool CEmitter::evalLocalComptime(SharedIdentifier type, SharedExpression init, const std::string& name, int line,
                                 CTValue& out)
{
    long savedSteps = _ctSteps; int savedDepth = _ctDepth; bool savedFailed = _ctFailed;
    std::string savedOwner = _ctCurrentOwner;
    _ctSteps = 0; _ctDepth = 0; _ctFailed = false;
    _ctCurrentOwner = _currentClass ? _currentClass->name : std::string();
    bool ok = [&]() -> bool {
        CTEnv env; CTValue v;
        CTValue elemProto; int64_t an; std::string aelem;
        const bool isArray = ctArrayInfo(type, elemProto, an, aelem);
        bool good = (isArray && dynamic_cast<ArrayLiteralNode*>(init.get()))
                  ? (v.isArray = true, ctBuildArrayInit(init, env, elemProto, (size_t)an, v.elems))
                  : ctEvalExpr(init, env, v);
        if (!good) {
            if (!_ctFailed)
                ctFail(("a `comptime` local must have a compile-time-constant initializer (a literal, an array "
                        "literal, `sizeof` of a fixed-width scalar, const arithmetic, another `comptime`, or a "
                        "`comptime fn` call) — `" + name + "`; use `const` for a runtime-initialized immutable").c_str(),
                       line);
            return false;
        }
        if (isArray) {
            if (!v.isArray || (int64_t)v.elems.size() != an)
                return ctFail(("a `comptime` array local's initializer must produce an `InlineArray` of the declared "
                               "size — `" + name + "`").c_str(), line);
            v.elemCType = aelem;
            out = v;
            return true;
        }
        CTValue shape;
        const bool shaped = ctValueProto(type, shape);
        if ((shaped && shape.isStruct) != v.isStruct || (shape.isStruct && v.structClass != shape.structClass))
            return ctFail(("`" + name + "` is declared a `" + (type && type->value ? *type->value : std::string("?"))
                           + "`, and its initializer builds " + (v.isStruct ? "a `" + demangleForDisplay(v.structClass) + "`"
                                                                            : std::string("a number"))).c_str(), line);
        CTValue proto;
        if (ctTypeInfo(type, proto)) ctCoerce(proto, v);   // as evalDeferredConst: an unmodelled scalar (an enum,
        out = v;                                           // `isize`) keeps the value it folded to
        return true;
    }();
    _ctSteps = savedSteps; _ctDepth = savedDepth; _ctFailed = savedFailed; _ctCurrentOwner = savedOwner;
    return ok;
}

// ---------------------------------------------------------------------------
// Plain enum member values
//
// `type enum K { A = 1, B = K::A + 1 }` had no spelling that built: the initializer went to the RUNTIME
// expression emitter, so `K::A + 1` was refused by the enum-arithmetic rule (right for a VALUE, beside
// the point when the member is being DEFINED as an integer), `cast<int32>(K::A) + 1` reached clang as
// `KAMA_ADD(...)` — a `_Generic` function call in a debug build, not a constant expression, and a plain
// `+` in release: a build-mode divergence — and a cross-enum alias or a `comptime` constant emitted a C
// token that `_enums`' map order had not declared yet. Folding here, with the interpreter that already
// evaluates every `comptime` initializer, takes the initializer out of the emitter's reach entirely: the
// value-side rules never see it, and what is emitted is a decimal.
//
// A member is folded on demand and memoised, so a forward reference to a later sibling and a reference
// into another enum both work, and the only order that matters is the one C also imposes: an implicit
// member is one more than the member before it.
bool CEmitter::enumMemberValue(EnumInfo& ei, size_t idx, int64_t& out)
{
    EnumMember& m = ei.members[idx];
    if (ei.isExtern) return false;   // KR-55: never folded; the one caller that reaches here reports at the use
    if (m.hasFolded) { out = m.folded; return true; }
    if (m.folding)
        return ctFail(("enum member `" + demangleForDisplay(ei.name) + "::" + m.name
                       + "` is defined in terms of itself").c_str(), m.line);
    m.folding = true;
    bool ok = false;
    if (!m.value) {
        int64_t prev = -1;
        ok = (idx == 0) || enumMemberValue(ei, idx - 1, prev);
        if (ok) m.folded = prev + 1;
    } else {
        // Under the DECLARING enum's scope and file: an initializer reached on demand from another
        // enum's fold must still resolve its names where it was written, and blame its own file.
        NsCtx savedCtx = _nsCtx;
        std::string savedFile = _collectingUnitPath;
        // The whole context a class body gets, not scope + usings alone: without the per-symbol imports and
        // the file rung, a module constant — even one declared in the SAME file of a module — was an
        // "unknown identifier" here, so an enum in any project could not read one.
        _nsCtx = NsCtx{}; _nsCtx.scope = ei.scope; _nsCtx.usings = ei.usings; _nsCtx.symbolAliases = ei.symbolAliases;
        restoreFileRung(_nsCtx, ei.declFile);
        _collectingUnitPath = ei.declFile;

        // A sibling is spelled `K::A`, as a variant is everywhere else. Said here, with the hint the
        // identifier arm already gives, rather than left to the interpreter's "unknown identifier".
        std::function<bool(const SharedExpression&)> bareSibling = [&](const SharedExpression& e) -> bool {
            ASTNode* n = e.get();
            if (!n) return false;
            if (auto* id = dynamic_cast<IdentifierNode*>(n)) {
                if (id->value && (!id->qualifier || id->qualifier->empty()))
                    for (auto& s : ei.members)
                        if (s.name == *id->value) {
                            unsupported(("`" + s.name + "` is a variant of `" + ei.name + "` — write `"
                                         + ei.name + "::" + s.name + "`").c_str(), m.line);
                            return true;
                        }
                return false;
            }
            if (auto* b = dynamic_cast<BinaryExpressionNode*>(n))      return bareSibling(b->LHS) || bareSibling(b->RHS);
            if (auto* u = dynamic_cast<SimpleUnaryExpressionNode*>(n)) return bareSibling(u->expression);
            if (auto* c = dynamic_cast<CastNode*>(n))                  return bareSibling(c->unaryExpression);
            if (auto* l = dynamic_cast<LogicalAndOrNode*>(n))          return bareSibling(l->LHS) || bareSibling(l->RHS);
            if (auto* t = dynamic_cast<TernaryExpressionNode*>(n))
                return bareSibling(t->condition) || bareSibling(t->LHS) || bareSibling(t->RHS);
            return false;
        };
        if (!bareSibling(m.value)) {
            CTEnv env; CTValue v;
            if (ctEvalExpr(m.value, env, v)) {
                const std::string member = "`" + ei.name + "::" + m.name + "`";
                if (v.kind != CTValue::Int) {
                    unsupported(("enum member " + member + " must be an integer — its initializer is a "
                                 + std::string(v.kind == CTValue::Float ? "float" : "bool")).c_str(), m.line);
                } else {
                    // It must fit the tag: the pinned `IntType`, or C's `int` for an unpinned enum. C
                    // would truncate a pinned one in silence (`: uint8 { A = 300 }` built, and `A` was 44).
                    const bool pinned = !ei.underlyingCType.empty();
                    int  bits = 32;
                    bool sgn  = true;
                    auto dn = _enumDeclNodes.find(ei.name);
                    CTValue tag;
                    if (pinned && dn != _enumDeclNodes.end() && dn->second
                        && ctTypeInfo(dn->second->underlyingType, tag) && tag.kind == CTValue::Int) {
                        bits = tag.width; sgn = tag.isSigned;
                    }
                    bool fits = true;
                    if (bits > 0 && bits < 64) {
                        const int64_t hi = sgn ? ((int64_t)1 << (bits - 1)) - 1 : ((int64_t)1 << bits) - 1;
                        const int64_t lo = sgn ? -((int64_t)1 << (bits - 1)) : 0;
                        fits = v.i >= lo && v.i <= hi;
                    } else if (bits == 64 && !sgn) {
                        fits = v.i >= 0;   // held in int64; a `uint64` value past INT64_MAX has no spelling here
                    }
                    if (!fits)
                        unsupported(("enum member " + member + " is " + std::to_string((long long)v.i)
                                     + ", which does not fit the enum's `"
                                     + (pinned ? primKeyOfCType(ei.underlyingCType) : std::string("int32"))
                                     + "` tag" + (pinned ? "" : " (an unpinned enum's tag is `int32`; pin it with "
                                                                 "`type enum E : IntType`)")).c_str(), m.line);
                    else { m.folded = v.i; ok = true; }
                }
            }
        }
        _nsCtx = savedCtx;
        _collectingUnitPath = savedFile;
    }
    m.folding = false;
    // A member that failed counts as folded (to 0) from here on: its diagnostic has been emitted and the
    // build is refused, and re-evaluating it from every member that names it would only repeat the message.
    m.hasFolded = true;
    out = m.folded;
    return ok;
}

// Every plain enum, every member, after the module `comptime` constants are baked (an initializer may
// read one). Each top-level evaluation resets the interpreter's budget the way evalComptimeConsts does.
void CEmitter::foldEnumMembers()
{
    for (auto& kv : _enums)
        for (size_t i = 0; i < kv.second.members.size() && !kv.second.isExtern; ++i) {
            _ctSteps = 0; _ctDepth = 0; _ctFailed = false; _ctCurrentOwner.clear();
            int64_t v;
            enumMemberValue(kv.second, i, v);
        }
}
