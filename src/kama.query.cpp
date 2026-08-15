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
#include <functional>   // std::function — the recursive type-spelling lambda (libc++ pulls it
                       // in transitively on macOS; the container toolchain does not)
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
        case CompletionKind::Constant:   return "constant";
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

// Every identifier token in the buffer, keywords filtered out — the reference index's coverage oracle. See
// the header for why this exists at all. Reuses sanitizePrefix over the WHOLE text (its return value only
// reports the state at the end, which is not a question here), so a name in a comment or a string body can
// never be mistaken for code, and the rule matches the one completion already scans by.
std::vector<SourceIdent> sourceIdentifiers(const std::string& text)
{
    std::vector<SourceIdent> out;
    std::string code;
    sanitizePrefix(text, text.size(), code);

    int line = 1, col = 0;
    for (size_t i = 0; i < code.size(); ) {
        if (code[i] == '\n') { ++line; col = 0; ++i; continue; }
        if (!identStart(code[i])) { ++col; ++i; continue; }
        size_t b = i;
        while (i < code.size() && identChar(code[i])) ++i;
        std::string name = code.substr(b, i - b);
        // Ask the lexer's own table rather than carrying a keyword list here (kamaIsKeyword, kama.l).
        if (!kamaIsKeyword(name.c_str())) out.push_back(SourceIdent{ line, col, name });
        col += (int)(i - b);
    }
    return out;
}

// What the index knows at a position, as one stable token. Deliberately distinguishes the two ways a
// position can answer nothing, because only one of them is a bug:
//   `-`           nothing is indexed here at all — the spelling never reached the index (a GAP)
//   `unresolved`  indexed, but the key names no def-site (a builtin like `int32`, a type parameter)
// A pure read of _positions/_defSites: no resolution, and never cType (a query path must not diagnose).
std::string CEmitter::coverageAt(const std::string& uri, int line, int col) const
{
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return "-";
    const PosEntry* e = posAt(unit, line, col);
    if (!e) return "-";
    auto it = _defSites.find(e->declKey);
    if (it == _defSites.end()) return "unresolved";
    return std::string(e->isDeclName ? "decl:" : "ref:") + symKindName(it->second.kind);
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

// Attribute decls to their owning USER unit. Only user units go in the map, so prelude/std decls fall
// through to nullptr (unitOfDecl) and are excluded from documentSymbols while still resolvable.
//
// Split out of buildDefSites (M6 B3) because generic-instance emission needs unitOfDecl WHILE IT RUNS, to
// attribute a template's body to the template's own unit — and that happens in the header pass, long
// before buildDefSites. collectProgram calls this after pruneInactiveDecls, which is the one real ordering
// constraint: pruning rewrites the decl list in place, so a dropped decl must never get an entry here.
// Idempotent, so buildDefSites still calls it and stays self-sufficient.
void CEmitter::buildDeclUnits()
{
    _declUnit.clear();
    for (auto& u : _units) {
        if (!u || !u->codeDeclarationList) continue;
        for (auto& decl : *u->codeDeclarationList)
            if (decl) _declUnit[decl.get()] = u.get();
    }
}

void CEmitter::buildDefSites()
{
    _defSites.clear();
    buildDeclUnits();

    auto bareOf = [](const SharedIdentifier& id, const std::string& key) -> std::string {
        if (id && id->value) return *id->value;                 // the source spelling, when we have the name id
        auto p = key.rfind("__");                               // else demangle the trailing segment
        return p == std::string::npos ? key : key.substr(p + 2);
    };

    // A type's members. Methods + ctors share the type's owning unit (their nodes aren't top-level decls).
    // Used for concrete types AND for generic templates — one body, because a template's members are
    // declared exactly like anyone else's, and a generic member with no def-site would leave B3's node-keyed
    // references pointing at nothing.
    auto addMembers = [&](ClassInfo& ci, const std::string& key, const CompilationUnit* unit,
                          const std::string& bare) {
        for (auto& mkv : ci.methods) {
            MethodInfo& mi = mkv.second;
            ASTNode* mnode = mi.node ? (ASTNode*)mi.node : (ASTNode*)mi.opDecl;
            if (!mnode) continue;   // intrinsic / synthesized (serde, bag ctor) — no source site
            SharedIdentifier mname = mi.node ? mi.node->name : SharedIdentifier();
            addDefSite(mi.cName, mi.isCtor ? SymKind::Ctor : SymKind::Method, unit, mnode, mname,
                       bare + "." + mkv.first, bare);
        }
        // Fields (M3.4). Only the DECLARING type gets an entry: a generic INSTANCE shares the template's
        // field nodes, and instances are skipped by the caller, so a field can never be keyed twice.
        for (auto& fi : ci.fields) {
            if (!fi.nameId) continue;   // synthesized (variant payload of an instantiated template, etc.)
            addDefSite(fieldKey(key, fi.name), SymKind::Field, unit, fi.nameId.get(), fi.nameId, fi.name, bare);
        }
        for (auto& ckv : ci.ctors) {
            CtorInfo& ctor = ckv.second;
            if (!ctor.node) continue;
            // key the ctor under a synthetic "<type>::ctor <name>" — it is not a resolveFunc target, but it
            // gives the outline a construction entry with a real span.
            addDefSite(key + "::ctor:" + ckv.first, SymKind::Ctor, unit, ctor.node,
                       SharedIdentifier(), bare + "." + ckv.first, bare);
        }
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
        addMembers(ci, kv.first, unit, bare);
    }

    // Generic type templates (Box<T>) — parked out of _classes; the ClassInfo.node is the template decl.
    // Their MEMBERS are registered here and nowhere else (M6 B3): the template is deliberately kept out of
    // _classes and every instance is skipped there, so this is the only place `Box<T>`'s `v` and `get` can
    // get a def-site. Every instantiation shares these very nodes, which is what makes one declaration one
    // symbol however many instantiations exist.
    for (auto& kv : _genericTypes) {
        ClassInfo& ci = kv.second;
        if (!ci.node) continue;
        const CompilationUnit* unit = unitOfDecl(ci.node);
        std::string bare = bareOf(ci.node->name, kv.first);
        addDefSite(kv.first, SymKind::GenericType, unit, ci.node, ci.node->name, bare, "");
        addMembers(ci, kv.first, unit, bare);
    }

    // Contracts (non-generic + generic templates). node is the `type contract` ClassDeclarationNode.
    for (auto* table : { &_interfaces, &_genericContracts }) {
        for (auto& kv : *table) {
            InterfaceInfo& ii = kv.second;
            if (!ii.node) continue;
            const CompilationUnit* unit = unitOfDecl(ii.node);
            std::string bare = bareOf(ii.node->name, kv.first);
            addDefSite(kv.first, SymKind::Contract, unit, ii.node, ii.node->name, bare, "");
            // The contract's METHOD declarations (M6 B3c). They have no `cName` — a contract declares a
            // vtbl slot, not a function — so they get an index-only key in the same style as `field:` /
            // `enum:`. Without these there is no def-site for a fat-pointer call to point at, and renaming
            // an implementing method half-applied: it rewrote the implementation and left the contract
            // saying the old name.
            for (const auto& im : ii.methods) {
                if (!im.nameId) continue;                      // the operator arm carries no name node
                addDefSite(contractMethodKey(kv.first, im.name), SymKind::Method, unit,
                           im.nameId.get(), im.nameId, im.name, bare);
            }
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
            const std::string mkey = enumMemberKey(kv.first, *m->identifier->value);
            addDefSite(mkey, SymKind::EnumMember, unit, m.get(), m->identifier, *m->identifier->value, bare);
            // A tagged variant's PAYLOAD fields (`Circle(int32 r)`) — M6 B3e. They are real named
            // declarations that construction sites spell as labels, but they live only on the variant
            // BACKING ClassInfo, which the _classes loop skips as compiler-synthesized, so this is their
            // only def-site. Keyed under the member (an index-only key, like every other prefixed one) and
            // noded on the payload parameter's own identifier — which a generic union instance shares, so
            // `Optional<int32>` and `Optional<string>` stay one symbol.
            if (!m->payload) continue;
            for (auto& p : *m->payload) {
                if (!p || !p->identifier || !p->identifier->value) continue;
                addDefSite(fieldKey(mkey, *p->identifier->value), SymKind::Field, unit,
                           p->identifier.get(), p->identifier, *p->identifier->value,
                           bare + "." + *m->identifier->value);
            }
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

    buildRenameGroups();
}

// M6 B3c — the contract-method rename GROUP.
//
// A contract's `fn int32 speak();` and every implementation of it must be renamed together, or the rename
// half-applies: B3a made the direct calls follow the implementation, and this makes the contract
// declaration, the OTHER implementations and the fat-pointer call sites follow it too. This is what every
// mature server does — clangd rewrites the whole override set, rust-analyzer the trait item plus every
// impl, TypeScript the interface member plus all implementations.
//
// The members keep SEPARATE def-sites on purpose (shape (b), not "collapse onto the contract's key"), so
// go-to-definition from `c.speak()` still lands on Cat's implementation rather than on the contract. Only
// referencesAt / renameRangeAt / declarationsAt consult the group.
//
// Union is TRANSITIVE, and that is correct: if one type implements two contracts that both declare
// `speak`, the name genuinely has to move in all three places at once.
//
// Derived from the tables rather than from the conformance loops in collectProgram, so the emission path
// is untouched and both a type's own `implements` clause and a `type intrinsic` block's conformance are
// covered at once (an injected conformance pushes onto `ci.interfaces` too).
void CEmitter::buildRenameGroups()
{
    _renameGroup.clear();

    std::map<std::string, std::string> parent;          // DSU over def-site keys
    std::function<std::string(std::string)> find = [&](std::string k) {
        while (parent.count(k) && parent[k] != k) k = parent[k];
        return k;
    };
    auto unite = [&](const std::string& a, const std::string& b) {
        if (a.empty() || b.empty() || !_defSites.count(a) || !_defSites.count(b)) return;
        if (!parent.count(a)) parent[a] = a;
        if (!parent.count(b)) parent[b] = b;
        std::string ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };

    for (auto* table : { &_classes, &_genericTypes }) {
        for (auto& kv : *table) {
            ClassInfo& ci = kv.second;
            if (ci.isGenericInst || ci.isIntrinsicColl || ci.isExternStruct) continue;
            for (const auto& ifn : ci.interfaces) {
                auto it = _interfaces.find(ifn);
                if (it == _interfaces.end()) continue;
                for (const auto& im : it->second.methods) {
                    if (!im.nameId) continue;                 // the operator arm has no name node
                    ClassInfo* owner = nullptr;
                    // findMethod, not ci.methods: the implementation may be INHERITED from a base, which
                    // is the same rule emitClassInterfaceVtables uses to fill the vtbl slot.
                    MethodInfo* mi = findMethod(&ci, im.name, &owner);
                    if (!mi) continue;
                    unite(contractMethodKey(ifn, im.name), mi->cName);
                }
            }
        }
    }

    // Flatten once, here, so every query stays a pure lookup — the same discipline as buildPositions'
    // resolve-fill. Singletons are dropped: a symbol in no group must cost the query paths nothing.
    std::map<std::string, std::vector<std::string>> byRoot;
    for (const auto& kv : parent) byRoot[find(kv.first)].push_back(kv.first);
    for (auto& g : byRoot) {
        if (g.second.size() < 2) continue;
        std::sort(g.second.begin(), g.second.end());
        for (const auto& k : g.second) _renameGroup[k] = g.second;
    }
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
    if (site->synthesized) return;   // an emitter-built node: no source text to point at, and often a
                                     // temporary whose address would dangle (see ASTNode::synthesized)
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

// A use-site recorded by the DECLARATION NODE it names (M6 A2 for labels, M6 B3 for members). See
// RecordedNodeRef in kama.cemit.h for why the key cannot be built here. Same pure-append discipline as
// recordRef; there is no `key.empty()` guard because there is no key yet.
void CEmitter::recordNodeRef(const IdentifierNode* site, const ASTNode* declNode)
{
    if (!_analysis || !_refUnit || !site || !declNode) return;
    if (site->synthesized || declNode->synthesized) return;   // see recordRef
    _nodeRefs.push_back(RecordedNodeRef{ _refUnit, site, declNode });
}

// A field use-site. The FieldInfo's `nameId` is what buildDefSites keys the field's DefSite on, and a
// generic instance shares the template's, so this collapses every instantiation onto one symbol. A
// synthesized field (a variant payload of an instantiated template) has no nameId and no def-site, so it
// records nothing — exactly as the key-based form dropped it.
void CEmitter::recordFieldRef(const ClassInfo* owner, const std::string& name, const IdentifierNode* site)
{
    if (!_analysis || !_refUnit || !owner || !site) return;
    for (const auto& fi : owner->fields)
        if (fi.name == name) { recordNodeRef(site, fi.nameId.get()); return; }
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

std::string CEmitter::contractMethodKey(const std::string& contractKey, const std::string& name)
{
    if (contractKey.empty() || name.empty()) return "";
    return "contract:" + contractKey + "::" + name;
}

// The index key for a MODULE/NAMESPACE path, given its dotted source spelling — or "" if the path names
// no namespace in this program (M6 B3f).
//
// `module:` keys are deliberately NOT def-site keys. In kama the namespace IS the module path IS the
// directory path (SPEC § Modules / namespaces: `import a::b::c` resolves to `a/b/c.kama` or `a/b/c/`),
// so renaming a namespace is a file-and-directory move, not a symbol rename. Every mature server draws
// the same line — clangd's `#include`, TypeScript's module specifier and gopls' import path are all
// go-to-definition targets that rename never touches. Because nothing registers a DefSite under this
// key, `_refIndex`, rename and semantic tokens never learn it, and that refusal costs no new flag.
std::string CEmitter::moduleKeyOf(const std::string& dotted) const
{
    if (dotted.empty()) return "";
    auto a = _nsCtx.aliases.find(dotted);      // `import physics as phys;` -> phys:: and physics:: are one
    if (a != _nsCtx.aliases.end()) return "module:" + a->second;
    std::string m = mangleNs(dotted);
    if (_namespaces.count(m)) return "module:" + m;
    // A PREFIX of a declared namespace: no file declares `namespace std;`, but `std` in `std::collections`
    // still names a real module directory. _namespaces is sorted, so this is a lower_bound, not a scan.
    auto it = _namespaces.lower_bound(m + "__");
    if (it != _namespaces.end() && it->compare(0, m.size() + 2, m + "__") == 0) return "module:" + m;
    return "";
}

// The index key for ONE segment of a `::`-separated name list (M6 B3f) — segment `i` of `segs`, qualified
// by everything to its left. Requires `_nsCtx` to be the segment's own unit.
//
// A segment is a TYPE (`Color` in `Color::Green`) or a MODULE (`std` in `std::collections::DynamicArray`).
// The type case resolves through the ordinary resolver and gets a real def-site, so go-to-definition and
// rename work on it — that is the gap this closes. Anything that is neither is left unindexed rather than
// given a fabricated key; `resolveUserName` hands an unresolved name back VERBATIM, so a key with no
// def-site behind it says nothing and could collide with a real one.
std::string CEmitter::listSegmentKey(const StringList& segs, size_t i, const std::string& dotted)
{
    if (i >= segs.size() || !segs[i]) return "";
    SharedStringList pre = std::make_shared<StringList>();
    for (size_t j = 0; j < i; ++j) pre->push_back(segs[j]);
    std::string k = resolveUserName(*segs[i], pre);   // no `site` => records nothing
    if (_defSites.count(k)) return k;
    return moduleKeyOf(dotted);
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
    _modules.clear();

    // (0) Which unit does a module PATH open? A file's own `namespace a::b;` declaration answers it, so
    // no import-resolution state has to be threaded in from the driver. A DIRECTORY module is several
    // units under one namespace — pick by lowest path spelling, never by map iteration order, or
    // go-to-definition on the same import would land in a different file run to run.
    for (auto& u : _units) {
        if (!u || !u->nameSpace || !u->nameSpace->name || !u->name) continue;
        std::string key = "module:" + mangleNs(qualifiedName(u->nameSpace->name));
        ModuleSite& ms = _modules[key];
        if (ms.unit && !(*u->name < *ms.unit->name)) continue;
        ms.unit  = u.get();
        ms.range = rangeOfId(u->nameSpace->name);
    }

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
        // (3b) Use-sites recorded by DECLARATION NODE — argument labels (M6 A2) and members (M6 B3). These
        // arrive without a key because a key built at the record site would embed ambient context that is
        // wrong for the referent (the caller's unit for a label, the instance's mangled name for a generic
        // member — see RecordedNodeRef). Invert it here instead: _defSites is already populated
        // (buildDefSites ran before this function), so one pass builds decl-node -> key and the answer no
        // longer depends on whether the callee's module happened to be walked before the caller's.
        //
        // Keyed by ASTNode* and matched by UPCASTING the recorded node, never by downcasting the DefSite's:
        // an upcast is unconditionally safe, and identity is all this needs.
        //
        // Restricted to the MEMBER-ish kinds on purpose. Functions are excluded because a generic free fn's
        // instances share the template's `sig.node`, so several keys would collapse onto one node and the
        // last one to be walked would win.
        std::map<const ASTNode*, const std::string*> keyOfDeclNode;
        for (const auto& kv : _defSites) {
            if (!kv.second.node) continue;
            SymKind k = kv.second.kind;
            if (k == SymKind::Param || k == SymKind::Field || k == SymKind::Method || k == SymKind::Ctor)
                keyOfDeclNode[kv.second.node] = &kv.first;
        }
        for (const auto& nr : _nodeRefs) {
            if (!nr.unit || !nr.site || !nr.declNode) continue;
            auto k = keyOfDeclNode.find(nr.declNode);
            if (k == keyOfDeclNode.end()) continue;   // prelude/builtin/intrinsic: no def-site, so no rename
            if (!seen[nr.unit].insert(nr.site).second) continue;
            _positions[nr.unit].push_back(
                PosEntry{ SrcRange{ nr.site->line, nr.site->column, nr.site->endLine, nr.site->endColumn },
                          const_cast<IdentifierNode*>(nr.site), false, *k->second });
        }
        _nodeRefs.clear();
        _nodeRefs.shrink_to_fit();
        _bodyRefs.clear();
        _bodyRefs.shrink_to_fit();
    }

    // (4) Resolve-fill — give every remaining entry its DefSite key. Only the signature type refs from
    // addTypeRef arrive empty; resolve them in their own unit's namespace context, exactly as the old
    // per-query replay did (resolveUserName reads only _nsCtx + the tables, both stable after analyze()),
    // so precomputing here is equivalent and lets the query facade stay const and replay-free.
    //
    // (4b) then indexes the `::`-separated NAME LISTS (M6 B3f). A qualifier, an import path and an export
    // manifest keep their spellings as plain strings, so their segments have no identifier node and no
    // entry from any step above — the grammar carries a parallel span per segment instead
    // (CodeGenContext::listSegPos -> the `…Pos` node fields). It belongs here and not in a recorder
    // because resolving a segment needs `_nsCtx` set to its own unit, which this loop already does.
    {
        NsCtx saved = _nsCtx;
        for (auto& kv : _positions) {
            auto uit = _unitCtx.find(kv.first);
            if (uit == _unitCtx.end()) continue;
            _nsCtx = uit->second;
            for (auto& e : kv.second)
                if (e.declKey.empty() && e.id && e.id->value)
                    e.declKey = resolveUserName(*e.id->value, e.id->qualifier);   // no `site` => not re-recorded

            // (4b) Appending while iterating would invalidate, so collect first and merge after. The
            // gather below is deliberately the cheap half: an identifier with no qualifier costs one
            // compare, and most of them have none — only a unit that actually spells `::` pays for the
            // dedup structure, which is why it is built lazily down below.
            const CompilationUnit* u = kv.first;
            std::vector<PosEntry> segs;
            // Register a module key's source spelling as we go, so hover can name it and
            // go-to-definition can find its unit without unmangling anything.
            auto noteModule = [&](const std::string& key, const std::string& dotted) {
                if (key.compare(0, 7, "module:") != 0) return;
                ModuleSite& ms = _modules[key];
                if (ms.display.empty()) {
                    ms.display = dotted;
                    for (size_t p = ms.display.find('.'); p != std::string::npos;
                         p = ms.display.find('.', p + 2))
                        ms.display.replace(p, 1, "::");
                }
            };

            // A qualifier: `Color` in `Color::Green`, `Point` in `Point::origin()`, `std::collections::X`.
            for (const auto& e : kv.second) {
                const IdentifierNode* id = e.id;
                if (!id || !id->qualifier || id->qualifier->empty()) continue;
                if (id->qualifierPos.size() != id->qualifier->size()) continue;   // synthesized: no spans
                std::string dotted;
                for (size_t i = 0; i < id->qualifier->size(); ++i) {
                    if (!(*id->qualifier)[i]) break;
                    if (i) dotted += ".";
                    dotted += *(*id->qualifier)[i];
                    const SrcRange& r = id->qualifierPos[i];
                    if (r.line <= 0) continue;
                    std::string k = listSegmentKey(*id->qualifier, i, dotted);
                    if (k.empty()) continue;
                    noteModule(k, dotted);
                    segs.push_back(PosEntry{ r, nullptr, false, k });
                }
            }
            // An import PATH, and the file's own `namespace` declaration — both name modules end to end.
            auto addModulePath = [&](const StringList& names, const std::vector<SrcRange>& pos) {
                if (pos.size() != names.size()) return;
                std::string dotted;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (!names[i]) return;
                    if (i) dotted += ".";
                    dotted += *names[i];
                    if (pos[i].line <= 0) continue;
                    std::string k = moduleKeyOf(dotted);
                    if (k.empty()) continue;
                    noteModule(k, dotted);
                    segs.push_back(PosEntry{ pos[i], nullptr, false, k });
                }
            };
            if (u->importDeclarationList)
                for (const auto& imp : *u->importDeclarationList)
                    if (imp && imp->modulePath) addModulePath(*imp->modulePath, imp->modulePathPos);
            // `namespace a::b;` — the qualifier segments plus the name itself, which is the only one of
            // these lists whose last element is a real identifier NODE rather than a bare string.
            if (u->nameSpace && u->nameSpace->name && u->nameSpace->name->value) {
                const IdentifierNode* n = u->nameSpace->name.get();
                StringList names;
                std::vector<SrcRange> pos;
                if (n->qualifier && n->qualifierPos.size() == n->qualifier->size()) {
                    names = *n->qualifier;
                    pos   = n->qualifierPos;
                }
                names.push_back(n->value);
                pos.push_back(SrcRange{ n->line, n->column, n->endLine, n->endColumn });
                addModulePath(names, pos);
            }
            // An export manifest. The key is `qualify(name)` — the same key the export-validation loop
            // builds, and deliberately NOT a scope search: SPEC requires a listed name to be a top-level
            // declaration in this very file.
            if (u->exportList && u->exportListPos.size() == u->exportList->size())
                for (size_t i = 0; i < u->exportList->size(); ++i) {
                    if (!(*u->exportList)[i] || u->exportListPos[i].line <= 0) continue;
                    std::string k = qualify(*(*u->exportList)[i]);
                    if (_defSites.count(k))
                        segs.push_back(PosEntry{ u->exportListPos[i], nullptr, false, k });
                }

            // Dedup by POSITION, not by node: these entries have no node, and two IdentifierNodes can
            // carry the same segment spans (the emitter copies identifier nodes, sharing the qualifier
            // list they came from). Built only now, so a unit that spells no `::` never pays for it.
            if (!segs.empty()) {
                std::set<std::pair<int, int>> at;
                for (const auto& e : kv.second) at.insert({ e.range.line, e.range.column });
                for (auto& s : segs)
                    if (at.insert({ s.range.line, s.range.column }).second)
                        kv.second.push_back(std::move(s));
            }
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
        if (!best || e.range.span() < best->range.span()) { best = &e; continue; }
        // Equal spans: prefer the DECLARATION. A type that declares a `ctor` gets both entries over one
        // range (the ctor's implicit result type resolves through the class's own decl identifier — see
        // the reverse-index note in buildPositions), and without this the winner is decided by an unstable
        // sort. Both carry the same key, so only the decl/ref marker differed, but a query answer must not
        // depend on sort order.
        if (e.range.span() == best->range.span() && e.isDeclName && !best->isDeclName) best = &e;
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

    // A MODULE path segment opens the module (M6 B3f) — the `#include` / import-specifier gesture. There
    // is no def-site behind the key, on purpose; see moduleKeyOf.
    if (e->declKey.compare(0, 7, "module:") == 0) {
        auto m = _modules.find(e->declKey);
        if (m == _modules.end() || !m->second.unit || !m->second.unit->name) return Location{};
        return Location{ *m->second.unit->name, m->second.range };
    }

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

    auto m = _modules.find(e->declKey);
    if (m != _modules.end()) return "module " + m->second.display;
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

    // A contract method and its implementations are ONE renameable name (M6 B3c) — see buildRenameGroups.
    // A symbol in no group answers for itself, which is every symbol but these.
    auto g = _renameGroup.find(key);
    const std::vector<std::string> self{ key };
    const std::vector<std::string>& keys = g == _renameGroup.end() ? self : g->second;

    for (const auto& k : keys) {
        auto s = _defSites.find(k);
        if (s == _defSites.end() || !s->second.unit) continue;
        if (includeDecl)
            out.push_back(Location{ s->second.unit->name ? *s->second.unit->name : uri,
                                    s->second.selectionRange });
        auto r = _refIndex.find(k);
        if (r != _refIndex.end()) out.insert(out.end(), r->second.begin(), r->second.end());
    }
    return out;
}

// Every DECLARATION the cursor's rename would have to rewrite — the symbol itself, plus the rest of its
// rename group (M6 B3c). The rename path's ownership guard needs this and not `definitionAt`: with groups,
// renaming a type's `next` that implements `std::Iterator` has an OWNED definition at the cursor while the
// contract's declaration sits in the stdlib, and rewriting one without the other silently breaks
// conformance. Every mature server refuses exactly this case.
std::vector<Location> CEmitter::declarationsAt(const std::string& uri, int line, int col) const
{
    std::vector<Location> out;
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return out;
    std::string key = declKeyAt(unit, line, col);
    if (key.empty()) return out;

    auto g = _renameGroup.find(key);
    const std::vector<std::string> self{ key };
    const std::vector<std::string>& keys = g == _renameGroup.end() ? self : g->second;
    for (const auto& k : keys) {
        auto s = _defSites.find(k);
        if (s == _defSites.end()) continue;
        // An unowned def-site with no unit (prelude/builtin) still has to be REPORTED, or the guard cannot
        // refuse on it. Name it with the empty uri the caller already treats as "not a project file".
        out.push_back(Location{ s->second.unit && s->second.unit->name ? *s->second.unit->name : std::string(),
                                s->second.selectionRange });
    }
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

// Semantic tokens for one file (M6 B2) — every indexed position in it that resolves to a known
// declaration, classified by that declaration's kind. A pure read off the already-built index: no
// re-analysis, no resolution replay, and `_positions[unit]` is ALREADY sorted by (line, column), which is
// exactly the order the protocol's delta encoding wants.
//
// Unlike find-references this does NOT filter out prelude/std targets. A def-site with `unit == nullptr`
// is one the project does not own — which is the right reason to refuse to RENAME it, and the wrong reason
// to refuse to COLOUR it. `DynamicArray` should look like a type wherever it appears.
//
// Four properties of `_positions` make the filtering below load-bearing rather than defensive:
//
//   1. It contains DUPLICATE and potentially OVERLAPPING ranges. The dedup in buildPositions is keyed on
//      `IdentifierNode*`, but decl-name entries are pushed with `id == nullptr` and so never enter that
//      set; the known collision (a type declaring a `ctor`, whose implicit result type resolves through
//      the class's own decl identifier) is filtered only in `_refIndex`, not here. The protocol FORBIDS
//      overlapping tokens, so a token that starts before the previous one ended is dropped outright —
//      which subsumes exact duplicates.
//   2. `selectionRange` can be MULTI-LINE when a decl has no name identifier and falls back to the whole
//      node's span. A token cannot span lines, so those are dropped.
//   3. `endLine`/`endColumn == 0` means UNKNOWN, not "column zero". Length then comes from the
//      identifier's own text, and the entry is dropped if even that is unavailable — guessing a length
//      would mis-colour a range the editor then can't correct.
//   4. An EMPTY `declKey` means the name resolved to nothing (a builtin like `int32`, or an unresolved
//      identifier). Emitting nothing lets the TextMate layer underneath colour it, which for a builtin is
//      already correct.
std::vector<SemanticToken> CEmitter::semanticTokensFor(const std::string& uri) const
{
    std::vector<SemanticToken> out;
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return out;
    auto it = _positions.find(unit);
    if (it == _positions.end()) return out;

    int lastLine = -1, lastEnd = -1;
    for (const auto& e : it->second) {
        if (e.declKey.empty()) continue;                                   // (4)
        auto d = _defSites.find(e.declKey);
        if (d == _defSites.end()) continue;
        const SrcRange& r = e.range;
        if (r.line <= 0 || r.column < 0) continue;
        if (r.endLine != 0 && r.endLine != r.line) continue;               // (2)

        int len = (r.endLine == r.line && r.endColumn > r.column) ? r.endColumn - r.column
                : (e.id && e.id->value)                           ? (int)e.id->value->size()
                : 0;                                                       // (3)
        if (len <= 0) continue;
        if (r.line == lastLine && r.column < lastEnd) continue;            // (1)

        out.push_back(SemanticToken{ r.line, r.column, len, d->second.kind, e.isDeclName });
        lastLine = r.line;
        lastEnd  = r.column + len;
    }
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

// ---- M4.1: the semantic layer — member completion after `.` ---------------------------------------------
//
// Three pieces: find the callable the cursor is in, collect the bindings it declares (with their declared
// TYPE nodes — the one thing no recorded state retains), and walk a canonicalized receiver path to a class
// key whose members can be listed.
//
// Everything here avoids `cType`, whose `unsupported()` side effect would land a phantom diagnostic on the
// very file the editor is showing. That rules out the three helpers the kickoff brief proposed reusing:
// `exprClass` (calls cType on six paths, and reads `_localTypes`, which is cleared at every function entry
// and after analyze() holds the LAST emitted function's locals), `isTypeReceiver` (calls both), and
// `canAccess` (calls unsupported() on denial and decides from `_currentClass`/`_currentFunc`, both dead).
// The substitutes are `mangleElem` — the exact "type node -> mangled key" function, generic instances
// included — plus `findMethod` / `findFieldOwner` for the inheritance walks, each verified clean.

std::string CEmitter::classKeyOfName(const SharedIdentifier& name)
{
    if (!name || !name->value) return "";
    std::string key = resolveUserName(*name->value, name->qualifier);   // site=nullptr => records nothing
    if (_classes.count(key) || _genericTypes.count(key) || _interfaces.count(key)
        || _genericContracts.count(key) || _enums.count(key)) return key;
    return "";
}

CEmitter::QueryCtx CEmitter::enclosingCallable(const CompilationUnit* unit, int line, int col)
{
    QueryCtx qc;
    qc.unit = unit;
    if (!unit || !unit->codeDeclarationList) return qc;
    auto holds = [&](const ASTNode* n) { return n && rangeOfNode(n).contains(line, col); };
    auto take = [&](SharedParameterList ps, SharedBlock body, const SharedIdentifier& nm) {
        qc.params = ps; qc.body = body;
        if (nm && nm->value) qc.funcName = *nm->value;
    };
    auto takeBounds = [&](const SharedStringList& ps, const SharedBoundsList& bs) {
        if (!ps || !bs) return;
        for (size_t i = 0; i < ps->size() && i < bs->size(); ++i)
            if ((*ps)[i] && (*bs)[i]) qc.typeParamBounds[*(*ps)[i]] = (*bs)[i];
    };
    for (auto& d : *unit->codeDeclarationList) {
        if (!d || !holds(d.get())) continue;
        if (auto* fn = dynamic_cast<FunctionDeclarationNode*>(d.get())) {
            take(fn->parameters, fn->block, fn->name);
            takeBounds(fn->typeParams, fn->typeBounds);
            return qc;
        }
        ClassMemberDeclarationList* members = nullptr;
        if (auto* cd = dynamic_cast<ClassDeclarationNode*>(d.get())) {
            qc.typeKey = classKeyOfName(cd->name);
            takeBounds(cd->typeParams, cd->typeBounds);
            members = cd->members.get();
        } else if (auto* ii = dynamic_cast<IntrinsicImplNode*>(d.get())) {
            // `type intrinsic <T1, T2> implements C { … <T1> { … } … }`. The enclosing type has no
            // ClassDeclarationNode here at all, and the block has N targets but only ONE source span, so
            // the key is the FIRST target by definition — there is no cursor position that could pick
            // between them. (Every scalar target keys to "".)
            if (ii->targets && !ii->targets->empty()) qc.typeKey = classKeyOfName((*ii->targets)[0]);
            members = ii->members.get();
            // A member lives EITHER in the shared body or in a specialization section, so find the list
            // that actually holds the cursor before the member loop below walks it.
            if (ii->sections)
                for (auto& sec : *ii->sections) {
                    if (!sec || !sec->members) continue;
                    for (auto& m : *sec->members)
                        if (m && holds(m.get())) { members = sec->members.get(); break; }
                }
        }
        if (!members) return qc;
        for (auto& m : *members) {
            if (!m || !holds(m.get())) continue;
            if (auto* me = dynamic_cast<ClassMethodDeclarationNode*>(m.get()))
                take(me->params, me->body, me->name);
            else if (auto* ct = dynamic_cast<ClassConstructorDeclarationNode*>(m.get())) {
                if (ct->declarator) take(ct->declarator->params, ct->body, ct->declarator->constructorName);
            } else if (auto* dt = dynamic_cast<ClassDestructorDeclarationNode*>(m.get()))
                take(SharedParameterList(), dt->body, dt->destructorName);
            else if (auto* op = dynamic_cast<ClassOperatorDeclarationNode*>(m.get()))
                take(SharedParameterList(), op->body, SharedIdentifier());
            break;
        }
        return qc;   // inside the type but not inside a member body: typeKey alone is still worth having
    }
    return qc;
}

// The callable's parameters plus every binding it declares at or above `line`. A binding declared BELOW
// the cursor is not in scope at it; sibling scopes are deliberately NOT separated (see QueryBinding).
std::vector<CEmitter::QueryBinding> CEmitter::bindingsAt(const QueryCtx& qc, int line) const
{
    std::vector<QueryBinding> binds;
    if (qc.params) for (auto& p : *qc.params)
        if (p && p->identifier && p->identifier->value)
            binds.push_back(QueryBinding{ *p->identifier->value, p->type, p->identifier->line, true, "" });
    collectBindings(qc.body, binds);
    binds.erase(std::remove_if(binds.begin(), binds.end(),
                               [&](const QueryBinding& b) { return b.line > line; }), binds.end());
    return binds;
}

void CEmitter::collectBindings(SharedStatement s, std::vector<QueryBinding>& out, bool stmtOnly) const
{
    if (!s) return;
    ASTNode* n = s.get();
    auto add = [&](const SharedIdentifier& name, const SharedIdentifier& type) {
        if (name && name->value) out.push_back(QueryBinding{ *name->value, type, name->line, false });
    };
    if (auto* b = dynamic_cast<BlockNode*>(n)) {
        if (b->statements) for (auto& st : *b->statements) collectBindings(st, out, stmtOnly);
    } else if (auto* sc = dynamic_cast<ScopeNode*>(n)) {
        collectBindings(sc->body, out, stmtOnly);
    } else if (auto* i = dynamic_cast<IfNode*>(n)) {
        if (!stmtOnly) collectBindingsExpr(i->booleanExpression, out);
        collectBindings(i->ifStatement, out, stmtOnly);
        collectBindings(i->elseStatement, out, stmtOnly);
    } else if (auto* w = dynamic_cast<WhileNode*>(n)) {
        if (!stmtOnly) collectBindingsExpr(w->booleanExpression, out);
        collectBindings(w->whileStatement, out, stmtOnly);
    } else if (auto* dw = dynamic_cast<DoWhileNode*>(n)) {
        if (!stmtOnly) collectBindingsExpr(dw->booleanExpression, out);
        collectBindings(dw->doWhileStatement, out, stmtOnly);
    } else if (auto* f = dynamic_cast<ForNode*>(n)) {
        if (f->initializerStatements) for (auto& st : *f->initializerStatements) collectBindings(st, out, stmtOnly);
        if (!stmtOnly) collectBindingsExpr(f->booleanExpression, out);
        if (f->iteratorStatements) for (auto& st : *f->iteratorStatements) collectBindings(st, out, stmtOnly);
        collectBindings(f->body, out, stmtOnly);
    } else if (auto* fe = dynamic_cast<ForEachNode*>(n)) {
        add(fe->name, fe->type);
        if (!stmtOnly) collectBindingsExpr(fe->expression, out);
        collectBindings(fe->body, out, stmtOnly);
    } else if (auto* pf = dynamic_cast<ParallelForNode*>(n)) {
        add(pf->name, pf->type);
        if (!stmtOnly) collectBindingsExpr(pf->expression, out);
        collectBindings(pf->body, out, stmtOnly);
    } else if (auto* bn = dynamic_cast<BorrowNode*>(n)) {
        // A `borrow` alias carries NO declared type — it is inferred from the host's `.view()` return —
        // so it registers with a null type node rather than a synthesized one.
        if (bn->bindings) for (auto& b : *bn->bindings) if (b) {
            add(b->alias, nullptr);
            if (!stmtOnly) collectBindingsExpr(b->host, out);
        }
        collectBindings(bn->body, out, stmtOnly);
    } else if (auto* lv = dynamic_cast<LocalVariableDeclaration*>(n)) {
        if (lv->variables) for (auto& d : *lv->variables) if (d) {
            add(d->name, lv->type);
            if (!stmtOnly) collectBindingsExpr(d->initializer, out);
        }
    } else if (auto* cl = dynamic_cast<ConstLocalVariableDeclaration*>(n)) {
        if (cl->variables) for (auto& d : *cl->variables) if (d) {
            add(d->name, cl->type);
            if (!stmtOnly) collectBindingsExpr(d->initializer, out);
        }
    } else if (auto* r = dynamic_cast<ReturnNode*>(n)) {
        if (!stmtOnly) collectBindingsExpr(r->expression, out);
    } else if (dynamic_cast<ExpressionStatementNode*>(n)) {
        // Assignment / invocation / object-creation / match / isolate in statement position: all of these
        // are BOTH an ExpressionNode and a StatementNode, so re-enter through the expression side.
        if (!stmtOnly) collectBindingsExpr(std::dynamic_pointer_cast<ExpressionNode>(s), out);
    }
}

// The expression half exists for ONE reason: a `match` in expression position (`int32 x = match (o) { … };`)
// whose arms bind payloads and may open blocks. Mirrors scanExprForCollections' composite set so a match
// nested inside an assignment, a call argument or a ternary is still reached.
void CEmitter::collectBindingsExpr(SharedExpression e, std::vector<QueryBinding>& out) const
{
    if (!e) return;
    ASTNode* n = e.get();
    if (auto* mm = dynamic_cast<MatchNode*>(n)) {
        collectBindingsExpr(mm->subject, out);
        // A payload binding's TYPE comes from the subject's variant case. The subject is a plain local in
        // the overwhelming majority of matches, and that is the only shape resolved here — anything more
        // would be re-implementing exprClass, which this file exists to avoid. An unresolved binding still
        // completes as a NAME; only `binding.` cannot resolve.
        std::string subjKey;
        if (auto* sid = dynamic_cast<IdentifierNode*>(mm->subject.get()))
            if (sid->value && (!sid->qualifier || sid->qualifier->empty()))
                for (auto it = out.rbegin(); it != out.rend(); ++it)
                    if (it->name == *sid->value) {
                        subjKey = const_cast<CEmitter*>(this)->classOfTypeNodeIn(it->ownerKey, it->type);
                        break;
                    }
        const ClassInfo* subj = nullptr;
        { auto c = _classes.find(subjKey); if (c != _classes.end()) subj = &c->second; }
        if (mm->arms) for (auto& a : *mm->arms) if (a) {
            if (a->bindingIds) {
                const std::vector<FieldInfo>* payload = nullptr;
                if (subj && a->variantName)
                    for (auto& vc : subj->variants) if (vc.name == *a->variantName) { payload = &vc.payload; break; }
                for (size_t i = 0; i < a->bindingIds->size(); ++i) {
                    const SharedIdentifier& id = (*a->bindingIds)[i];
                    if (!id || !id->value) continue;
                    SharedIdentifier ty = (payload && i < payload->size()) ? (*payload)[i].type : SharedIdentifier();
                    out.push_back(QueryBinding{ *id->value, ty, id->line, false, subjKey });
                }
            }
            collectBindingsExpr(a->body, out);
            collectBindings(a->block, out);
        }
        return;
    }
    if (auto* as = dynamic_cast<AssignmentNode*>(n)) {
        collectBindingsExpr(as->unaryExpression, out); collectBindingsExpr(as->expression, out);
    } else if (auto* b = dynamic_cast<BinaryExpressionNode*>(n)) {
        collectBindingsExpr(b->LHS, out); collectBindingsExpr(b->RHS, out);
    } else if (auto* l = dynamic_cast<LogicalAndOrNode*>(n)) {
        collectBindingsExpr(l->LHS, out); collectBindingsExpr(l->RHS, out);
    } else if (auto* t = dynamic_cast<TernaryExpressionNode*>(n)) {
        collectBindingsExpr(t->condition, out); collectBindingsExpr(t->LHS, out); collectBindingsExpr(t->RHS, out);
    } else if (auto* inv = dynamic_cast<InvocationNode*>(n)) {
        collectBindingsExpr(inv->expression, out);
        if (inv->args) for (auto& a : *inv->args) if (a) collectBindingsExpr(a->expression, out);
    } else if (auto* oc = dynamic_cast<ObjectCreationNode*>(n)) {
        if (oc->args) for (auto& a : *oc->args) if (a) collectBindingsExpr(a->expression, out);
    } else if (auto* ea = dynamic_cast<ElementAccessNode*>(n)) {
        collectBindingsExpr(ea->expression, out);
        if (ea->expressionlist) for (auto& x : *ea->expressionlist) collectBindingsExpr(x, out);
    } else if (auto* ma = dynamic_cast<MemberAccessNode*>(n)) {
        collectBindingsExpr(ma->expression, out);
    } else if (auto* c = dynamic_cast<CastNode*>(n)) {
        collectBindingsExpr(c->unaryExpression, out);
    } else if (auto* ad = dynamic_cast<AsDowncastNode*>(n)) {
        collectBindingsExpr(ad->operand, out);
    } else if (auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(n)) {
        collectBindingsExpr(su->expression, out);
    } else if (auto* pe = dynamic_cast<PreIncrDecrNode*>(n)) {
        collectBindingsExpr(pe->expression, out);
    } else if (auto* po = dynamic_cast<PostIncrDecrNode*>(n)) {
        collectBindingsExpr(po->expression, out);
    } else if (auto* al = dynamic_cast<ArrayLiteralNode*>(n)) {
        if (al->elements) for (auto& x : *al->elements) collectBindingsExpr(x, out);
        collectBindingsExpr(al->fillValue, out);
    }
}

// ---- type resolution, cType-free -------------------------------------------------------------------------

// Point `_nsCtx` (and, for a generic instance, `_typeSubst`) at the context in which `ownerKey`'s member
// declarations were written, so their type spellings resolve the way emission resolves them. Callers must
// hold a QueryScope; this only installs.
void CEmitter::installOwnerScope(const std::string& ownerKey)
{
    if (ownerKey.empty()) return;
    auto ic = _genericTypeInstCtx.find(ownerKey);
    if (ic != _genericTypeInstCtx.end()) _nsCtx = ic->second;
    else {
        auto c = _classes.find(ownerKey);
        if (c != _classes.end()) { _nsCtx.scope = c->second.scope; _nsCtx.usings = c->second.usings;
                                   _nsCtx.symbolAliases = c->second.symbolAliases; }
        else {
            auto i = _interfaces.find(ownerKey);
            if (i != _interfaces.end()) { _nsCtx.scope = i->second.scope; _nsCtx.usings = i->second.usings;
                                          _nsCtx.symbolAliases = i->second.symbolAliases; }
        }
    }
    // A generic INSTANCE stores its members with the TEMPLATE's type spellings (`T`), resolved at emit
    // under `_typeSubst`. mangleElem already consults that map, so binding it here is all it takes.
    auto gi = _genericTypeInsts.find(ownerKey);
    if (gi == _genericTypeInsts.end()) return;
    auto ps = _genericTypeParams.find(gi->second.templateKey);
    if (ps == _genericTypeParams.end()) return;
    _typeSubst.clear();
    for (size_t i = 0; i < ps->second.size() && i < gi->second.typeArgs.size(); ++i)
        if (gi->second.typeArgs[i]) _typeSubst[ps->second[i]] = gi->second.typeArgs[i];
}

std::string CEmitter::classOfTypeNodeIn(const std::string& ownerKey, SharedIdentifier t)
{
    if (!t) return "";
    QueryScope guard(this);
    installOwnerScope(ownerKey);
    std::string key = mangleElem(t);            // resolveUserName inside passes site=nullptr: records nothing
    if (_classes.count(key) || _interfaces.count(key) || _genericTypes.count(key)
        || _genericContracts.count(key) || _enums.count(key)) return key;
    return "";
}

std::string CEmitter::spellTypeIn(const std::string& ownerKey, const SharedIdentifier& t)
{
    if (!t) return "";
    QueryScope guard(this);
    installOwnerScope(ownerKey);
    // Recursive so a nested generic reads as it was written. A bare type-param bound by the instance
    // substitution spells its concrete argument, which is what the reader of `detail` wants to see.
    std::function<std::string(const SharedIdentifier&)> spell = [&](const SharedIdentifier& id) -> std::string {
        if (!id) return "";
        switch (id->builtInVal) {
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
            default: break;
        }
        if (!id->value) return "";
        if (!id->genericArg && !id->genericArgs) {
            auto s = _typeSubst.find(*id->value);
            if (s != _typeSubst.end()) return spell(s->second);
        }
        // An INTRINSIC member's type node is synthesized from an already-mangled name (a smart pointer's
        // `deref` returns `ns__Node`); a user-written node holds the source spelling and has no `__`, so
        // stripping the namespace prefix is a no-op there. Same convention as buildDefSites' bareOf.
        std::string out = *id->value;
        { size_t sep = out.rfind("__"); if (sep != std::string::npos) out = out.substr(sep + 2); }
        if (id->genericArgs && !id->genericArgs->empty()) {
            out += "<";
            for (size_t i = 0; i < id->genericArgs->size(); ++i) out += (i ? ", " : "") + spell((*id->genericArgs)[i]);
            out += ">";
        } else if (id->genericArg) {
            out += "<" + spell(id->genericArg) + ">";
        }
        return out;
    };
    return spell(t);
}

// ---- receiver path -> class key --------------------------------------------------------------------------

namespace {
// Split a canonicalized path into (separator, segment) pairs. The first pair's separator is "".
struct PathSeg { std::string sep, name, suffix; };   // suffix is "()" / "[]" / ""
std::vector<PathSeg> splitPath(const std::string& path)
{
    std::vector<PathSeg> segs;
    size_t i = 0;
    std::string sep;
    while (i < path.size()) {
        size_t b = i;
        while (i < path.size() && (isalnum((unsigned char)path[i]) || path[i] == '_')) ++i;
        PathSeg s; s.sep = sep; s.name = path.substr(b, i - b);
        while (i + 1 < path.size() && (path[i] == '(' || path[i] == '[')) { s.suffix += path.substr(i, 2); i += 2; }
        segs.push_back(s);
        if (i + 1 < path.size() && path[i] == ':' && path[i + 1] == ':') { sep = "::"; i += 2; }
        else if (i < path.size() && path[i] == '.')                     { sep = ".";  i += 1; }
        else break;
    }
    return segs;
}
}  // namespace

std::string CEmitter::classOfPath(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                                  const std::string& path, bool& isType)
{
    isType = false;
    std::vector<PathSeg> segs = splitPath(path);
    if (segs.empty() || segs[0].name.empty()) return "";

    // ---- head ----
    // The head NAME is resolved independently of its suffixes: `cells[0]` still starts from the local
    // `cells`. Only a leading `()` on a name that is not a binding means "call the free function".
    std::string cur;
    const PathSeg& h = segs[0];
    size_t sfx = 0;                                  // first suffix pair not yet applied
    if (h.name == "this") {
        cur = qc.typeKey;
    } else {
        // A live binding WINS over a same-spelled type — the precedence isTypeReceiver uses. Last match
        // wins so a later declaration in a sibling scope beats an earlier one.
        for (auto it = binds.rbegin(); it != binds.rend(); ++it)
            if (it->name == h.name) { cur = classOfTypeNodeIn(it->ownerKey, it->type);
                                     cur = boundOfTypeParam(qc, it->type, cur); break; }
        if (cur.empty() && h.suffix.compare(0, 2, "()") == 0) {
            // A free-function call. Take the RETURN TYPE NODE off the signature rather than its retCType
            // string: the node re-mangles cleanly, the C string would have to be reverse-engineered.
            auto f = _funcs.find(resolveFunc(h.name, nullptr));
            if (f != _funcs.end() && f->second.node) {
                cur = classOfTypeNodeIn("", f->second.node->returnType);
                sfx = 2;                             // the `()` was the call itself
            }
        }
        if (cur.empty()) {
            std::string k = resolveUserName(h.name, nullptr);
            if (_classes.count(k) || _interfaces.count(k) || _enums.count(k)
                || _genericTypes.count(k) || _genericContracts.count(k)) {
                cur = k;
                isType = h.suffix.empty();           // `Point.` offers ctors; `Point().` would not be a type
            }
        }
    }
    if (cur.empty()) return "";
    if (!cur.empty() && sfx < h.suffix.size()) cur = stepMemberType(cur, "", h.suffix.substr(sfx));

    // ---- tail ----
    for (size_t i = 1; i < segs.size() && !cur.empty(); ++i) {
        isType = false;                       // only the HEAD can be a type; `Type.member` does not chain
        cur = stepMemberType(cur, segs[i].name, segs[i].suffix);
    }
    return cur;
}

// A receiver typed by a bare type-param (`fn f<T: Drawable>(T x) { x.| }`) has no concrete class — `T` is in
// no table. Its BOUND contract is the only thing that can answer, and `linkContracts()` has already merged
// every parent `refines` into the contract's method list. Returns `fallback` unchanged when `t` is not an
// in-scope type-param, so the ordinary path is untouched.
std::string CEmitter::boundOfTypeParam(const QueryCtx& qc, const SharedIdentifier& t, const std::string& fallback)
{
    if (!fallback.empty() || !t || !t->value || t->genericArg || t->genericArgs) return fallback;
    auto b = qc.typeParamBounds.find(*t->value);
    if (b == qc.typeParamBounds.end() || !b->second) return fallback;
    for (auto& bound : *b->second) {
        if (!bound || !bound->value) continue;
        std::string k = resolveUserName(*bound->value, bound->qualifier);
        if (_interfaces.count(k) || _genericContracts.count(k)) return k;
    }
    return fallback;
}

// One step along a receiver path: the member `name` of `cls`, followed by whatever `()` / `[]` suffixes that
// segment carried. Whether the segment is CALLED decides field-vs-method precedence, and it genuinely
// matters: a type may carry both spellings (std::process::Command has a `args` field AND an `args` method),
// and `this.args.add(…)` names the field. Auto-derefs a smart pointer or a user `Deref<T>` when the member
// is not found directly, mirroring emitDispatch's fallback.
std::string CEmitter::stepMemberType(const std::string& cls, const std::string& name, const std::string& suffix)
{
    std::string cur = cls;
    size_t p = 0;
    if (!name.empty()) {
        bool called = suffix.compare(0, 2, "()") == 0;
        cur = memberTypeOf(cur, name, called);
        if (called) p = 2;                                    // that `()` WAS the call
    }
    // Remaining suffixes. A `()` here applied to no name (`f()()`) leaves the value as it is; only `[]`
    // steps to an element type.
    for (; !cur.empty() && p + 1 < suffix.size(); p += 2)
        if (suffix.compare(p, 2, "[]") == 0) cur = elementTypeOf(cur);
    return cur;
}

// The type of `cls`'s member `name`. `preferMethod` reflects whether the source called it.
std::string CEmitter::memberTypeOf(const std::string& cls, const std::string& name, bool preferMethod)
{
    auto ci = _classes.find(cls);
    if (ci == _classes.end()) return "";
    ClassInfo* c = &ci->second;
    auto asMethod = [&]() -> std::string {
        ClassInfo* owner = nullptr;
        MethodInfo* mi = findMethod(c, name, &owner);
        return (mi && owner) ? classOfTypeNodeIn(owner->name, mi->returnType) : "";
    };
    auto asField = [&]() -> std::string {
        // Walk `fields` rather than findFieldOwner: same base chain, but one list instead of two, and the
        // list is the one addMembers offers from — so what completes is what resolves.
        for (ClassInfo* fo = c; fo; fo = fo->base)
            for (auto& f : fo->fields) if (f.name == name) return classOfTypeNodeIn(fo->name, f.type);
        return "";
    };
    std::string hit = preferMethod ? asMethod() : asField();
    if (hit.empty()) hit = preferMethod ? asField() : asMethod();
    if (!hit.empty()) return hit;
    std::string pointee = derefTargetForQuery(cls);
    return pointee.empty() ? "" : memberTypeOf(pointee, name, preferMethod);
}

// The element type behind `coll[i]`.
std::string CEmitter::elementTypeOf(const std::string& cls)
{
    auto ci = _classes.find(cls);
    if (ci == _classes.end()) return "";
    ClassInfo* c = &ci->second;
    if (c->isIntrinsicColl && !c->collElemClass.empty()) return c->collElemClass;
    ClassInfo* owner = nullptr;
    MethodInfo* op = findMethod(c, "op_index", &owner);      // `a[i]` is registered under this name
    if (op && owner) return classOfTypeNodeIn(owner->name, op->returnType);
    std::string pointee = derefTargetForQuery(cls);
    return pointee.empty() ? "" : elementTypeOf(pointee);
}

// The pointee of a smart pointer or a user `Deref<T>`, "" if `cls` is neither. The cType-free counterpart of
// derefTarget, which resolves through cTypeInInstance.
std::string CEmitter::derefTargetForQuery(const std::string& cls)
{
    auto it = _classes.find(cls);
    if (it == _classes.end()) return "";
    ClassInfo& c = it->second;
    if (c.isIntrinsicColl && (c.collKind == CollKind::Owned || c.collKind == CollKind::Shared
                              || c.collKind == CollKind::Weak || c.collKind == CollKind::Bindable))
        return c.collElemClass;
    if (_derefContract.empty()) return "";
    for (auto& ifn : c.interfaces) {
        auto ii = _interfaces.find(ifn);
        if (ii == _interfaces.end() || !ii->second.isGenericInst || ii->second.templateKey != _derefContract)
            continue;
        ClassInfo* owner = nullptr;
        MethodInfo* mi = findMethod(&c, "deref", &owner);
        if (mi && owner) return classOfTypeNodeIn(owner->name, mi->returnType);
    }
    return "";
}

// ---- member listing --------------------------------------------------------------------------------------

bool CEmitter::visibleFrom(const ClassInfo* owner, Visibility vis, const std::string& member,
                           const QueryCtx& qc) const
{
    if (vis == Visibility::Public || !owner) return true;
    const ClassInfo* from = nullptr;
    { auto f = _classes.find(qc.typeKey); if (f != _classes.end()) from = &f->second; }
    if (vis == Visibility::Protected) {
        for (const ClassInfo* c = from; c; c = c->base) if (c == owner) return true;
        return false;
    }
    if (from == owner) return true;
    for (auto& g : owner->friendGrants) {                       // an owner-granted `friend`
        if (!g.members.empty() && !g.members.count(member)) continue;
        if (g.accessorIsClass) { if (from && from->name == g.accessor) return true; }
        else                   { if (!qc.funcName.empty() && qc.funcName == g.accessor) return true; }
    }
    return false;
}

void CEmitter::addMembers(const std::string& clsKey, bool wantStatic, const QueryCtx& qc,
                          std::vector<CompletionItem>& out)
{
    auto found = _classes.find(clsKey);
    if (found == _classes.end()) {
        // A CONTRACT receiver (a type-param bound, or a contract-typed value): `linkContracts()` has
        // already merged every parent `refines` into `methods`, so this IS the full slot set.
        auto ic = _interfaces.find(clsKey);
        if (ic == _interfaces.end()) return;
        for (auto& m : ic->second.methods) {
            if (m.isCtor != wantStatic) continue;
            std::string detail = "fn " + spellTypeIn(clsKey, m.returnType) + " " + m.name + "(";
            if (m.params) for (size_t i = 0; i < m.params->size(); ++i) {
                const auto& p = (*m.params)[i];
                if (!p || !p->identifier || !p->identifier->value) continue;
                detail += (i ? ", " : "") + *p->identifier->value + ": " + spellTypeIn(clsKey, p->type);
            }
            out.push_back(CompletionItem{ m.name, CompletionKind::Method, detail + ")", ic->second.name });
        }
        return;
    }

    std::set<std::string> seen;
    auto emit = [&](CompletionItem it) {
        if (seen.insert(it.label).second) out.push_back(std::move(it));
    };
    auto renderParams = [&](const std::string& ownerKey, const std::vector<ParamSig>& ps,
                            const SharedParameterList& nodes) {
        std::string s = "(";
        for (size_t i = 0; i < ps.size(); ++i) {
            s += (i ? ", " : "") + ps[i].name + ": ";
            s += (nodes && i < nodes->size() && (*nodes)[i]) ? spellTypeIn(ownerKey, (*nodes)[i]->type)
                                                            : ps[i].className;
        }
        return s + ")";
    };

    for (ClassInfo* c = &found->second; c; c = c->base) {
        if (!wantStatic) for (auto& f : c->fields) {
            if (f.name.rfind("__", 0) == 0) continue;
            if (!visibleFrom(c, f.visibility, f.name, qc)) continue;
            emit(CompletionItem{ f.name, CompletionKind::Field, spellTypeIn(c->name, f.type), c->name });
        }
        for (auto& kv : c->methods) {
            const MethodInfo& m = kv.second;
            // Operators are not spellable as members; the synthesized serde/format bodies have no source
            // form a user would call. `of`/`zero` (isSynthBag) and the intrinsic collection ops ARE
            // user-callable and are exactly what a `.` on a collection should offer.
            if (m.isOperator || m.isSynthSer || m.isSynthDe || m.isSynthFormat) continue;
            if (kv.first.rfind("__", 0) == 0) continue;
            bool isStaticish = m.isStatic || m.isCtor;
            if (isStaticish != wantStatic) continue;
            if (!visibleFrom(c, m.visibility, kv.first, qc)) continue;
            emit(CompletionItem{ kv.first, m.isCtor ? CompletionKind::Ctor : CompletionKind::Method,
                                 "fn " + spellTypeIn(c->name, m.returnType) + " " + kv.first
                                     + renderParams(c->name, m.params, m.node ? m.node->params : SharedParameterList()),
                                 c->name });
        }
        if (wantStatic) for (auto& kv : c->ctors) {
            if (!visibleFrom(c, kv.second.visibility, kv.first, qc)) continue;
            emit(CompletionItem{ kv.first, CompletionKind::Ctor,
                                 "ctor " + kv.first + renderParams(c->name, kv.second.params,
                                     kv.second.node && kv.second.node->declarator
                                         ? kv.second.node->declarator->params : SharedParameterList()),
                                 c->name });
        }
    }

    // A smart pointer or a user `Deref<T>` also offers the pointee's members, AFTER its own — strictly more
    // helpful than emitDispatch's either/or, and the wrapper-first order matches how dispatch resolves.
    std::string pointee = derefTargetForQuery(clsKey);
    if (!pointee.empty() && pointee != clsKey) {
        std::vector<CompletionItem> inner;
        addMembers(pointee, wantStatic, qc, inner);
        for (auto& it : inner) emit(std::move(it));
    }
}

// ---- M4.2: after `::` -------------------------------------------------------------------------------
//
// A `::` head is always a TYPE or a NAMESPACE — never a value (SPEC forbids it, and the emitter rejects it
// outright). So there are exactly two answers: the type's scope-level members (enum cases, statics, ctors,
// type-associated constants), or everything the namespace declares.

std::string CEmitter::resolvePathAsType(const std::string& path)
{
    std::vector<PathSeg> segs = splitPath(path);
    if (segs.empty() || segs.back().name.empty()) return "";
    auto qual = std::make_shared<StringList>();
    for (size_t i = 0; i + 1 < segs.size(); ++i) qual->push_back(std::make_shared<std::string>(segs[i].name));
    std::string key = resolveUserName(segs.back().name, qual->empty() ? SharedStringList() : qual);
    if (_classes.count(key) || _enums.count(key) || _interfaces.count(key)
        || _genericTypes.count(key) || _genericContracts.count(key)) return key;
    return "";
}

void CEmitter::addScopeMembers(const std::string& key, const QueryCtx& qc, std::vector<CompletionItem>& out)
{
    // A plain enum keeps its members in _enums; a TAGGED enum becomes a variant ClassInfo and never reaches
    // that table, so both spellings have to be consulted. A generic variant template (`Optional`) keeps its
    // cases on the template shape, which _classes does not hold either.
    auto e = _enums.find(key);
    if (e != _enums.end())
        for (auto& m : e->second.members)
            out.push_back(CompletionItem{ m.name, CompletionKind::EnumMember, "", e->second.name });
    for (const std::map<std::string, ClassInfo>* tbl : { &_classes, &_genericTypes }) {
        auto c = tbl->find(key);
        if (c == tbl->end() || !c->second.isVariant) continue;
        for (auto& v : c->second.variants) {
            std::string detail;
            for (size_t i = 0; i < v.payload.size(); ++i)
                detail += (i ? ", " : "(") + v.payload[i].name + ": " + spellTypeIn(key, v.payload[i].type);
            out.push_back(CompletionItem{ v.name, CompletionKind::Variant,
                                          detail.empty() ? "" : detail + ")", c->second.name });
        }
        break;
    }
    // Type-associated `comptime` constants, read as `Type::NAME`.
    for (auto& kv : _typeConsts) {
        if (kv.second.owner != key) continue;
        size_t sep = kv.first.rfind("::");
        if (sep == std::string::npos) continue;
        std::string nm = kv.first.substr(sep + 2);
        const ClassInfo* owner = nullptr;
        { auto c = _classes.find(key); if (c != _classes.end()) owner = &c->second; }
        if (!visibleFrom(owner, kv.second.visibility, nm, qc)) continue;
        out.push_back(CompletionItem{ nm, CompletionKind::Constant, spellTypeIn(key, kv.second.type), key });
    }
    // Static methods and named constructors. A primitive or intrinsic-collection head carries its statics
    // through a `type intrinsic` conformance, which lives in a different table.
    if (_classes.count(key)) addMembers(key, /*wantStatic*/ true, qc, out);
    else if (ClassInfo* rt = implTargetInfo(key)) addMembers(rt->name, /*wantStatic*/ true, qc, out);
}

void CEmitter::addNamespaceSymbols(const std::string& path, const QueryCtx& qc,
                                   std::vector<CompletionItem>& out)
{
    std::vector<PathSeg> segs = splitPath(path);
    if (segs.empty()) return;
    // `global::` — the floor, explicitly. This is the completion payoff the alias was deferred for
    // (see docs/SPEC.md § Modules): the always-in-scope surface is otherwise undiscoverable, because
    // there is no module to `import` and therefore nothing to type that would list it.
    if (segs.size() == 1 && segs[0].name == "global") {
        QueryScope guard(this);
        _nsCtx = NsCtx{};                       // no own scope, no usings: bareNameOf yields ONLY floor keys
        // Only the UNIT carries over: it is what tells a library C-ABI binding from the user's own FFI.
        // Bindings and the enclosing type's members are deliberately dropped — `global::` names neither.
        QueryCtx floorCtx;
        floorCtx.unit = qc.unit;
        addNamesInScope(floorCtx, {}, out);
        out.erase(std::remove_if(out.begin(), out.end(), [](const CompletionItem& c) {
            return c.kind == CompletionKind::Keyword;   // `global::while` is not a thing
        }), out.end());
        return;
    }
    std::string dotted;
    for (auto& s : segs) dotted += (dotted.empty() ? "" : ".") + s.name;
    std::string ns = mangleNs(dotted);
    if (segs.size() == 1) {                       // a 1-segment head may be a module alias
        auto a = _nsCtx.aliases.find(segs[0].name);
        if (a != _nsCtx.aliases.end()) ns = a->second;
    }
    if (!_namespaces.count(ns)) return;
    const std::string prefix = ns + "__";
    // One level deep: `std::` offers `collections`, not `collections::DynamicArray`.
    auto leaf = [&](const std::string& key, std::string& name) {
        if (key.compare(0, prefix.size(), prefix) != 0) return false;
        name = key.substr(prefix.size());
        return name.find("__") == std::string::npos && !name.empty();
    };
    std::set<std::string> seen;
    auto add = [&](const std::string& key, CompletionKind kind, const std::string& detail) {
        std::string name;
        if (!leaf(key, name) || !seen.insert(name).second) return;
        out.push_back(CompletionItem{ name, kind, detail, dotted });
    };
    for (auto& kv : _classes) {
        if (kv.second.isGenericInst || kv.second.isIntrinsicColl || kv.second.isExternStruct) continue;
        add(kv.first, kv.second.isVariant ? CompletionKind::Type : CompletionKind::Type, "");
    }
    for (auto& kv : _genericTypes)      add(kv.first, CompletionKind::Type, "");
    for (auto& kv : _enums)             add(kv.first, CompletionKind::Type, "enum");
    for (auto& kv : _interfaces)        { if (!kv.second.isGenericInst) add(kv.first, CompletionKind::Contract, ""); }
    for (auto& kv : _genericContracts)  add(kv.first, CompletionKind::Contract, "");
    for (auto& kv : _funcs)             add(kv.first, CompletionKind::Function, "");
    for (auto& n : _namespaces) {                    // nested namespaces (`std::` -> `collections`)
        std::string name;
        if (!leaf(n, name) || !seen.insert(name).second) continue;
        out.push_back(CompletionItem{ name, CompletionKind::Namespace, "", dotted });
    }
}

// ---- M4.3: bare names ---------------------------------------------------------------------------------

// The bare spelling `key` would answer to at this cursor, or "" if it is unreachable from here. This is the
// exact inverse of resolveUserNameImpl's lookup order — the file's own scope, then each `using`d namespace,
// then the bare/global floor — and it is what keeps the list honest: `_classes` and `_funcs` span the entire
// import closure, thousands of entries, and a name that cannot be spelled here must not be offered.
// A remainder still containing `__` means the key sits in a DEEPER namespace than any `using` reaches
// (`std__collections__X` under a `using std`), and is likewise unspellable.
std::string CEmitter::bareNameOf(const std::string& key) const
{
    auto under = [&](const std::string& ns) -> std::string {
        if (ns.empty()) return "";
        std::string pre = ns + "__";
        if (key.compare(0, pre.size(), pre) != 0) return "";
        std::string rest = key.substr(pre.size());
        return (rest.empty() || rest.find("__") != std::string::npos) ? "" : rest;
    };
    std::string n = under(_nsCtx.scope);
    if (!n.empty()) return n;
    for (auto& u : _nsCtx.usings) { n = under(u); if (!n.empty()) return n; }
    return key.find("__") == std::string::npos ? key : "";   // the bare floor, in scope everywhere
}

void CEmitter::addNamesInScope(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                               std::vector<CompletionItem>& out)
{
    std::set<std::string> seen;
    auto emit = [&](const std::string& label, CompletionKind k, const std::string& detail,
                    const std::string& container) {
        if (!label.empty() && seen.insert(label).second)
            out.push_back(CompletionItem{ label, k, detail, container });
    };

    // Innermost first, so a local shadows nothing but still sorts ahead of a same-named global.
    for (auto it = binds.rbegin(); it != binds.rend(); ++it)
        emit(it->name, it->isParam ? CompletionKind::Param : CompletionKind::Local,
             spellTypeIn(it->ownerKey, it->type), "");

    // Inside a method, a field or method of the enclosing type is spellable bare (that is precisely why
    // a local may not shadow one).
    if (!qc.typeKey.empty()) {
        std::vector<CompletionItem> mine;
        addMembers(qc.typeKey, /*wantStatic*/ false, qc, mine);
        for (auto& m : mine) emit(m.label, m.kind, m.detail, m.container);
        emit("this", CompletionKind::Keyword, qc.typeKey, "");
    }

    for (auto& kv : _classes) {
        if (kv.second.isGenericInst || kv.second.isIntrinsicColl || kv.second.isVariant
            || kv.second.isExternStruct) continue;                     // not names anyone can spell
        emit(bareNameOf(kv.first), CompletionKind::Type, "", "");
    }
    for (auto& kv : _genericTypes)     emit(bareNameOf(kv.first), CompletionKind::Type, "", "");
    for (auto& kv : _enums)            emit(bareNameOf(kv.first), CompletionKind::Type, "enum", "");
    for (auto& kv : _interfaces)       { if (!kv.second.isGenericInst) emit(bareNameOf(kv.first), CompletionKind::Contract, "", ""); }
    for (auto& kv : _genericContracts) emit(bareNameOf(kv.first), CompletionKind::Contract, "", "");
    for (auto& kv : _funcs) {
        std::string label = bareNameOf(kv.first);
        if (label.empty()) continue;
        // An `extern fn` is a C-ABI BINDING, not language surface — `kama_args_at`,
        // `kama_ctrl_release_strong`, `malloc`/`free`. They are spellable, which is exactly why the
        // namespace walk finds them, and offering them would bury `print` and `args` under plumbing.
        // Offer one only when it is declared in the file being edited, so a user's own FFI still
        // completes. "Not in the prelude" is NOT the test: `free` is declared in the prelude AND in
        // std::collections::allocator, and the std unit is a real analyzed unit.
        if (isExtern(kv.second.node)) {
            auto d = _defSites.find(kv.first);
            if (d == _defSites.end() || d->second.unit != qc.unit) continue;
        }
        // `main` is the one name the resolver rewrites unconditionally (resolveFuncImpl), so the table
        // key is `kama_main` and the walk would otherwise offer that unspellable spelling.
        if (kv.first == "kama_main") label = "main";
        std::string detail = "fn " + (kv.second.node ? spellTypeIn("", kv.second.node->returnType) : std::string())
                           + " " + label + "(";
        for (size_t i = 0; i < kv.second.params.size(); ++i) {
            detail += (i ? ", " : "") + kv.second.params[i].name + ": ";
            detail += (kv.second.node && kv.second.node->parameters && i < kv.second.node->parameters->size()
                       && (*kv.second.node->parameters)[i])
                          ? spellTypeIn("", (*kv.second.node->parameters)[i]->type)
                          : kv.second.params[i].className;
        }
        emit(label, CompletionKind::Function, detail + ")", "");
    }
    // Per-symbol imports (`import a::b::{X as Y}`) bind a LOCAL spelling that no key-prefix walk can find.
    for (auto& a : _nsCtx.symbolAliases)
        if (_classes.count(a.second) || _funcs.count(a.second) || _enums.count(a.second)
            || _interfaces.count(a.second) || _genericTypes.count(a.second))
            emit(a.first, _funcs.count(a.second) ? CompletionKind::Function : CompletionKind::Type, "", "");

    for (size_t i = 0; i < kamaKeywordCount(); ++i) emit(kamaKeywordAt(i), CompletionKind::Keyword, "", "");
}

// ---- M4.4: signature help + argument labels -------------------------------------------------------------
//
// kama has NO positional arguments — kama.y's `argument` productions are all `IDENTIFIER COLON …` — so
// "which parameter am I on" and "what labels may I type here" are one question with one answer, and the
// callee resolution below serves both.

CEmitter::CalleeSig CEmitter::resolveCallee(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                                            const std::string& callee)
{
    CalleeSig sig;
    if (callee.empty()) return sig;
    std::vector<PathSeg> segs = splitPath(callee);
    if (segs.empty() || segs.back().name.empty()) return sig;
    const std::string name = segs.back().name;

    auto fromParamSigs = [&](const std::vector<ParamSig>& ps, const SharedParameterList& nodes,
                             const std::string& ownerKey) {
        for (size_t i = 0; i < ps.size(); ++i) {
            std::string ty = (nodes && i < nodes->size() && (*nodes)[i])
                                 ? spellTypeIn(ownerKey, (*nodes)[i]->type) : ps[i].className;
            sig.params.push_back(SignatureParam{ ps[i].name, ty });
        }
        sig.found = true;
    };
    auto fromNodes = [&](const SharedParameterList& nodes, const std::string& ownerKey) {
        if (nodes) for (auto& p : *nodes)
            if (p && p->identifier && p->identifier->value)
                sig.params.push_back(SignatureParam{ *p->identifier->value, spellTypeIn(ownerKey, p->type) });
        sig.found = true;
    };

    sig.display = callee;
    if (segs.size() == 1) {
        auto f = _funcs.find(resolveFunc(name, nullptr));
        if (f != _funcs.end()) {
            sig.ret = f->second.node ? spellTypeIn("", f->second.node->returnType) : "";
            fromParamSigs(f->second.params, f->second.node ? f->second.node->parameters : SharedParameterList(), "");
            return sig;
        }
        // A local bound to a function-POINTER typedef is callable by the same spelling.
        for (auto it = binds.rbegin(); it != binds.rend(); ++it) {
            if (it->name != name || !it->type || !it->type->value) continue;
            auto si = _sigs.find(resolveUserName(*it->type->value, it->type->qualifier));
            if (si != _sigs.end()) { fromParamSigs(si->second.params, SharedParameterList(), ""); return sig; }
            break;
        }
        return sig;
    }

    // A qualified head (`a::B.make`) needs the qualifier-aware resolver; a value receiver needs the path
    // walker. Try the one that can see qualifiers first, since classOfPath's head drops them.
    std::string recvPath = callee.substr(0, callee.size() - name.size());
    while (!recvPath.empty() && (recvPath.back() == '.' || recvPath.back() == ':')) recvPath.pop_back();
    bool isType = false;
    std::string recv;
    if (recvPath.find("::") != std::string::npos) { recv = resolvePathAsType(recvPath); isType = !recv.empty(); }
    if (recv.empty()) recv = classOfPath(qc, binds, recvPath, isType);
    if (recv.empty()) return sig;

    // Walk the receiver, auto-dereferencing a smart pointer / user `Deref<T>` until the member is found.
    // Bounded by a visited set: `derefTargetForQuery` can hand back the same key for a self-referential
    // wrapper, and re-entering resolveCallee with an unchanged path recurses forever.
    std::set<std::string> seen;
    for (std::string cls = recv; !cls.empty() && seen.insert(cls).second; cls = derefTargetForQuery(cls)) {
        // A variant CASE is callable with its payload as labelled arguments (`Optional::Some(value: …)`).
        if (isType) for (const std::map<std::string, ClassInfo>* tbl : { &_classes, &_genericTypes }) {
            auto c = tbl->find(cls);
            if (c == tbl->end() || !c->second.isVariant) continue;
            for (auto& v : c->second.variants) if (v.name == name) {
                for (auto& f : v.payload) sig.params.push_back(SignatureParam{ f.name, spellTypeIn(cls, f.type) });
                sig.found = true;
                return sig;
            }
            break;
        }
        auto ci = _classes.find(cls);
        if (ci != _classes.end()) {
            ClassInfo* owner = nullptr;
            if (MethodInfo* mi = findMethod(&ci->second, name, &owner)) {
                sig.ret = spellTypeIn(owner->name, mi->returnType);
                fromParamSigs(mi->params, mi->node ? mi->node->params : SharedParameterList(), owner->name);
                return sig;
            }
            auto ct = ci->second.ctors.find(name);
            if (ct != ci->second.ctors.end()) {
                sig.ret = cls;
                fromParamSigs(ct->second.params,
                              ct->second.node ? ct->second.node->declarator->params : SharedParameterList(), cls);
                return sig;
            }
            continue;
        }
        // A CONTRACT receiver keeps its parameters as AST nodes, not ParamSigs.
        auto ii = _interfaces.find(cls);
        if (ii != _interfaces.end()) for (auto& m : ii->second.methods) if (m.name == name) {
            sig.ret = spellTypeIn(cls, m.returnType);
            fromNodes(m.params, cls);
            return sig;
        }
    }
    return sig;
}

SignatureHelp CEmitter::signatureAt(const std::string& uri, const CompletionContext& ctx)
{
    SignatureHelp help;
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit || ctx.callee.empty()) return help;
    QueryScope guard(this);
    auto uc = _unitCtx.find(unit);
    if (uc != _unitCtx.end()) _nsCtx = uc->second;
    _typeSubst.clear();

    QueryCtx qc = enclosingCallable(unit, ctx.line, ctx.column);
    std::vector<QueryBinding> binds = bindingsAt(qc, ctx.line);
    CalleeSig sig = resolveCallee(qc, binds, ctx.callee);
    if (!sig.found) return help;

    help.params = sig.params;
    // The ACTIVE parameter is the one the cursor's label names, not the comma count: kama arguments are
    // named, so they may be written in any order and `f(b: 1, |` is on `a`, not on "the second parameter".
    help.activeParam = -1;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        bool used = false;
        for (auto& f : ctx.filled) if (f == sig.params[i].label) { used = true; break; }
        if (!used) { help.activeParam = (int)i; break; }
    }
    if (ctx.activeParam >= 0 && (size_t)ctx.activeParam < sig.params.size() && ctx.filled.empty())
        help.activeParam = ctx.activeParam;
    help.label = sig.display + "(";
    for (size_t i = 0; i < sig.params.size(); ++i)
        help.label += (i ? ", " : "") + sig.params[i].label + ": " + sig.params[i].detail;
    help.label += ")";
    if (!sig.ret.empty()) help.label += " -> " + sig.ret;
    return help;
}

std::vector<CompletionItem> CEmitter::completionsAt(const std::string& uri, const CompletionContext& ctx)
{
    std::vector<CompletionItem> out;
    const CompilationUnit* unit = unitForUri(uri);
    if (!unit) return out;
    QueryScope guard(this);                    // every path below resolves names; restore on ANY exit
    auto uc = _unitCtx.find(unit);
    if (uc != _unitCtx.end()) _nsCtx = uc->second;
    _typeSubst.clear();

    if (ctx.trigger == CompletionTrigger::Scope) {
        QueryCtx sqc = enclosingCallable(unit, ctx.line, ctx.column);
        std::string key = resolvePathAsType(ctx.receiver);
        if (!key.empty()) addScopeMembers(key, sqc, out);
        else              addNamespaceSymbols(ctx.receiver, sqc, out);
        std::sort(out.begin(), out.end(), [](const CompletionItem& a, const CompletionItem& b) {
            if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
            return a.label < b.label;
        });
        return out;
    }
    if (ctx.trigger != CompletionTrigger::Dot && ctx.trigger != CompletionTrigger::Bare
        && ctx.trigger != CompletionTrigger::ArgLabel) return out;

    QueryCtx qc = enclosingCallable(unit, ctx.line, ctx.column);
    std::vector<QueryBinding> binds = bindingsAt(qc, ctx.line);

    if (ctx.trigger == CompletionTrigger::ArgLabel) {
        // Every kama argument is named, so an empty slot admits exactly the callee's unsupplied labels.
        CalleeSig sig = resolveCallee(qc, binds, ctx.callee);
        for (auto& p : sig.params) {
            bool used = false;
            for (auto& f : ctx.filled) if (f == p.label) { used = true; break; }
            if (!used) out.push_back(CompletionItem{ p.label + ":", CompletionKind::Label, p.detail, sig.display });
        }
    } else if (ctx.trigger == CompletionTrigger::Bare) {
        addNamesInScope(qc, binds, out);
    } else {
        bool isType = false;
        std::string cls = classOfPath(qc, binds, ctx.receiver, isType);
        if (!cls.empty()) addMembers(cls, isType, qc, out);
    }
    // Rank by RELEVANCE at this trigger, not by enum order: at a bare position the nearest names win
    // (a local beats a keyword); after a `.` the member kinds are already in the right order.
    bool bare = (ctx.trigger == CompletionTrigger::Bare);
    auto rank = [&](CompletionKind k) {
        if (!bare) return (int)k;
        switch (k) {
            case CompletionKind::Local: case CompletionKind::Param:    return 0;
            case CompletionKind::Field: case CompletionKind::Method:   return 1;
            case CompletionKind::Function:                             return 2;
            case CompletionKind::Type: case CompletionKind::Contract:  return 3;
            case CompletionKind::Keyword:                              return 5;
            default:                                                   return 4;
        }
    };
    std::sort(out.begin(), out.end(), [&](const CompletionItem& a, const CompletionItem& b) {
        int ra = rank(a.kind), rb = rank(b.kind);
        if (ra != rb) return ra < rb;
        if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
        return a.label < b.label;
    });
    return out;
}
