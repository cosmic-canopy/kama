#ifndef __CSTAR_CEMIT_H__
#define __CSTAR_CEMIT_H__

// C-emitting backend. An external visitor over the AST (dispatch via
// dynamic_cast for now) that writes portable C. Kept entirely out of the AST
// headers so emission can evolve without recompiling the world.

#include <ostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>     // fixed-width ints — not transitive on all libcs (e.g. Windows UCRT)
#include "cstar.forward.h"

// A function parameter, in declared order. Named cstar arguments are matched
// against these to recover C's positional order at each call site.
struct ParamSig {
    std::string name;
    bool        byRef;        // ref/out => passed as a pointer (call site emits &arg)
    std::string className;    // class type (for ref upcast at call sites), "" if primitive
    bool        isConst = false;   // `const` param — emits `const T*` for FFI pointers
};

struct FuncSig {
    std::string            cName;    // mangled C name (e.g. main -> cstar_main)
    std::string            retCType; // resolved C return type (signature check)
    std::vector<ParamSig>  params;
};

// A function-pointer signature type: a bodiless `fn ret Name(params);`.
// Lowers to `typedef ret (*cName)(paramtypes);`. FunctionPtr<Name> spells `cName`.
struct SigInfo {
    std::string            cName;       // typedef name (namespace-mangled)
    std::string            retCType;    // resolved C return type (for the typedef)
    std::vector<ParamSig>  params;      // names (named-arg invoke) + C types (className)
};

// ---- Class model ----------------------------------------------------------

// Member access level. Default Private; `value` fields are forced Public.
enum class Visibility { Private, Protected, Public };

struct FieldInfo {
    std::string      name;
    SharedIdentifier type;
    SharedExpression initializer;    // optional; applied in the constructor
    Visibility       visibility = Visibility::Private;
};

// One case of a discriminated-union `enum` (tagged union). `name` is the variant, `payload`
// its named fields (empty = no payload); the tag value is the declaration index. Stored on ClassInfo
// (a payload/generic enum is backed by a ClassInfo, reusing monomorphization + RAII + move analysis).
struct VariantCase {
    std::string            name;      // "Circle"
    std::vector<FieldInfo> payload;   // named fields (name + type); empty for a no-payload variant
};

// A type's ownership kind. `Value` owns nothing (copies); `Resource` owns/has identity (moves,
// RAII-dropped); `Contract` is the interface path (handled via InterfaceInfo) — these three come
// from a user `type <kind> Name` marker. `Intrinsic` is the neutral kind for compiler-built types
// with no marker (collections, smart-ptrs, tagged-union enums); their ownership is driven by their
// own machinery (isCollection/isSmartPtr/isVariant + destructibility), not the kind.
enum class TypeKind { Value, Resource, Contract, Intrinsic };

struct MethodInfo {
    std::string                  cName;   // Class__method (declaring class)
    SharedIdentifier             returnType;
    std::vector<ParamSig>        params;
    ClassMethodDeclarationNode*  node = nullptr;   // for body emission (null for intrinsics)
    bool                         isVirtual  = false;  // virtual/override/abstract
    bool                         isOverride = false;
    bool                         isAbstract = false;  // null body
    bool                         isIntrinsic = false; // collection op: body is in cstar_runtime.h, not AST
    bool                         isConst = false;     // `const fn …` — non-mutating
    Visibility                   visibility = Visibility::Private;
    bool                         isFinal = false;     // `final fn` — seals a virtual slot
    bool                         isStatic = false;    // `static fn` — no implicit `self`; called `Type::m(...)`
    std::string                  whenParam;   // `fn … when T: Bound` — the gated type-param ("" = unconditional)
    std::string                  whenBound;   // the required contract (source name; e.g. "Copyable")
    // Operator overloads register as methods under a synthetic name (`op_add`, `op_neg`, …).
    // They are NOT ClassMethodDeclarationNode, so `node` stays null: emit from `opDecl` instead.
    bool                         isOperator = false;
    int                          arity = 0;           // 0 = unary-on-this, 1 = binary method (`this`+rhs), 2 = binary free
    ClassOperatorDeclarationNode* opDecl = nullptr;   // the operator decl (body/params) when isOperator
    bool                         isPlaceReturn = false;  // `ref T operator[]` — returns a PLACE (T*), deref'd at the caller
};

// A built-in generic collection / smart-pointer kind. Backed by a C
// runtime template. Owned<T> is a unique heap-owning pointer kind.
enum class CollKind { Array, List, String, Owned, Shared, Weak, Bindable, Fixed };

// Per-file namespace context. A file with `namespace X;` is public (scope
// = mangled X); a file without one is private (scope = "_F<idx>"). Bare names
// resolve to the file's own scope, then its `using`s — never another file's
// private symbols (private-by-default).
struct NsCtx {
    std::string scope;        // mangle prefix: "Graphics" or "_F3"
    bool        isPublic = false;
    std::vector<std::string> usings;                  // imported public namespaces (mangled)
    std::map<std::string, std::string> aliases;       // alias -> mangled namespace (module alias / `using X = Y`)
    std::map<std::string, std::string> symbolAliases; // per-symbol import: local name -> mangled global symbol
};

// A resolved `friend` grant on the OWNING class. `accessor` is a resolved key:
// a class name (accessorIsClass) or a function/method C-name (matched vs _currentFunc).
// `members` empty => all private members.
struct FriendGrant {
    std::string            accessor;
    bool                   accessorIsClass = false;
    std::set<std::string>  members;
};
// Pre-resolution form captured at collection (accessor spelling + member names).
struct RawFriendGrant {
    SharedIdentifier       accessor;
    std::set<std::string>  members;
    int                    line = 0;
};

struct ClassInfo {
    std::string                       name;       // struct name (== cstar class name)
    TypeKind                          kind = TypeKind::Intrinsic;   // set to value/resource for user types
    std::vector<FieldInfo>            fields;      // declaration order
    std::set<std::string>            fieldNames;
    std::set<std::string>            constFields;   // `const` data members — write-once in the ctor
    std::map<std::string, MethodInfo> methods;    // by cstar method name
    bool                              hasCtor = false;
    bool                              synthCtor = false;  // default ctor synthesized (vtable init)
    bool                              preludeStatic = false;  // a non-generic prelude type (e.g. Chars) whose
                                                              // method bodies must be emitted static-inline in
                                                              // the header (the prelude is otherwise collect-only)
    std::vector<ParamSig>             ctorParams;
    ClassConstructorDeclarationNode*  ctorNode = nullptr;
    ClassDeclarationNode*             node    = nullptr;

    // RAII
    bool                              hasDtor = false;   // declares its own ~dtor
    ClassDestructorDeclarationNode*   dtorNode = nullptr;
    bool                              destructible = false; // own dtor OR a destructible field (transitive)
    // Opted into the `Copyable` contract — declares a public nullary `copy` returning
    // `implements Copyable(bare: give|copy)`: this resource opts into copy (a public nullary `copy()`),
    // and its `bareDefault` says what a BARE hand-off means (give=move, copy=`copy()`/retain). Movable +
    // Droppable are universal (every value/resource); there is no `!Movable`/"copy-only". A value is
    // implicitly copyable (bitwise) and its bare-default is copy.
    bool                              copyable = false;
    int                               bareDefault = 0;   // GIVE or COPY token (the mandatory param on a Copyable resource); 0=none
    // `implements Copyable(bare: …) when <param>: <bound>` on a generic type — the capability is
    // CONDITIONAL: each instance is Copyable only when its `<param>` satisfies `<bound>` (evaluated in
    // registerGenericTypeInst; the gated `copy()` is dropped from instances where it doesn't hold).
    std::string                       copyableWhenParam;   // gated type-param name ("" = unconditional)
    std::string                       copyableWhenBound;   // required contract (source name; "Copyable" for the container case)

    // Inheritance + virtual dispatch
    std::string                       baseName;        // "" if no base
    ClassInfo*                        base = nullptr;  // resolved by linkBases()
    bool                              isAbstractClass = false;
    // `virtual`/`abstract`/`final` are extensibility qualifiers on a `resource`.
    bool                              isVirtualClass = false; // `virtual resource` — extensible base
    bool                              isFinalClass = false;   // `final class` — sealed leaf
    Visibility                        ctorVisibility = Visibility::Public;   // synth/default ctor is public; an EXPLICIT ctor defaults private
    std::vector<RawFriendGrant>       friendGrantsRaw;        // captured at collection
    std::vector<FriendGrant>          friendGrants;           // resolved (resolveFriends)
    bool                              hasVtable = false;     // this or an ancestor has a virtual
    std::string                       vtableRoot;            // class owning the __vptr member
    std::map<std::string,std::string> slotImpl;             // virtual slot name -> impl cName (most-derived here)

    // Contracts
    std::vector<std::string>          interfaces;            // implemented contract names

    // Collections: a monomorphized Coll<T> is a synthetic ClassInfo whose
    // method bodies come from a C-template macro (not cstar AST).
    bool                              isCollection = false;
    CollKind                          collKind = CollKind::Array;
    std::string                       collElemClass;         // element class name ("" if primitive)
    bool                              isGenericInst = false; // a specialized generic-type instance (Box_int32)

    // Tagged unions: a payload/generic `enum` is backed by a ClassInfo whose layout is a
    // discriminant tag + a union of per-variant payloads (not the flat `fields`). `variants` drives
    // struct + per-variant dtor emission; `tagCType` pins the tag width (`: IntType`), "" = Name_Tag.
    bool                              isVariant = false;
    std::vector<VariantCase>          variants;
    std::string                       tagCType;              // "" -> the synthesized `Name_Tag` enum

    // Namespaces: the declaring file's scope/usings/symbol-aliases, for resolving this
    // type's field/base/method references during header emission (a field typed with an
    // imported generic — `import std::memory::{Shared}` then a `Shared<T>` field — needs the
    // per-symbol alias, not just scope+usings).
    std::string                       scope;                 // mangle prefix ("" for collections)
    std::vector<std::string>          usings;
    std::map<std::string, std::string> symbolAliases;        // per-symbol import alias -> mangled global

    // FFI: an `extern class` is an external C struct — cstar uses its
    // fields for access but never emits it (a header/linked code provides it),
    // keeps its literal C name, and never manages its lifetime.
    bool                              isExternStruct = false;
};

// A monomorphized collection instantiation (e.g. Array<int32> -> Array_int32).
struct CollectionInfo {
    CollKind     kind;
    std::string  cName;            // mangled struct/func prefix: "Array_int32"
    std::string  elemCType;        // "int32_t" / "Point" (C spelling of the element)
    std::string  elemClass;        // element class name ("" if primitive)
    std::string  elemMangle;       // "int32" / "Point" (mangling suffix)
    bool         elemDestructible = false;
    bool         elemCopyable = false;   // element is a `Copyable` resource -> deep-copy each
    bool         elemIsInterface = false;   // owned-contract smart ptr (fat {obj, vtbl} element)
    int64_t      constValue = 0;         // Fixed<T,N> only: the compile-time size N (the array length)
    // The paired smart-ptr INSTANCE name across the Shared<->Weak pair: for a Weak, the Shared it upgrades
    // to (the `SHARED_NAME` for __upgrade + `Optional<that>`); for a Shared, the Weak it downgrades to.
    // Defaults to the conventional `Shared_`/`Weak_` prefix; set explicitly for a library `Rc`/`RcWeak` pair.
    std::string  ifacePartner;
    std::string  downgradeName;    // a Shared with a library weak partner: the `downgrade` method name to emit ("" = none)
};

// A `contract`: a set of method prototypes, lowered to a vtable struct
// type + a fat-pointer value type. Implemented by classes via a C__as_I vtable. The method's
// return type + params are stored directly (not a node pointer) so it can be built from a
// `type contract` (ClassMethodDeclarationNode).
struct InterfaceMethod { std::string name; SharedIdentifier returnType; SharedParameterList params;
                         bool isPlaceReturn = false; };  // `fn ref T m()` — vtbl slot/cast spells `T*`
struct InterfaceInfo {
    std::string                  name;
    std::vector<InterfaceMethod> methods;
    std::string                  scope;
    std::vector<std::string>     usings;
    // Kind-gate (`for value|resource|both`): which kinds may `implements` this contract. Both true = `both`.
    bool                         allowsValue = false;
    bool                         allowsResource = false;
    // A specialized generic-contract instance (`Iterator_int32`) — emitted under a bound _typeSubst so
    // its `T`-typed method sigs resolve; the template itself lives in _genericContracts, not here.
    bool                         isGenericInst = false;
    std::string                  templateKey;   // the generic contract this specializes (e.g. "Iterator")
    std::vector<SharedIdentifier> typeArgs;      // the concrete args (e.g. [int32])
};

// An enum: lowered to a C `enum` with members mangled `Enum_Member`.
struct EnumMember { std::string name; SharedExpression value; };  // value optional
struct EnumInfo   {
    std::string name;
    std::vector<EnumMember> members;
    std::string scope;
    std::vector<std::string> usings;
    std::string underlyingCType;                // `enum E : IntType` -> fixed-width int C type; "" = plain `enum`
};

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // The implicit prelude (library sum types Optional/Result). Collected before user code
    // with a global namespace, so its templates register but emit nothing unless instantiated.
    void setPrelude(SharedCompilationUnit u) { _preludeUnit = u; }

    // Emit a single self-contained translation unit (transpile / single-file
    // build). Returns the number of unsupported nodes (0 == fully lowered).
    int emit(SharedCompilationUnit unit);

    // Emit a multi-file program: one shared header (all decls) + one .c of
    // definitions per source file. `headerName` is the #include spelling the
    // modules use; `moduleStreams` is parallel to `units`.
    int emitProgram(const std::vector<SharedCompilationUnit>& units,
                    const std::string& headerName, std::ostream& header,
                    const std::vector<std::ostream*>& moduleStreams,
                    const std::vector<std::string>& sourcePaths);  // per-module #line paths

private:
    std::ostream* _out;
    SharedCompilationUnit _preludeUnit;   // implicit prelude (Optional/Result), collect-only
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    std::map<std::string, FuncSig> _funcs;   // cstar function name -> signature
    std::map<std::string, SigInfo> _sigs;    // function-pointer signature types
    bool isSigType(const std::string& name) const { return _sigs.count(name) != 0; }
    std::set<std::string> _refParams;        // by-ref params of the function being emitted
    std::vector<std::string> _foreachColls;  // root bindings of collections being iterated (nested foreach) —
                                             // growing one mid-iteration (`add`) invalidates its element refs

    std::map<std::string, ClassInfo>   _classes;     // class name -> info
    std::map<std::string, std::string> _localTypes;  // local/param -> class name ("" if primitive)
    std::map<std::string, std::string> _localCTypes; // local -> full C type (incl. primitives) — the
                                                     // target type for a value-producing RHS at an assignment
    // Compile-time move analysis for `resource` (destructible) VALUES. Per-local
    // move-state, consulted by emitScopeCleanup (skip a moved local's dtor) and the hand-off
    // sites (reject use-after-move). A value moved on some-but-not-all paths that is live at
    // scope exit is *rejected* (conditional-drop) — zero runtime drop-flags by construction.
    enum class MoveState { NotMoved, MaybeMoved, Moved };
    std::map<std::string, MoveState> _moveState;   // move-only local/param cVar -> state
    ClassInfo*                         _currentClass = nullptr;  // when emitting a method/ctor
    std::string                        _currentFunc;             // C-name of the function/method being emitted (friend match)
    std::string                        _thisType;                // C name `This` resolves to (the class being emitted, or the contract type inside its vtbl slot)

    // Virtual dispatch: per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots
    std::set<std::pair<std::string,std::string>> _overriddenSlots;  // (vtableRoot, slot) overridden somewhere -> keep dynamic

    std::map<std::string, InterfaceInfo> _interfaces;        // contract name -> info
    std::map<std::string, EnumInfo>      _enums;             // enum name -> info
    std::map<std::string, CollectionInfo> _collections;      // cName -> info
    std::vector<std::string>              _collectionOrder;  // registration order (inner-first; a
                                                             // collection's dtor calls its element's,
                                                             // so the element must emit first)

    // Generic functions (monomorphization). A generic template is registered by its
    // mangled cName; each reachable (template, concrete-type-args) pair is a synthetic
    // instantiation emitted as a `static` C function. Call-site inference runs once at
    // discovery and records the target per call node, so emission is a lookup, not re-inference.
    struct GenericInst { std::string templateKey; std::string mangledName; std::vector<SharedIdentifier> typeArgs; };
    std::map<std::string, FunctionDeclarationNode*> _generics;      // template cName -> node
    std::map<std::string, NsCtx>                    _genericCtx;    // template cName -> home namespace ctx
    std::map<std::string, GenericInst>              _genericInsts;  // mangled name -> instantiation (dedup)
    std::map<const InvocationNode*, std::string>    _callInst;      // generic call site -> instantiation mangled name
    std::map<std::string, SharedIdentifier>         _typeSubst;     // type-param name -> concrete (only while emitting an instantiation)
    std::map<std::string, int64_t>                  _constSubst;    // const-param name (`const N: int`) -> value (parallel to _typeSubst)
    std::map<int, SharedIdentifier>                 _primTypeCache; // synthesized primitive type nodes (for inference)
    std::shared_ptr<CodeGenContext>                 _synthCtx;      // context for synthesizing those nodes

    // Generic TYPES (`type value Box<T>`). The TEMPLATE is kept OUT of _classes (so the normal
    // class loops never see it); each reachable `Box<Arg>` becomes a synthetic specialized ClassInfo
    // (`Box_int32`, isGenericInst=true) registered in _classes and emitted under _typeSubst.
    struct GenericTypeInst { std::string templateKey; std::string mangledName; std::vector<SharedIdentifier> typeArgs; };
    std::map<std::string, ClassInfo>          _genericTypes;        // template name -> ClassInfo shape (NOT in _classes)
    std::map<std::string, std::vector<std::string>> _genericTypeParams;  // template name -> type-param names [A, B]
    std::map<std::string, SharedBoundsList>   _genericTypeBounds;   // template name -> per-param contract bounds
    std::map<std::string, NsCtx>              _genericTypeCtx;      // template name -> home namespace ctx
    std::map<std::string, NsCtx>              _genericTypeInstCtx;  // instance -> registration (use-site) ctx, so a
                                                                    // prelude template's user-type args resolve at emit time
    std::map<std::string, GenericTypeInst>    _genericTypeInsts;    // mangled name -> instantiation (dedup)
    std::map<std::string, std::string>        _genericTypeInstOf;   // mangled name -> template name (construction)
    std::vector<std::string>                  _genericTypeInstOrder;// registration order (inner-first; struct-typedef emit)
    bool                                      _emitStaticClass = false;  // prefix `static` on specialized class fns (header ODR)

    // Generic CONTRACTS (`type contract Iterator<T>`) — the exact parallel of generic TYPES above. The
    // TEMPLATE is kept OUT of _interfaces (so the eager vtable-emit loop never sees its unbound `T`);
    // each reachable `Iterator<Arg>` becomes a specialized InterfaceInfo (`Iterator_int32`,
    // isGenericInst=true) registered in _interfaces and emitted under _typeSubst. Bound-checking is by
    // method NAME (T-independent), so it reads the template's methods directly (no instance needed).
    std::map<std::string, InterfaceInfo>            _genericContracts;      // template name -> InterfaceInfo shape (NOT in _interfaces)
    std::map<std::string, std::vector<std::string>> _genericContractParams; // template name -> type-param names [T]
    std::map<std::string, NsCtx>                    _genericContractCtx;    // template name -> home namespace ctx
    std::map<std::string, NsCtx>                    _genericContractInstCtx;// instance -> use-site ctx (its type args, e.g. a user `Point`, resolve here — like _genericTypeInstCtx)
    std::set<std::string>                           _genericContractInsts;  // mangled instance names already registered (dedup)
    std::string                                     _derefContract;         // resolved name of the prelude `Deref` contract ("" if none in scope) — gates auto-deref
    std::string                                     _heapOwnerContract;     // resolved name of the prelude `HeapOwner` contract — `new` placement-constructs into a type implementing it
    std::string                                     _movableContract;       // resolved name of the prelude `Movable` marker (implicit on every resource; `!Movable` subtracts it)
    std::string                                     _copyableContract;      // resolved name of the prelude `Copyable` marker

    // Namespaces: current-file scope + the helpers that mangle/resolve names.
    NsCtx _nsCtx;
    std::set<std::string> _namespaces;   // registered public namespaces (mangled)
    std::set<std::string> _exported;     // mangled names of `export`ed top-level decls (module public surface)
    std::set<std::string> _externNames;  // FFI: literal C names of extern structs
    void emitIncludes(const std::vector<SharedCompilationUnit>& units);  // FFI #include directives
    std::map<const CompilationUnit*, NsCtx> _unitCtx;   // each file's context (for emit)
    NsCtx ctxOf(SharedCompilationUnit unit, int fileIndex);      // build a file's NsCtx
    static std::string qualifiedName(SharedIdentifier id);       // dotted "a.b.c" from value+qualifier
    static std::string mangleNs(const std::string& ns);          // "a.b" -> "a__b"
    std::string qualify(const std::string& name) const;          // scope-prefix a declared name
    std::string resolveUserName(const std::string& value, SharedStringList qualifier);  // class/enum/iface ref
    std::string resolveFunc(const std::string& name, SharedStringList qualifier);       // function ref
    bool isNamespace(const std::string& name) const;             // a known public namespace (or alias)

    // RAII scope stack: live destructible locals per lexical scope.
    struct LiveLocal { std::string cVar; std::string className; };
    struct Scope { std::vector<LiveLocal> locals; bool isLoopBoundary = false; bool isFunctionRoot = false; };
    std::vector<Scope> _scopes;
    // By-value smart-ptr params the callee owns — dropped at fn-end. emitFunction
    // records them here (its function-root scope is created later, in emitBlockScoped,
    // which drains this); emitMethodOrCtorBody records them in its root scope directly.
    std::vector<LiveLocal> _pendingParamDtors;
    std::string        _currentReturnCType = "void";  // for return-temp
    bool               _returnIsPlace = false;         // emitting a `ref T operator[]` body: `return e` -> `return &(place)`
    std::string        _matchTargetCType;              // result C type of a value-producing `match` (set by the liftable site)
    std::string        _variantTargetType;             // target union instance for a generic-variant construction (Optional<int32>)
    int                _tempCounter = 0;
    // Temp-hoist buffer. An inline constructor in argument position materializes into an
    // ordinary local ("Cls __tmp; Cls__ctor(&__tmp, …);") pushed here and flushed by the enclosing
    // leaf statement BEFORE its own line — pure ISO C, no GNU statement-expression. `_hoistOK` gates
    // hoisting to the wired statement sites (expression-stmt / return / local-init); elsewhere an
    // inline ctor cleanly falls back to the existing rejection rather than emit a dangling temp.
    std::vector<std::string> _hoisted;
    bool                     _hoistOK = false;
    void flushHoisted(int depth);
    // Emit an if/while/for condition with value-producing constructs allowed (they hoist a temp);
    // any hoisted temps are left in `_hoisted` for the caller to flush (empty => the fast path).
    std::string emitCondition(SharedExpression cond);
    int                _curLine = 0;                   // last source line seen (conditional-drop diagnostics)
    bool               _inUnsafe = false;             // inside an `unsafe { }` block
    bool               _inCtor   = false;             // emitting a ctor (const fields writable here)
    bool               _inStaticMethod = false;        // emitting a `static` method body (no `self`/`this`)

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Pre-pass
    void collectSignatures(SharedCompilationUnit unit);
    void collectInterfaces(SharedCompilationUnit unit);
    void collectEnums(SharedCompilationUnit unit);
    ClassInfo buildVariantClassInfo(EnumDeclarationNode* ed, const std::string& name);   // tagged-union ClassInfo
    void emitEnum(EnumInfo& ei);
    bool isEnum(const std::string& name) const { return _enums.count(name) != 0; }
    void collectClasses(SharedCompilationUnit unit);

    // Collections: discover used Coll<T> instantiations, register a synthetic
    // ClassInfo + CollectionInfo for each, and emit the C-template macro lines.
    void collectCollections(SharedCompilationUnit unit);
    void scanStmtForCollections(SharedStatement s);
    void scanExprForCollections(SharedExpression e);
    void scanTypeForCollections(SharedIdentifier t);
    bool isCollectionType(SharedIdentifier t) const;
    std::string mangleElem(SharedIdentifier elem);
    void registerCollection(SharedIdentifier collType);
    void registerFixed(SharedIdentifier fixedType);   // Fixed<T,N> — the const-generic value array
    // Const generics: the compile-time integer value of a const argument/param expression (an
    // integer literal, or a const-param identifier bound in the current instantiation via _constSubst).
    bool constValue(SharedExpression e, int64_t& out);   // returns false if not a resolvable const int
    bool constArgN(SharedIdentifier arg, int64_t& out);  // same, for a type-arg node (literal or bound param)
    // A `Fixed<T,N>` intrinsic instance (a value-semantics collection). Its indexing/foreach reuse the
    // collection machinery, but it is carved out of ownership (never destructible, copies freely).
    bool isFixedColl(const std::string& cls) const;
    std::string emitArrayLiteral(ArrayLiteralNode* al);   // `[a,b,c]` / `[v; N]` -> a Fixed value
    void registerSmartPtr(CollKind kind, SharedIdentifier elem, const std::string& customName = "");   // Owned/Shared/Weak (customName: a library `Box<Contract>` routed here)
    void registerOptionalOfShared(SharedIdentifier elem);          // Optional<Shared<elem>> for Weak.tryUpgrade
    void registerOptionalOfName(const std::string& sharedName);    // Optional<sharedName> — a library `Rc_<elem>` partner
    void emitWeakTryUpgrade(const CollectionInfo& info);           // the tryUpgrade wrapper (builds the Optional)
    void emitSharedToWeakDowngrade(const CollectionInfo& info);    // a library `Rc<Shape>`'s downgrade() (Shared IFACE -> Weak partner)
    void registerBindable(SharedIdentifier elem);                  // BindableFunctionPtr<Sig>
    // The CSTAR_*_DEFINE macros, split: typesOnly emits the struct typedefs (`_TYPE`,
    // before class struct bodies so a class may hold one BY VALUE); else the funcs
    // (`_FUNCS`, after class prototypes where element dtors are declared).
    void emitCollectionDefs(bool typesOnly);
    // If `ea` indexes a collection, fill coll/recvExpr/idx and return true.
    bool collectionElemAccess(ElementAccessNode* ea, std::string& coll,
                              std::string& recvExpr, std::string& idx);
    // A C lvalue (a PLACE) for `e`. An indexed element is lowered through the bounds-checked
    // `NAME__at(self,i) -> T*` intrinsic (`(*NAME__at(&recv, i))`, recursing so `a[i][j]` chains),
    // or a user place-`operator[]` (`(*Class__op_index(&recv, i))`), so it can be a write target / a
    // `.field` receiver / a nested-index receiver. Anything else (a name, a member access, `this`) is
    // already an lvalue and falls through to emitExpression.
    std::string emitPlace(SharedExpression e);
    // The user-defined place-returning `operator[]` on `cls` (or a base), else null.
    MethodInfo* userIndexOp(const std::string& cls);
    // `foreach` over a user type via the iterator protocol (structural — direct monomorphized calls):
    // value = `iterator()`/`next() -> Optional<T>`; mutable (`ref`) = `iterMut()`/`hasNext()` + a
    // place-returning `next()`. A type that IS an iterator (has `next()`) is iterated directly.
    void emitForeachIterator(ForEachNode* fe, const std::string& container, int depth);
    // cType(typeNode) resolved in the type-substitution context of generic instance `inCls` (binds its
    // type args, like computeDestructible); plain cType for a non-generic class.
    std::string cTypeInInstance(const std::string& inCls, SharedIdentifier typeNode);
    // `ea` indexes a value whose class defines a place-returning `operator[]` (not a built-in collection).
    bool indexesUserOp(ElementAccessNode* ea);

    // Deep-substitute a type node under the active _typeSubst: a bare param `T` -> its (already concrete)
    // binding; a nested generic `Rc<T>` -> `Rc<Counter>` (recurse into args). Keeps a generic arg that
    // carries a type-param from being stored raw in _typeSubst (a self-referential binding that loops
    // mangleElem) — the case a mutually-recursive generic type (`Rc`↔`RcWeak`→`Optional<Rc<T>>`) hits.
    SharedIdentifier deepSubstType(SharedIdentifier t);
    // Rebind a (deep-substituted) type node so it resolves to the SAME C name in any namespace ctx:
    // a user class/enum/contract name is replaced by its use-site-resolved mangled name (qualifier
    // cleared), recursively for generic args. This lets a cross-module generic instance
    // (`std::memory::Owned<Counter>` used in another file) carry its concrete args through the
    // template's ctx without the arg's home mangle being stripped. Primitives/`Ptr`/`This` pass through.
    SharedIdentifier absolutizeType(SharedIdentifier t);
    // Generic TYPES: discover `Box<Arg>` uses, build one specialized ClassInfo each, emit under subst.
    void scanTypeForGenericTypes(SharedIdentifier t);
    void registerGenericTypeInst(const std::string& tmpl, SharedIdentifierList args);
    std::string genericTypeMangle(const std::string& tmpl, SharedIdentifierList args);  // "Pair" + "_int32" + "_string"
    void emitGenericTypeInst(const GenericTypeInst& gi, int phase);   // 0=struct typedef, 1=protos, 2=bodies

    // Generic CONTRACTS: discover `Iterator<int32>` uses, build one specialized InterfaceInfo each
    // (registered in _interfaces so the vtable-emit loop picks it up), emit under subst.
    void scanTypeForGenericContracts(SharedIdentifier t);
    void registerGenericContractInst(const std::string& tmpl, SharedIdentifierList args);
    // A contract's method-prototype list, from _interfaces (concrete/instance) or _genericContracts
    // (a template). Bound-checking matches by method NAME, which is type-parameter-independent, so it
    // reads either table through this one accessor. Returns nullptr for an unknown name.
    const std::vector<InterfaceMethod>* contractMethods(const std::string& name);
    // If `cls` implements the prelude `Deref<T>` contract, the pointee class `T` (auto-deref target);
    // "" otherwise. Nominal — the `implements Deref<T>` is the opt-in gate. Inert when no Deref is in scope.
    std::string derefTarget(const std::string& cls);
    // If `cls` implements the prelude `HeapOwner<T>` contract, the owned element `T` (so `new T(args)` can
    // placement-construct into `cls` via its `adopt(Ptr<T>)`); "" otherwise. Inert when no HeapOwner in scope.
    std::string heapOwnerTarget(const std::string& cls);
    // RAII: while emitting a generic-contract instance's vtbl / a class's impl-vtable for it, bind
    // T->concrete (and its home ctx) so the `T`-typed method sigs resolve — a no-op for a plain
    // contract. Mirrors emitGenericTypeInst's subst bind; nested so it can touch CEmitter's privates.
    struct ContractSubst {
        CEmitter& e; NsCtx savedCtx; std::map<std::string, SharedIdentifier> savedSubst; bool active;
        ContractSubst(CEmitter& e_, const InterfaceInfo& ii);
        ~ContractSubst();
    };

    // Generics: discover reachable generic-function instantiations, infer their
    // type args from call-site arguments, and emit one specialized `static` C function each.
    void collectGenericInsts(SharedCompilationUnit unit);
    void scanStmtForGenerics(SharedStatement s, std::map<std::string, SharedIdentifier>& localTys);
    void scanExprForGenerics(SharedExpression e, std::map<std::string, SharedIdentifier>& localTys);
    // The concrete type node of an argument expression ("" cases return null): literals map to
    // their builtin kind; identifiers resolve through `localTys` (declared types in scope).
    SharedIdentifier exprTypeNode(SharedExpression e, std::map<std::string, SharedIdentifier>& localTys);
    SharedIdentifier primTypeNode(int builtInVal);          // cached synthesized primitive type node
    bool isConcreteTypeArg(SharedIdentifier t);             // a primitive/class/enum/collection (not a bare type-param)
    // Unify a generic call's args against the template's params -> a deduped instantiation.
    bool inferGenericInst(FunctionDeclarationNode* tmpl, const std::string& key, SharedArgumentList args,
                          std::map<std::string, SharedIdentifier>& localTys, int line, GenericInst& out);
    // Turbofish: bind a generic function's type params directly from explicit `::<…>` args (bypassing
    // argument inference — reaches return-only generics inference can't). Arity + bounds are checked.
    bool explicitGenericInst(FunctionDeclarationNode* tmpl, const std::string& key, SharedIdentifierList typeArgs,
                             int line, GenericInst& out);
    void emitGenericInst(const GenericInst& gi, bool prototypeOnly);

    // Smart pointers (Owned, Shared). If `cls` is a smart-pointer type,
    // rewrite `cls` -> pointee T and `recvExpr` -> "(recv).ptr" (a T*) (auto-deref).
    bool derefSmartPtr(std::string& cls, std::string& recvExpr);
    bool isSmartPtrClass(const std::string& cls) const;  // Owned_T or Shared_T
    bool isBindableClass(const std::string& cls) const;  // BindableFunctionPtr_Sig
    CollKind smartKind(const std::string& cls) const;    // Owned/Shared (precond: isSmartPtrClass)
    bool isSmartPtrExpr(SharedExpression e);             // e's static class is a smart pointer
    bool isSmartPtrLValue(SharedExpression e);           // e is a bare identifier of smart-ptr type
    // A NAMED value you can hand off (variable / field / element / base member),
    // as opposed to a FRESH rvalue (a `new`/constructor/call result/literal). A marker
    // (`give`/`copy`) rides a named value; a fresh rvalue is consumed in place, never marked.
    bool isNamedValue(ASTNode* e);
    // A contract value BORROWS its object (a fat pointer), so it's second-class —
    // it can't be stored beyond the call that made it (it would dangle). Reject a bare
    // contract in a stored/returned position; own the object instead (`Shared<I>`).
    // `whereClause` completes "it can't be ___" (e.g. "stored in a field").
    void rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line);
    std::string smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem = false);  // null the dtor's guard field
    // A move-only VALUE — a destructible class value that isn't a smart-ptr/collection/
    // extern struct. It MOVES on hand-off (its dtor is suppressed) and is never silently copied.
    bool isMoveOnlyValue(const std::string& cls) const;
    // A move-only VALUE that opted into `Copyable` (a public nullary `copy` returning its
    // own type). Its presence makes the give/copy marker mandatory: bare hand-off = error, `copy`
    // deep-copies via copy(), `give` moves.
    bool isCopyable(const std::string& cls) const;
    bool satisfiesBound(const std::string& t, const std::string& bound) const;   // does concrete C-type `t` satisfy contract `bound`? (Copyable: value/primitive yes, resource iff it implements it)
    void markMoved(const std::string& cVar);                // state -> Moved
    void checkNotMoved(const std::string& cVar, int line);  // reject a use of a moved local
    // The source of a move hand-off: a bare move-only local -> its name (caller marks it moved);
    // a field/element/base member -> reject (moving out would leave the owner moved-from).
    std::string moveOnlySource(SharedExpression e, int line);
    // Dispatch `recv.method(args)` on a smart-pointer receiver: an intrinsic
    // (lock/expired/valid) on the pointer itself, else auto-deref to the pointee.
    std::string emitSmartPtrCall(const std::string& cls, const std::string& recvExpr,
                                 const std::string& method, SharedArgumentList args, int srcLine);

    void linkBases();
    void buildVtables();
    void computeDestructible();
    std::vector<ParamSig> paramSigsOf(SharedParameterList params);
    static bool isExtern(FunctionDeclarationNode* fn);

    // Inheritance/vtable resolution
    std::vector<ClassInfo*> topoOrderClasses();
    // All struct-body types (normal classes + generic instances + tagged unions) ordered so
    // every BY-VALUE dependency precedes its holder (base-before-derived AND held-value-before-holder).
    // A by-value cycle is an infinite-size type (reported). Collections/extern structs are excluded.
    std::vector<ClassInfo*> unifiedStructOrder();
    ClassInfo* findFieldOwner(ClassInfo* ci, const std::string& field);   // class declaring `field`
    MethodInfo* findMethod(ClassInfo* ci, const std::string& name, ClassInfo** owner);
    // Does `ci` structurally satisfy contract `contract` (have all its methods, public)?
    bool classSatisfiesBound(ClassInfo* ci, const std::string& contract);
    // Does `ci` NOMINALLY `implements` a contract whose template is `tmpl` (any instantiation)? Checks the
    // recorded `interfaces` list (a plain name == tmpl, or a generic instance whose `templateKey` == tmpl).
    bool implementsContractTemplate(ClassInfo* ci, const std::string& tmpl);
    // Verify a concrete type arg satisfies each contract bound on a type parameter (else diagnose).
    void checkBounds(const std::string& paramName, SharedIdentifier concreteArg,
                     SharedIdentifierList bounds, int line);
    std::string basePathTo(ClassInfo* from, ClassInfo* to);   // "__base." chain from `from` down to `to`
    std::string vptrPrefix(ClassInfo* ci);                    // "__base." * (hops to vtableRoot)
    void emitVtableType(ClassInfo& ci);                       // only when ci is its own vtableRoot
    void emitVtableInstance(ClassInfo& ci);                   // for every class with hasVtable
    std::string vtableSlotSig(const VSlot& s);                // "(Owner* self, T a, ...)"

    // Contracts
    bool isInterface(const std::string& name) const { return _interfaces.count(name) != 0; }
    std::string ifaceSlotSig(SharedParameterList params);     // "(void* self, T a, ...)"
    void emitInterfaceTypes(InterfaceInfo& ii);               // vtbl struct + fat-pointer struct
    void emitClassInterfaceVtables(ClassInfo& ci);            // the C__as_I instances
    // (I){ (void*)&<obj>, &<C>__as_I } — wrap a concrete lvalue as an interface value
    std::string fatPointer(const std::string& iface, const std::string& concrete, const std::string& addrExpr);
    std::string emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                      const std::string& method, SharedArgumentList args, int srcLine,
                                      const std::string& recvCType = "");

    // Declarations / top level
    bool paramByRef(FunctionParameterNode* p);
    std::string paramListC(SharedParameterList params, const char* selfType);
    // `nameOverride`: emit under a supplied mangled name instead of the declared one
    // (used for generic instantiations, whose C name carries the concrete type args).
    void emitFunctionPrototype(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);
    void emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);

    // Classes
    bool isClass(const std::string& name) const { return _classes.count(name) != 0; }
    bool isBaseOf(const std::string& base, const std::string& derived) const;   // base in derived's chain
    std::string ptrElemType(SharedExpression e);   // if `e` is a raw `this.field[i]` where field is Ptr<T>, the element C-type; else ""
    std::string exprClass(SharedExpression e);          // class name of expr, "" if unknown/primitive
    void emitStruct(ClassInfo& ci);
    void emitVariantStruct(ClassInfo& ci);   // tag + union layout of a discriminated-union enum
    void emitClassPrototypes(ClassInfo& ci);
    void emitClassDefinitions(ClassInfo& ci);
    void emitMethodOrCtorBody(const std::string& cName, const char* retType,
                              SharedParameterList params, SharedBlock body,
                              ClassInfo& owner, bool isCtor, bool isConstMethod = false,
                              bool isStatic = false);
    std::string emitMemberAccess(MemberAccessNode* ma);
    std::string emitMethodCall(InvocationNode* call, MemberAccessNode* recv);
    // Dispatch a call on a receiver of static class `clsName`, given the C pointer
    // expression `recvPtr` (e.g. "self" or "&(c)"): virtual -> via __vptr; else direct.
    std::string emitDispatch(const std::string& clsName, const std::string& recvPtr,
                             const std::string& method, SharedArgumentList args, int srcLine);
    // `new T(args)` reordered against the ctor signature -> "T__ctor(&dst, a0, ...)"
    std::string emitCtorCall(const std::string& cVar, ClassInfo& ci, SharedArgumentList args, int srcLine);

    // Statements
    void emitStatement(SharedStatement stmt, int depth);
    void emitBlock(BlockNode* block, int depth);
    void emitBlockScoped(BlockNode* block, int depth, bool loopBoundary, bool functionRoot);
    void emitBody(SharedStatement stmt, int depth, bool loopBoundary);  // brace-wrapped control-flow body
    std::string inlineStatement(SharedStatement stmt);         // for-clause form (no ; / newline)
    std::string emitForClause(SharedStatementList list);       // comma-joined inlineStatements

    // RAII cleanup
    void emitScopeCleanup(const Scope& s, int depth);          // reverse-order dtors for one scope
    void emitUnwindToLoop(int depth);                          // break/continue: innermost..loop boundary
    void emitUnwindAll(int depth);                             // return: innermost..function root
    void recordDestructibleLocal(const std::string& cVar, const std::string& className);
    static bool stmtIsJump(SharedStatement s);                 // direct return/break/continue
    static bool bodyDiverges(SharedStatement s);               // body ends in return/break/continue
    void emitDtorDefinition(ClassInfo& ci);

    // Expressions -> C expression text
    std::string emitExpression(SharedExpression expr);
    std::string emitInvocation(InvocationNode* call);
    std::string emitVariantConstruction(ClassInfo& ci, const std::string& variant,
                                        SharedArgumentList args, int srcLine);
    // If `e` is an inline construction for exactly `targetCType` in a hoist-enabled context,
    // materialize a preceding temp (ISO C, no `({…})`) and return its name; else "". Generalizes the
    // argument-position lowering to any value site (return, variant payload, …).
    std::string tryHoistInlineCtor(SharedExpression e, const std::string& targetCType, int srcLine);
    std::string tryHoistInlineNew(SharedExpression e, const std::string& targetCType, int srcLine);
    // The variant type named by a `::` qualifier — a non-generic union directly, or a generic
    // template resolved to its target instance (`Optional` + `_variantTargetType` Optional_int32). null if none.
    ClassInfo* resolveVariantType(const std::string& qualResolved);
    // The value-producing `match`. `emitMatch` lifts an expression-position match to a temp
    // (strict ISO C11 — no statement-expression); `emitMatchStatement` emits a statement-position
    // match (value discarded). Both build the switch via `emitMatchSwitch`.
    std::string emitMatch(MatchNode* m);
    void        emitMatchStatement(MatchNode* m, int depth);
    void        emitMatchSwitch(MatchNode* m, const std::string* resultTemp, int depth);
    // A `match` over a plain (payload-less) enum lowers to a C `switch` on the integer value,
    // with the same compile-time exhaustiveness + `_` wildcard as the tagged-union path.
    void        emitMatchPlainEnum(MatchNode* m, const std::string& enumTy, const std::string* resultTemp, int depth);
    std::string exprEnumType(SharedExpression e);       // plain-enum type name of expr, "" if not a plain enum
    // Merge per-arm move-states at a match join. A `match` is exhaustive, so a local moved on some but
    // not all reaching arms becomes MaybeMoved (rejected as an undecidable drop at scope exit) — the
    // same conditional-drop guard `if`/`else` has.
    void        mergeMatchMoveStates(const std::map<std::string, MoveState>& before,
                                     const std::vector<std::map<std::string, MoveState>>& armEnds,
                                     const std::vector<bool>& armDivs);

    // Const-correctness (deep): a const binding is immutable.
    std::set<std::string> _constLocals;                       // const local names in scope
    std::string rootBinding(SharedExpression e) const;        // the root identifier a write targets
    bool        rootIsConst(const std::string& root) const;   // const local/param/this/field
    bool        isConstFieldWrite(SharedExpression target);   // writing a const data member
    // Access control.
    Visibility  visibilityOf(SharedModifierList mods, Visibility dflt, int line);
    Visibility  fieldVisibility(const ClassInfo& ci, SharedModifierList mods, int line);   // per-field
    bool        modHas(SharedModifierList mods, const char* name);
    bool        canAccess(ClassInfo* owner, Visibility vis, const std::string& member, int line);
    void        checkFieldAccess(ClassInfo* owner, const std::string& field, int line);
    void        resolveFriends();   // resolve each class's raw friend grants to keys
    void        checkConstWrite(SharedExpression target, int srcLine);  // error if writing const
    bool        isConstReceiver(SharedExpression receiver) const;       // const-call restriction
    std::string emitFnPtrBind(const std::string& sigCName, SharedExpression init, int line);
    bool        sigMatches(const SigInfo& sig, const FuncSig& fn) const;
    // BindableFunctionPtr<Sig> — construct/promote/invoke a bindable callable.
    void        emitBindableNew(const std::string& nm, const std::string& octy,
                                ObjectCreationNode* oc, int depth);
    void        emitBindablePromote(const std::string& nm, const std::string& ty,
                                    SharedExpression init, int depth);
    std::string emitBindableInvoke(const std::string& recv, const std::string& cls,
                                   SharedArgumentList args, int line);
    // Emit `cName(leadArg, <args reordered to params>)`. leadArg "" omits self.
    std::string emitReorderedCall(const std::string& cName, const std::string& leadArg,
                                  const std::vector<ParamSig>& params, SharedArgumentList args, int srcLine);

    // Helpers
    std::string cType(SharedIdentifier type);
    std::string cFunctionName(const std::string& cstarName);   // main -> cstar_main
    std::string mangledFunctionName(FunctionDeclarationNode* fn, bool& isEntryPoint);
    std::string binaryOperator(int token);
    std::string assignmentOperator(int token);
    // Operator overloading. `operatorMangle` maps a token + arity-class (0=unary, ≥1=binary)
    // to a stable C-safe method name (`op_add`, `op_neg`, …), "" if the op has no such form.
    // `operatorParamList` synthesizes a ParameterList from an operator declarator's param1/param2 so
    // all normal method machinery (paramListC, paramSigsOf, emitMethodOrCtorBody) is reused verbatim.
    std::string operatorMangle(int opToken, int arity);
    // Type-based dispatch: the full operator name adds an operand-type suffix so one type can carry
    // several `operator*` (mat*vec vs mat*mat). `findBinaryOperator` resolves `a OP b` by operand types.
    std::string operatorName(int opToken, int arity, SharedIdentifier paramType, const std::string& owner);
    MethodInfo* findBinaryOperator(int token, const std::string& lc, const std::string& rc, ClassInfo** ownerOut);
    SharedParameterList operatorParamList(ClassOperatorDeclaratorNode* d);
    std::string emitBinaryOperator(int token, SharedExpression lhs, SharedExpression rhs, int line);   // user operand → dispatch, else raw C
    int         compoundToBinary(int token);   // PLUSEQ -> PLUS … (compound assignment on a user type)
    std::string bareCtorClass(SharedExpression e);       // the class if `e` is a bare inline ctor call, else ""
    void        rejectUnhoistableCtor(SharedExpression e);   // clean error for an inline ctor with no statement slot
    std::string hoistCtorIfInline(SharedExpression e);   // an inline ctor operand → a hoisted temp name, else ""
    std::string emitOperandByValue(SharedExpression e);  // emit an operator operand by value (hoisting an inline ctor)
    std::string emitUnaryUserOp(int opToken, SharedExpression operand, int line);   // unary/incr/decr on a user type
    std::string operatorResultClass(int opToken, int arity, SharedExpression lhs, SharedExpression rhs);  // nested-operator type
    // `&<operand>` for a method-form/unary operator's `self`. A simple lvalue is addressed
    // directly; an rvalue (a nested operator result / call) is first materialized into a hoisted temp
    // (ISO C — no statement-expressions) so chained `a + b + c` works.
    std::string addrOfOperand(SharedExpression e, const std::string& cls, int line);

    void unsupported(const char* what, int srcLine);

    // Multi-file: collect a whole program, then emit declarations (shared
    // header) and definitions (per module) separately.
    void collectProgram(const std::vector<SharedCompilationUnit>& units);
    void emitHeaderContent(const std::vector<SharedCompilationUnit>& units);  // typedefs/structs/protos/macros
    void emitModuleContent(SharedCompilationUnit unit);                       // this file's vtables + defs
};

#endif // __CSTAR_CEMIT_H__
