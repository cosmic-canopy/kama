// LSP / front-end-as-library query index (M0 T4/T5).
//
// Everything here runs STRICTLY AFTER analyze()'s analysis walk, over the now-stable symbol tables. It is
// READ-ONLY: it never resolves-and-registers, never emits, never mutates the tables (the one nuance is the
// resolution replay in definitionAt(), which save/restores _nsCtx — see T4c). Emission is completely
// untouched, so `kama build` stays byte-identical.
//
// Scope for M0: DECLARATION-SITE and SIGNATURE/TYPE-REFERENCE positions only — types, contracts, enums,
// free/generic functions, methods, ctors. NOT body-expression use-sites (that is the M3 find-references
// walk over the ~150 stmt/expr node kinds) and NOT locals-hover (M2; per-function scopes are torn down).
// Fields and enum MEMBERS are also deferred to M2 — the tables don't retain a per-field/per-member decl
// node (hence no span), and adding that retention is M2 work when hover needs it.

#include "kama.cemit.h"
#include "kama.ast.h"

// ---- SrcRange / SymKind helpers -------------------------------------------------------------------------

bool SrcRange::contains(int l, int c) const
{
    // A point span (end==0) matches only its own line:col. Otherwise [start, end) with line-major order.
    int el = endLine ? endLine : line;
    int ec = endLine ? endColumn : column;
    if (l < line || l > el) return false;
    if (l == line && c < column) return false;
    if (l == el && c > ec) return false;
    return true;
}

long SrcRange::span() const
{
    // Coarse width for "smallest span wins". Multi-line spans are wider than any single-line one; a point
    // span (end==0) is treated as maximally wide so a real enclosing range is preferred when both match.
    if (!endLine) return 1L << 30;
    if (endLine != line) return (long)(endLine - line) * 100000 + endColumn;
    return endColumn - column;
}

const char* symKindName(SymKind k)
{
    switch (k) {
        case SymKind::Class:       return "class";
        case SymKind::Value:       return "value";
        case SymKind::Resource:    return "resource";
        case SymKind::Contract:    return "contract";
        case SymKind::Enum:        return "enum";
        case SymKind::EnumMember:  return "enum-member";
        case SymKind::Function:    return "function";
        case SymKind::Method:      return "method";
        case SymKind::Ctor:        return "ctor";
        case SymKind::Field:       return "field";
        case SymKind::GenericType: return "generic-type";
        case SymKind::GenericFn:   return "generic-fn";
    }
    return "symbol";
}

// ---- span extraction ------------------------------------------------------------------------------------

static SrcRange rangeOfNode(const ASTNode* n)
{
    if (!n) return SrcRange{};
    return SrcRange{ n->line, n->column, n->endLine, n->endColumn };
}
static SrcRange rangeOfId(const SharedIdentifier& id)
{
    return id ? rangeOfNode(id.get()) : SrcRange{};
}

// ---- def-site table (T4a) -------------------------------------------------------------------------------

const CompilationUnit* CEmitter::unitOfDecl(const ASTNode* topLevelDecl) const
{
    auto it = _declUnit.find(topLevelDecl);
    return it == _declUnit.end() ? nullptr : it->second;   // not a user top-level decl => prelude/std
}

void CEmitter::addDefSite(const std::string& key, SymKind kind, const CompilationUnit* unit,
                          ASTNode* declNode, const SharedIdentifier& nameId,
                          const std::string& display, const std::string& container)
{
    if (key.empty()) return;
    DefSite d;
    d.key            = key;
    d.kind           = kind;
    d.range          = rangeOfNode(declNode);
    d.selectionRange = nameId ? rangeOfId(nameId) : d.range;   // the NAME id is the tighter click target
    d.unit           = unit;
    d.node           = declNode;
    d.display        = display;
    d.container      = container;
    _defSites[key] = d;   // one entry per resolved mangled name (map keys are already unique)
}

void CEmitter::buildDefSites()
{
    _defSites.clear();
    _declUnit.clear();

    // Attribute decls to their owning USER unit. Only user units go in the map, so prelude/std decls fall
    // through to nullptr (unitOfDecl) and are excluded from documentSymbols while still resolvable.
    for (auto& u : _units) {
        if (!u || !u->codeDeclarationList) continue;
        for (auto& decl : *u->codeDeclarationList)
            if (decl) _declUnit[decl.get()] = u.get();
    }

    auto bareOf = [](const SharedIdentifier& id, const std::string& key) -> std::string {
        if (id && id->value) return *id->value;                 // the source spelling, when we have the name id
        auto p = key.rfind("__");                               // else demangle the trailing segment
        return p == std::string::npos ? key : key.substr(p + 2);
    };

    // Concrete user types (value/resource). Skip compiler-synthesized entries: generic instances, intrinsic
    // collections, tagged-union backings, extern FFI structs — none is a user `type` declaration.
    for (auto& kv : _classes) {
        ClassInfo& ci = kv.second;
        if (ci.isGenericInst || ci.isIntrinsicColl || ci.isVariant || ci.isExternStruct || !ci.node) continue;
        SymKind k = ci.kind == TypeKind::Value ? SymKind::Value
                  : ci.kind == TypeKind::Resource ? SymKind::Resource
                  : SymKind::Class;
        const CompilationUnit* unit = unitOfDecl(ci.node);
        std::string bare = bareOf(ci.node->name, kv.first);
        addDefSite(kv.first, k, unit, ci.node, ci.node->name, bare, "");
        // Methods + ctors share the type's owning unit (their nodes aren't top-level decls).
        for (auto& mkv : ci.methods) {
            MethodInfo& mi = mkv.second;
            ASTNode* mnode = mi.node ? (ASTNode*)mi.node : (ASTNode*)mi.opDecl;
            if (!mnode) continue;   // intrinsic / synthesized (serde, bag ctor) — no source site
            SharedIdentifier mname = mi.node ? mi.node->name : SharedIdentifier();
            addDefSite(mi.cName, mi.isCtor ? SymKind::Ctor : SymKind::Method, unit, mnode, mname,
                       bare + "." + mkv.first, bare);
        }
        for (auto& ckv : ci.ctors) {
            CtorInfo& ctor = ckv.second;
            if (!ctor.node) continue;
            // key the ctor under a synthetic "<type>::ctor <name>" — it is not a resolveFunc target, but it
            // gives the outline a construction entry with a real span.
            addDefSite(kv.first + "::ctor:" + ckv.first, SymKind::Ctor, unit, ctor.node,
                       SharedIdentifier(), bare + "." + ckv.first, bare);
        }
    }

    // Generic type templates (Box<T>) — parked out of _classes; the ClassInfo.node is the template decl.
    for (auto& kv : _genericTypes) {
        ClassInfo& ci = kv.second;
        if (!ci.node) continue;
        const CompilationUnit* unit = unitOfDecl(ci.node);
        addDefSite(kv.first, SymKind::GenericType, unit, ci.node, ci.node->name,
                   bareOf(ci.node->name, kv.first), "");
    }

    // Contracts (non-generic + generic templates). node is the `type contract` ClassDeclarationNode.
    for (auto* table : { &_interfaces, &_genericContracts }) {
        for (auto& kv : *table) {
            InterfaceInfo& ii = kv.second;
            if (!ii.node) continue;
            const CompilationUnit* unit = unitOfDecl(ii.node);
            addDefSite(kv.first, SymKind::Contract, unit, ii.node, ii.node->name,
                       bareOf(ii.node->name, kv.first), "");
        }
    }

    // Enums (plain + tagged). _enumDeclNodes carries the decl node keyed by the same qualified name.
    for (auto& kv : _enums) {
        auto dn = _enumDeclNodes.find(kv.first);
        EnumDeclarationNode* en = dn == _enumDeclNodes.end() ? nullptr : dn->second;
        if (!en) continue;
        const CompilationUnit* unit = unitOfDecl(en);
        addDefSite(kv.first, SymKind::Enum, unit, en, en->identifier,
                   bareOf(en->identifier, kv.first), "");
    }

    // Free functions (+ generic free-fn templates). node was captured in collectSignatures.
    for (auto& kv : _funcs) {
        FuncSig& sig = kv.second;
        if (!sig.node) continue;
        const CompilationUnit* unit = unitOfDecl(sig.node);
        SymKind k = _generics.count(kv.first) ? SymKind::GenericFn : SymKind::Function;
        addDefSite(kv.first, k, unit, sig.node, sig.node->name, bareOf(sig.node->name, kv.first), "");
    }
}
