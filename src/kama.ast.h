#ifndef __KAMA_AST_H__
#define __KAMA_AST_H__

#include <iostream>
#include <cstdint>      // int8_t … uint64_t (not transitively available on all libcs, e.g. Windows UCRT)
#include <set>          // CompilationUnit::prunedNames
#include "kama.forward.h"
#include "kama.query.h"  // SrcRange — the per-segment spans of the `::`-separated name lists (M6 B3f)

enum SymbolType {
  UNDEFINED = 0,
  VARIABLE
};

class ASTNode {
public:
    int line;
    int column;
    // Source span end (one past the last column), captured at construction from the lexer's position (T3).
    // Like `line`/`column`, this is the lexer position at reduction time — approximate for multi-token
    // nodes (bison lookahead skew); precise per-node spans are refined where hover/rename need them.
    int endLine;
    int endColumn;
    // Built by the EMITTER, not the parser — a synthesized type node (`Chars`, `Split`, the `Optional<T>`
    // a fallible ctor returns) or a substituted/absolutized clone of one. Two things follow, and the
    // reference index (recordRef / recordNodeRef) relies on both:
    //   - it names no source text, so indexing its `line`/`column` would point a cursor at whatever
    //     happens to sit at the synth context's default position (line 1, column 1);
    //   - it is usually a TEMPORARY — `cType(std::make_shared<IdentifierNode>(…))` frees it at the end of
    //     the full expression — so storing its raw address outlives the node (a real use-after-free that
    //     crashed `kama check`/the language server on any file reaching such a site).
    // A parser-built node is owned by its CompilationUnit and outlives the emitter, so it is safe to index;
    // nothing the emitter invents is. Set via CEmitter::synthId(), and on every hand-made clone.
    bool synthesized = false;
    explicit ASTNode(CodeGenContext& context);
    ASTNode(const ASTNode&) = default;                  // Copy constructor
    ASTNode(ASTNode&&) = default;                       // Move constructor
    ASTNode& operator=(const ASTNode&) & = default;     // Copy assignment operator
    ASTNode& operator=(ASTNode&&) & = default;          // Move assignment operator
    virtual ~ASTNode() {}                               // Destructor
    virtual SymbolType symbolType() { return SymbolType::UNDEFINED; }
};

class ExpressionNode : public virtual ASTNode {
public:
    explicit ExpressionNode(CodeGenContext& context) : ASTNode(context) { }
    virtual ~ExpressionNode() { }
};

class StatementNode : public virtual ASTNode {
public:
    explicit StatementNode(CodeGenContext& context) : ASTNode(context) { }
    virtual ~StatementNode() { }
};


class ExpressionStatementNode : public ExpressionNode, public StatementNode {
public:
    explicit ExpressionStatementNode(CodeGenContext& context) : ASTNode(context), ExpressionNode(context), StatementNode(context) { }
    virtual ~ExpressionStatementNode() { }
};

//------------------------------------------------------------------------------ 
//                              Compilation Unit 
//------------------------------------------------------------------------------

class CompilationUnit : public StatementNode {
public:
    SharedString name;
    SharedNamespaceDeclaration nameSpace;
    SharedImportDeclarationList importDeclarationList;
    SharedStringList exportList;              // the module's public surface (`export { … };`)
    std::vector<SrcRange> exportListPos;      // one span per exportList entry (M6 B3f), or empty
    SharedStatementList codeDeclarationList;
    // Names `@compileFor` dropped from codeDeclarationList — recorded HERE, on the unit, and not only on
    // the emitter that did the pruning. pruneInactiveDecls rewrites the decl list IN PLACE, so a second
    // emitter over the same unit (a cached unit reused across analyses: `kama lsp` per keystroke, `kama
    // check --each` per program) finds the decls already gone and would rebuild an EMPTY pruned set —
    // and then report a phantom "export list names `sort` but there is no such top-level declaration"
    // for a gated-but-exported decl. Pruning is idempotent under a fixed build-flag set; this makes its
    // by-product idempotent too.
    std::set<std::string> prunedNames;
    // ---- closure-pruning facts, harvested at PARSE time (kama.y `compilation_unit`) ----------------
    // A directory-module import loads only the files needed to satisfy its `{…}` symbol list, plus their
    // transitive intra-directory closure (closureOfModule, kama.driver.cpp). These three fields are what
    // that closure reads.
    //
    // Harvested at parse time, and NOT recomputed later, because both of the obvious later moments are
    // wrong: pruneInactiveDecls (kama.cemit.cpp) rewrites codeDeclarationList IN PLACE, and the parse
    // cache (kama.driver.cpp) hands the SAME unit back to a second analysis — `kama lsp` per keystroke,
    // `kama check --each` per program. Anything derived from the decl list after an emitter has run has
    // already lost every `@compileFor`-gated name, which would silently shrink the index and make a
    // program's unit set depend on its position in a batch. Written once, before any emitter exists.
    std::set<std::string> topLevelNames;   // every top-level DECLARED name — not just the `export` list,
                                           // because same-namespace siblings reach each other's
                                           // unexported names through the shared namespace scope.
    std::set<std::string> identTokens;     // every identifier-token spelling in this file, straight from
                                           // the lexer: a sound SUPERSET of the names it references.
                                           // Over-pulling costs pruning; under-pulling would emit calls
                                           // to undefined functions, so the superset is the safe side.
    bool unprunable = false;               // holds a declaration with a program-wide effect and NO name to
                                           // reference it by, so no closure can reach it. See
                                           // harvestUnitFacts for the two kinds and why each is one.
    CompilationUnit(CodeGenContext& context, SharedString name,
                    SharedNamespaceDeclaration nameSpace,
                    SharedImportDeclarationList importDeclarationList,
                    SharedStringList exportList,
                    SharedStatementList codeDeclarationList)
        : ASTNode(context)
        , StatementNode(context)
        , name(name)
        , nameSpace(nameSpace)
        , importDeclarationList(importDeclarationList)
        , exportList(exportList)
        , codeDeclarationList(codeDeclarationList)
        { }
};

class NamespaceDeclarationNode : public StatementNode {
public:
    SharedIdentifier name;
    NamespaceDeclarationNode(CodeGenContext& context, SharedIdentifier name)
        : ASTNode(context),  StatementNode(context), name(name) { }
};

// `extern "<header.h>";` — emit a C `#include` for FFI.
class IncludeNode : public StatementNode {
public:
    SharedString header;   // the raw string, e.g. <stdlib.h> or math.h
    IncludeNode(CodeGenContext& context, SharedString header)
        : ASTNode(context),  StatementNode(context), header(header) { }
};

class UsingDeclarationNode : public StatementNode {
public:
    SharedIdentifier identifier;
    SharedIdentifier alias;
    UsingDeclarationNode(CodeGenContext& context, SharedIdentifier identifier)
        : ASTNode(context),  StatementNode(context), identifier(identifier) { }
    UsingDeclarationNode(CodeGenContext& context, SharedIdentifier identifier, SharedIdentifier alias)
        : ASTNode(context),  StatementNode(context), identifier(identifier), alias(alias) { }
};

// `import a::b::c;` (bare) | `import a::b as m;` (module alias) | `import a::b::{X, Y as Z};`
// (per-symbol). `modulePath` = the `::`-segments; `symbols` = per-symbol (each carries an
// optional local alias, reusing UsingDeclarationNode's identifier+alias); `moduleAlias` is set
// only for the `as m` form. Empty `symbols` + null `moduleAlias` = the bare (qualified-only) form.
class ImportDeclarationNode : public StatementNode {
public:
    SharedStringList          modulePath;
    std::vector<SrcRange>     modulePathPos;   // one span per modulePath segment (M6 B3f), or empty
    SharedUsingDeclarationList symbols;
    SharedString              moduleAlias;
    ImportDeclarationNode(CodeGenContext& context, SharedStringList modulePath,
                          SharedUsingDeclarationList symbols, SharedString moduleAlias)
        : ASTNode(context), StatementNode(context)
        , modulePath(modulePath), symbols(symbols), moduleAlias(moduleAlias) { }
};

//------------------------------------------------------------------------------ 
//                              Primitive Types
//------------------------------------------------------------------------------

class Int8Node : public ExpressionNode {
public:
    int8_t value;
    Int8Node(CodeGenContext& context, int8_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class Int16Node : public ExpressionNode {
public:
    int16_t value;
    Int16Node(CodeGenContext& context, int16_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class Int32Node : public ExpressionNode {
public:
    int32_t value;
    Int32Node(CodeGenContext& context, int32_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class Int64Node : public ExpressionNode {
public:
    int64_t value;
    Int64Node(CodeGenContext& context, int64_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class UInt8Node : public ExpressionNode {
public:
    uint8_t value;
    UInt8Node(CodeGenContext& context, uint8_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class UInt16Node : public ExpressionNode {
public:
    uint16_t value;
    UInt16Node(CodeGenContext& context, uint16_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class UInt32Node : public ExpressionNode {
public:
    uint32_t value;
    UInt32Node(CodeGenContext& context, uint32_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

// A `char` literal (`'a'`, `'\n'`, `'\u{1F600}'`) — holds the Unicode scalar value (codepoint).
// `char` is a distinct primitive backed by uint32_t; the value is the codepoint, not a UTF-8 byte.
class CharNode : public ExpressionNode {
public:
    uint32_t value;   // Unicode scalar value (codepoint)
    CharNode(CodeGenContext& context, uint32_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class UInt64Node : public ExpressionNode {
public:
    uint64_t value;
    UInt64Node(CodeGenContext& context, uint64_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class Float32Node : public ExpressionNode {
public:
    float value;
    Float32Node(CodeGenContext& context, float value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class Float64Node : public ExpressionNode {
public:
    double value;
    Float64Node(CodeGenContext& context, double value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class StringNode : public ExpressionNode {
public:
    SharedString value;
    StringNode(CodeGenContext& context, SharedString value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

// A string interpolation `"a ${x} b ${y} c"` — an ordered alternation of literal PARTS and hole
// EXPRESSIONS, with the invariant `parts.size() == holes.size() + 1` (chunk, hole, chunk, …, chunk).
// Both lists are explicit so a Campaign-2 tagged string (`sql"…${x}…"`) is an ADDITIVE `tag` on the SAME
// node, not a reshape — an untagged interpolation is the "default tag" (write each hole into a Formatter).
// Lowers to a Formatter build: writeStr(part) then hole.format(ref f) per segment, then finish().
class InterpolatedStringNode : public ExpressionNode {
public:
    std::vector<SharedString> parts;        // literal chunks — always holes.size()+1 of them
    std::vector<SharedExpression> holes;    // interpolated hole expressions (identifier/member/index)
    std::vector<SharedString> specs;        // per-hole format spec (`${x:.2}`), parallel to holes; null = no spec
    SharedString tag;                       // Campaign 2: the tag name, or null for a plain interpolation
    InterpolatedStringNode(CodeGenContext& context) : ASTNode(context), ExpressionNode(context) { }
};

class BooleanNode : public ExpressionNode {
public:
    bool value;
    BooleanNode(CodeGenContext& context, bool value) : ASTNode(context),  ExpressionNode(context), value(value) { }
};

class NullNode : public ExpressionNode {
public:
    NullNode(CodeGenContext& context) : ASTNode(context),  ExpressionNode(context) { }
};

//------------------------------------------------------------------------------ 
//                              Basic Identification
//------------------------------------------------------------------------------

#define IDENTIFIER_NONE_VAL 0
#define IDENTIFIER_INT8_VAL 1
#define IDENTIFIER_INT16_VAL 2
#define IDENTIFIER_INT32_VAL 3
#define IDENTIFIER_INT64_VAL 4
#define IDENTIFIER_UINT8_VAL 5
#define IDENTIFIER_UINT16_VAL 6
#define IDENTIFIER_UINT32_VAL 7
#define IDENTIFIER_UINT64_VAL 8
#define IDENTIFIER_BOOL_VAL 9
#define IDENTIFIER_FLOAT32_VAL 10
#define IDENTIFIER_FLOAT64_VAL 11
#define IDENTIFIER_STRING_VAL 12
#define IDENTIFIER_VOID_VAL 13
#define IDENTIFIER_CHAR_VAL 14


class IdentifierNode : public ExpressionNode {
public:
    int builtInVal;
    SharedString value;
    SharedStringList qualifier;
    std::vector<SrcRange> qualifierPos;  // one span per qualifier segment (M6 B3f), or empty
    SharedIdentifier genericArg;   // element type for Coll<T> (a full type); == genericArgs[0]
    SharedIdentifierList genericArgs;  // all type args for Pair<A,B> etc.; genericArg mirrors [0]
    SharedIdentifierList bounds;       // when this node is a type-PARAMETER (`K` in `<K: I + J>`),
                                       // its contract bounds [I, J]; empty/unset otherwise.
    bool isConstParam = false;         // const generic PARAMETER (`const N: int`) — a value, not a type
    // The declared integral type of that parameter (`int32` in `const N: int32`). Carried because the
    // param is READ as a value in the body: a bare literal would be a C `int`, so `const N: uint32` or
    // `int8` would promote and compare differently from a real local of the type the author wrote.
    SharedIdentifier constType;
    // `<T is This>` on a `type contract` — the parameter is PINNED to the implementing type. `is` is an
    // identity constraint, which is why it is not a `bounds` entry: a bound list holds contracts, and
    // admitting a non-contract there would need an exception plus a hand-rejection of `This + Contract`.
    // `pin` is the operand as written (only `This` is accepted today; the slot leaves room for more).
    SharedIdentifier pin;
    int  bareDefault = 0;              // `implements Copyable(bare: give|copy)` — the contract-parameter token (GIVE/COPY), 0=unset
    // `implements C when [P1: B1, P2: B2, …]` — the gate's per-condition type-param names + required
    // contracts (index-aligned). Empty = unconditional. Multiple = AND (the impl holds only when all do).
    SharedIdentifierList whenParams;
    SharedIdentifierList whenBounds;
    SharedExpression constArgValue;    // const generic ARGUMENT that is a literal (`4` in `Fixed<T,4>`)
    SharedIdentifier defaultArg;       // type-PARAMETER default (`H: BuildHasher = DefaultHasher`) — the default type; null if none
    SharedString argName;              // use-site type-ARGUMENT named override (`A:` in `Map<int32, A: Arena>`); null = positional
    // `Box::<int32>::tag()` — type args riding the QUALIFIER (the owning type), not this name. Distinct
    // from `genericArgs`, which on a called name means the CALLEE's own type args (`deserialize::<T>()`).
    // `qualifier` is a StringList of bare segments and has nowhere to put them, so they live here.
    SharedIdentifierList qualifierGenericArgs;
    void setQualifier(SharedStringList qualifier){ this->qualifier = qualifier; }

    IdentifierNode(CodeGenContext& context, SharedString value, int builtInVal = IDENTIFIER_NONE_VAL)
        : ASTNode(context),  ExpressionNode(context), builtInVal(builtInVal), value(value), qualifier( std::make_shared<StringList>() ), genericArg( SharedIdentifier() ) { }
    IdentifierNode(CodeGenContext& context, SharedString value, SharedStringList qualifier)
        : ASTNode(context),  ExpressionNode(context), builtInVal(IDENTIFIER_NONE_VAL), value(value), qualifier(qualifier), genericArg( SharedIdentifier() ) { }
    // Coll<T>: the generic argument is a full type (IdentifierNode), so primitives
    // (List<int32>) and class element types (List<Point>) both work.
    IdentifierNode(CodeGenContext& context, SharedString value, SharedStringList qualifier, SharedIdentifier genericArg)
        : ASTNode(context),  ExpressionNode(context), builtInVal(IDENTIFIER_NONE_VAL), value(value), qualifier(qualifier), genericArg(genericArg) { }
};

//------------------------------------------------------------------------------ 
//                              Modifiers
//------------------------------------------------------------------------------

class ModifierNode : public ExpressionNode {
public:
    SharedString value;
    SharedIdentifierList targets;
    SharedArgumentList args;   // `virtual(maxDepth: 2)` — the extension budget; null for a bare modifier
    ModifierNode(CodeGenContext& context, SharedString value)
        : ASTNode(context),  ExpressionNode(context), value(value), targets( std::make_shared<IdentifierList>() ) { }
    ModifierNode(CodeGenContext& context, SharedString value, SharedIdentifierList targets)
        : ASTNode(context),  ExpressionNode(context), value(value), targets(targets) { }
    ModifierNode(CodeGenContext& context, SharedString value, SharedArgumentList args)
        : ASTNode(context),  ExpressionNode(context), value(value), targets( std::make_shared<IdentifierList>() ), args(args) { }
};

//------------------------------------------------------------------------------ 
//                              Functions
//------------------------------------------------------------------------------

class FunctionDeclarationNode : public StatementNode {
public:
    SharedModifier modifier; // Only extern
    SharedIdentifier returnType;
    SharedIdentifier name;
    SharedParameterList parameters;
    SharedBlock block;
    SharedStringList typeParams;   // <T, ...> — generic fn; empty for non-generic
    SharedBoundsList typeBounds;   // contract bounds parallel to typeParams (empty entry = unbounded)
    SharedIdentifierList typePins; // `<T is This>` identity pin parallel to typeParams; null entry = unpinned
    SharedStringList constParams;  // names of const generic params (`const N: int`); subset of typeParams order
    SharedIdentifierList constTypes; // each const param's declared integral type, PARALLEL TO typeParams (null entry = a type param)
    bool isRef = false;            // `fn ref T …` — returns a PLACE (a T*), deref'd at the caller (mirrors the method form)
    bool isComptime = false;       // `comptime fn …` — a compile-time-only function (const-eval 6b-3); never emitted as C
    SharedAttributeList attributes; // `@interrupt`/`@section(".x")` (null when none) — MCU codegen attributes
    FunctionDeclarationNode(CodeGenContext& context,  SharedModifier modifier, SharedIdentifier returnType, SharedIdentifier name,
                            SharedParameterList parameters, SharedBlock block, SharedStringList typeParams = SharedStringList() )
        : ASTNode(context),  StatementNode(context)
        , modifier(modifier)
        , returnType(returnType)
        , name(name)
        , parameters(parameters)
        , block(block)
        , typeParams(typeParams) { }
};

class FunctionParameterNode : public ExpressionNode {
public:
    SharedModifier modifier;
    SharedIdentifier type;
    SharedIdentifier identifier;
    bool isConst = false;   // `const [ref] T x` — immutable param
    bool isHardware = false; // `hardware Ptr<T> x` — MMIO register pointer, emits `volatile T*`
    FunctionParameterNode(CodeGenContext& context, SharedModifier modifier, SharedIdentifier type, SharedIdentifier identifier)
        : ASTNode(context),  ExpressionNode(context), modifier(modifier), type(type), identifier(identifier) { }
};

//------------------------------------------------------------------------------ 
//                              Statements
//------------------------------------------------------------------------------

class BlockNode : public StatementNode {
public:
    SharedStatementList statements;
    BlockNode(CodeGenContext& context, SharedStatementList statements)
        : ASTNode(context),  StatementNode(context), statements(statements) { }
};

// `unsafe { ... }` — a scoped block inside which raw pointer index/store is
// permitted. The single, explicit, greppable unsafe surface of the language.
class UnsafeNode : public StatementNode {
public:
    SharedStatement body;   // a BlockNode
    UnsafeNode(CodeGenContext& context, SharedStatement body)
        : ASTNode(context),  StatementNode(context), body(body) { }
};

// `asm("...")` — inline assembly (MCU step 6a). Statement-only; requires an enclosing `unsafe { }`.
// Lowers to `__asm__ __volatile__(<text> : : : "memory")` — always volatile + a full compiler memory
// barrier (so `cpsid i`/`dsb`/`dmb` are correct by default). One literal operand; no interpolation.
class AsmNode : public StatementNode {
public:
    SharedString code;   // the raw asm text (a STRING_LITERAL value); C-escaped at emit
    AsmNode(CodeGenContext& context, SharedString code)
        : ASTNode(context),  StatementNode(context), code(code) { }
};

// `scope { ... }` — a structured-concurrency block (M4). It owns the isolates `spawn`ed inside it and
// JOINS them all at the closing brace, BEFORE any local destructor runs (join-before-drop). That ordering
// is the whole point: it makes a child that borrows an enclosing local sound with no lifetime inference.
// Statement-only (unlike IsolateNode); the emitter lowers it like a scoped block with a join barrier.
class ScopeNode : public StatementNode {
public:
    SharedStatement body;   // a BlockNode
    ScopeNode(CodeGenContext& context, SharedStatement body)
        : ASTNode(context),  StatementNode(context), body(body) { }
};

// `isolate worker(p: give x)` — spawn a top-level fn on a fresh OS thread with a MOVED-in argument bundle.
// Both a STATEMENT (`isolate worker(...);` — fused spawn+join) and an EXPRESSION (`Isolate h = isolate
// worker(...);` — spawn now, returning an RAII handle whose drop=join): hence ExpressionStatementNode.
// `call` is the whole InvocationNode; the emitter validates the callee is a bare top-level fn (no receiver
// → no env capture → shared-nothing) and reuses the `give` move machinery so post-spawn use is an error.
class IsolateNode : public ExpressionStatementNode {
public:
    SharedExpression call;   // an InvocationNode
    IsolateNode(CodeGenContext& context, SharedExpression call)
        : ASTNode(context),  ExpressionStatementNode(context), call(call) { }
};

class VariableDeclarator : public StatementNode {
public:
    SharedIdentifier name;
    SharedExpression initializer;
    VariableDeclarator(CodeGenContext& context, SharedIdentifier name, SharedExpression initializer)
        : ASTNode(context),  StatementNode(context), name(name), initializer(initializer) { }
};

class ConstVariableDeclarator : public StatementNode {
public:
    SharedIdentifier name;
    SharedExpression initializer;
    ConstVariableDeclarator(CodeGenContext& context, SharedIdentifier name, SharedExpression initializer)
        : ASTNode(context),  StatementNode(context), name(name), initializer(initializer) { }
};

class LocalVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedVariableDeclaratorList variables;
    // `slot T x;` — storage that holds NO value yet. Illegal to read, call on, or pass by value until it
    // is definitely assigned, and an unassigned slot has NO destructor emitted ("drop only if live",
    // proven statically instead of defended against with a runtime niche check).
    bool isSlot = false;
    LocalVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
    virtual SymbolType symbolType() { return SymbolType::VARIABLE; }
};

// A module-level `static T name = const-expr;` (MCU campaign step 1). Per-isolate by construction —
// lowered `static KAMA_ISOLATE_LOCAL T …`. Reuses VariableDeclarator; const-init + value/Ptr/InlineArray
// legality are enforced semantically in the emitter (there is no grammar-level const check).
class ModuleVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedVariableDeclaratorList variables;
    bool isHardware = false;    // `static hardware T name` — MMIO/ISR static, emits `volatile T`
    bool isComptime = false;    // `comptime T NAME = <expr>` (6b-2) — a named compile-time constant (immutable, folded)
    SharedAttributeList attributes; // `@section(".x")` (null when none) — linker-section placement
    ModuleVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
    virtual SymbolType symbolType() { return SymbolType::VARIABLE; }
};

class ConstLocalVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedConstVariableDeclaratorList variables;
    bool isComptime = false;    // `comptime T NAME = <expr>` (6b-2) — explicit compile-time local (vs plain `const`)
    ConstLocalVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedConstVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
};

class IfNode : public StatementNode {
public:
    SharedExpression booleanExpression;
    SharedStatement ifStatement;
    SharedStatement elseStatement;
    IfNode(CodeGenContext& context,  SharedExpression booleanExpression, SharedStatement ifStatement, SharedStatement elseStatement)
        : ASTNode(context),  StatementNode(context)
        , booleanExpression(booleanExpression)
        , ifStatement(ifStatement)
        , elseStatement(elseStatement) { }
};

class WhileNode : public StatementNode {
public:
    SharedExpression booleanExpression;
    SharedStatement whileStatement;
    WhileNode(CodeGenContext& context,  SharedExpression booleanExpression, SharedStatement whileStatement)
        : ASTNode(context),  StatementNode(context)
        , booleanExpression(booleanExpression)
        , whileStatement(whileStatement) {}
};

class DoWhileNode : public StatementNode {
public:
    SharedExpression booleanExpression;
    SharedStatement doWhileStatement;
    DoWhileNode(CodeGenContext& context,  SharedExpression booleanExpression, SharedStatement doWhileStatement)
        : ASTNode(context),  StatementNode(context)
        , booleanExpression(booleanExpression)
        , doWhileStatement(doWhileStatement) {}
};

class ForNode : public StatementNode {
public:
    SharedStatementList initializerStatements;
    SharedExpression booleanExpression;
    SharedStatementList iteratorStatements;
    SharedStatement body;
    ForNode(CodeGenContext& context,  SharedStatementList initializerStatements, 
            SharedExpression booleanExpression, 
            SharedStatementList iteratorStatements,
            SharedStatement body)
    : ASTNode(context),  StatementNode(context)
    , initializerStatements(initializerStatements)
    , booleanExpression(booleanExpression)
    , iteratorStatements(iteratorStatements)
    , body(body) {}
};

class ForEachNode : public StatementNode {
public:
    SharedIdentifier type;
    SharedIdentifier name;
    SharedExpression expression;
    SharedStatement body;
    bool isRef = false;   // `foreach (ref T e in …)` — bind each element by place (mutable, in-place)
    ForEachNode(CodeGenContext& context,  SharedIdentifier type,
                SharedIdentifier name,
                SharedExpression expression,
                SharedStatement body)
    : ASTNode(context),  StatementNode(context)
    , type(type)
    , name(name)
    , expression(expression)
    , body(body) {}
};

// `parallel_for (ref T e in coll) { ... }` — disjoint-slice data-parallel loop (M6.3). Splits `coll` into
// K non-overlapping sub-Views, one per worker isolate, mutating each in place, and joins them ALL at the
// closing brace (self-joining barrier). Statement-only, like ScopeNode. The binding is always `ref` (the
// grammar forces it — disjoint mutable is the whole point), so no isRef flag is needed. The emitter hoists
// the body into a synthesized worker fn, threading captured outer locals in as `ref` params.
class ParallelForNode : public StatementNode {
public:
    SharedIdentifier type;         // element T
    SharedIdentifier name;         // loop binding `e`
    SharedExpression expression;   // the View<T> or a contiguous container exposing .view()
    SharedStatement  body;         // a BlockNode (its braces are the join barrier)
    ParallelForNode(CodeGenContext& context, SharedIdentifier type,
                    SharedIdentifier name,
                    SharedExpression expression,
                    SharedStatement body)
    : ASTNode(context),  StatementNode(context)
    , type(type)
    , name(name)
    , expression(expression)
    , body(body) {}
};

class BreakNode : public StatementNode {
public:
    BreakNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
};

class ContinueNode : public StatementNode {
public:
    ContinueNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
};

class ReturnNode : public StatementNode {
public:
    SharedExpression expression;
    ReturnNode(CodeGenContext& context, SharedExpression expression) : ASTNode(context),  StatementNode(context), expression(expression) { }
};

//------------------------------------------------------------------------------ 
//                              Expressions
//------------------------------------------------------------------------------

class MemberAccessNode : public ExpressionNode {
public:
    SharedIdentifier identifier;
    SharedExpression expression;
    SharedIdentifier classType;
    MemberAccessNode(CodeGenContext& context, SharedIdentifier identifier, SharedExpression expression)
        : ASTNode(context),  ExpressionNode(context)
        , identifier(identifier)
        , expression(expression) { }
    MemberAccessNode(CodeGenContext& context, SharedIdentifier identifier, SharedIdentifier classType)
        : ASTNode(context),  ExpressionNode(context)
        , identifier(identifier)
        , classType(classType) { }
};

class ArgumentNode : public ExpressionNode {
public:
    SharedIdentifier name;
    SharedModifier modifier;
    SharedExpression expression;
    ArgumentNode(CodeGenContext& context, SharedIdentifier name, SharedModifier modifier, SharedExpression expression)
        : ASTNode(context),  ExpressionNode(context)
        , name(name)
        , modifier(modifier)
        , expression(expression) { }
};

// `@name` / `@name(args)` — a declaration attribute (serialization metadata + codegen trigger), attached to
// a type declaration or a field. `args` reuses ArgumentNode: a BARE entry (`@generate(Serialize)`) carries
// its identifier in `name` with a null `expression`; a NAMED entry (`@field(name: "wire")`) carries the key
// in `name` and the value in `expression`.
class AttributeNode : public ExpressionNode {
public:
    SharedString name;
    SharedArgumentList args;
    AttributeNode(CodeGenContext& context, SharedString name, SharedArgumentList args)
        : ASTNode(context),  ExpressionNode(context)
        , name(name)
        , args(args) { }
};

// `give x` (move; source consumed) / `copy x` (duplicate). The explicit hand-off
// marker that rides a NAMED value; a fresh rvalue never needs one.
class HandoffNode : public ExpressionNode {
public:
    bool             isGive;   // true = give (move), false = copy
    SharedExpression value;
    HandoffNode(CodeGenContext& context, bool isGive, SharedExpression value)
        : ASTNode(context), ExpressionNode(context), isGive(isGive), value(value) { }
};

class ElementAccessNode : public ExpressionNode {
public:
    SharedIdentifier identifier;
    SharedExpression expression;
    SharedExpressionList expressionlist;
    ElementAccessNode(CodeGenContext& context, SharedIdentifier identifier, SharedExpressionList expressionlist)
        : ASTNode(context),  ExpressionNode(context)
        , identifier(identifier)
        , expressionlist(expressionlist) { }
    ElementAccessNode(CodeGenContext& context, SharedExpression expression, SharedExpressionList expressionlist)
        : ASTNode(context),  ExpressionNode(context)
        , expression(expression)
        , expressionlist(expressionlist) { }
};

// A fixed-array value literal used to initialize a `Fixed<T,N>`: `[a, b, c]` (elements) or
// `[v; N]` (fill: value repeated N times).
class ArrayLiteralNode : public ExpressionNode {
public:
    SharedExpressionList elements;   // `[a, b, c]` — the elements; null for the fill form
    SharedExpression fillValue;      // `[v; N]` — the repeated value
    SharedExpression fillCount;      // `[v; N]` — the count N (a constant expr)
    ArrayLiteralNode(CodeGenContext& context, SharedExpressionList elements)
        : ASTNode(context), ExpressionNode(context), elements(elements) { }
    ArrayLiteralNode(CodeGenContext& context, SharedExpression fillValue, SharedExpression fillCount)
        : ASTNode(context), ExpressionNode(context), fillValue(fillValue), fillCount(fillCount) { }
};

class ThisAccessNode : public ExpressionNode {
public:
    ThisAccessNode(CodeGenContext& context) : ASTNode(context),  ExpressionNode(context) { }
};

class BaseAccessNode : public ExpressionNode {
public:
    SharedIdentifier identifier;
    SharedExpressionList expressionlist;
    BaseAccessNode(CodeGenContext& context, SharedIdentifier identifier) : ASTNode(context),  ExpressionNode(context), identifier(identifier) { }
    BaseAccessNode(CodeGenContext& context, SharedExpressionList expressionlist) : ASTNode(context),  ExpressionNode(context), expressionlist(expressionlist) { }
};

class SimpleUnaryExpressionNode : public ExpressionNode {
public:
    int token;
    SharedExpression expression;
    SimpleUnaryExpressionNode(CodeGenContext& context, int token, SharedExpression expression) 
        : ASTNode(context),  ExpressionNode(context), token(token), expression(expression) { }
};

class CastNode : public ExpressionNode {
public:
    SharedIdentifier type;
    SharedExpression unaryExpression;
    CastNode(CodeGenContext& context, SharedIdentifier type, SharedExpression unaryExpression)
        : ASTNode(context),  ExpressionNode(context)
        , type(type)
        , unaryExpression(unaryExpression) { }
};

// `bitcast<T>(expr)` — a same-size *reinterpret* of a numeric scalar's bits (e.g. `bitcast<uint32>(f)`
// exposes a `float32`'s IEEE-754 bits). Unlike `cast<T>` (a value conversion) it changes no bits; source
// and target must be equal-width numeric scalars (`intN`/`uintN`/`floatN`). Lowers to a no-UB union
// type-pun (ISO C11 §6.5.2.3). Mirrors `CastNode`'s shape.
class BitcastNode : public ExpressionNode {
public:
    SharedIdentifier type;
    SharedExpression unaryExpression;
    BitcastNode(CodeGenContext& context, SharedIdentifier type, SharedExpression unaryExpression)
        : ASTNode(context),  ExpressionNode(context)
        , type(type)
        , unaryExpression(unaryExpression) { }
};

// `expr.as<T>()` — Model C runtime downcast of a boxed poly-dispatch error (an `Owned<Error>` or a borrowing
// `Error`) to a concrete implementing enum `T`. Yields `Optional<T>`: `Some(<the enum by value>)` if the
// box's vtbl is `T`'s, else `None` (a vtbl-pointer compare — no type-id table). Borrows the operand (peek +
// copy-out), so the operand stays valid on the `None` branch.
class AsDowncastNode : public ExpressionNode {
public:
    SharedExpression operand;
    SharedIdentifier type;
    AsDowncastNode(CodeGenContext& context, SharedExpression operand, SharedIdentifier type)
        : ASTNode(context),  ExpressionNode(context)
        , operand(operand)
        , type(type) { }
};

// `sizeof(T)` / `alignof(T)` — the compile-time byte size / alignment of a type, a `usize`
// (lowers to C `sizeof(cType)` / `_Alignof(cType)`).
class SizeofNode : public ExpressionNode {
public:
    SharedIdentifier type;
    bool isAlign = false;   // `alignof(T)` rather than `sizeof(T)`
    SizeofNode(CodeGenContext& context, SharedIdentifier type)
        : ASTNode(context),  ExpressionNode(context), type(type) { }
};

// COMPILER-INTERNAL zero-initialization of a type (lowers to the C compound literal `(T){0}`). It has
// NO grammar rule — the serialization deserialize codegen splices it into a synthesized `deserialize`
// (bypass-ctor construction: zero the struct, then populate fields). Never user-writable, so no one can
// hand-craft a half-initialized resource; see kama.driver.cpp `injectZeroInitForDeserialize`.
class ZeroValueNode : public ExpressionNode {
public:
    SharedIdentifier type;
    ZeroValueNode(CodeGenContext& context, SharedIdentifier type)
        : ASTNode(context),  ExpressionNode(context), type(type) { }
};

class BinaryExpressionNode : public ExpressionNode {
public:
    int token;
    SharedExpression LHS;
    SharedExpression RHS;
    BinaryExpressionNode(CodeGenContext& context, int token, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), token(token), LHS(LHS), RHS(RHS) { }
};

class LogicalAndOrNode : public ExpressionNode {
public:
    int token;
    SharedExpression LHS;
    SharedExpression RHS;
    LogicalAndOrNode(CodeGenContext& context, int token, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), token(token), LHS(LHS), RHS(RHS) { }
};

class TernaryExpressionNode : public ExpressionNode {
public:
    SharedExpression condition;
    SharedExpression LHS;
    SharedExpression RHS;
    TernaryExpressionNode(CodeGenContext& context, SharedExpression condition, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), condition(condition), LHS(LHS), RHS(RHS) { }
};

//------------------------------------------------------------------------------ 
//                              Statement Expressions
//------------------------------------------------------------------------------

class AssignmentNode : public ExpressionStatementNode {
public:
    SharedExpression unaryExpression;
    int token;
    SharedExpression expression;
    AssignmentNode(CodeGenContext& context, SharedExpression unaryExpression, int token, SharedExpression expression)
        : ASTNode(context),  ExpressionStatementNode(context)
        , unaryExpression(unaryExpression)
        , token(token)
        , expression(expression) { }
    virtual ~AssignmentNode(){}
};

class ObjectCreationNode : public ExpressionStatementNode {
public:
    SharedIdentifier type;
    SharedArgumentList args;
    SharedArgumentList placement;   // null for bare `new`; carries the placement `allocator: expr` list
    SharedIdentifier ctorName;      // null for `new Type(...)`; the named ctor for `new Type.name(...)` (M4)
    bool isTry = false;             // `try new T(...)` (M-step5): non-panic construction -> Optional<Owned<T>> (None on OOM)
    ObjectCreationNode(CodeGenContext& context, SharedIdentifier type, SharedArgumentList args,
                       SharedArgumentList placement = SharedArgumentList())
        : ASTNode(context),  ExpressionStatementNode(context)
        , type(type)
        , args(args)
        , placement(placement) { }
};

class InvocationNode : public ExpressionStatementNode {
public:
    SharedExpression expression;
    SharedIdentifier identifier;
    SharedArgumentList args;
    InvocationNode(CodeGenContext& context, SharedExpression expression, SharedArgumentList args)
        : ASTNode(context),  ExpressionStatementNode(context)
        , expression(expression)
        , args(args) { }
    InvocationNode(CodeGenContext& context, SharedIdentifier identifier, SharedArgumentList args)
        : ASTNode(context),  ExpressionStatementNode(context)
        , identifier(identifier)
        , args(args) { }
};

class PreIncrDecrNode : public ExpressionStatementNode {
public:
    int token;
    SharedExpression expression;
    PreIncrDecrNode(CodeGenContext& context, int token, SharedExpression expression)
        : ASTNode(context),  ExpressionStatementNode(context)
        , token(token)
        , expression(expression) { }
};

class PostIncrDecrNode : public ExpressionStatementNode {
public:
    int token;
    SharedExpression expression;
    PostIncrDecrNode(CodeGenContext& context, int token, SharedExpression expression)
        : ASTNode(context),  ExpressionStatementNode(context)
        , token(token)
        , expression(expression) { }
};

//------------------------------------------------------------------------------ 
//                              Class
//------------------------------------------------------------------------------

class ClassDeclarationNode : public StatementNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier name;
    SharedClassBaseDeclaration baseTypes;
    SharedClassMemberDeclarationList members;
    // The kind word from a `type <kind> Name { … }` declaration ("value"/"resource"/"contract").
    // Drives the ownership/access model.
    SharedString typeKind;
    // Type parameters from `type value Box<T> { … }` — monomorphized per concrete arg;
    // empty for a non-generic type. Set by the grammar action (like typeKind).
    SharedStringList typeParams;
    SharedBoundsList typeBounds;   // contract bounds parallel to typeParams (empty entry = unbounded)
    SharedStringList constParams;  // names of const generic params (`const N: int`); subset of typeParams order
    SharedIdentifierList constTypes; // each const param's declared integral type, PARALLEL TO typeParams (null entry = a type param)
    SharedIdentifierList typeDefaults; // per-param default type (`= DefaultHasher`) parallel to typeParams; null entry = no default
    SharedIdentifierList typePins; // `<T is This>` identity pin parallel to typeParams; null entry = unpinned
    SharedStringList forKinds;     // `type contract X for value|resource|both` — which kinds may implement it
    SharedAttributeList attributes;  // `@generate(...)` etc. (null when none); serialization metadata
    ClassDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
                        SharedIdentifier name,
                        SharedClassBaseDeclaration baseTypes,
                        SharedClassMemberDeclarationList members)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , name(name)
        , baseTypes(baseTypes)
        , members(members) { }
};

class ClassBaseDeclarationNode : public StatementNode {
public:
    SharedIdentifier base;
    SharedIdentifierList interfaces;
    ClassBaseDeclarationNode(CodeGenContext& context, SharedIdentifier base, SharedIdentifierList interfaces)
        : ASTNode(context),  StatementNode(context)
        , base(base)
        , interfaces(interfaces) { }
};

class ClassMemberDeclarationNode : public StatementNode {
public:
    ClassMemberDeclarationNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
};

// `comptime assert(cond: …, msg: "…");` — a compile-time assertion (const-generics M7). Valid at
// module, type-member and statement scope, which is why it derives from ClassMemberDeclarationNode:
// that already IS a StatementNode, so one node reaches all three positions.
//
// Inside a generic it is checked once per instantiation (with `_constSubst`/`_typeSubst` bound), so a
// failure names the use site. Two lowerings under one surface: a predicate that folds is answered by
// kama; a pure layout predicate over an aggregate's `sizeof`/`alignof` — which kama deliberately
// cannot fold — becomes a C11 `_Static_assert` and clang answers it.
//
// The parser accepts ANY bare/qualified callee here: the trailing `(` is what keeps the production
// LALR(1)-clean against `comptime <type> <name>`, so the name check belongs in the emitter, where a
// real diagnostic can be written. `assert` stays an ordinary identifier, never a keyword.
class ComptimeAssertNode : public ClassMemberDeclarationNode {
public:
    SharedIdentifier   callee;      // must be the bare name `assert` — checked in the emitter
    SharedArgumentList args;        // `cond:` (a predicate) and `msg:` (a string LITERAL), both mandatory
    SharedModifierList modifiers;   // only ever non-empty at member scope; an assert has no visibility
    ComptimeAssertNode(CodeGenContext& context, SharedIdentifier callee, SharedArgumentList args)
        : ASTNode(context), ClassMemberDeclarationNode(context)
        , callee(callee), args(args) { }
};

// `friend <accessor>(member, …);` (or `friend <accessor>;` = all privates): the
// OWNING class grants the named accessor (a class / free function / Class::method) access
// to the named private members. Owner-granted, narrow, greppable.
class FriendGrantNode : public ClassMemberDeclarationNode {
public:
    SharedIdentifier     accessor;   // class / free function / Class::method to grant to
    SharedIdentifierList members;    // specific private members; empty => all privates
    FriendGrantNode(CodeGenContext& context, SharedIdentifier accessor, SharedIdentifierList members)
        : ASTNode(context), ClassMemberDeclarationNode(context)
        , accessor(accessor), members(members) { }
};

class ClassConstDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier type;
    SharedConstVariableDeclaratorList declarators;
    bool isComptime = false;    // `comptime T NAME` (6b-2) — type-associated compile-time constant (`Type::NAME`), not a per-instance field
    ClassConstDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
            SharedIdentifier type,
            SharedConstVariableDeclaratorList declarators) 
    : ASTNode(context),  ClassMemberDeclarationNode(context)
    , modifiers(modifiers)
    , type(type)
    , declarators(declarators) { }
};

class ClassFieldDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier type;
    SharedVariableDeclaratorList declarators;
    SharedAttributeList attributes;  // `@field`/`@skip`/`@bits(...)` (null when none); serialization metadata
    ClassFieldDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
            SharedIdentifier type,
            SharedVariableDeclaratorList declarators)
            : ASTNode(context),  ClassMemberDeclarationNode(context)
            , modifiers(modifiers)
            , type(type)
            , declarators(declarators) { }
};

class ClassMethodDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier returnType;
    SharedIdentifier name;
    SharedParameterList params;
    SharedBlock body;
    bool isConst = false;   // `const fn …` — a non-mutating method
    bool isRef = false;     // `fn ref T …` — returns a PLACE (a T*), deref'd at the caller
    bool isCtor = false;    // `ctor name(…)` — a named constructor (static factory returning the enclosing
                            // type / `Result<This,E>`); reuses the method pipeline. returnType null => infallible.
    bool isComptime = false; // `comptime fn …` — a type-associated compile-time-only function (6b-3); read `Type::name()`
    // `fn … when [P1: B1, …]` — the gated type-params + required contracts (index-aligned, AND). Empty = unconditional.
    SharedIdentifierList whenParams;
    SharedIdentifierList whenBounds;
    ClassMethodDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
            SharedIdentifier returnType,
            SharedIdentifier name,
            SharedParameterList params,
            SharedBlock body)
    : ASTNode(context),  ClassMemberDeclarationNode(context)
    , modifiers(modifiers)
    , returnType(returnType)
    , name(name)
    , params(params)
    , body(body) { }
};

class ClassOperatorDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedClassOperatorDeclarator operatorDeclarator;
    SharedBlock body;
    ClassOperatorDeclarationNode(CodeGenContext& context, SharedModifierList modifiers, SharedClassOperatorDeclarator operatorDeclarator, SharedBlock body)
            : ASTNode(context),  ClassMemberDeclarationNode(context)
            , modifiers(modifiers)
            , operatorDeclarator(operatorDeclarator)
            , body(body) { }
};

class ClassOperatorDeclaratorNode : public StatementNode {
public:
    SharedIdentifier returnType;
    int opToken;
    SharedIdentifier param1Type;
    SharedIdentifier param1Name;
    SharedIdentifier param2Type;
    SharedIdentifier param2Name;
    bool refReturn = false;   // `ref T operator[](…)` — returns a PLACE (a T*), not a value
    ClassOperatorDeclaratorNode(CodeGenContext& context, SharedIdentifier returnType,
            int opToken,
            SharedIdentifier param1Type,
            SharedIdentifier param1Name,
            SharedIdentifier param2Type,
            SharedIdentifier param2Name) 
        : ASTNode(context),  StatementNode(context)
        , returnType(returnType)
        , opToken(opToken)
        , param1Type(param1Type)
        , param1Name(param1Name)
        , param2Type(param2Type)
        , param2Name(param2Name) {}
};

class ClassConstructorDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedClassConstructorDeclarator declarator;
    SharedBlock body;
    ClassConstructorDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
            SharedClassConstructorDeclarator declarator,
            SharedBlock body)
        : ASTNode(context),  ClassMemberDeclarationNode(context)
        , modifiers(modifiers)
        , declarator(declarator)
        , body(body) {}
};

class ClassConstructorDeclaratorNode : public StatementNode {
public:
    SharedIdentifier constructorName;
    SharedParameterList params;
    SharedClassConstructorInitializer initializer;
    ClassConstructorDeclaratorNode(CodeGenContext& context, SharedIdentifier constructorName,
                SharedParameterList params,
                SharedClassConstructorInitializer initializer)
            : ASTNode(context),  StatementNode(context)
            , constructorName(constructorName)
            , params(params)
            , initializer(initializer) {}
};

class ClassConstructorInitializerNode : public StatementNode {
public:
    SharedArgumentList args;
    ClassConstructorInitializerNode(CodeGenContext& context, SharedArgumentList args) : ASTNode(context),  StatementNode(context), args(args) {}
};

class ClassDestructorDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier destructorName;
    SharedBlock body;
    ClassDestructorDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
                SharedIdentifier destructorName,
                SharedBlock body) 
        : ASTNode(context),  ClassMemberDeclarationNode(context)
        , modifiers(modifiers)
        , destructorName(destructorName)
        , body(body) {}
};

//------------------------------------------------------------------------------ 
//                              Enums
//------------------------------------------------------------------------------

class EnumDeclarationNode : public StatementNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier identifier;
    SharedEnumMemberDeclarationList body;
    // `enum Name : IntType { … }` — pins the underlying integer (plain enum) / tag width
    // (tagged union); null = compiler-chosen. Set by the grammar action after construction.
    SharedIdentifier underlyingType;
    // `enum Optional<T> { … }` — type parameters + contract bounds, monomorphized per
    // concrete arg (mirror of ClassDeclarationNode); empty for a non-generic enum.
    SharedStringList typeParams;
    SharedBoundsList typeBounds;
    SharedStringList constParams;  // names of const generic params (`const N: int`); subset of typeParams order
    SharedIdentifierList constTypes; // each const param's declared integral type, PARALLEL TO typeParams (null entry = a type param)
    SharedIdentifierList typePins; // `<T is This>` identity pin parallel to typeParams; null entry = unpinned
    SharedIdentifierList typeDefaults; // per-param default type (`= …`) parallel to typeParams; null entry = no default
    SharedAttributeList attributes;  // `@generate(Serialize, Deserialize)` on the enum (null when un-attributed)
    // `type enum E : uint8 implements C, D { A, B; …methods… }` — the conformance clause and the members
    // that satisfy it. An enum is a full type kind, so it declares conformance inline like every other
    // kind; before this it had no `class_base_opt` at all and needed a retroactive `implements C for E`.
    // `extends` is rejected by the emitter (an enum has no base), as are fields and a destructor.
    SharedClassBaseDeclaration baseTypes;
    SharedClassMemberDeclarationList members;   // methods/consts after the `;`; null when the body has none
    EnumDeclarationNode(CodeGenContext& context, SharedModifierList modifiers, SharedIdentifier identifier, SharedEnumMemberDeclarationList body)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , identifier(identifier)
        , body(body) { }
};

// `type intrinsic <int8, int16, …> implements C { …methods… <int8> { …methods… } }` — conformance for a
// PRIMITIVE, the kind that had no kama spelling at all (it existed only as a compiler-internal notion, so
// the prelude had to retro-implement onto it 68 times).
//
// The set form is load-bearing: one body serves every target whose implementation is genuinely identical,
// and a `<…>` SECTION overrides it for the targets where it is not. A primitive never gets a `_classes`
// entry — its conformance lives in a separate registry — so this is not a `ClassDeclarationNode`.
class IntrinsicImplNode : public StatementNode {
public:
    SharedModifierList               modifiers;
    SharedString                     kindWord;   // the positional kind word — must be `intrinsic`
    SharedIdentifierList             targets;    // the set, in declaration order
    SharedClassBaseDeclaration       baseTypes;  // `implements C` — exactly one contract per block
    SharedClassMemberDeclarationList members;    // bodies shared by every target
    SharedIntrinsicSectionList       sections;   // per-target overrides
    IntrinsicImplNode(CodeGenContext& context, SharedModifierList modifiers, SharedIdentifierList targets,
                      SharedClassBaseDeclaration baseTypes, SharedIntrinsicBody body)
        : ASTNode(context), StatementNode(context)
        , modifiers(modifiers)
        , targets(targets)
        , baseTypes(baseTypes)
        , members(body ? body->members : SharedClassMemberDeclarationList())
        , sections(body ? body->sections : SharedIntrinsicSectionList()) { }
};

class EnumMemberDeclarationNode : public StatementNode {
public:
    SharedIdentifier identifier;
    SharedExpression constantExpression;
    // `Circle(float64 radius)` — named payload fields of a discriminated-union variant;
    // null/empty for a plain (no-payload) variant. Reuses the ordinary parameter list.
    SharedParameterList payload;
    EnumMemberDeclarationNode(CodeGenContext& context, SharedIdentifier identifier, SharedExpression constantExpression)
        : ASTNode(context),  StatementNode(context), identifier(identifier), constantExpression(constantExpression) { }
};

//------------------------------------------------------------------------------
//                              Match
//------------------------------------------------------------------------------

// One arm: `case Variant(bind1, bind2): expr;` (or `case _: expr;` — the wildcard).
// `:= expr;` — the value a match arm's block evaluates to (the arm's result), assigned to whatever the
// whole `match` is bound to. Must be the arm block's FINAL statement (single-exit). Distinct from
// `return`, which leaves the enclosing function.
class ArmValueNode : public StatementNode {
public:
    SharedExpression value;
    ArmValueNode(CodeGenContext& context, SharedExpression value)
        : ASTNode(context), StatementNode(context), value(value) { }
};

class MatchArmNode : public StatementNode {
public:
    SharedString     variantName;   // the variant matched; "_" = wildcard
    SharedStringList bindings;      // the LOCALS a payload pattern introduces; null/empty if none
    SharedStringList labels;        // parallel to `bindings`: the FIELD each local binds, from `field: local`.
                                    // Patterns are named, never positional — see the grammar's match_bindings.
    SharedIdentifierList labelIds;  // the labels again as nodes, so a pattern label carries a span and can
                                    // resolve to the field it names (hover / go-to-definition / references).
    // LSP (M3.4): the same two names again, as nodes carrying a source span, so find-references and
    // rename can reach a `case Ok:` arm and its payload bindings. Kept ALONGSIDE the strings above —
    // every existing reader spells `*variantName` / `*bindings[i]` and is untouched.
    SharedIdentifier     variantId;
    SharedIdentifierList bindingIds;   // parallel to `bindings`, same order
    SharedExpression body;          // single-expression arm: the arm's value / side-effect expression
    SharedBlock      block;         // block arm `{ … }` (multi-statement); one of body/block is set
    explicit MatchArmNode(CodeGenContext& context)
        : ASTNode(context), StatementNode(context) { }
    bool isWildcard() const { return variantName && *variantName == "_"; }
};

// `match (subject) { arms }` — a single value-producing construct usable in statement position
// (value discarded) and expression position (lifted to a temp, strict ISO C11). Dual-nature via
// ExpressionStatementNode (both an ExpressionNode and a StatementNode).
class MatchNode : public ExpressionStatementNode {
public:
    SharedExpression   subject;
    SharedMatchArmList arms;
    MatchNode(CodeGenContext& context, SharedExpression subject, SharedMatchArmList arms)
        : ASTNode(context), ExpressionStatementNode(context), subject(subject), arms(arms) { }
};

// Fill a freshly parsed unit's `topLevelNames` / `hasIntrinsicImpl` (see CompilationUnit). Called from
// the `compilation_unit` action in kama.y — the single reduction every parse goes through — so the walk
// sees the RAW decl list, before `@compileFor` pruning can rewrite it.
//
// The kind list must stay exhaustive against `CEmitter::emitModuleContent` (kama.cemit.cpp), which is the
// only walk that errors on an unhandled top-level kind and is therefore the cross-check when a new
// declaration form is added. `CEmitter::pruneInactiveDecls`'s local `nameOf` lambda answers the same
// question for a narrower purpose (it needs only the `@compileFor`-gatable kinds, and omits
// ModuleVariableDeclaration); if it grows, the two want reconciling.
//
// Missing a name here does NOT produce a wrong build: an import naming a symbol the index cannot find
// falls back to loading the whole module (closureOfModule, kama.driver.cpp). It only costs pruning.
inline void harvestUnitFacts(const SharedCompilationUnit& unit)
{
    if (!unit || !unit->codeDeclarationList) return;
    for (auto& decl : *unit->codeDeclarationList) {
        ASTNode* d = decl.get();
        if (auto* f = dynamic_cast<FunctionDeclarationNode*>(d)) {          // fn / extern fn / comptime fn
            if (f->name && f->name->value) unit->topLevelNames.insert(*f->name->value);
        } else if (auto* c = dynamic_cast<ClassDeclarationNode*>(d)) {      // type <kind> Name — the kind
            if (c->name && c->name->value) unit->topLevelNames.insert(*c->name->value);   // word is a bare
        } else if (auto* e = dynamic_cast<EnumDeclarationNode*>(d)) {       // IDENTIFIER, not a closed set
            if (e->identifier && e->identifier->value) unit->topLevelNames.insert(*e->identifier->value);
        } else if (auto* v = dynamic_cast<ModuleVariableDeclaration*>(d)) { // static / comptime T NAME = …
            if (v->variables)
                for (auto& var : *v->variables)
                    if (var && var->name && var->name->value) unit->topLevelNames.insert(*var->name->value);
        } else if (dynamic_cast<IntrinsicImplNode*>(d)) {
            // `type intrinsic <int32> implements FromStr { … }` registers a conformance for a PRIMITIVE,
            // program-wide, under no name of its own. Two files in lib/ have one.
            unit->unprunable = true;
        } else if (auto* inc = dynamic_cast<IncludeNode*>(d)) {
            // A bare `extern "hdr.h";` is normally prunable — it supports that file's own functions, so if
            // nothing references them the header is not needed either. The exception is a header a
            // CONSTRUCT requires: `spawn` and `parallel_for` both demand the isolate seam and reject a
            // program without it (CEmitter::isolatePrep, emitParallelFor), while naming nothing at all in
            // the file that provides it. And the demand is program-wide — the file doing the `spawn` need
            // not be the one that imported std::concurrent — so no per-import closure can see it.
            //
            // Keeping the provider unconditionally is one extra unit for programs that import
            // std::concurrent, and nothing for anyone else. The blanket alternative (keep every file with
            // any `extern`) would cost four of lib/std/collections' fourteen for no reason.
            // KEEP IN SYNC with the `externsHeader(...)` calls in kama.cemit.cpp that REQUIRE rather than
            // merely detect a header. Today that is exactly this one.
            if (inc->header && *inc->header == "kama_isolate.h") unit->unprunable = true;
        }
    }
}

#endif //__KAMA_AST_H__