// LSP / front-end-as-library query index (M0 T4/T5).
//
// Everything here runs STRICTLY AFTER analyze()'s analysis walk, over the now-stable symbol tables. It is
// READ-ONLY: it never resolves-and-registers, never emits, never mutates the tables (the one nuance is the
// resolve-fill sweep in buildPositions(), which save/restores _nsCtx). Emission is completely untouched,
// so `kama build` stays byte-identical. The query facade itself is const and does no resolution at all —
// every indexed position carries its resolved DefSite key.
//
// Scope: DECLARATION-SITE, SIGNATURE/TYPE-REFERENCE, and (M3) BODY use-site positions — types, contracts,
// enums, free/generic functions, methods, ctors, and (M3.4) LOCALS, PARAMS, FIELDS and enum MEMBERS. Body
// coverage does NOT come from a walker here: the real resolver records each use as analysis resolves it
// (CEmitter::recordRef), so references and go-to-definition agree by construction. Function-scoped
// bindings have no table entry to key on, so their DECLARATIONS are recorded the same way, during the
// walk that knows the enclosing scope (CEmitter::recordDef) — see the "M3.4 keys" note below.

#include "kama.cemit.h"
#include "kama.ast.h"

#include <set>

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
        case SymKind::Local:       return "local";
        case SymKind::Param:       return "param";
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
        // Fields (M3.4). Only the declaring type gets an entry: a generic INSTANCE shares the template's
        // field nodes, and instances are skipped above, so a field can never be keyed twice.
        for (auto& fi : ci.fields) {
            if (!fi.nameId) continue;   // synthesized (variant payload of an instantiated template, etc.)
            addDefSite(fieldKey(kv.first, fi.name), SymKind::Field, unit,
                       fi.nameId.get(), fi.nameId, fi.name, bare);
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

    // Enums + their MEMBERS. Driven by _enumDeclNodes, not _enums: a TAGGED enum is lowered to a variant
    // ClassInfo and never reaches _enums, and the _classes loop above skips variant backings — so this is
    // the only place either kind of enum gets a def-site. Every member carries its own identifier node.
    for (auto& kv : _enumDeclNodes) {
        EnumDeclarationNode* en = kv.second;
        if (!en) continue;
        const CompilationUnit* unit = unitOfDecl(en);
        std::string bare = bareOf(en->identifier, kv.first);
        addDefSite(kv.first, SymKind::Enum, unit, en, en->identifier, bare, "");
        if (!en->body) continue;
        for (auto& m : *en->body) {
            if (!m || !m->identifier || !m->identifier->value) continue;
            addDefSite(enumMemberKey(kv.first, *m->identifier->value), SymKind::EnumMember, unit,
                       m.get(), m->identifier, *m->identifier->value, bare);
        }
    }

    // Free functions (+ generic free-fn templates). node was captured in collectSignatures.
    for (auto& kv : _funcs) {
        FuncSig& sig = kv.second;
        if (!sig.node) continue;
        const CompilationUnit* unit = unitOfDecl(sig.node);
        SymKind k = _generics.count(kv.first) ? SymKind::GenericFn : SymKind::Function;
        addDefSite(kv.first, k, unit, sig.node, sig.node->name, bareOf(sig.node->name, kv.first), "");
    }

    // Bindings the walk recorded (M3.4): locals, params, foreach/match bindings. Their key already encodes
    // the declaration site, and the identifier node IS the declaration — range and selectionRange coincide.
    for (const auto& d : _localDefs) {
        if (!d.unit || !d.id || !d.id->value) continue;
        DefSite s;
        s.key   = d.key;
        s.kind  = d.kind;
        s.range = s.selectionRange = SrcRange{ d.id->line, d.id->column, d.id->endLine, d.id->endColumn };
        s.unit      = d.unit;
        s.node      = const_cast<IdentifierNode*>(d.id);
        s.display   = *d.id->value;
        s.container = d.container;
        _defSites[d.key] = s;   // a generic body re-walked per instantiation re-records an identical entry
    }
    _localDefs.clear();
    _localDefs.shrink_to_fit();
}

// ---- position index (T4b) -------------------------------------------------------------------------------
//
// Two position sources, unioned per unit: (1) declaration NAMES — taken straight from _defSites (each
// carries its selectionRange + resolved key + owning unit), so a cursor ON a decl name resolves to its own
// DefSite; (2) signature TYPE references — a small read-only walk over decl/signature structure (params,
// return, base, field, ctor-param types + their nested generic args), recording the raw IdentifierNode so
// definitionAt() can replay resolution at the cursor. The walk NEVER descends into bodies (`block`) — that
// is the M3 use-site walk — and never touches _nsCtx or the tables.

// Record a type-reference identifier and recurse into its generic arguments (List<Point> -> Point).
static void addTypeRef(const SharedIdentifier& t, std::vector<PosEntry>& out)
{
    if (!t) return;
    out.push_back(PosEntry{ SrcRange{ t->line, t->column, t->endLine, t->endColumn }, t.get(), false, "" });
    if (t->genericArgs) for (auto& a : *t->genericArgs) addTypeRef(a, out);
    else                addTypeRef(t->genericArg, out);   // single-arg collections mirror [0] here
}

static void addParamTypes(const SharedParameterList& params, std::vector<PosEntry>& out)
{
    if (!params) return;
    for (auto& p : *params) if (p) addTypeRef(p->type, out);
}

// A body use-site, recorded by the REAL resolver as the analysis walk resolved it (see resolveUserName /
// resolveFunc). Pure append: no diagnostics, no cType, no table mutation — so instrumenting the resolvers
// cannot perturb emission. Dropped outside a module-body walk (_refUnit == nullptr: header/prelude passes,
// whose type refs buildPositions indexes structurally anyway) and in build mode (_analysis == false).
void CEmitter::recordRef(const std::string& key, const IdentifierNode* site)
{
    if (!_analysis || !_refUnit || !site || key.empty()) return;
    _bodyRefs.push_back(RecordedRef{ _refUnit, site, key });
}

// A binding DECLARATION (local / param / foreach or match binding), recorded by the walk that knows the
// enclosing scope. Same gating and same pure-append discipline as recordRef.
void CEmitter::recordDef(const std::string& key, const IdentifierNode* site, SymKind kind,
                         const std::string& container)
{
    if (!_analysis || !_refUnit || !site || key.empty()) return;
    _localDefs.push_back(RecordedDef{ _refUnit, site, key, kind, container });
}

// ---- M3.4 index-only keys -------------------------------------------------------------------------------

std::string CEmitter::bindingKey(const IdentifierNode* declSite) const
{
    if (!declSite || !declSite->value) return "";
    const std::string file = (_refUnit && _refUnit->name) ? *_refUnit->name : "";
    return "local:" + file + ":" + std::to_string(declSite->line) + ":"
         + std::to_string(declSite->column) + ":" + *declSite->value;
}

std::string CEmitter::fieldKey(const std::string& ownerKey, const std::string& name)
{
    if (ownerKey.empty() || name.empty()) return "";
    return "field:" + ownerKey + "::" + name;
}

std::string CEmitter::enumMemberKey(const std::string& enumKey, const std::string& name)
{
    if (enumKey.empty() || name.empty()) return "";
    return "enum:" + enumKey + "::" + name;
}

void CEmitter::registerBinding(const IdentifierNode* declSite, SymKind kind)
{
    if (!_analysis || !_refUnit || !declSite || !declSite->value) return;
    std::string key = bindingKey(declSite);
    if (key.empty()) return;
    recordDef(key, declSite, kind, "");
    if (kind == SymKind::Param)      _paramDeclKeys[*declSite->value] = key;
    else if (!_scopes.empty())       _scopes.back().indexDecls.push_back(Scope::IndexDecl{ *declSite->value, key });
}

std::string CEmitter::bindingKeyOf(const std::string& name) const
{
    if (!_analysis || name.empty()) return "";
    for (int i = (int)_scopes.size() - 1; i >= 0; --i)
        for (auto& d : _scopes[i].indexDecls)
            if (d.name == name) return d.key;
    auto p = _paramDeclKeys.find(name);
    return p == _paramDeclKeys.end() ? std::string() : p->second;
}

void CEmitter::buildPositions()
{
    _positions.clear();
    _refIndex.clear();

    // (1) Declaration names — one entry per user-unit DefSite (prelude/std have unit==nullptr, skipped).
    for (auto& kv : _defSites) {
        const DefSite& d = kv.second;
        if (!d.unit) continue;
        _positions[d.unit].push_back(PosEntry{ d.selectionRange, nullptr, true, d.key });
    }

    // (2) Signature type references — structural walk of each user unit's top-level decls.
    for (auto& u : _units) {
        if (!u || !u->codeDeclarationList) continue;
        auto& out = _positions[u.get()];
        for (auto& decl : *u->codeDeclarationList) {
            if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(decl.get())) {
                addTypeRef(fn->returnType, out);
                addParamTypes(fn->parameters, out);
            } else if (auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get())) {
                if (cd->baseTypes) {
                    addTypeRef(cd->baseTypes->base, out);
                    if (cd->baseTypes->interfaces) for (auto& i : *cd->baseTypes->interfaces) addTypeRef(i, out);
                }
                if (cd->members) for (auto& m : *cd->members) {
                    if (auto* fld = dynamic_cast<ClassFieldDeclarationNode*>(m.get())) {
                        addTypeRef(fld->type, out);
                    } else if (auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get())) {
                        addTypeRef(md->returnType, out);
                        addParamTypes(md->params, out);
                    } else if (auto* ct = dynamic_cast<ClassConstructorDeclarationNode*>(m.get())) {
                        if (ct->declarator) addParamTypes(ct->declarator->params, out);
                    } else if (auto* op = dynamic_cast<ClassOperatorDeclarationNode*>(m.get())) {
                        if (auto* d = op->operatorDeclarator.get()) {
                            addTypeRef(d->returnType, out);
                            addTypeRef(d->param1Type, out);
                            addTypeRef(d->param2Type, out);
                        }
                    }
                }
            }
        }
    }

    // (3) Body use-sites (M3) — merge what the real resolver recorded during the analysis walk. Dedup by
    // identifier NODE: an expression re-walked by an analysis helper, or a generic template body re-emitted
    // per instantiation, hands us the same source node repeatedly, and one source spelling is one reference.
    {
        std::map<const CompilationUnit*, std::set<const IdentifierNode*>> seen;
        for (auto& kv : _positions)
            for (const auto& e : kv.second) if (e.id) seen[kv.first].insert(e.id);
        for (const auto& r : _bodyRefs) {
            if (!r.unit || !r.id) continue;
            if (!seen[r.unit].insert(r.id).second) continue;   // already indexed (sig ref or earlier record)
            _positions[r.unit].push_back(
                PosEntry{ SrcRange{ r.id->line, r.id->column, r.id->endLine, r.id->endColumn },
                          const_cast<IdentifierNode*>(r.id), false, r.key });
        }
        _bodyRefs.clear();
        _bodyRefs.shrink_to_fit();
    }

    // (4) Resolve-fill — give every remaining entry its DefSite key. Only the signature type refs from
    // addTypeRef arrive empty; resolve them in their own unit's namespace context, exactly as the old
    // per-query replay did (resolveUserName reads only _nsCtx + the tables, both stable after analyze()),
    // so precomputing here is equivalent and lets the query facade stay const and replay-free.
    {
        NsCtx saved = _nsCtx;
        for (auto& kv : _positions) {
            auto uit = _unitCtx.find(kv.first);
            if (uit == _unitCtx.end()) continue;
            _nsCtx = uit->second;
            for (auto& e : kv.second)
                if (e.declKey.empty() && e.id && e.id->value)
                    e.declKey = resolveUserName(*e.id->value, e.id->qualifier);   // no `site` => not re-recorded
        }
        _nsCtx = saved;
    }

    // Sort each unit's entries by start position so posAt can scan deterministically.
    for (auto& kv : _positions) {
        auto& v = kv.second;
        std::sort(v.begin(), v.end(), [](const PosEntry& a, const PosEntry& b) {
            if (a.range.line != b.range.line) return a.range.line < b.range.line;
            return a.range.column < b.range.column;
        });
    }

    // (5) The reverse index: DefSite key -> every USE of it. Declaration names are excluded (they are the
    // definition, added back only when a caller asks for includeDecl); prelude/std targets are filtered by
    // the same `!unit` guard the outline and go-to-definition use.
    for (auto& kv : _positions) {
        const CompilationUnit* u = kv.first;
        if (!u || !u->name) continue;
        for (const auto& e : kv.second) {
            if (e.isDeclName || e.declKey.empty()) continue;
            auto d = _defSites.find(e.declKey);
            if (d == _defSites.end() || !d->second.unit) continue;
            // A "use" sitting exactly on its own declaration's name IS the declaration, however it got
            // recorded — includeDecl adds it back, so letting it through would double-count. It reaches
            // here for a type that declares a `ctor`: the ctor's implicit result type resolves through the
            // class's OWN decl identifier, so recordRef stamps the decl name's range as a reference. Left
            // in, find-references shows the declaration twice and rename emits two identical TextEdits over
            // one range — which the LSP spec forbids within a file.
            const DefSite& s = d->second;
            if (s.unit == u && e.range.line == s.selectionRange.line &&
                e.range.column == s.selectionRange.column) continue;
            _refIndex[e.declKey].push_back(Location{ *u->name, e.range });
        }
    }
}

// The smallest-span indexed position containing (line,col), or nullptr. Smallest-span-wins disambiguates
// overlapping approximate spans (e.g. a decl name inside its own decl's coarser range).
const PosEntry* CEmitter::posAt(const CompilationUnit* unit, int line, int col) const
{
    auto it = _positions.find(unit);
    if (it == _positions.end()) return nullptr;
    const PosEntry* best = nullptr;
    for (const auto& e : it->second) {
        if (!e.range.contains(line, col)) continue;
        if (!best || e.range.span() < best->range.span()) best = &e;
    }
    return best;
}

// ---- query facade (T4c + T5) ----------------------------------------------------------------------------

const CompilationUnit* CEmitter::unitForUri(const std::string& uri) const
{
    for (auto& u : _units) if (u && u->name && *u->name == uri) return u.get();
    return nullptr;
}

// The DefSite key the cursor names, or "" if it names nothing indexed. Every PosEntry carries its key
// already (buildPositions' resolve-fill), so this is a pure lookup — no resolution replay, no _nsCtx
// mutation, and correct for FUNCTION references, which the old replay would have run through the TYPE
// resolver. cType is still deliberately never called from a query path: its unsupported() side effect
// would pollute _diagnostics.
std::string CEmitter::declKeyAt(const CompilationUnit* unit, int line, int col) const
{
    const PosEntry* e = posAt(unit, line, col);
    return e ? e->declKey : std::string();
}

// Go-to-definition. On a decl name -> itself; on a reference -> the def-site its resolved key names.
Location CEmitter::definitionAt(const std::string& uri, int line, int col) const
{
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return Location{};
    const PosEntry* e = posAt(unit, line, col);
    if (!e) return Location{};

    // Cursor on a declaration name: the definition is here.
    if (e->isDeclName) {
        auto it = _defSites.find(e->declKey);
        if (it == _defSites.end()) return Location{};
        return Location{ uri, it->second.selectionRange };
    }
    auto it = _defSites.find(e->declKey);
    if (it == _defSites.end() || !it->second.unit) return Location{};   // builtin / prelude / unresolved
    const DefSite& d = it->second;
    return Location{ d.unit->name ? *d.unit->name : uri, d.selectionRange };
}

// Hover: a short "<kind> <name>" for a declaration or a resolved reference.
std::string CEmitter::typeAtPosition(const std::string& uri, int line, int col) const
{
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return "";
    const PosEntry* e = posAt(unit, line, col);
    if (!e) return "";

    auto it = _defSites.find(e->declKey);
    if (it != _defSites.end()) return std::string(symKindName(it->second.kind)) + " " + it->second.display;
    if (e->isDeclName || !e->id || !e->id->value) return "";
    return *e->id->value;   // a builtin/unresolved type — echo the source spelling
}

// Find-references. The cursor may sit on the declaration or on any use — both resolve to the same key, so
// both return the same set (that is the point of keying the index the way go-to-definition keys it).
// Spans every unit analyze() was given, i.e. the open file plus its transitive imports.
std::vector<Location> CEmitter::referencesAt(const std::string& uri, int line, int col,
                                             bool includeDecl) const
{
    std::vector<Location> out;
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return out;
    std::string key = declKeyAt(unit, line, col);
    if (key.empty()) return out;

    auto d = _defSites.find(key);
    if (d == _defSites.end() || !d->second.unit) return out;   // builtin / prelude / unresolved: not renameable
    if (includeDecl) {
        const DefSite& s = d->second;
        out.push_back(Location{ s.unit->name ? *s.unit->name : uri, s.selectionRange });
    }
    auto r = _refIndex.find(key);
    if (r != _refIndex.end()) out.insert(out.end(), r->second.begin(), r->second.end());
    return out;
}

// prepareRename: the click-target range, non-empty only when the cursor names a user symbol we can
// actually rewrite. Returning an empty range makes the editor grey out F2 rather than offer a rename that
// would silently do nothing (builtins, prelude/std, unresolved names, and — until M3.4 — locals).
SrcRange CEmitter::renameRangeAt(const std::string& uri, int line, int col) const
{
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return SrcRange{};
    const PosEntry* e = posAt(unit, line, col);
    if (!e || e->declKey.empty()) return SrcRange{};
    auto d = _defSites.find(e->declKey);
    if (d == _defSites.end() || !d->second.unit) return SrcRange{};
    return e->range;
}

// Document outline: every user-unit def-site in this file, sorted by declaration order. Function-scoped
// bindings are indexed (for go-to-def / references / rename) but deliberately NOT outlined — an outline
// listing every local is noise, and no editor expects one.
std::vector<SymbolInfo> CEmitter::documentSymbols(const std::string& uri) const
{
    const CompilationUnit* unit = unitForUri(uri);
    std::vector<SymbolInfo> out;
    if (!unit) return out;
    for (auto& kv : _defSites) {
        if (kv.second.kind == SymKind::Local || kv.second.kind == SymKind::Param) continue;
        const DefSite& d = kv.second;
        if (d.unit != unit) continue;
        out.push_back(SymbolInfo{ d.display, d.kind, d.range, d.selectionRange, d.container, uri });
    }
    std::sort(out.begin(), out.end(), [](const SymbolInfo& a, const SymbolInfo& b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.column < b.range.column;
    });
    return out;
}

// Project-wide symbol search (M3.5) — documentSymbols without the single-unit filter, so every symbol
// carries its own `uri`. Name matching only: restricting the result to the PROJECT (this index also holds
// std and dependency units, which a project symbol picker must not offer) is the driver seam's job, since
// deciding whether a path is inside a directory needs real-path resolution the facade has no business
// owning. Sorted by file then position; the caller caps.
std::vector<SymbolInfo> CEmitter::workspaceSymbols(const std::string& query) const
{
    std::vector<SymbolInfo> out;
    std::string needle;
    for (char c : query) needle += (char)tolower((unsigned char)c);

    for (auto& kv : _defSites) {
        const DefSite& d = kv.second;
        if (d.kind == SymKind::Local || d.kind == SymKind::Param) continue;   // as in the outline: noise
        if (!d.unit || !d.unit->name) continue;                               // prelude / builtin
        if (!needle.empty()) {
            std::string hay;
            for (char c : d.display) hay += (char)tolower((unsigned char)c);
            if (hay.find(needle) == std::string::npos) continue;
        }
        out.push_back(SymbolInfo{ d.display, d.kind, d.range, d.selectionRange, d.container, *d.unit->name });
    }
    std::sort(out.begin(), out.end(), [](const SymbolInfo& a, const SymbolInfo& b) {
        if (a.uri != b.uri) return a.uri < b.uri;
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.column < b.range.column;
    });
    return out;
}

// Diagnostics for one file. Single-program M0: filter the accumulated semantic diagnostics by file. (Parse
// diagnostics live on CodeGenContext and are merged by the driver/server, which owns both streams.)
std::vector<Diagnostic> CEmitter::diagnosticsFor(const std::string& uri) const
{
    std::vector<Diagnostic> out;
    for (auto& d : _diagnostics) if (d.file == uri) out.push_back(d);
    return out;
}
