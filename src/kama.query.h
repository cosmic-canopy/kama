#ifndef __KAMA_QUERY_H__
#define __KAMA_QUERY_H__

#include <string>
#include <vector>
#include "kama.forward.h"

// LSP / front-end-as-library query surface (M0 T4/T5). Framework-free, LSP-shaped value types that the
// driver (and later the `kama lsp` server) map to protocol JSON — no editor/framework types leak in here,
// so the same query index drives VS Code, Neovim, Emacs (eglot), and any other LSP client unchanged.
// Positions are 1-based LINE and 0-based COLUMN, matching ASTNode's span fields and Diagnostic.
// The two halves differ deliberately, so do not assume: LSP wants both 0-based, and `lspRange`
// (kama.lsp.cpp) converts the LINE while passing the COLUMN straight through. `kama query` speaks
// these coordinates raw, on input and output alike. The fixture that settles an argument: `Point`
// on line 6 of tests/query/shapes.kama starts at 1-based column 12, and `--symbols` prints `6:11`.

// A source span [ (line,column) .. (endLine,endColumn) ). end==0 means "unknown" -> treated as a point
// at (line,column). Spans are APPROXIMATE at decl granularity (Bison lookahead skew); the NAME-identifier
// span (selectionRange below) is the tighter, click-target range.
struct SrcRange {
    int line = 0, column = 0, endLine = 0, endColumn = 0;
    bool contains(int l, int c) const;   // is (l,c) within this span? (point spans match their own line:col)
    long span() const;                   // width heuristic for "smallest span wins" (points sort largest)
};

enum class SymKind { Class, Value, Resource, Contract, Enum, EnumMember,
                     Function, Method, Ctor, Field, GenericType, GenericFn,
                     Local, Param };   // M3.4: function-scoped bindings — indexed, but kept OUT of outlines
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

// One indexed source position: a declaration NAME, a signature TYPE reference, or (M3) a BODY use-site.
// `declKey` is filled for EVERY entry by the resolve-fill sweep at the tail of buildPositions() — decl
// names carry their own key, signature refs are resolved there, body refs were resolved by the real
// resolver as analysis ran (see recordRef). An empty declKey means the name resolved to nothing.
struct PosEntry {
    SrcRange        range;
    IdentifierNode* id = nullptr;
    bool            isDeclName = false;   // true => this identifier IS a def-site name (its own DefSite)
    std::string     declKey;              // the DefSite key this identifier declares or references
};

// One classified source position for `textDocument/semanticTokens` — a TextMate grammar colours by regex,
// this layer colours by what the RESOLVER concluded, which is the only thing that can tell a type from a
// value or a local from a field. Deliberately NOT protocol-shaped: no legend indices, no delta encoding,
// and kama-native coordinates (line 1-based, column 0-based), so kamaPos/lspRange in kama.lsp.cpp remain
// the only two places a coordinate convention is converted. A LENGTH rather than an end, because the
// protocol forbids a token from spanning lines.
struct SemanticToken {
    int     line = 0, column = 0, length = 0;
    SymKind kind = SymKind::Class;
    bool    isDecl = false;      // this position IS the declaration's own name, not a use of it
};

// Query results (LSP-shaped, framework-free).
struct Location   { std::string uri; SrcRange range; };
// `uri` is the declaring unit's path. It is redundant for documentSymbols (every symbol is in the file you
// asked about) but load-bearing for workspaceSymbols, whose results span the project.
struct SymbolInfo { std::string name; SymKind kind; SrcRange range, selectionRange; std::string container;
                    std::string uri; };

// ---- completion + signature help (M4) --------------------------------------------------------------------
//
// What the cursor sits after, decided by a purely LEXICAL scan of the buffer — never by looking the position
// up in the index. That is not a shortcut, it is the only thing that can work: kama.y has NO error
// productions, so a buffer being completed into (`p.`, `f(a: 1, `) does not parse at all, and the AST in the
// server's last-good index therefore PREDATES the receiver the user just typed. Asking the index "what node
// is at this cursor" would be asking it to find something provably absent.

enum class CompletionTrigger {
    Bare,           // a bare identifier position — names in scope, types, keywords
    Dot,            // after `recv.`   — instance members (or a type's ctors)
    Scope,          // after `recv::`  — enum members, statics, namespace symbols
    ArgLabel,       // inside a call's argument list, in a slot with no `label:` yet — kama args are ALL named
    ImportPath,     // inside `import a::b|` — a module path segment
    ImportSymbol,   // inside `import a::b::{X, |}` — a symbol exported by that module
};
const char* completionTriggerName(CompletionTrigger t);   // stable lowercase tag, mirrors symKindName

// The lexical facts recovered at the cursor. `receiver` and `callee` are CANONICALIZED paths: whitespace
// squeezed out, and any call/index group reduced to a bare `()` / `[]` suffix, so `foo(a: 1).bar` arrives as
// `foo().bar`. Separators (`.` vs `::`) are preserved — they mean different things to the resolver.
struct CompletionContext {
    CompletionTrigger        trigger = CompletionTrigger::Bare;
    std::string              receiver;    // path left of a Dot/Scope trigger, or the module path of an Import
    std::string              callee;      // path left of the innermost UNCLOSED `(` — "" if not inside a call
    std::string              prefix;      // identifier characters already typed at the cursor
    std::vector<std::string> filled;      // argument labels / import symbols already supplied (filter them out)
    int                      activeParam = -1;      // 0-based argument slot within `callee`, -1 if not in a call
    int                      line = 0, column = 0;  // kama coords: line 1-based, column 0-based
};

// Recover the lexical context at (line, col) in raw buffer text. Skips string literals, char literals and
// comments — but DESCENDS into `${…}` interpolation holes, which are ordinary code. Deliberately does not
// balance `<` `>` (ambiguous with less-than). Total: an unrecognizable position yields Bare with an empty
// prefix, and a cursor inside a literal or comment yields Bare with everything empty.
//
// Lives here rather than in the LSP server so `kama query --complete` and `textDocument/completion` share one
// scan and can never drift — the same single-source-of-truth reasoning that put kamaIsKeyword in the lexer.
CompletionContext completionContextAt(const std::string& text, int line, int col);

// Every identifier-shaped token in a buffer, in source order, with the keywords filtered out (asking the
// lexer's own table, so the two can never disagree). Shares completionContextAt's literal/comment scan.
//
// This is the reference index's COVERAGE ORACLE (M6 B3). The index knows what it indexed; nothing knew what
// it SHOULD have — so a gap could only be found by someone thinking of the spelling, which is exactly how a
// method's call sites went unindexed for the whole campaign. The source's own identifiers are the missing
// ground truth: whatever the parser could have named, it spelled here first.
struct SourceIdent { int line = 0, column = 0; std::string name; };   // line 1-based, column 0-based
std::vector<SourceIdent> sourceIdentifiers(const std::string& text);

enum class CompletionKind { Field, Method, Ctor, Variant, EnumMember, Type, Contract,
                            Function, Local, Param, Label, Keyword, Module, Namespace, Constant };
const char* completionKindName(CompletionKind k);   // stable lowercase tag, mirrors symKindName

struct CompletionItem {
    std::string    label;                              // shown AND inserted (no snippets in M4)
    CompletionKind kind = CompletionKind::Local;
    std::string    detail;                             // "int32", "fn Point midpoint(a: Point, b: Point)"
    std::string    container;                          // declaring type / namespace ("" for a local)
};

struct SignatureParam { std::string label, detail; };  // label "a", detail "Point"
struct SignatureHelp  { std::string label;             // "midpoint(a: Point, b: Point) -> Point"
                        std::vector<SignatureParam> params;
                        int activeParam = -1; };       // an empty label means nothing callable is here

#endif // __KAMA_QUERY_H__
