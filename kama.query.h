#ifndef __KAMA_QUERY_H__
#define __KAMA_QUERY_H__

#include <string>
#include "kama.forward.h"

// LSP / front-end-as-library query surface (M0 T4/T5). Framework-free, LSP-shaped value types that the
// driver (and later the `kama lsp` server) map to protocol JSON — no editor/framework types leak in here,
// so the same query index drives VS Code, Neovim, Emacs (eglot), and any other LSP client unchanged.
// Positions are 1-based line / column, matching ASTNode's span fields and Diagnostic.

// A source span [ (line,column) .. (endLine,endColumn) ). end==0 means "unknown" -> treated as a point
// at (line,column). Spans are APPROXIMATE at decl granularity (Bison lookahead skew); the NAME-identifier
// span (selectionRange below) is the tighter, click-target range.
struct SrcRange {
    int line = 0, column = 0, endLine = 0, endColumn = 0;
    bool contains(int l, int c) const;   // is (l,c) within this span? (point spans match their own line:col)
    long span() const;                   // width heuristic for "smallest span wins" (points sort largest)
};

enum class SymKind { Class, Value, Resource, Contract, Enum, EnumMember,
                     Function, Method, Ctor, Field, GenericType, GenericFn };
const char* symKindName(SymKind k);      // stable lowercase tag ("class", "method", …) for text/JSON output

// A declaration's location + identity. Keyed in `_defSites` by its RESOLVED mangled name (exactly what
// resolveUserName / resolveFunc return), so a cursor on a reference resolves to it in one map lookup.
struct DefSite {
    std::string             key;                 // resolved mangled name (the map key)
    SymKind                 kind = SymKind::Class;
    SrcRange                range;               // full decl node span (approximate)
    SrcRange                selectionRange;      // the NAME identifier span — the go-to-definition target
    const CompilationUnit*  unit = nullptr;      // owning USER unit; nullptr => prelude/std (excluded from outlines)
    ASTNode*                node = nullptr;      // decl node (null for a node-less table entry)
    std::string             display;             // human name ("Point", "Point.area")
    std::string             container;           // enclosing type for methods/fields ("Point"), else ""
};

// One indexed source position: a declaration NAME or a signature TYPE reference. M0 indexes ONLY decl /
// signature identifiers (body use-sites are the M3 find-references walk). `id` carries value + qualifier
// for resolution replay at a cursor.
struct PosEntry {
    SrcRange        range;
    IdentifierNode* id = nullptr;
    bool            isDeclName = false;   // true => this identifier IS a def-site name (its own DefSite)
    std::string     declKey;              // when isDeclName: the DefSite key it declares
};

// Query results (LSP-shaped, framework-free).
struct Location   { std::string uri; SrcRange range; };
struct SymbolInfo { std::string name; SymKind kind; SrcRange range, selectionRange; std::string container; };

#endif // __KAMA_QUERY_H__
