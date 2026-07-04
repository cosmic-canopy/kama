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
    bool        isConst = false;   // `const` param — emits `const T*` for FFI pointers (M24e)
};

struct FuncSig {
    std::string            cName;    // mangled C name (e.g. main -> cstar_main)
    std::string            retCType; // resolved C return type (M21 signature check)
    std::vector<ParamSig>  params;
};

// A function-pointer signature type (M21): a bodiless `fn ret Name(params);`.
// Lowers to `typedef ret (*cName)(paramtypes);`. FunctionPtr<Name> spells `cName`.
struct SigInfo {
    std::string            cName;       // typedef name (namespace-mangled)
    std::string            retCType;    // resolved C return type (for the typedef)
    std::vector<ParamSig>  params;      // names (named-arg invoke) + C types (className)
};

// ---- Class model (M4) -----------------------------------------------------

// Member access level (M25). Default Private; `pod` fields are forced Public.
enum class Visibility { Private, Protected, Public };

struct FieldInfo {
    std::string      name;
    SharedIdentifier type;
    SharedExpression initializer;    // optional; applied in the constructor
    Visibility       visibility = Visibility::Private;   // M25
};

// M28a — one case of a discriminated-union `enum` (tagged union). `name` is the variant, `payload`
// its named fields (empty = no payload); the tag value is the declaration index. Stored on ClassInfo
// (a payload/generic enum is backed by a ClassInfo, reusing monomorphization + RAII + move analysis).
struct VariantCase {
    std::string            name;      // "Circle"
    std::vector<FieldInfo> payload;   // named fields (name + type); empty for a no-payload variant
};

// M26h — a type's declared ownership kind. `Value` owns nothing (copies); `Resource` owns/has
// identity (moves, RAII-dropped); `Contract` is the interface path (handled via InterfaceInfo).
// `Legacy` = an old `class`/`pod class` (no `type` marker) — behaves exactly as before M26h until
// the fixtures migrate (h-3), at which point `Legacy` is retired.
enum class TypeKind { Legacy, Value, Resource, Contract };

struct MethodInfo {
    std::string                  cName;   // Class__method (declaring class)
    SharedIdentifier             returnType;
    std::vector<ParamSig>        params;
    ClassMethodDeclarationNode*  node = nullptr;   // for body emission (null for intrinsics)
    bool                         isVirtual  = false;  // virtual/override/abstract
    bool                         isOverride = false;
    bool                         isAbstract = false;  // null body
    bool                         isIntrinsic = false; // collection op: body is in cstar_runtime.h, not AST
    bool                         isConst = false;     // `const fn …` — non-mutating (M24b)
    Visibility                   visibility = Visibility::Private;   // M25
    bool                         isFinal = false;     // `final fn` — seals a virtual slot (M25)
    bool                         isStatic = false;    // `static fn` — no implicit `self` (M31a); called `Type::m(...)`
    // M31b — operator overloads register as methods under a synthetic name (`op_add`, `op_neg`, …).
    // They are NOT ClassMethodDeclarationNode, so `node` stays null: emit from `opDecl` instead.
    bool                         isOperator = false;
    int                          arity = 0;           // 0 = unary-on-this, 1 = binary method (`this`+rhs), 2 = binary free
    ClassOperatorDeclarationNode* opDecl = nullptr;   // the operator decl (body/params) when isOperator
};

// A built-in generic collection / smart-pointer kind (M9/M10). Backed by a C
// runtime template. Owned<T> (M10) is a 4th kind: a unique heap-owning pointer.
enum class CollKind { Array, List, String, Owned, Shared, Weak, Bindable };

// Per-file namespace context (M14). A file with `namespace X;` is public (scope
// = mangled X); a file without one is private (scope = "_F<idx>"). Bare names
// resolve to the file's own scope, then its `using`s — never another file's
// private symbols (private-by-default).
struct NsCtx {
    std::string scope;        // mangle prefix: "Graphics" or "_F3"
    bool        isPublic = false;
    std::vector<std::string> usings;                  // imported public namespaces (mangled)
    std::map<std::string, std::string> aliases;       // alias -> mangled namespace
};

// M25c — a resolved `friend` grant on the OWNING class. `accessor` is a resolved key:
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
    std::string                       name;       // struct name (== cstar class name in M4)
    TypeKind                          kind = TypeKind::Legacy;   // M26h: value/resource (contract → InterfaceInfo)
    std::vector<FieldInfo>            fields;      // declaration order
    std::set<std::string>            fieldNames;
    std::set<std::string>            constFields;   // `const` data members — write-once in the ctor (M24d)
    std::map<std::string, MethodInfo> methods;    // by cstar method name
    bool                              hasCtor = false;
    bool                              synthCtor = false;  // M19: default ctor synthesized (vtable init)
    std::vector<ParamSig>             ctorParams;
    ClassConstructorDeclarationNode*  ctorNode = nullptr;
    ClassDeclarationNode*             node    = nullptr;

    // RAII (M5)
    bool                              hasDtor = false;   // declares its own ~dtor
    ClassDestructorDeclarationNode*   dtorNode = nullptr;
    bool                              destructible = false; // own dtor OR a destructible field (transitive)
    // M26f-4: opted into the `Copyable` contract — declares a public nullary `copy` returning
    // its own type. Makes the give/copy marker MANDATORY on a `resource` value ("scream when
    // ambiguous"). Structural for now; the explicit `: Copyable` form lands with M26h/M27.
    bool                              copyable = false;

    // Inheritance + virtual dispatch (M6)
    std::string                       baseName;        // "" if no base
    ClassInfo*                        base = nullptr;  // resolved by linkBases()
    bool                              isAbstractClass = false;
    // M26h — `virtual`/`abstract`/`final` are extensibility qualifiers on a `resource`.
    bool                              isVirtualClass = false; // `virtual resource` — extensible base
    bool                              isFinalClass = false;   // `final class` — sealed leaf
    Visibility                        ctorVisibility = Visibility::Public;   // synth/default ctor is public; an EXPLICIT ctor defaults private
    std::vector<RawFriendGrant>       friendGrantsRaw;        // M25c — captured at collection
    std::vector<FriendGrant>          friendGrants;           // M25c — resolved (resolveFriends)
    bool                              hasVtable = false;     // this or an ancestor has a virtual
    std::string                       vtableRoot;            // class owning the __vptr member
    std::map<std::string,std::string> slotImpl;             // virtual slot name -> impl cName (most-derived here)

    // Interfaces (M6b)
    std::vector<std::string>          interfaces;            // implemented interface names

    // Collections (M9): a monomorphized Coll<T> is a synthetic ClassInfo whose
    // method bodies come from a C-template macro (not cstar AST).
    bool                              isCollection = false;
    CollKind                          collKind = CollKind::Array;
    std::string                       collElemClass;         // element class name ("" if primitive)
    bool                              isGenericInst = false; // M27b: a specialized generic-type instance (Box_int32)

    // Tagged unions (M28a): a payload/generic `enum` is backed by a ClassInfo whose layout is a
    // discriminant tag + a union of per-variant payloads (not the flat `fields`). `variants` drives
    // struct + per-variant dtor emission; `tagCType` pins the tag width (`: IntType`), "" = Name_Tag.
    bool                              isVariant = false;
    std::vector<VariantCase>          variants;
    std::string                       tagCType;              // "" -> the synthesized `Name_Tag` enum

    // Namespaces (M14): the declaring file's scope/usings, for resolving this
    // type's field/base/method references during header emission.
    std::string                       scope;                 // mangle prefix ("" for collections)
    std::vector<std::string>          usings;

    // FFI (M16): an `extern class` is an external C struct — cstar uses its
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
    bool         elemCopyable = false;   // M26f-5: element is a `Copyable` resource -> deep-copy each
    bool         elemIsInterface = false;   // M26g: owned-interface smart ptr (fat {obj, vtbl} element)
};

// An interface (M6b) / `contract` (M26h): a set of method prototypes, lowered to a vtable struct
// type + a fat-pointer value type. Implemented by classes via a C__as_I vtable. The method's
// return type + params are stored directly (not a node pointer) so it can be built from either an
// `interface` (FunctionDeclarationNode) or a `type contract` (ClassMethodDeclarationNode).
struct InterfaceMethod { std::string name; SharedIdentifier returnType; SharedParameterList params; };
struct InterfaceInfo {
    std::string                  name;
    std::vector<InterfaceMethod> methods;
    std::string                  scope;        // M14
    std::vector<std::string>     usings;
};

// An enum (M7): lowered to a C `enum` with members mangled `Enum_Member`.
struct EnumMember { std::string name; SharedExpression value; };  // value optional
struct EnumInfo   {
    std::string name;
    std::vector<EnumMember> members;
    std::string scope;                          // M14
    std::vector<std::string> usings;
    std::string underlyingCType;                // M28a: `enum E : IntType` -> fixed-width int C type; "" = plain `enum`
};

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // M28c: the implicit prelude (library sum types Optional/Result). Collected before user code
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
    SharedCompilationUnit _preludeUnit;   // M28c: implicit prelude (Optional/Result), collect-only
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    std::map<std::string, FuncSig> _funcs;   // cstar function name -> signature
    std::map<std::string, SigInfo> _sigs;    // M21: function-pointer signature types
    bool isSigType(const std::string& name) const { return _sigs.count(name) != 0; }
    std::set<std::string> _refParams;        // by-ref params of the function being emitted

    std::map<std::string, ClassInfo>   _classes;     // class name -> info
    std::map<std::string, std::string> _localTypes;  // local/param -> class name ("" if primitive)
    std::map<std::string, std::string> _localCTypes; // M29c: local -> full C type (incl. primitives) — the
                                                     // target type for a value-producing RHS at an assignment
    // M26f-2: compile-time move analysis for `resource` (destructible) VALUES. Per-local
    // move-state, consulted by emitScopeCleanup (skip a moved local's dtor) and the hand-off
    // sites (reject use-after-move). A value moved on some-but-not-all paths that is live at
    // scope exit is *rejected* (conditional-drop) — zero runtime drop-flags by construction.
    enum class MoveState { NotMoved, MaybeMoved, Moved };
    std::map<std::string, MoveState> _moveState;   // move-only local/param cVar -> state
    ClassInfo*                         _currentClass = nullptr;  // when emitting a method/ctor
    std::string                        _currentFunc;             // C-name of the function/method being emitted (M25c friend match)
    std::string                        _thisType;                // M27c: C name `This` resolves to (the class being emitted, or the interface type inside its vtbl slot)

    // Virtual dispatch (M6): per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots

    std::map<std::string, InterfaceInfo> _interfaces;        // interface name -> info (M6b)
    std::map<std::string, EnumInfo>      _enums;             // enum name -> info (M7)
    std::map<std::string, CollectionInfo> _collections;      // cName -> info (M9)
    std::vector<std::string>              _collectionOrder;  // registration order (inner-first; a
                                                             // collection's dtor calls its element's,
                                                             // so the element must emit first)

    // M27a — generic functions (monomorphization). A generic template is registered by its
    // mangled cName; each reachable (template, concrete-type-args) pair is a synthetic
    // instantiation emitted as a `static` C function. Call-site inference runs once at
    // discovery and records the target per call node, so emission is a lookup, not re-inference.
    struct GenericInst { std::string templateKey; std::string mangledName; std::vector<SharedIdentifier> typeArgs; };
    std::map<std::string, FunctionDeclarationNode*> _generics;      // template cName -> node
    std::map<std::string, NsCtx>                    _genericCtx;    // template cName -> home namespace ctx
    std::map<std::string, GenericInst>              _genericInsts;  // mangled name -> instantiation (dedup)
    std::map<const InvocationNode*, std::string>    _callInst;      // generic call site -> instantiation mangled name
    std::map<std::string, SharedIdentifier>         _typeSubst;     // type-param name -> concrete (only while emitting an instantiation)
    std::map<int, SharedIdentifier>                 _primTypeCache; // synthesized primitive type nodes (for inference)
    std::shared_ptr<CodeGenContext>                 _synthCtx;      // context for synthesizing those nodes

    // M27b — generic TYPES (`type value Box<T>`). The TEMPLATE is kept OUT of _classes (so the normal
    // class loops never see it); each reachable `Box<Arg>` becomes a synthetic specialized ClassInfo
    // (`Box_int32`, isGenericInst=true) registered in _classes and emitted under _typeSubst.
    struct GenericTypeInst { std::string templateKey; std::string mangledName; std::vector<SharedIdentifier> typeArgs; };
    std::map<std::string, ClassInfo>          _genericTypes;        // template name -> ClassInfo shape (NOT in _classes)
    std::map<std::string, std::vector<std::string>> _genericTypeParams;  // template name -> type-param names [A, B]
    std::map<std::string, SharedBoundsList>   _genericTypeBounds;   // M27c: template name -> per-param contract bounds
    std::map<std::string, NsCtx>              _genericTypeCtx;      // template name -> home namespace ctx
    std::map<std::string, NsCtx>              _genericTypeInstCtx;  // M28c: instance -> registration (use-site) ctx, so a
                                                                    // prelude template's user-type args resolve at emit time
    std::map<std::string, GenericTypeInst>    _genericTypeInsts;    // mangled name -> instantiation (dedup)
    std::map<std::string, std::string>        _genericTypeInstOf;   // mangled name -> template name (construction)
    std::vector<std::string>                  _genericTypeInstOrder;// registration order (inner-first; struct-typedef emit)
    bool                                      _emitStaticClass = false;  // prefix `static` on specialized class fns (header ODR)

    // Namespaces (M14): current-file scope + the helpers that mangle/resolve names.
    NsCtx _nsCtx;
    std::set<std::string> _namespaces;   // registered public namespaces (mangled)
    std::set<std::string> _externNames;  // FFI (M16): literal C names of extern structs
    void emitIncludes(const std::vector<SharedCompilationUnit>& units);  // FFI #include directives
    std::map<const CompilationUnit*, NsCtx> _unitCtx;   // each file's context (for emit)
    NsCtx ctxOf(SharedCompilationUnit unit, int fileIndex);      // build a file's NsCtx
    static std::string qualifiedName(SharedIdentifier id);       // dotted "a.b.c" from value+qualifier
    static std::string mangleNs(const std::string& ns);          // "a.b" -> "a__b"
    std::string qualify(const std::string& name) const;          // scope-prefix a declared name
    std::string resolveUserName(const std::string& value, SharedStringList qualifier);  // class/enum/iface ref
    std::string resolveFunc(const std::string& name, SharedStringList qualifier);       // function ref
    bool isNamespace(const std::string& name) const;             // a known public namespace (or alias)

    // RAII scope stack (M5): live destructible locals per lexical scope.
    struct LiveLocal { std::string cVar; std::string className; };
    struct Scope { std::vector<LiveLocal> locals; bool isLoopBoundary = false; bool isFunctionRoot = false; };
    std::vector<Scope> _scopes;
    // M26d: by-value smart-ptr params the callee owns — dropped at fn-end. emitFunction
    // records them here (its function-root scope is created later, in emitBlockScoped,
    // which drains this); emitMethodOrCtorBody records them in its root scope directly.
    std::vector<LiveLocal> _pendingParamDtors;
    std::string        _currentReturnCType = "void";  // for return-temp
    std::string        _matchTargetCType;              // M28b: result C type of a value-producing `match` (set by the liftable site)
    std::string        _variantTargetType;             // M28c: target union instance for a generic-variant construction (Optional<int32>)
    int                _tempCounter = 0;
    // M26i: temp-hoist buffer. An inline constructor in argument position materializes into an
    // ordinary local ("Cls __tmp; Cls__ctor(&__tmp, …);") pushed here and flushed by the enclosing
    // leaf statement BEFORE its own line — pure ISO C, no GNU statement-expression. `_hoistOK` gates
    // hoisting to the wired statement sites (expression-stmt / return / local-init); elsewhere an
    // inline ctor cleanly falls back to the existing rejection rather than emit a dangling temp.
    std::vector<std::string> _hoisted;
    bool                     _hoistOK = false;
    void flushHoisted(int depth);
    int                _curLine = 0;                   // M26f-2: last source line seen (conditional-drop diagnostics)
    bool               _inUnsafe = false;             // M17: inside an `unsafe { }` block
    bool               _inCtor   = false;             // M24d: emitting a ctor (const fields writable here)
    bool               _inStaticMethod = false;        // M31a: emitting a `static` method body (no `self`/`this`)

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Pre-pass
    void collectSignatures(SharedCompilationUnit unit);
    void collectInterfaces(SharedCompilationUnit unit);
    void collectEnums(SharedCompilationUnit unit);
    ClassInfo buildVariantClassInfo(EnumDeclarationNode* ed, const std::string& name);   // M28a: tagged-union ClassInfo
    void emitEnum(EnumInfo& ei);
    bool isEnum(const std::string& name) const { return _enums.count(name) != 0; }
    void collectClasses(SharedCompilationUnit unit);

    // Collections (M9): discover used Coll<T> instantiations, register a synthetic
    // ClassInfo + CollectionInfo for each, and emit the C-template macro lines.
    void collectCollections(SharedCompilationUnit unit);
    void scanStmtForCollections(SharedStatement s);
    void scanExprForCollections(SharedExpression e);
    void scanTypeForCollections(SharedIdentifier t);
    bool isCollectionType(SharedIdentifier t) const;
    std::string mangleElem(SharedIdentifier elem);
    void registerCollection(SharedIdentifier collType);
    void registerSmartPtr(CollKind kind, SharedIdentifier elem);   // Owned/Shared/Weak (M10-12)
    void registerOptionalOfShared(SharedIdentifier elem);          // M28d: Optional<Shared<elem>> for Weak.tryUpgrade
    void emitWeakTryUpgrade(const CollectionInfo& info);           // M28d: the tryUpgrade wrapper (builds the Optional)
    void registerBindable(SharedIdentifier elem);                  // BindableFunctionPtr<Sig> (M22)
    // The CSTAR_*_DEFINE macros, split: typesOnly emits the struct typedefs (`_TYPE`,
    // before class struct bodies so a class may hold one BY VALUE); else the funcs
    // (`_FUNCS`, after class prototypes where element dtors are declared).
    void emitCollectionDefs(bool typesOnly);
    // If `ea` indexes a collection, fill coll/recvExpr/idx and return true.
    bool collectionElemAccess(ElementAccessNode* ea, std::string& coll,
                              std::string& recvExpr, std::string& idx);

    // Generic TYPES (M27b): discover `Box<Arg>` uses, build one specialized ClassInfo each, emit under subst.
    void scanTypeForGenericTypes(SharedIdentifier t);
    void registerGenericTypeInst(const std::string& tmpl, SharedIdentifierList args);
    std::string genericTypeMangle(const std::string& tmpl, SharedIdentifierList args);  // "Pair" + "_int32" + "_string"
    void emitGenericTypeInst(const GenericTypeInst& gi, int phase);   // 0=struct typedef, 1=protos, 2=bodies

    // Generics (M27a): discover reachable generic-function instantiations, infer their
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
    void emitGenericInst(const GenericInst& gi, bool prototypeOnly);

    // Smart pointers (M10 Owned, M11 Shared). If `cls` is a smart-pointer type,
    // rewrite `cls` -> pointee T and `recvExpr` -> "(recv).ptr" (a T*) (auto-deref).
    bool derefSmartPtr(std::string& cls, std::string& recvExpr);
    bool isSmartPtrClass(const std::string& cls) const;  // Owned_T or Shared_T
    bool isBindableClass(const std::string& cls) const;  // BindableFunctionPtr_Sig (M22)
    CollKind smartKind(const std::string& cls) const;    // Owned/Shared (precond: isSmartPtrClass)
    bool isSmartPtrExpr(SharedExpression e);             // e's static class is a smart pointer
    bool isSmartPtrLValue(SharedExpression e);           // e is a bare identifier of smart-ptr type
    // M26c/d: a NAMED value you can hand off (variable / field / element / base member),
    // as opposed to a FRESH rvalue (a `new`/constructor/call result/literal). A marker
    // (`give`/`copy`) rides a named value; a fresh rvalue is consumed in place, never marked.
    bool isNamedValue(ASTNode* e);
    // M26e: an interface value BORROWS its object (a fat pointer), so it's second-class —
    // it can't be stored beyond the call that made it (it would dangle). Reject a bare
    // interface in a stored/returned position; own the object instead (`Shared<I>`, M26f).
    // `whereClause` completes "it can't be ___" (e.g. "stored in a field").
    void rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line);
    std::string smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem = false);  // null the dtor's guard field
    // M26f-2: a move-only VALUE — a destructible class value that isn't a smart-ptr/collection/
    // extern struct. It MOVES on hand-off (its dtor is suppressed) and is never silently copied.
    bool isMoveOnlyValue(const std::string& cls) const;
    // M26f-4: a move-only VALUE that opted into `Copyable` (a public nullary `copy` returning its
    // own type). Its presence makes the give/copy marker mandatory: bare hand-off = error, `copy`
    // deep-copies via copy(), `give` moves.
    bool isCopyable(const std::string& cls) const;
    void markMoved(const std::string& cVar);                // state -> Moved (loop-guard added in Increment 3)
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

    // Inheritance/vtable resolution (M6)
    std::vector<ClassInfo*> topoOrderClasses();
    // M30a: all struct-body types (normal classes + generic instances + tagged unions) ordered so
    // every BY-VALUE dependency precedes its holder (base-before-derived AND held-value-before-holder).
    // A by-value cycle is an infinite-size type (reported). Collections/extern structs are excluded.
    std::vector<ClassInfo*> unifiedStructOrder();
    ClassInfo* findFieldOwner(ClassInfo* ci, const std::string& field);   // class declaring `field`
    MethodInfo* findMethod(ClassInfo* ci, const std::string& name, ClassInfo** owner);
    // M27c: does `ci` structurally satisfy contract `contract` (have all its methods, public)?
    bool classSatisfiesBound(ClassInfo* ci, const std::string& contract);
    // M27c: verify a concrete type arg satisfies each contract bound on a type parameter (else diagnose).
    void checkBounds(const std::string& paramName, SharedIdentifier concreteArg,
                     SharedIdentifierList bounds, int line);
    std::string basePathTo(ClassInfo* from, ClassInfo* to);   // "__base." chain from `from` down to `to`
    std::string vptrPrefix(ClassInfo* ci);                    // "__base." * (hops to vtableRoot)
    void emitVtableType(ClassInfo& ci);                       // only when ci is its own vtableRoot
    void emitVtableInstance(ClassInfo& ci);                   // for every class with hasVtable
    std::string vtableSlotSig(const VSlot& s);                // "(Owner* self, T a, ...)"

    // Interfaces (M6b)
    bool isInterface(const std::string& name) const { return _interfaces.count(name) != 0; }
    std::string ifaceSlotSig(SharedParameterList params);     // "(void* self, T a, ...)"
    void emitInterfaceTypes(InterfaceInfo& ii);               // vtbl struct + fat-pointer struct
    void emitClassInterfaceVtables(ClassInfo& ci);            // the C__as_I instances
    // (I){ (void*)&<obj>, &<C>__as_I } — wrap a concrete lvalue as an interface value
    std::string fatPointer(const std::string& iface, const std::string& concrete, const std::string& addrExpr);
    std::string emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                      const std::string& method, SharedArgumentList args, int srcLine);

    // Declarations / top level
    bool paramByRef(FunctionParameterNode* p);
    std::string paramListC(SharedParameterList params, const char* selfType);
    // `nameOverride` (M27a): emit under a supplied mangled name instead of the declared one
    // (used for generic instantiations, whose C name carries the concrete type args).
    void emitFunctionPrototype(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);
    void emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);

    // Classes
    bool isClass(const std::string& name) const { return _classes.count(name) != 0; }
    std::string exprClass(SharedExpression e);          // class name of expr, "" if unknown/primitive
    void emitStruct(ClassInfo& ci);
    void emitVariantStruct(ClassInfo& ci);   // M28a: tag + union layout of a discriminated-union enum
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

    // RAII cleanup (M5)
    void emitScopeCleanup(const Scope& s, int depth);          // reverse-order dtors for one scope
    void emitUnwindToLoop(int depth);                          // break/continue: innermost..loop boundary
    void emitUnwindAll(int depth);                             // return: innermost..function root
    void recordDestructibleLocal(const std::string& cVar, const std::string& className);
    static bool stmtIsJump(SharedStatement s);                 // direct return/break/continue
    static bool bodyDiverges(SharedStatement s);               // M26f-2: body ends in return/break/continue
    void emitDtorDefinition(ClassInfo& ci);

    // Expressions -> C expression text
    std::string emitExpression(SharedExpression expr);
    std::string emitInvocation(InvocationNode* call);
    std::string emitVariantConstruction(ClassInfo& ci, const std::string& variant,
                                        SharedArgumentList args, int srcLine);   // M28a
    // M29b: if `e` is an inline construction for exactly `targetCType` in a hoist-enabled context,
    // materialize a preceding temp (ISO C, no `({…})`) and return its name; else "". Generalizes the
    // M26i argument-position lowering to any value site (return, variant payload, …).
    std::string tryHoistInlineCtor(SharedExpression e, const std::string& targetCType, int srcLine);
    std::string tryHoistInlineNew(SharedExpression e, const std::string& targetCType, int srcLine);
    // M28a/c: the variant type named by a `::` qualifier — a non-generic union directly, or a generic
    // template resolved to its target instance (`Optional` + `_variantTargetType` Optional_int32). null if none.
    ClassInfo* resolveVariantType(const std::string& qualResolved);
    // M28b: the value-producing `match`. `emitMatch` lifts an expression-position match to a temp
    // (strict ISO C11 — no statement-expression); `emitMatchStatement` emits a statement-position
    // match (value discarded). Both build the switch via `emitMatchSwitch`.
    std::string emitMatch(MatchNode* m);
    void        emitMatchStatement(MatchNode* m, int depth);
    void        emitMatchSwitch(MatchNode* m, const std::string* resultTemp, int depth);

    // M24a — const-correctness (deep): a const binding is immutable.
    std::set<std::string> _constLocals;                       // const local names in scope
    std::string rootBinding(SharedExpression e) const;        // the root identifier a write targets
    bool        rootIsConst(const std::string& root) const;   // const local/param/this/field
    bool        isConstFieldWrite(SharedExpression target);   // writing a const data member (M24d)
    // M25 — access control.
    Visibility  visibilityOf(SharedModifierList mods, Visibility dflt, int line);
    Visibility  fieldVisibility(const ClassInfo& ci, SharedModifierList mods, int line);   // M26h per-field
    bool        modHas(SharedModifierList mods, const char* name);
    bool        canAccess(ClassInfo* owner, Visibility vis, const std::string& member, int line);
    void        checkFieldAccess(ClassInfo* owner, const std::string& field, int line);
    void        resolveFriends();   // M25c — resolve each class's raw friend grants to keys
    void        checkConstWrite(SharedExpression target, int srcLine);  // error if writing const
    bool        isConstReceiver(SharedExpression receiver) const;       // const-call restriction (M24b)
    std::string emitFnPtrBind(const std::string& sigCName, SharedExpression init, int line);  // M21
    bool        sigMatches(const SigInfo& sig, const FuncSig& fn) const;
    // BindableFunctionPtr<Sig> (M22) — construct/promote/invoke a bindable callable.
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
    // M31b — operator overloading. `operatorMangle` maps a token + arity-class (0=unary, ≥1=binary)
    // to a stable C-safe method name (`op_add`, `op_neg`, …), "" if the op has no such form.
    // `operatorParamList` synthesizes a ParameterList from an operator declarator's param1/param2 so
    // all normal method machinery (paramListC, paramSigsOf, emitMethodOrCtorBody) is reused verbatim.
    std::string operatorMangle(int opToken, int arity);
    SharedParameterList operatorParamList(ClassOperatorDeclaratorNode* d);
    std::string emitBinaryOperator(BinaryExpressionNode* v);   // user-typed operand → operator dispatch, else raw C
    std::string emitUnaryUserOp(int opToken, SharedExpression operand, int line);   // unary/incr/decr on a user type

    void unsupported(const char* what, int srcLine);

    // Multi-file (M13): collect a whole program, then emit declarations (shared
    // header) and definitions (per module) separately.
    void collectProgram(const std::vector<SharedCompilationUnit>& units);
    void emitHeaderContent(const std::vector<SharedCompilationUnit>& units);  // typedefs/structs/protos/macros
    void emitModuleContent(SharedCompilationUnit unit);                       // this file's vtables + defs
};

#endif // __CSTAR_CEMIT_H__
