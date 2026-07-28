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

#include <cctype>
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

// ---- M4: the lexical layer ------------------------------------------------------------------------------
//
// A completion request arrives while the buffer is mid-edit and therefore usually does NOT parse — kama.y has
// no error productions, so there is no partial AST to interrogate, in this index or any other. Everything the
// cursor's CONTEXT depends on (what is left of the dot, which argument slot we are in, what has been typed so
// far) is recovered here, from raw text, and handed to the semantic layer as data. See kama.query.h.

const char* completionTriggerName(CompletionTrigger t)
{
    switch (t) {
        case CompletionTrigger::Bare:         return "bare";
        case CompletionTrigger::Dot:          return "dot";
        case CompletionTrigger::Scope:        return "scope";
        case CompletionTrigger::ArgLabel:     return "arg-label";
        case CompletionTrigger::ImportPath:   return "import-path";
        case CompletionTrigger::ImportSymbol: return "import-symbol";
    }
    return "bare";
}

const char* completionKindName(CompletionKind k)
{
    switch (k) {
        case CompletionKind::Field:      return "field";
        case CompletionKind::Method:     return "method";
        case CompletionKind::Ctor:       return "ctor";
        case CompletionKind::Variant:    return "variant";
        case CompletionKind::EnumMember: return "enum-member";
        case CompletionKind::Type:       return "type";
        case CompletionKind::Contract:   return "contract";
        case CompletionKind::Function:   return "function";
        case CompletionKind::Local:      return "local";
        case CompletionKind::Param:      return "param";
        case CompletionKind::Label:      return "label";
        case CompletionKind::Keyword:    return "keyword";
        case CompletionKind::Module:     return "module";
        case CompletionKind::Namespace:  return "namespace";
    }
    return "symbol";
}

namespace {

const size_t kNpos = std::string::npos;

inline bool identStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
inline bool identChar(char c)  { return isalnum((unsigned char)c) || c == '_'; }
inline bool spaceChar(char c)  { return isspace((unsigned char)c) != 0; }

// Blank every byte that is not CODE: string and char literal bodies (delimiters included) and comments become
// spaces, newlines are preserved, so every offset in `out` still names the same source position. `${…}`
// interpolation holes stay CODE — they hold ordinary expressions, and completing inside one is as useful as
// anywhere else. Walks only as far as the cursor; returns false if the cursor itself landed in a literal or a
// comment, where there is nothing to complete.
//
// Block comments do NOT nest (the lexer's IN_COMMENT state has no re-entry rule for `/*`), so a flat state
// machine is faithful, not an approximation.
bool sanitizePrefix(const std::string& text, size_t upto, std::string& out)
{
    enum class S { Code, Line, Block, RegStr, VerbStr, ChrLit };
    S st = S::Code;
    std::vector<int> holes;          // brace depth inside each open `${…}`; non-empty => we are in a hole
    out.assign(upto, ' ');
    for (size_t i = 0; i < upto; ) {
        char c = text[i];
        char d = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (c == '\n') { out[i] = '\n'; if (st == S::Line) st = S::Code; ++i; continue; }
        switch (st) {
            case S::Code:
                if (c == '/' && d == '/') { st = S::Line;  i += 2; continue; }
                if (c == '/' && d == '*') { st = S::Block; i += 2; continue; }
                if (c == '@' && d == '"') { st = S::VerbStr; i += 2; continue; }
                if (c == '"')             { st = S::RegStr;  ++i;   continue; }
                if (c == '\'')            { st = S::ChrLit;  ++i;   continue; }
                if (!holes.empty()) {
                    if (c == '{') ++holes.back();
                    else if (c == '}') {
                        if (holes.back() == 0) { holes.pop_back(); st = S::RegStr; ++i; continue; }
                        --holes.back();
                    }
                }
                out[i] = c; ++i; continue;
            case S::Line:  ++i; continue;
            case S::Block: if (c == '*' && d == '/') { st = S::Code; i += 2; continue; } ++i; continue;
            case S::RegStr:
                if (c == '\\')            { i += 2; continue; }
                if (c == '$' && d == '{') { holes.push_back(0); st = S::Code; i += 2; continue; }
                if (c == '"')             { st = S::Code; ++i; continue; }
                ++i; continue;
            case S::VerbStr:
                if (c == '"' && d == '"') { i += 2; continue; }   // `""` is an escaped quote, not the end
                if (c == '"')             { st = S::Code; ++i; continue; }
                ++i; continue;
            case S::ChrLit:
                if (c == '\\')  { i += 2; continue; }
                if (c == '\'')  { st = S::Code; ++i; continue; }
                ++i; continue;
        }
    }
    return st == S::Code;
}

size_t skipWsBack(const std::string& s, size_t i) { while (i > 0 && spaceChar(s[i - 1])) --i; return i; }

// Index OF the opener matching the closer at s[i-1]. kNpos if unbalanced.
size_t matchOpenBack(const std::string& s, size_t i)
{
    char close = s[i - 1];
    char open  = (close == ')') ? '(' : (close == ']') ? '[' : '{';
    int depth = 0;
    for (size_t j = i; j > 0; --j) {
        char c = s[j - 1];
        if (c == close) ++depth;
        else if (c == open && --depth == 0) return j - 1;
    }
    return kNpos;
}

// Scan a CANONICALIZED path backwards from `end`: whitespace squeezed out, every call/index group reduced to a
// bare `()` / `[]` suffix, `.` and `::` separators preserved (they mean different things to the resolver). So
// `  foo(a: 1) . bar ` arrives as `foo().bar`. Returns false when what precedes is not a path at all — a
// numeric literal (`1.5`), a turbofish (we deliberately do not balance `<>`), or nothing.
bool scanPathBack(const std::string& s, size_t end, std::string& path)
{
    std::vector<std::string> parts;   // segments and separators, collected right-to-left
    size_t pos = end;
    for (;;) {
        pos = skipWsBack(s, pos);
        std::string suffix;
        while (pos > 0 && (s[pos - 1] == ')' || s[pos - 1] == ']')) {
            char close = s[pos - 1];
            size_t open = matchOpenBack(s, pos);
            if (open == kNpos) return false;
            suffix = (close == ')' ? "()" : "[]") + suffix;
            pos = skipWsBack(s, open);
        }
        size_t e = pos;
        while (pos > 0 && identChar(s[pos - 1])) --pos;
        if (pos == e || !identStart(s[pos])) return false;
        parts.push_back(s.substr(pos, e - pos) + suffix);
        size_t p = skipWsBack(s, pos);
        if (p >= 2 && s[p - 1] == ':' && s[p - 2] == ':') { parts.push_back("::"); pos = p - 2; continue; }
        if (p >= 1 && s[p - 1] == '.')                    { parts.push_back(".");  pos = p - 1; continue; }
        break;
    }
    path.clear();
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) path += *it;
    return true;
}

// The innermost UNCLOSED opener before `from`, with its kind. kNpos if the cursor is at top level.
size_t innermostOpen(const std::string& s, size_t from, char& kind)
{
    int depth = 0;
    for (size_t i = from; i > 0; --i) {
        char c = s[i - 1];
        if (c == ')' || c == ']' || c == '}') { ++depth; continue; }
        if (c == '(' || c == '[' || c == '{') {
            if (depth == 0) { kind = c; return i - 1; }
            --depth;
        }
    }
    return kNpos;
}

// Walk an argument list forwards from its opener, counting TOP-LEVEL commas (the active slot) and collecting
// the `label:` spellings already supplied, so they can be filtered out of the suggestions.
void scanArgList(const std::string& s, size_t open, size_t cursor,
                 int& active, std::vector<std::string>& filled, bool& slotHasColon)
{
    int depth = 0;
    active = 0;
    slotHasColon = false;
    size_t slotStart = open + 1;
    for (size_t i = open + 1; i < cursor; ++i) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') { ++depth; continue; }
        if (c == ')' || c == ']' || c == '}') { --depth; continue; }
        if (depth != 0) continue;
        if (c == ',') { ++active; slotStart = i + 1; slotHasColon = false; continue; }
        if (c != ':') continue;
        if (s[i + 1] == ':') { ++i; continue; }                  // `::` is a qualifier, not a label
        if (slotHasColon) continue;                              // a later `:` is a ternary arm, not the label
        slotHasColon = true;
        size_t e = i;      while (e > slotStart && spaceChar(s[e - 1])) --e;
        size_t b = e;      while (b > slotStart && identChar(s[b - 1])) --b;
        if (b < e && identStart(s[b])) filled.push_back(s.substr(b, e - b));
    }
}

size_t lineStartOf(const std::string& s, size_t pos)
{
    if (pos == 0) return 0;
    size_t nl = s.rfind('\n', pos - 1);
    return nl == kNpos ? 0 : nl + 1;
}

bool lineOpensImport(const std::string& s, size_t lineStart)
{
    size_t i = lineStart;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (s.compare(i, 6, "import") != 0) return false;
    return i + 6 >= s.size() || !identChar(s[i + 6]);
}

}  // namespace

CompletionContext completionContextAt(const std::string& text, int line, int col)
{
    CompletionContext ctx;
    ctx.line = line;
    ctx.column = col;

    // (1) cursor byte offset. Line is 1-based, column 0-based (the convention kamaPos and lspRange establish);
    // both are clamped, since an editor can ask about a position the buffer no longer has.
    size_t off = 0;
    for (int cur = 1; cur < line; ++cur) {
        size_t nl = text.find('\n', off);
        if (nl == kNpos) { off = text.size(); break; }
        off = nl + 1;
    }
    size_t eol = text.find('\n', off);
    if (eol == kNpos) eol = text.size();
    size_t cursor = off + (size_t)(col > 0 ? col : 0);
    if (cursor > eol) cursor = eol;

    // (2) blank out literals and comments — nothing to complete inside one.
    std::string code;
    if (!sanitizePrefix(text, cursor, code)) return ctx;

    // (3) the identifier characters already typed. A run starting with a digit is a numeric literal, not a
    // partially-typed name, so there is nothing to complete.
    size_t pstart = cursor;
    while (pstart > 0 && identChar(code[pstart - 1])) --pstart;
    if (pstart < cursor && !identStart(code[pstart])) return ctx;
    ctx.prefix = code.substr(pstart, cursor - pstart);

    // (4) enclosing bracket. A call gives the callee and the active slot for signature help, regardless of
    // which trigger we end up reporting; a brace may be an import's symbol list.
    char openKind = '\0';
    size_t open = innermostOpen(code, pstart, openKind);
    bool slotHasColon = false;
    // A `(` with no path in front of it is a GROUPING paren, not a call — do not report an argument slot for
    // it, or `(a == b)` would look like a one-argument call to signature help.
    if (open != kNpos && openKind == '(' && scanPathBack(code, open, ctx.callee))
        scanArgList(code, open, cursor, ctx.activeParam, ctx.filled, slotHasColon);

    // (5) imports first: `import std::coll` would otherwise read as a Scope trigger on a type named `std`.
    if (open != kNpos && openKind == '{' && lineOpensImport(code, lineStartOf(code, open))) {
        ctx.trigger = CompletionTrigger::ImportSymbol;
        size_t q = skipWsBack(code, open);
        if (q >= 2 && code[q - 1] == ':' && code[q - 2] == ':') scanPathBack(code, q - 2, ctx.receiver);
        ctx.filled.clear();
        for (size_t i = open + 1, b = i; i <= cursor; ++i) {         // the symbols already listed
            if (i < cursor && identChar(code[i])) continue;
            if (b < i && identStart(code[b]) && !(i == cursor && b == pstart)) ctx.filled.push_back(code.substr(b, i - b));
            b = i + 1;
        }
        return ctx;
    }
    if (lineOpensImport(code, lineStartOf(code, cursor))) {
        ctx.trigger = CompletionTrigger::ImportPath;
        size_t q = skipWsBack(code, pstart);
        if (q >= 2 && code[q - 1] == ':' && code[q - 2] == ':') scanPathBack(code, q - 2, ctx.receiver);
        return ctx;
    }

    // (6) member access. Whitespace between the operator and the cursor is legal kama and is tolerated here.
    size_t k = skipWsBack(code, pstart);
    if (k >= 2 && code[k - 1] == ':' && code[k - 2] == ':') {
        if (scanPathBack(code, k - 2, ctx.receiver)) { ctx.trigger = CompletionTrigger::Scope; return ctx; }
    } else if (k >= 1 && code[k - 1] == '.') {
        if (scanPathBack(code, k - 1, ctx.receiver)) { ctx.trigger = CompletionTrigger::Dot; return ctx; }
    }

    // (7) an argument slot with no label yet. Every argument in kama is named, so this is the only thing that
    // can go here — ranking it above the bare-name fallback is what makes `f(` useful.
    if (open != kNpos && openKind == '(' && !slotHasColon && !ctx.callee.empty())
        ctx.trigger = CompletionTrigger::ArgLabel;

    return ctx;
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
