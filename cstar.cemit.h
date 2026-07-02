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
    // M25 — class kind + access. Kinds: plain (none) | pod | virtual | abstract | final.
    bool                              isPod = false;          // `pod class` — public data, no vtable/dtor
    bool                              isVirtualClass = false; // `virtual class` — extensible
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
};

// An interface (M6b): a set of method prototypes, lowered to a vtable struct
// type + a fat-pointer value type. Implemented by classes via a C__as_I vtable.
struct InterfaceMethod { std::string name; FunctionDeclarationNode* node; };
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
};

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

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
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    std::map<std::string, FuncSig> _funcs;   // cstar function name -> signature
    std::map<std::string, SigInfo> _sigs;    // M21: function-pointer signature types
    bool isSigType(const std::string& name) const { return _sigs.count(name) != 0; }
    std::set<std::string> _refParams;        // by-ref params of the function being emitted

    std::map<std::string, ClassInfo>   _classes;     // class name -> info
    std::map<std::string, std::string> _localTypes;  // local/param -> class name ("" if primitive)
    // M26f-2: compile-time move analysis for `resource` (destructible) VALUES. Per-local
    // move-state, consulted by emitScopeCleanup (skip a moved local's dtor) and the hand-off
    // sites (reject use-after-move). A value moved on some-but-not-all paths that is live at
    // scope exit is *rejected* (conditional-drop) — zero runtime drop-flags by construction.
    enum class MoveState { NotMoved, MaybeMoved, Moved };
    std::map<std::string, MoveState> _moveState;   // move-only local/param cVar -> state
    ClassInfo*                         _currentClass = nullptr;  // when emitting a method/ctor
    std::string                        _currentFunc;             // C-name of the function/method being emitted (M25c friend match)

    // Virtual dispatch (M6): per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots

    std::map<std::string, InterfaceInfo> _interfaces;        // interface name -> info (M6b)
    std::map<std::string, EnumInfo>      _enums;             // enum name -> info (M7)
    std::map<std::string, CollectionInfo> _collections;      // cName -> info (M9)

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
    int                _tempCounter = 0;
    int                _curLine = 0;                   // M26f-2: last source line seen (conditional-drop diagnostics)
    bool               _inUnsafe = false;             // M17: inside an `unsafe { }` block
    bool               _inCtor   = false;             // M24d: emitting a ctor (const fields writable here)

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Pre-pass
    void collectSignatures(SharedCompilationUnit unit);
    void collectInterfaces(SharedCompilationUnit unit);
    void collectEnums(SharedCompilationUnit unit);
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
    void registerBindable(SharedIdentifier elem);                  // BindableFunctionPtr<Sig> (M22)
    // The CSTAR_*_DEFINE macros, split: typesOnly emits the struct typedefs (`_TYPE`,
    // before class struct bodies so a class may hold one BY VALUE); else the funcs
    // (`_FUNCS`, after class prototypes where element dtors are declared).
    void emitCollectionDefs(bool typesOnly);
    // If `ea` indexes a collection, fill coll/recvExpr/idx and return true.
    bool collectionElemAccess(ElementAccessNode* ea, std::string& coll,
                              std::string& recvExpr, std::string& idx);

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
    static bool isNamedValue(ASTNode* e);
    // M26e: an interface value BORROWS its object (a fat pointer), so it's second-class —
    // it can't be stored beyond the call that made it (it would dangle). Reject a bare
    // interface in a stored/returned position; own the object instead (`Shared<I>`, M26f).
    // `whereClause` completes "it can't be ___" (e.g. "stored in a field").
    void rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line);
    std::string smartPtrInvalidate(const std::string& expr, CollKind kind);  // null the dtor's guard field
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
    ClassInfo* findFieldOwner(ClassInfo* ci, const std::string& field);   // class declaring `field`
    MethodInfo* findMethod(ClassInfo* ci, const std::string& name, ClassInfo** owner);
    std::string basePathTo(ClassInfo* from, ClassInfo* to);   // "__base." chain from `from` down to `to`
    std::string vptrPrefix(ClassInfo* ci);                    // "__base." * (hops to vtableRoot)
    void emitVtableType(ClassInfo& ci);                       // only when ci is its own vtableRoot
    void emitVtableInstance(ClassInfo& ci);                   // for every class with hasVtable
    std::string vtableSlotSig(const VSlot& s);                // "(Owner* self, T a, ...)"

    // Interfaces (M6b)
    bool isInterface(const std::string& name) const { return _interfaces.count(name) != 0; }
    std::string ifaceSlotSig(FunctionDeclarationNode* m);     // "(void* self, T a, ...)"
    void emitInterfaceTypes(InterfaceInfo& ii);               // vtbl struct + fat-pointer struct
    void emitClassInterfaceVtables(ClassInfo& ci);            // the C__as_I instances
    // (I){ (void*)&<obj>, &<C>__as_I } — wrap a concrete lvalue as an interface value
    std::string fatPointer(const std::string& iface, const std::string& concrete, const std::string& addrExpr);
    std::string emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                      const std::string& method, SharedArgumentList args, int srcLine);

    // Declarations / top level
    bool paramByRef(FunctionParameterNode* p);
    std::string paramListC(SharedParameterList params, const char* selfType);
    void emitFunctionPrototype(FunctionDeclarationNode* fn);
    void emitFunction(FunctionDeclarationNode* fn);

    // Classes
    bool isClass(const std::string& name) const { return _classes.count(name) != 0; }
    std::string exprClass(SharedExpression e);          // class name of expr, "" if unknown/primitive
    void emitStruct(ClassInfo& ci);
    void emitClassPrototypes(ClassInfo& ci);
    void emitClassDefinitions(ClassInfo& ci);
    void emitMethodOrCtorBody(const std::string& cName, const char* retType,
                              SharedParameterList params, SharedBlock body,
                              ClassInfo& owner, bool isCtor, bool isConstMethod = false);
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
    // M24a — const-correctness (deep): a const binding is immutable.
    std::set<std::string> _constLocals;                       // const local names in scope
    std::string rootBinding(SharedExpression e) const;        // the root identifier a write targets
    bool        rootIsConst(const std::string& root) const;   // const local/param/this/field
    bool        isConstFieldWrite(SharedExpression target);   // writing a const data member (M24d)
    // M25 — access control.
    Visibility  visibilityOf(SharedModifierList mods, Visibility dflt, int line);
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

    void unsupported(const char* what, int srcLine);

    // Multi-file (M13): collect a whole program, then emit declarations (shared
    // header) and definitions (per module) separately.
    void collectProgram(const std::vector<SharedCompilationUnit>& units);
    void emitHeaderContent(const std::vector<SharedCompilationUnit>& units);  // typedefs/structs/protos/macros
    void emitModuleContent(SharedCompilationUnit unit);                       // this file's vtables + defs
};

#endif // __CSTAR_CEMIT_H__
