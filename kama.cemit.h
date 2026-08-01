#ifndef __KAMA_CEMIT_H__
#define __KAMA_CEMIT_H__

// C-emitting backend. An external visitor over the AST (dispatch via
// dynamic_cast for now) that writes portable C. Kept entirely out of the AST
// headers so emission can evolve without recompiling the world.

#include <ostream>
#include <sstream>     // analysis-mode throwaway sink (analyze() — the LSP query path)
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>   // std::find (contract-implementor lookups in the graph-node closure)
#include <cstdint>     // fixed-width ints — not transitive on all libcs (e.g. Windows UCRT)
#include "kama.forward.h"
#include "kama.diagnostic.h"   // structured Diagnostic accumulated by unsupported() (query surface)
#include "kama.query.h"        // LSP query surface: SrcRange / DefSite / PosEntry / SymbolInfo / Location

// A function parameter, in declared order. Named kama arguments are matched
// against these to recover C's positional order at each call site.
struct ParamSig {
    std::string name;
    bool        byRef;        // ref/out => passed as a pointer (call site emits &arg)
    std::string className;    // class type (for ref upcast at call sites), "" if primitive
    // `out T x` — a WRITE-ONLY borrow: the callee must assign it on every path before returning, and may
    // not read the incoming value. Lowers identically to `ref` (a `T*`); the difference is entirely in the
    // rules, which is why the call site must SAY `out` (see emitReorderedCall) — the marker is what lets
    // the caller's definite-assignment analysis mark an otherwise-unassigned local live across the call.
    bool        isOut = false;
    bool        isConst = false;   // `const` param — emits `const T*` for FFI pointers
    bool        isHardware = false; // `hardware Ptr<T>` param — emits `volatile T*` for MMIO
    // The parameter's declaration identifier — the SAME node registerBinding keys its DefSite on, which
    // is what lets a call-site LABEL be indexed as a reference to it (LSP M6 A2). Analysis-only; null for
    // the synthesized signatures of string/collection intrinsics, which have no source declaration.
    const IdentifierNode* declSite = nullptr;
};

struct FuncSig {
    std::string            cName;    // mangled C name (e.g. main -> kama_main)
    std::string            retCType; // resolved C return type (signature check)
    std::vector<ParamSig>  params;
    bool                   isPlaceReturn = false;  // `fn ref T …` — returns a place (T*), deref'd at the call site
    FunctionDeclarationNode* node = nullptr;  // decl site (LSP def-site table; unused by emission)
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
    SharedIdentifier nameId;         // LSP (M3.4): the name's own identifier node — `name` alone has no span
    SharedIdentifier type;
    SharedExpression initializer;    // optional; applied in the constructor
    Visibility       visibility = Visibility::Private;
    // Serialization metadata (from `@field`/`@skip` on a `@generate`d type; see collectClasses).
    bool             serSkip = false;   // `@skip` — omit from serialization
    std::string      serName;           // wire name (`@field(name: "…")`; empty => use `name`)
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
// own machinery (isIntrinsicColl/isSmartPtr/isVariant + destructibility), not the kind.
enum class TypeKind { Value, Resource, Contract, Intrinsic };

struct MethodInfo {
    std::string                  cName;   // Class__method (declaring class)
    SharedIdentifier             returnType;
    std::vector<ParamSig>        params;
    ClassMethodDeclarationNode*  node = nullptr;   // for body emission (null for intrinsics)
    bool                         isVirtual  = false;  // virtual/override/abstract
    bool                         isOverride = false;
    bool                         isAbstract = false;  // null body
    bool                         isIntrinsic = false; // collection op: body is in kama_runtime.h, not AST
    bool                         isConst = false;     // `const fn …` — non-mutating
    Visibility                   visibility = Visibility::Private;
    bool                         isFinal = false;     // `final fn` — seals a virtual slot
    bool                         isStatic = false;    // `static fn` — no implicit `self`; called `Type::m(...)`
    bool                         isCtor = false;      // a named constructor (`ctor name(…)`) — a static factory
                                                      // returning the enclosing type (or `Result<This,E>`)
    bool                         isDefaultCtor = false; // `default ctor …()` — the canonical zero-arg ctor (M8b);
                                                        // the field-fill target for complete-init (Part 2). Explicit only.
    bool                         isRetro = false;     // injected by a retroactive `implements C for T` block —
                                                      // emitted static-inline in the header, skipped by the
                                                      // per-class proto/body loops (avoids a dup for a user target)
    // Compiler-synthesized by-value serialization (a `@generate` tree struct with no hand impl). `node` is
    // null: the proto/body loops skip these and emit via emitSerializeDefinition/emitDeserializeDefinition.
    bool                         isSynthSer = false;  // synthesized `serialize(ref Serializer)`
    bool                         isSynthDe  = false;  // synthesized static `deserialize(Deserializer) -> This`
    // Compiler-synthesized `@generate(Format)` field-dump `format(ref Formatter)`. `node` is null: the
    // proto/body loops skip the ordinary path and emit via emitFormatDefinition.
    bool                         isSynthFormat = false;
    // Compiler-synthesized `@generate(of|zero)` bag ctor (M6). `node` is null: the proto/body loops skip the
    // ordinary path and emit via bagCtorSig/emitBagCtorBody, dispatching on the method key ("of"/"zero").
    bool                         isSynthBag = false;
    // Compiler-synthesized `@generate(Equatable|Hashable)` — memberwise `equals` / field-walked `hash`.
    // `node` is null: emitted via emitEqualsDefinition / emitHashDefinition, keyed on the method name.
    bool                         isSynthCmp = false;
    // `fn … when [P1: B1, …]` — the gated type-params + required contracts (index-aligned, AND). Empty = unconditional.
    std::vector<std::string>     whenParams;
    std::vector<std::string>     whenBounds;
    // Operator overloads register as methods under a synthetic name (`op_add`, `op_neg`, …).
    // They are NOT ClassMethodDeclarationNode, so `node` stays null: emit from `opDecl` instead.
    bool                         isOperator = false;
    int                          arity = 0;           // 0 = unary-on-this, 1 = binary method (`this`+rhs), 2 = binary free
    ClassOperatorDeclarationNode* opDecl = nullptr;   // the operator decl (body/params) when isOperator
    bool                         isPlaceReturn = false;  // `ref T operator[]` — returns a PLACE (T*), deref'd at the caller
};

// A built-in generic collection / smart-pointer kind. Backed by a C runtime template. Owned<T> is a
// unique heap-owning pointer kind. (The growable/fixed heap arrays are the pure-kama library types
// `DynamicArray`/`FixedArray`, not kinds here; `Fixed` is `InlineArray<T,N>`, the const-generic value array.)
enum class CollKind { String, Owned, Shared, Weak, Bindable, Fixed };

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

// A constructor of a type. Today there is exactly one per type — the *class-named* ctor, keyed in the
// `ClassInfo::ctors` map by the class name. The construction-model campaign (M2+) adds *named* ctors keyed
// by their own name and the dot-on-type call form. `isFallible`/`returnType` are populated in M2 (a `-> T`
// vs `-> Result<T,E>` return); in M1 the map is populated alongside the legacy single-ctor fields and read
// only via the accessors below (readers migrate to it in later milestones).
struct CtorInfo {
    ClassConstructorDeclarationNode* node = nullptr;
    std::vector<ParamSig>            params;
    Visibility                       visibility = Visibility::Private;
    bool                             isFallible = false;   // returns Result<T,E> (M2)
    SharedIdentifier                 returnType;           // explicit `-> …` (M2); null => infallible `T`
    bool                             isDefaultCtor = false; // `default ctor …()` — the canonical zero-arg ctor (M8b)
};

struct ClassInfo {
    std::string                       name;       // struct name (== kama class name)
    TypeKind                          kind = TypeKind::Intrinsic;   // set to value/resource for user types
    // `type view` — a non-escaping, stack-only borrow (C# `ref struct`): codegens like a `value`
    // (inline, owns nothing, no dtor) but the escape checker forbids it as a return/field/collection
    // element (like a `contract`). It borrows raw `Ptr<T>` it does not own; see isNonEscapingBorrow.
    bool                              isBorrow = false;
    std::vector<FieldInfo>            fields;      // declaration order
    std::set<std::string>            fieldNames;
    std::set<std::string>            constFields;   // `const` data members — write-once in the ctor
    // `@generate(Serialize|Deserialize)` opt-in (pay-for-what-you-use): only set for a marked type; drives
    // emission of the reflective `__serialize`/`__deserialize` helpers (see emitSerialize/DeserializeDefinition).
    bool                              genSerialize = false;
    bool                              genDeserialize = false;
    // `@generate(of|zero)` — bag-only opt-in ctors on a TRANSPARENT value (all public fields, see
    // isTransparentValue). `of` = a synthesized memberwise ctor `V.of(f1: …, …)`; `zero` = a zero-init ctor
    // `V.zero()`. Both synthesize a named `ctor` (registered in `ctors`/`methods`) whose body is emitted by
    // emitBagCtorDefinitions. See the construction-model campaign (M6).
    bool                              genOf = false;
    bool                              genZero = false;
    // `@generate(Format)` — opt-in synthesized field-dump `Format` impl (`Type { f: v, … }`), infallible;
    // the display analog of genSerialize. Body emitted by emitFormatDefinition.
    bool                              genFormat = false;
    // `@generate(Equatable|Hashable)` — opt-in memberwise `equals` / field-walked `hash`, plus the nominal
    // conformance (so `==` lowers to it and a `<K: Hashable + Equatable>` bound is satisfied). Structural
    // equality stays a deliberate NON-default: you ask for it. Bodies: emitEqualsDefinition/emitHashDefinition.
    bool                              genEquatable = false;
    bool                              genHashable = false;
    std::map<std::string, MethodInfo> methods;    // by kama method name
    bool                              preludeStatic = false;  // a non-generic prelude type (e.g. Chars) whose
                                                              // method bodies must be emitted static-inline in
                                                              // the header (the prelude is otherwise collect-only)
    // Named ctors, keyed by their own name (`make`, `of`, `zero`, …) — the ONLY way a type is constructed.
    // Empty => the type cannot be built at all (see rejectNamelessConstruction).
    std::map<std::string, CtorInfo>   ctors;
    CtorInfo*       ctorByName(const std::string& n)       { auto it = ctors.find(n); return it == ctors.end() ? nullptr : &it->second; }
    const CtorInfo* ctorByName(const std::string& n) const { auto it = ctors.find(n); return it == ctors.end() ? nullptr : &it->second; }
    ClassDeclarationNode*             node    = nullptr;

    // RAII
    bool                              hasDtor = false;   // declares its own ~dtor
    ClassDestructorDeclarationNode*   dtorNode = nullptr;
    bool                              destructible = false; // own dtor OR a destructible field (transitive)
    // Serialization mode gate (the tighter sibling of `destructible`): transitively reaches a
    // Shared/Weak/Owned pointer field. false => by-value/tree serialization; true => object-graph.
    // Recurses through collection elements + owned fields, but STOPS at a pointer (doesn't recurse
    // through it). Populated by computeReachesPointer(). Consumed by the serialization lowering (Phase C+).
    bool                              reachesPointer = false;
    // Channel-sendability gate (M3): transitively reaches a NON-ATOMIC shared refcount — a `Shared<X>`
    // or `Weak<X>` field/base/variant-payload/collection-element. `Owned<X>` is fine (unique ownership,
    // the move transfers it whole), so this is the Shared|Weak-only sibling of `reachesPointer`. A type
    // that reaches one may not cross a `channel<T>` (its refcount would race across isolates). Populated
    // by computeReachesSharedWeak(); consumed by the channel-sendability check.
    bool                              reachesSharedWeak = false;
    // `immutable value|resource T` — the greppable qualifier opting into cross-isolate sharing (M6.2). The
    // emitter VERIFIES deep/transitive immutability (computeDeeplyImmutable); a qualified type with a mutable
    // field is a compile error. Distinct from a `const` binding (which permits a mutable alias elsewhere).
    bool                              isImmutableQualified = false;
    // Deeply/transitively immutable (computed): `isImmutableQualified` AND every field/base/variant-payload/
    // element is itself deeply immutable, with no raw `Ptr`/mutable-`Owned`/mutable collection. Populated by
    // computeDeeplyImmutable(). A `Shared<T>`/`Weak<T>` over such a `T` is sendable across isolates (its
    // control block uses the ATOMIC refcount flavor; see useAtomicRefcount) and does NOT set reachesSharedWeak.
    bool                              deeplyImmutable = false;
    // Refcount flavor (M6.2): a `Shared<T>`/`Weak<T>` instance whose element is deeplyImmutable uses the
    // Arc-correct ATOMIC control-block ops (race-free clone/drop across isolates); every other Shared/Weak
    // keeps the cheap non-atomic `Rc` ops. Set at monomorphization from deeplyImmutable(elem). Selected at
    // emit time (zero runtime branch) by the kama_ctrl.h seam — the CollectionInfo sibling carries it too.
    bool                              useAtomicRefcount = false;
    // A node in a serializable object graph: either a graph root (`reachesPointer`) OR a pointee reached via
    // some graph type's Shared/Weak/Owned field (a tree type like `Leaf` that is only ever a `Shared<Leaf>`
    // target). Populated by computeGraphNodeTypes() (a closure over the smart-ptr fields, seeded by
    // reachesPointer). Such a type emits the graph node helpers (serializeNode/allocShell/wireShell) and its
    // synthesized `deserialize` returns `Shared<T>` (the two-pass graph driver), not `T`.
    bool                              isGraphNode = false;
    // A graph node whose public `deserialize` is the two-pass driver returning `Shared<T>` — set iff the
    // `Shared<T>` instance actually exists (a root, or a Shared/Weak pointee). A pure `Owned`-only pointee
    // (reconstructed via the node helpers, never named as `Shared<T>`) keeps a by-value `deserialize` instead.
    bool                              graphDeserialize = false;
    // Stable per-graph-node id (index into `_graphNodeOrder`), assigned by computeGraphNodeTypes(). Used to
    // recover a pointee's concrete type behind a polymorphic contract edge during the two-pass read. -1 = not a node.
    int                               graphTypeId = -1;
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
    // `implements Copyable(bare: …) when [P1: B1, …]` — the gated type-params + contracts (index-aligned,
    // AND) that make this instance Copyable. Empty = unconditional.
    std::vector<std::string>          copyableWhenParams;
    std::vector<std::string>          copyableWhenBounds;

    // Inheritance + virtual dispatch
    std::string                       baseName;        // "" if no base
    ClassInfo*                        base = nullptr;  // resolved by linkBases()
    bool                              isAbstractClass = false;
    // `virtual`/`abstract`/`final` are extensibility qualifiers on a `resource`.
    bool                              isVirtualClass = false; // `virtual resource` — extensible base
    bool                              isFinalClass = false;   // `final class` — sealed leaf
    std::vector<RawFriendGrant>       friendGrantsRaw;        // captured at collection
    std::vector<FriendGrant>          friendGrants;           // resolved (resolveFriends)
    bool                              hasVtable = false;     // this or an ancestor has a virtual
    std::string                       vtableRoot;            // class owning the __vptr member
    std::map<std::string,std::string> slotImpl;             // virtual slot name -> impl cName (most-derived here)

    // Contracts
    std::vector<std::string>          interfaces;            // implemented contract names
    // Contracts satisfied via a retroactive `implements C for T { … }` block (a subset of `interfaces`).
    // These dispatch statically/monomorphized through the injected methods, so they get NO fat-pointer
    // interface vtable (a primitive/foreign target can't be boxed as one) — skipped in vtable emission.
    std::vector<std::string>          retroInterfaces;

    // Collections: a monomorphized Coll<T> is a synthetic ClassInfo whose
    // method bodies come from a C-template macro (not kama AST).
    bool                              isIntrinsicColl = false;
    CollKind                          collKind = CollKind::String;   // arbitrary: only read when isIntrinsicColl
    std::string                       collElemClass;         // element class name ("" if primitive)
    bool                              isGenericInst = false; // a specialized generic-type instance (Box_int32)
    // A synthetic ClassInfo for a PRIMITIVE target of a retroactive `implements C for int32` — it carries
    // only the injected contract methods, whose receiver `this` is the SCALAR itself (by value), not a
    // `T* self`. So `k.hash()` -> `int32_t__hash(k)` (value), and the method emits `int32_t self`.
    bool                              isScalarRecv = false;

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

    // FFI: an `extern class` is an external C struct — kama uses its
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
    // The intrinsic INTERFACE box's allocator type arg (`Shared<Shape, BumpAllocator>` -> "..BumpAllocator");
    // "" or "GlobalAllocator" => the default libc path (existing macros, byte-identical). A stateful one selects
    // the `KAMA_*_IFACE_ALLOC_*` macros + threads `A alloc`/`objsize` through the handle (M11d). Set at the
    // interface divert (the intrinsic collection has no `alloc` FIELD, so it can't be read via boxAllocatorArg).
    std::string  allocType;
    // Refcount flavor (M6.2): true iff this Shared/Weak's element is deeplyImmutable -> the control block
    // uses the Arc-correct ATOMIC kama_ctrl ops (race-free across isolates). The ClassInfo sibling of the
    // same name carries it for the library (concrete-element) path; set at registerSmartPtr from the element.
    bool         useAtomicRefcount = false;
};

// A `contract`: a set of method prototypes, lowered to a vtable struct
// type + a fat-pointer value type. Implemented by classes via a C__as_I vtable. The method's
// return type + params are stored directly (not a node pointer) so it can be built from a
// `type contract` (ClassMethodDeclarationNode).
struct InterfaceMethod { std::string name; SharedIdentifier returnType; SharedParameterList params;
                         bool isPlaceReturn = false;     // `fn ref T m()` — vtbl slot/cast spells `T*`
                         bool isCtor = false;            // a contract-required `ctor` (M8a) — compile-time guarantee, NOT a vtbl slot
                         // The declaration's NAME identifier, for the reference index (M6 B3c). Null for
                         // the operator arm, which has no name node — as MethodInfo::node already is.
                         SharedIdentifier nameId; };
struct InterfaceInfo {
    std::string                  name;
    std::vector<InterfaceMethod> methods;
    std::string                  scope;
    std::vector<std::string>     usings;
    // Per-symbol imports (`import a::b::{X,Y as Z}`) of the declaring unit — needed so a contract method
    // signature that names an imported/library-generic type (`List<uint8>`, `Result<usize, IoError>`)
    // resolves to its fully-qualified, monomorphized C name in the vtbl slot + the `C__as_I` cast.
    // (Generic contracts stash the whole NsCtx via _genericContractCtx; non-generic ones carry it here.)
    std::map<std::string, std::string> symbolAliases;
    // Kind-gate (`for value|resource|both`): which kinds may `implements` this contract. Both true = `both`.
    bool                         allowsValue = false;
    bool                         allowsResource = false;
    // Refined parent contracts (`type contract Animated implements Drawable`) — resolved names. Their methods
    // are merged into `methods` by linkContracts() so vtable/conformance/dispatch see the full slot set.
    std::vector<std::string>     refines;
    // A specialized generic-contract instance (`Iterator_int32`) — emitted under a bound _typeSubst so
    // its `T`-typed method sigs resolve; the template itself lives in _genericContracts, not here.
    bool                         isGenericInst = false;
    std::string                  templateKey;   // the generic contract this specializes (e.g. "Iterator")
    std::vector<SharedIdentifier> typeArgs;      // the concrete args (e.g. [int32])
    ClassDeclarationNode*        node = nullptr;  // decl site (`type contract` node; LSP def-site table, unused by emission)
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

// True for a name the BUILD CONFIGURATION owns (DEBUG/RELEASE/HOSTED and the OS_/ARCH_/ABI_ namespaces
// derived from the target triple) rather than a project's own `flags` entry. Shared by the emitter's
// strict `@compileFor` validation and the driver's manifest check, so the rule has one definition.
// Defined in kama.cemit.cpp. See docs/targets.md.
bool kamaIsBuildConfigFlag(const std::string& name);

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // The implicit prelude (library sum types Optional/Result). Collected before user code
    // with a global namespace, so its templates register but emit nothing unless instantiated.
    void setPrelude(SharedCompilationUnit u) { _preludeUnit = u; }

    // `--no-heap` (MCU step 5): reject every emitter-visible heap allocation program-wide (the no-heap
    // subset — also serves game-engine hot paths / real-time audio, not just bare metal). Composes with
    // `--target embedded`. Per-region `@noheap` on a fn is handled per-body; both funnel through
    // `rejectIfNoHeap`. Set from the driver before emission.
    void setNoHeap(bool on) { _noHeapProgram = on; }

    // `--release`: strips `debugAssert(...)` (dev-only checks) at emit time, mirroring C's `NDEBUG` /
    // Rust's `debug_assert!`. `assert(...)` stays always-on. Set from the driver before emission.
    void setRelease(bool on) { _release = on; _logCompileMin = on ? 3 /*Debug*/ : 99 /*no strip*/; }

    // `@compileFor(FLAG)` conditional compilation: the active build-flag set (built-ins from
    // `--target`/`--release` + `--define`), the declared-flag universe (from `kama.json`), and whether
    // strict validation is on (a manifest was loaded). Set from the driver before emission; consumed by
    // `pruneInactiveDecls` at the top of `collectProgram` — inactive decls are dropped, and the
    // `@compileFor` attribute is stripped from kept decls so no downstream pass ever sees it.
    void setBuildFlags(const std::set<std::string>& active,
                       const std::set<std::string>& declared,
                       bool strict)
    { _activeFlags = active; _declaredFlags = declared; _strictFlags = strict; }

    // The baked `KAMA_LOG` default (from the manifest `log` section). When non-empty and the program imports
    // std::log, `main` seeds it into the process env (overwrite=0), so a shipped binary carries its project
    // default log filter while `--log`/`KAMA_LOG` still override it (M5).
    void setLogDefault(const std::string& spec) { _logDefault = spec; }

    // A namespaced built-in module (the smart-pointer triad, std::memory) — collected before user
    // code under its own `namespace`/`export`, plus an implicit `using` so its names are always in
    // scope. Like the prelude, its generic templates emit nothing unless instantiated.
    void addPreludeModule(SharedCompilationUnit u) { if (u) _preludeModuleUnits.push_back(u); }

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

    // FFI link hint: was this C header `extern "<…>";`'d anywhere? (pay-for-what-you-use — the driver
    // appends `-lm` only when `<math.h>` is used, `-lws2_32` only for sockets, etc.)
    bool externsHeader(const std::string& h) const { return _externedHeaders.count(h) > 0; }

    // ---- Semantic query surface (LSP / front-end-as-library) --------------------------------------
    // `collectProgram()` — the parse-time name resolution + type-checking + ownership analysis — is
    // already a self-contained pass that writes NO C (emission is a separate walk over the same tables).
    // `analyze()` runs exactly that pass with `_out` pointed at a discarded sink, so a caller can build
    // the semantic index WITHOUT emitting a byte of C, then read it via the accessors/queries. The
    // ordinary `emit()`/`emitProgram()` path is untouched. Returns the count of un-analyzable nodes
    // (0 == fully understood); structured diagnostics land in T2.
    explicit CEmitter(const std::string& sourcePath = "<analysis>");   // analysis-mode ctor (no real stream)
    int analyze(const std::vector<SharedCompilationUnit>& units);

    // The semantic diagnostics collected during the last emit()/analyze() run, in encounter order. Every
    // `unsupported()` call (the emitter's single error channel) also lands here as a structured Diagnostic,
    // so a non-emitting analyze() can hand them back without scraping stderr. Parse diagnostics live on
    // CodeGenContext (a syntax error stops the parse before emission); the LSP merges both streams.
    const std::vector<Diagnostic>& diagnostics() const { return _diagnostics; }

    // ---- Query facade (T5) — read off the index built by analyze() ---------------------------------------
    // All framework-free (kama.query.h types); the driver / `kama lsp` server maps them to protocol JSON.
    // `uri` is a source path matching a unit passed to analyze() (== *unit->name).
    std::vector<SymbolInfo> documentSymbols(const std::string& uri) const;         // outline (user decls only)
    Location    definitionAt(const std::string& uri, int line, int col) const;     // go-to-definition
    std::string typeAtPosition(const std::string& uri, int line, int col) const;   // hover: kind + name at a decl/type ref
    std::vector<Diagnostic> diagnosticsFor(const std::string& uri) const;          // diagnostics for one file
    // find-references (M3): every USE of the symbol at the cursor, across every unit passed to analyze().
    // `includeDecl` adds the declaration's own name range (LSP's context.includeDeclaration).
    std::vector<Location> referencesAt(const std::string& uri, int line, int col, bool includeDecl) const;
    // prepareRename (M3): the identifier range at the cursor IF it names a renameable user symbol
    // (a DefSite in a user unit), else an empty range. Never matches prelude/std/builtins.
    SrcRange renameRangeAt(const std::string& uri, int line, int col) const;
    // Every DECLARATION a rename here would rewrite — the symbol plus its rename group (M6 B3c). The
    // rename path's ownership guard needs all of them, not just the one under the cursor.
    std::vector<Location> declarationsAt(const std::string& uri, int line, int col) const;
    // workspace/symbol (M3.5): every user symbol in the index whose name contains `query`
    // (case-insensitive; "" matches all), each carrying its declaring file in SymbolInfo::uri. Spans every
    // unit, including std — restricting to one project is the driver seam's job (it needs real-path
    // resolution), as is capping the result.
    std::vector<SymbolInfo> workspaceSymbols(const std::string& query) const;
    // semanticTokens (M6 B2): every indexed position in one file that resolves to a known declaration,
    // classified by that declaration's kind — the layer that corrects what a regex cannot compute. Sorted
    // ascending and guaranteed NON-OVERLAPPING (the protocol forbids overlap), in kama coordinates with a
    // length; the server owns the legend mapping and the delta encoding. Unlike the rename/reference paths
    // this does NOT exclude prelude/std targets: not owning a symbol is a reason to refuse to rename it,
    // not a reason to refuse to colour it.
    std::vector<SemanticToken> semanticTokensFor(const std::string& uri) const;
    // coverage oracle (M6 B3): what the index knows at one position, as a stable token — `decl:<kind>`,
    // `ref:<kind>`, `unresolved` (indexed but naming no def-site: a builtin, a type parameter), or `-` for
    // nothing at all. Paired with sourceIdentifiers() (kama.query.h) by `kama query --coverage`, it turns
    // "which spellings does the reference index still miss" from a thing someone has to think of into a
    // diffable table. `-` on a user symbol is a gap; see tests/query/coverage/.
    std::string coverageAt(const std::string& uri, int line, int col) const;
    // completion (M4). `ctx` is the LEXICAL context recovered from the LIVE buffer (completionContextAt) —
    // NOT a cursor position: at completion time the buffer does not parse, so the receiver the user just
    // typed exists in no AST. NOT const, unlike the rest of the facade: resolving a type spelling means
    // swapping `_nsCtx` (and, inside a generic instance, `_typeSubst`) to the querying context and back,
    // exactly as buildPositions' resolve-fill sweep does. Nothing is recorded — every resolve passes
    // site=nullptr — and `cType` is never called, since its unsupported() side effect would inject phantom
    // diagnostics into the file the editor is showing.
    std::vector<CompletionItem> completionsAt(const std::string& uri, const CompletionContext& ctx);
    // signature help (M4.4). Same lexical context, same non-const reasoning as completionsAt.
    SignatureHelp signatureAt(const std::string& uri, const CompletionContext& ctx);

private:
    const CompilationUnit* unitForUri(const std::string& uri) const;   // *unit->name == uri, else nullptr
    std::string declKeyAt(const CompilationUnit* unit, int line, int col) const;  // cursor -> resolved DefSite key
    // ---- LSP query index (T4/T5) — built at the tail of analyze(), read by the query facade -------------
    // Populated from the symbol tables' existing decl-node pointers AFTER analysis, so it never perturbs
    // resolution/emission (the whole facade runs read-only on stable tables). See kama.query.cpp.
    std::vector<SharedCompilationUnit> _units;   // the USER units passed to analyze() (URI->unit, outline filter)
    std::map<std::string, DefSite>     _defSites;  // resolved mangled name -> declaration site
    std::map<const ASTNode*, const CompilationUnit*> _declUnit;  // top-level decl node -> owning unit
    std::map<const CompilationUnit*, std::vector<PosEntry>> _positions;  // per-unit sorted decl/sig/body positions
    // ---- M3 reference index -----------------------------------------------------------------------------
    // A body use-site as the REAL resolver produced it (recordRef), pending merge into _positions. Held only
    // between the analysis walk and buildPositions(), then cleared.
    struct RecordedRef { const CompilationUnit* unit; const IdentifierNode* id; std::string key; };
    std::vector<RecordedRef> _bodyRefs;
    // M3.4: a DECLARATION discovered during the walk. Locals, params and match bindings have no symbol-table
    // entry to key on, so — unlike types/functions — their def-sites can only be captured where the walk
    // knows the enclosing scope. Same lifetime as _bodyRefs: merged by buildDefSites(), then cleared.
    struct RecordedDef { const CompilationUnit* unit; const IdentifierNode* id; std::string key;
                         SymKind kind; std::string container; };
    std::vector<RecordedDef> _localDefs;
    // A use-site recorded by the DECLARATION NODE it refers to, rather than by a key — the inversion M6 A2
    // paid for and M6 B3 generalized. It exists because a key built at record time embeds AMBIENT CONTEXT
    // that is wrong for the symbol being named. Two independent proofs of that:
    //
    //   - a named-argument LABEL (A2): `bindingKey` reads `_refUnit` for its file component, and at a call
    //     site `_refUnit` is the CALLER's unit while the parameter's DefSite was keyed under the DECLARING
    //     unit. Every cross-unit call would mismatch — while same-file calls kept working, which is the
    //     worst possible failure mode for a test suite to face.
    //   - a generic MEMBER (B3): `b.v` on a `Box<int32>` resolves through the INSTANCE, so the key spells
    //     `field:Box_int32::v` while the only def-site is the template's. Canonicalizing the key would be
    //     string surgery over two shapes that a third would outgrow.
    //
    // Recording the node dissolves both, because every instantiation walks the SAME template AST nodes
    // (an instance ClassInfo is a copy of the template shape — see the `_genericTypes[tmpl]` copy in
    // registerGenericTypeInst). `Box<int32>` and `Box<string>` therefore arrive at one node and collapse
    // onto one key with no mapping table, and the answer cannot drift when a new key shape appears.
    // buildPositions resolves node -> key after buildDefSites, so it is also independent of walk order.
    struct RecordedNodeRef { const CompilationUnit* unit; const IdentifierNode* site;
                             const ASTNode* declNode; };
    std::vector<RecordedNodeRef> _nodeRefs;
    std::map<std::string, std::vector<Location>> _refIndex;   // DefSite key -> every USE site of that symbol
    // What a `module:` position points at (M6 B3f). Deliberately NOT a DefSite: a module is a directory on
    // disk, so it is a NAVIGATION target and never a rename target. Keeping it out of _defSites is what
    // makes rename, find-references and semantic tokens ignore these positions with no new flag. `unit` is
    // null for a path PREFIX no file declares (`std` in `std::collections`) — clangd answers a partial
    // include path the same way.
    struct ModuleSite { std::string display; const CompilationUnit* unit = nullptr; SrcRange range; };
    std::map<std::string, ModuleSite> _modules;
    const CompilationUnit* _refUnit = nullptr;   // unit whose bodies are being walked (set in emitModuleContent)
    bool _analysis = false;                      // analysis-mode ctor => record references; a build records none
    void recordRef(const std::string& key, const IdentifierNode* site);  // pure append; no diagnostics, no cType
    void recordDef(const std::string& key, const IdentifierNode* site, SymKind kind,
                   const std::string& container);                        // pure append (M3.4 bindings)
    // pure append, keyed later off the referent's DECLARATION node (see RecordedNodeRef above). `declNode`
    // is whatever buildDefSites keyed that symbol's DefSite on: a parameter's identifier, a field's
    // `nameId`, a method's ClassMethodDeclarationNode.
    void recordNodeRef(const IdentifierNode* site, const ASTNode* declNode);
    // A FIELD use-site, by node. `owner` is the DECLARING class (findFieldOwner), which for a generic
    // instance still carries the template's own `nameId` — that is what makes one field one symbol.
    void recordFieldRef(const ClassInfo* owner, const std::string& name, const IdentifierNode* site);
    // M3.4 keys. These name symbols the resolvers never produce a mangled name for, so they are PREFIXED —
    // an index-only namespace that cannot collide with a resolveUserName/resolveFunc result. A binding is
    // keyed by its DECLARATION SITE, which is what makes two same-named locals in sibling scopes (or in two
    // different functions) distinct symbols without threading a function key through every call.
    std::string bindingKey(const IdentifierNode* declSite) const;        // "local:<file>:<line>:<col>:<name>"
    static std::string fieldKey(const std::string& ownerKey, const std::string& name);       // "field:Owner::name"
    static std::string enumMemberKey(const std::string& enumKey, const std::string& name);   // "enum:Enum::name"
    // "contract:Contract::name" — a contract method declares a vtbl slot, so it has no cName to key on.
    static std::string contractMethodKey(const std::string& contractKey, const std::string& name);
    // Segment `i` of a `::`-separated name list, qualified by the segments to its left (M6 B3f).
    // `dotted` is that same prefix as a source spelling, for the module case.
    std::string listSegmentKey(const StringList& segs, size_t i, const std::string& dotted);
    std::string moduleKeyOf(const std::string& dotted) const;   // "module:<mangled>", or "" if not one
    // The binding key `name` currently resolves to: innermost enclosing scope first, then the parameters of
    // the function being emitted. Empty when `name` is neither (recordRef/recordDef ignore an empty key).
    std::string bindingKeyOf(const std::string& name) const;
    // Give a binding declaration an index key, record the def-site, and make the name resolvable for the
    // rest of its scope (SymKind::Param goes to _paramDeclKeys, everything else to the innermost scope).
    // A no-op outside analysis mode, so `kama build` pays nothing.
    void registerBinding(const IdentifierNode* declSite, SymKind kind);
    // Attribute every top-level decl to its owning USER unit (_declUnit). Split out of buildDefSites and
    // ALSO called from collectProgram, because generic-instance emission needs unitOfDecl while it runs —
    // long before buildDefSites. Depends on nothing but _units, and is idempotent.
    void buildDeclUnits();
    void buildDefSites();                        // fill _defSites/_declUnit from the tables (T4a)
    void buildRenameGroups();                    // contract method <-> its implementations (M6 B3c)
    // Def-site keys that must be renamed TOGETHER: a contract's method declaration and every
    // implementation of it. Each member maps to the whole sorted group; singletons are absent, so a
    // symbol in no group costs the query paths one failed lookup. Read only by referencesAt /
    // declarationsAt — go-to-definition deliberately stays precise.
    std::map<std::string, std::vector<std::string>> _renameGroup;
    void buildPositions();                       // fill _positions + _refIndex (decls, sig refs, body refs)
    // Point the reference recorders at `unit` for a dynamic extent. Save/restore rather than assign, so a
    // nested emission (a generic instance emitted inside a module body walk) cannot strand the pointer.
    // Setting it to nullptr is meaningful: it DISABLES recording, which is what a prelude/std template's
    // body should do.
    struct RefUnitScope {
        CEmitter* e; const CompilationUnit* prev;
        RefUnitScope(CEmitter* em, const CompilationUnit* u) : e(em), prev(em->_refUnit) { e->_refUnit = u; }
        ~RefUnitScope() { e->_refUnit = prev; }
    };
    const PosEntry* posAt(const CompilationUnit* unit, int line, int col) const;  // smallest span at cursor
    void addDefSite(const std::string& key, SymKind kind, const CompilationUnit* unit,
                    ASTNode* declNode, const SharedIdentifier& nameId,
                    const std::string& display, const std::string& container);  // one _defSites entry
    const CompilationUnit* unitOfDecl(const ASTNode* topLevelDecl) const;  // _declUnit lookup (nullptr => prelude/std)

    // ---- M4 completion helpers (kama.query.cpp) --------------------------------------------------------
    // Save/restore the two pieces of resolver state a query mutates. RAII rather than paired assignments
    // because the completion paths return early all over the place, and one leaked _nsCtx would corrupt
    // every subsequent query on this index.
    struct QueryScope {
        CEmitter* e; NsCtx ns; std::map<std::string, SharedIdentifier> subst;
        explicit QueryScope(CEmitter* e) : e(e), ns(e->_nsCtx), subst(e->_typeSubst) {}
        ~QueryScope() { e->_nsCtx = ns; e->_typeSubst = subst; }
    };
    // The user callable whose BODY contains (line,col), plus its enclosing type. Span containment over the
    // unit's top-level decls and class members — a flat loop, no scope stack (the emitter's `_scopes` is
    // long dead by index time, and rebuilding it is not needed: see collectBindings).
    struct QueryCtx { const CompilationUnit* unit = nullptr;   // the file the cursor is in
                      std::string typeKey;                 // enclosing type's _classes/_genericTypes key ("" at file scope)
                      std::string funcName;                // enclosing callable's kama name (friend-grant checks)
                      SharedParameterList params;
                      SharedBlock body;
                      // `<T: Drawable>` in scope here. A receiver typed by a bare type-param has no concrete
                      // class to look at, so its BOUND contract is the only thing that can answer `x.`.
                      std::map<std::string, SharedIdentifierList> typeParamBounds; };
    QueryCtx enclosingCallable(const CompilationUnit* unit, int line, int col);
    std::string classKeyOfName(const SharedIdentifier& name);   // a decl's name node -> its table key ("" if none)
    // One binding a body declares. No scope extent and no index key: kama FORBIDS shadowing
    // (kama.cemit.cpp, emitDeclarator), so within a callable a name is unique except across sibling scopes,
    // and two sibling bindings collapse to one completion LABEL with identical insert text. Rename needed
    // per-declaration identity; completion does not.
    struct QueryBinding { std::string name; SharedIdentifier type; int line = 0; bool isParam = false;
                          // The class whose generic substitution `type` must be read under. Non-empty only
                          // for a match-arm payload binding, whose type node is the VARIANT TEMPLATE's `T`.
                          std::string ownerKey; };
    // Every binding in `s`, in source order. MUST cover every block-bearing statement — Block, Unsafe,
    // Scope, If/Else, While, DoWhile, For, ForEach, ParallelFor and match arms. (scanStmtForCollections
    // omits Unsafe and Scope and has silently under-scanned ever since; do not copy that bug.)
    std::vector<QueryBinding> bindingsAt(const QueryCtx& qc, int line) const;
    void collectBindings(SharedStatement s, std::vector<QueryBinding>& out) const;
    void collectBindingsExpr(SharedExpression e, std::vector<QueryBinding>& out) const;  // finds match arms only
    // A type NODE -> its _classes / _genericTypes / _interfaces key, "" for a primitive or unknown. The
    // cType-FREE stand-in for exprClass, which is unusable here twice over: it calls cType on six paths,
    // and it reads _localTypes, which is cleared at every function entry and after analyze() holds the LAST
    // emitted function's locals. `ownerKey` supplies the generic-instance substitution for a member's type
    // (a field of `DynamicArray_Cell` is declared `T` on the template).
    std::string classOfTypeNodeIn(const std::string& ownerKey, SharedIdentifier t);
    std::string spellTypeIn(const std::string& ownerKey, const SharedIdentifier& t);   // source spelling, for `detail`
    void installOwnerScope(const std::string& ownerKey);   // _nsCtx + _typeSubst for reading ownerKey's members
    // A canonicalized receiver path (`h.cell`, `makeHolder()`, `cells[]`, `this`, `Point`) -> the class key
    // it names. `isType` distinguishes `Point.` (offer ctors + statics) from `p.` (offer instance members);
    // a live binding of the same spelling WINS, mirroring isTypeReceiver's precedence.
    std::string classOfPath(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                            const std::string& path, bool& isType);
    // A bare type-param receiver resolves through its contract BOUND — the only thing that can answer.
    std::string boundOfTypeParam(const QueryCtx& qc, const SharedIdentifier& t, const std::string& fallback);
    // One path segment: `cls`'s member `name` plus the `()` / `[]` suffixes that segment carried.
    std::string stepMemberType(const std::string& cls, const std::string& name, const std::string& suffix);
    std::string memberTypeOf(const std::string& cls, const std::string& name, bool preferMethod);
    std::string elementTypeOf(const std::string& cls);   // `coll[i]`
    // The pointee of a smart pointer or a user `Deref<T>` — derefTarget's cType-free counterpart.
    std::string derefTargetForQuery(const std::string& cls);
    // Visibility as a PURE predicate. canAccess is unusable from a query: it calls unsupported() on denial
    // and decides from _currentClass/_currentFunc, both dead after analysis.
    bool visibleFrom(const ClassInfo* owner, Visibility vis, const std::string& member,
                     const QueryCtx& qc) const;
    void addMembers(const std::string& clsKey, bool wantStatic, const QueryCtx& qc,
                    std::vector<CompletionItem>& out);
    // `Enum::` / `Variant::` cases and type-associated `comptime` constants (M4.2).
    void addScopeMembers(const std::string& key, const QueryCtx& qc, std::vector<CompletionItem>& out);
    // Everything declared in the namespace `path` names, one level deep (M4.2).
    void addNamespaceSymbols(const std::string& path, const QueryCtx& qc, std::vector<CompletionItem>& out);
    // A `::`-qualified path -> the table key it names, "" if it names a namespace or nothing (M4.2).
    std::string resolvePathAsType(const std::string& path);
    // Everything spellable as a BARE name at the cursor (M4.3): bindings, the enclosing type's members,
    // and the file-visible top-level decls. The last of those is the flooding risk — `_classes`/`_funcs`
    // span the whole import closure — so `bareNameOf` is the exact INVERSE of resolveUserNameImpl's
    // lookup order, and a key it cannot spell is a key that must not appear.
    std::string bareNameOf(const std::string& key) const;
    void addNamesInScope(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                         std::vector<CompletionItem>& out);
    // The callable a CANONICALIZED callee path names (M4.4). One resolution serves both signature help
    // and argument-LABEL completion — kama has no positional arguments, so the two ask the same question.
    struct CalleeSig { bool found = false; std::string display, ret; std::vector<SignatureParam> params; };
    CalleeSig resolveCallee(const QueryCtx& qc, const std::vector<QueryBinding>& binds,
                            const std::string& callee);


    // Discards any stray write during analysis mode (collectProgram writes no C, but `unsupported()` still
    // appends its `/* TODO */` marker to `*_out`; in analysis mode that marker goes here and is dropped).
    std::ostringstream _analysisSink;
    std::vector<Diagnostic> _diagnostics;   // structured semantic diagnostics (populated by unsupported())
    std::set<std::string> _externedHeaders;   // every `extern "<h>";` seen (populated by emitIncludes)
    // `isolate` lowering: per-module file-scope helper definitions (thread trampolines) to emit BEFORE a
    // module's bodies (a body takes the address of a trampoline, which C requires defined earlier in the
    // TU). Drained per module in emitModuleContent; deduped across the whole program by _isolateTrampolines
    // (one trampoline per distinct entry fn, even if spawned from several sites).
    std::vector<std::string> _fileScopeHelpers;
    std::set<std::string>    _isolateTrampolines;   // entry cNames whose trampoline is already emitted
    std::set<std::string> _exposedNames;       // bare C-ABI symbols of `expose fn`s — collision check
    std::ostream* _out;
    SharedCompilationUnit _preludeUnit;   // implicit prelude (Optional/Result), collect-only
    std::vector<SharedCompilationUnit> _preludeModuleUnits;  // namespaced built-ins (the triad), collect-only
    // Does the program use serde at all? Set in collectProgram from a `@generate` type or a Serializer/
    // Deserializer backend — the only ways to (de)serialize anything. When false we emit NONE of the serde
    // machinery: the prelude's primitive Serialize/Deserialize retro-impls are skipped, and a collection's
    // conditional `when [T: Serialize]` serde (serialize/serKey/…) is dropped via whenConditionsHold. Purely a
    // compile-time saving — all of it is static-inline / dead-strippable.
    bool                             _usesSerde = false;
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    std::map<std::string, FuncSig> _funcs;   // kama function name -> signature
    std::map<std::string, SigInfo> _sigs;    // function-pointer signature types
    std::map<std::string, SharedIdentifier> _moduleStatics;   // qualified C symbol -> type node of each
                                             // module-level `static` (MCU step 1). A bare ref resolves to the
                                             // symbol (name) and its type (element access / method dispatch)
                                             // when the name is not a local/param/field/func.
    bool isSigType(const std::string& name) const { return _sigs.count(name) != 0; }
    std::set<std::string> _refParams;        // by-ref params of the function being emitted
    std::set<std::string> _paramNames;       // parameter names of the function being emitted — a local
                                             // declaration shadowing one is a compile error (see emitDeclarator)
    std::map<std::string, std::string> _paramDeclKeys;   // LSP (M3.4), analysis mode: param name -> index key
                                             // for the function being emitted. Params outlive every scope, so
                                             // they sit here rather than in Scope::indexDecls; cleared with
                                             // _paramNames at each function/method entry.
    std::set<std::string> _viewParams;       // by-VALUE `type view` params of the fn being emitted — a valid
                                             // root for a view return (borrows caller memory that outlives the
                                             // call). (A `ref`-view param is already in _refParams.)
    std::set<std::string> _viewTypeNames;    // bare names of every `type view` declaration — recognizes a view
                                             // constructor call `View(...)` in the view-return escape check
    // A `type view` cType (a non-escaping borrow that codegens as a value). Distinct from
    // isNonEscapingBorrow, which also includes contracts.
    bool isViewCType(const std::string& name) const {
        auto it = _classes.find(name);
        return it != _classes.end() && it->second.isBorrow;
    }
    std::vector<std::string> _foreachColls;  // root bindings of collections being iterated (nested foreach) —
                                             // growing one mid-iteration (`add`) invalidates its element refs

    std::map<std::string, ClassInfo>   _classes;     // class name -> info
    std::map<std::string, std::string> _localTypes;  // local/param -> class name ("" if primitive)
    std::map<std::string, std::string> _localCTypes; // local -> full C type (incl. primitives) — the
                                                     // target type for a value-producing RHS at an assignment
    std::map<std::string, SharedIdentifier> _localTypeNodes; // local/param/foreach -> its KAMA type node; keeps the
                                                     // char-vs-uint32 distinction cType erases (for char interpolation holes)
    // Compile-time move analysis for `resource` (destructible) VALUES. Per-local
    // move-state, consulted by emitScopeCleanup (skip a moved local's dtor) and the hand-off
    // sites (reject use-after-move). A value moved on some-but-not-all paths that is live at
    // scope exit is *rejected* (conditional-drop) — zero runtime drop-flags by construction.
    enum class MoveState { NotMoved, MaybeMoved, Moved };
    std::map<std::string, MoveState> _moveState;   // move-only local/param cVar -> state
    // Locals declared `slot T x;` — a HOLE. Seeded MoveState::Moved at the declaration so no destructor
    // is emitted while they stay unassigned ("drop only if live", proven statically), which is what
    // retires the runtime `fd >= 0` / `handle != null` guards a raw-handle resource used to need. Kept
    // as its own set so `addr(of: x)` can distinguish vouching for a hole from resurrecting a moved value.
    std::set<std::string> _slotLocals;
    // Every local declared `slot` in this function, INCLUDING the ones since filled (unlike _slotLocals,
    // which is emptied as each hole is filled). A slot filled only on some paths merges to MaybeMoved,
    // which for an ordinary value is an undecidable-drop ERROR — but a slot's storage is always valid
    // (the declaration's field-default fill saw to that), so the honest answer there is just to drop it.
    std::set<std::string> _slotDeclared;
    // Owning payload bindings of a BORROWING `match (x)` arm — each aliases the box the subject still
    // owns, so `give`ing one out double-frees. Non-giveable: a give of a name in here is a hard error
    // (the consuming `match (give x)` is the way to move a payload out). Scoped per-arm.
    std::set<std::string> _borrowedMatchBindings;
    ClassInfo*                         _currentClass = nullptr;  // when emitting a method/ctor
    std::string                        _currentFunc;             // C-name of the function/method being emitted (friend match)
    std::string                        _thisType;                // C name `This` resolves to (the class being emitted, or the contract type inside its vtbl slot)

    // Virtual dispatch: per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots
    std::set<std::pair<std::string,std::string>> _overriddenSlots;  // (vtableRoot, slot) overridden somewhere -> keep dynamic

    // Retroactive conformances of a PRIMITIVE (`implements Hashable for int32`). Kept OUT of `_classes`
    // (an entry there would make every "user type?" test treat the primitive as a struct). Keyed by the
    // primitive cType (int32_t); the ClassInfo holds only the injected methods + `isScalarRecv`.
    std::map<std::string, ClassInfo>     _primConformances;
    std::map<std::string, InterfaceInfo> _interfaces;        // contract name -> info
    // Pre-scanned retroactive conformances: target cType -> the contracts a top-level `implements C for T`
    // grants it. Populated before the collection pass so a generic-type-arg bound check that fires during
    // collection (e.g. `Map<string, V>` needing `string: Hashable`) isn't a false negative — the methods
    // themselves are injected later in applyRetroactive, which also validates completeness/coherence.
    std::map<std::string, std::set<std::string>> _retroConformances;
    std::map<std::string, EnumInfo>      _enums;             // enum name -> info
    // Model C: a PLAIN (payload-less) enum's decl node + its declaring-unit ns context, captured at
    // collectEnums. If such an enum later retro-implements a METHOD-CARRYING contract (e.g. base `Error`),
    // it is PROMOTED to a tagged-union ClassInfo (an all-payload-less variant emits `struct{tag}`, no union)
    // so it can carry a `<Enum>__as_C` vtbl + be boxed — reusing the tagged-enum machinery. Keyed by
    // qualified name.
    std::map<std::string, EnumDeclarationNode*> _enumDeclNodes;
    std::map<std::string, NsCtx>                _enumNsCtx;
    std::set<std::string>                       _preludeEnums;   // enums declared in the prelude (a promoted one's vtbl/serde is header-static, no home module)
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
    // Build a type node the PARSER never saw (`Chars`, `Split`, a fallible ctor's `Optional<T>`), tagged
    // `synthesized` so the reference index skips it — see ASTNode::synthesized for why that matters.
    // Every emitter-built IdentifierNode should come from here; a hand-made clone sets the flag itself.
    SharedIdentifier synthId(const std::string& name, int builtInVal = 0 /* IDENTIFIER_NONE_VAL */);
    SharedIdentifier synthClone(const IdentifierNode& src);   // a copy of a real node is still not source text

    // Generic TYPES (`type value Box<T>`). The TEMPLATE is kept OUT of _classes (so the normal
    // class loops never see it); each reachable `Box<Arg>` becomes a synthetic specialized ClassInfo
    // (`Box_int32`, isGenericInst=true) registered in _classes and emitted under _typeSubst.
    struct GenericTypeInst { std::string templateKey; std::string mangledName; std::vector<SharedIdentifier> typeArgs; };
    std::map<std::string, ClassInfo>          _genericTypes;        // template name -> ClassInfo shape (NOT in _classes)
    std::map<std::string, std::vector<std::string>> _genericTypeParams;  // template name -> type-param names [A, B]
    std::map<std::string, SharedBoundsList>   _genericTypeBounds;   // template name -> per-param contract bounds
    std::map<std::string, std::vector<SharedIdentifier>> _genericTypeDefaults; // template name -> per-param default type (null entry = required, no default)
    std::map<std::string, NsCtx>              _genericTypeCtx;      // template name -> home namespace ctx
    std::map<std::string, NsCtx>              _genericTypeInstCtx;  // instance -> registration (use-site) ctx, so a
                                                                    // prelude template's user-type args resolve at emit time
    std::map<std::string, GenericTypeInst>    _genericTypeInsts;    // mangled name -> instantiation (dedup)
    std::map<std::string, std::string>        _genericTypeInstOf;   // mangled name -> template name (construction)
    std::vector<std::string>                  _genericTypeInstOrder;// registration order (inner-first; struct-typedef emit)
    bool                                      _emitStaticClass = false;  // prefix `static` on specialized class fns (header ODR)
    bool                                      _emitStaticInlineFn = false;// prefix `static inline` on a free fn (prelude helper body emitted in the header)
    bool                                      _noHeapProgram = false;    // `--no-heap`: reject every heap allocation program-wide
    bool                                      _release = false;          // `--release`: strip `debugAssert`
    bool                                      _noHeapActive  = false;    // inside a `@noheap` fn: reject heap allocation in this body
    std::set<std::string>                     _activeFlags;              // `@compileFor`: active build flags (membership gate)
    std::set<std::string>                     _declaredFlags;            // `kama.json` declared user-flag universe (strict validation)
    bool                                      _strictFlags   = false;    // a manifest was loaded -> validate `@compileFor`/`--define` names
    std::string                               _logDefault;               // baked `KAMA_LOG` project default (M5), seeded in main
    // std::log v2 (M7): the compile-time strip floor — the lowest level ORDINAL physically dropped at emit
    // time (like `debugAssert`). A recognized facade call is stripped iff `_release && level >= _logCompileMin`.
    // Derived from `_release` in `setRelease` — release => 3 (Debug), so Debug(3)/Trace(4) strip while
    // Error(0)/Warn(1)/Info(2) are kept; 99 = no strip. A single field so a future `kama.json log.compileMin`
    // override is a one-line change, not a refactor.
    int                                       _logCompileMin = 99;

    // Generic CONTRACTS (`type contract Iterator<T>`) — the exact parallel of generic TYPES above. The
    // TEMPLATE is kept OUT of _interfaces (so the eager vtable-emit loop never sees its unbound `T`);
    // each reachable `Iterator<Arg>` becomes a specialized InterfaceInfo (`Iterator_int32`,
    // isGenericInst=true) registered in _interfaces and emitted under _typeSubst. Bound-checking is by
    // method NAME (T-independent), so it reads the template's methods directly (no instance needed).
    std::map<std::string, InterfaceInfo>            _genericContracts;      // template name -> InterfaceInfo shape (NOT in _interfaces)
    std::map<std::string, std::vector<std::string>> _genericContractParams; // template name -> type-param names [T]
    std::map<std::string, std::vector<SharedIdentifier>> _genericContractDefaults; // template name -> per-param default type (null entry = required)
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
    // `site`, when non-null, is the source identifier this name was spelled at: in analysis mode the
    // resolved key is recorded against it for the M3 reference index (recordRef). Defaulted, so the ~68
    // call sites that have no identifier in hand (or don't want the use recorded) are unaffected.
    std::string resolveUserName(const std::string& value, SharedStringList qualifier,
                                const IdentifierNode* site = nullptr);                  // class/enum/iface ref
    std::string resolveFunc(const std::string& name, SharedStringList qualifier,
                            const IdentifierNode* site = nullptr);                      // function ref
    std::string resolveUserNameImpl(const std::string& value, SharedStringList qualifier);  // the search itself
    std::string resolveFuncImpl(const std::string& name, SharedStringList qualifier);       // the search itself
    bool isNamespace(const std::string& name) const;             // a known public namespace (or alias)

    // RAII scope stack: live destructible locals per lexical scope.
    struct LiveLocal { std::string cVar; std::string className; };
    struct Scope { std::vector<LiveLocal> locals; std::vector<std::string> declaredNames;
                   bool isLoopBoundary = false; bool isFunctionRoot = false;
                   // Structured concurrency (M4): a `scope { }` is a task scope. `taskChildren` are the C
                   // names of the `kama_isolate_t` handles `spawn`ed inside it; emitScopeCleanup joins them
                   // ALL before dropping any local (join-before-drop), on every exit path. `borrowedRoots`
                   // are the root locals its children `ref`-borrow (M4.2) — a second child borrowing the
                   // same root is rejected (the same-root disjointness rule: no two tasks share a cell).
                   bool isTaskScope = false; std::vector<std::string> taskChildren;
                   std::set<std::string> borrowedRoots;
                   // LSP (M3.4), analysis mode only: the bindings this scope declares, with the index key
                   // each was given. Deliberately PARALLEL to `declaredNames` rather than folded into it —
                   // that vector drives the shadowing rules, and it also (by design) excludes `foreach` and
                   // `match` bindings, which the index does want. Popping the scope is what makes two
                   // same-named locals in sibling scopes resolve to their own declaration.
                   struct IndexDecl { std::string name, key; };
                   std::vector<IndexDecl> indexDecls; };
    // Erase move-state for the closing scope's locals, then pop it. A name going out of scope is
    // lexically dead, so a sibling scope reusing it must start NotMoved (not inherit a stale Moved).
    void popScope();
    std::vector<Scope> _scopes;
    // By-value smart-ptr params the callee owns — dropped at fn-end. emitFunction
    // records them here (its function-root scope is created later, in emitBlockScoped,
    // which drains this); emitMethodOrCtorBody records them in its root scope directly.
    std::vector<LiveLocal> _pendingParamDtors;
    std::string        _currentReturnCType = "void";  // for return-temp
    bool               _returnIsPlace = false;         // emitting a `ref T operator[]` body: `return e` -> `return &(place)`
    std::string        _matchTargetCType;              // result C type of a value-producing `match` (set by the liftable site)
    std::string        _variantTargetType;             // target union instance for a generic-variant construction (Optional<int32>)
    // A1 — a value-producing variant ctor as a `match` SUBJECT (`match (Optional::Some(x))`). The instance
    // must be inferred + registered at DISCOVERY (so its struct emits), but that pass has no local types;
    // `_scanLocalTys` is a discovery-time name->type map (params + local decls of the fn being scanned) that
    // feeds the inference. The resolved instance's mangled name is stashed per-match for reuse at emit.
    std::map<std::string, SharedIdentifier> _scanLocalTys;
    std::map<MatchNode*, std::string>       _matchSubjInst;
    int                _tempCounter = 0;
    int                _parforSeq  = 0;   // monotonic id for parallel_for worker/arg/trampoline helper names (M6.3)
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
    bool               _inNamedCtorBody = false;       // emitting a named `ctor` factory body (const fields of the built local are writable)
    bool               _inStaticMethod = false;        // emitting a `static` method body (no `self`/`this`)

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Pre-pass
    // `@compileFor(FLAG)` conditional compilation: drop every top-level decl whose flag gate is
    // inactive (as if never written), and strip the `@compileFor` attribute from kept decls so no
    // downstream pass sees it. Runs at the top of `collectProgram`, before any collect pass.
    void pruneInactiveDecls(SharedCompilationUnit unit);
    bool compileForActive(const SharedAttributeList& attrs, int line);   // eval the gate (true = keep)
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
    void emitStringFind(const CollectionInfo& info);               // `.find()` wrapper: kama_string__find_raw -> Optional<usize>
    void emitSharedToWeakDowngrade(const CollectionInfo& info);    // a library `Rc<Shape>`'s downgrade() (Shared IFACE -> Weak partner)
    void registerBindable(SharedIdentifier elem);                  // BindableFunctionPtr<Sig>
    // The KAMA_*_DEFINE macros, split: typesOnly emits the struct typedefs (`_TYPE`,
    // before class struct bodies so a class may hold one BY VALUE); else the funcs
    // (`_FUNCS`, after class prototypes where element dtors are declared).
    void emitCollectionDefs(bool typesOnly);
    bool isIfaceAllocColl(const CollectionInfo& info) const;   // M11d: fat handle embeds `A alloc` by value
    void emitIfaceAllocType(const CollectionInfo& info);       // its TYPE, laid out after the allocator struct
    void emitIfaceAllocFuncs(CollectionInfo& info);            // its FUNCS, deferred past the allocator's protos
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
    // True iff EVERY type-param of a generic template carries a default — so it may be named BARE
    // (zero type args), like an all-defaulted `BitSet<A = GlobalAllocator>` written just `BitSet`.
    // Its defaults then fill in at genericTypeMangle / registerGenericTypeInst (empty args).
    bool allTypeParamsDefaulted(const std::string& tmpl) const;
    // True iff a (post-substitution) type arg still carries an UNBOUND type-parameter — a bare name resolving
    // to no known type (nor a primitive / This / Ptr / usize / isize), recursing into nested generic args.
    // Guards registerGenericTypeInst against a generic FUNCTION's signature scanned before instantiation.
    bool argCarriesUnboundParam(const SharedIdentifier& a);
    std::string genericTypeMangle(const std::string& tmpl, SharedIdentifierList args);  // "Pair" + "_int32" + "_string"
    // Expand use-site type args into the full positional binding for a generic template: leading POSITIONAL
    // args in order, NAMED overrides (`A: T`) placed by param name, trailing gaps filled from `defaults`.
    // `full[i]`/`isDefault[i]` are index-aligned with `params` (a null `full[i]` = a required param left
    // unbound). Silent + best-effort — the registration path re-validates and reports precise errors.
    void positionalizeGenericArgs(const std::vector<std::string>& params,
                                  const std::vector<SharedIdentifier>& defaults,
                                  SharedIdentifierList args,
                                  std::vector<SharedIdentifier>& full,
                                  std::vector<bool>& isDefault);
    // Reports a precise error (returns false) for a malformed use-site type-arg list; true when well-formed.
    bool validateGenericArgs(const std::string& tmpl, const char* kind,
                             const std::vector<std::string>& params, SharedIdentifierList args,
                             const std::vector<SharedIdentifier>& bound, int line);
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
    // Placement `new(allocator: a) T(...)`: the allocator arg's {C expression, allocator class}, or {"",""}
    // for a bare `new`. Diagnoses a missing/ill-typed `allocator:` slot. `emit` gates side-effecting emission
    // of the expression (false = just resolve the class, for a pre-flight check).
    std::pair<std::string,std::string> placementAllocator(ObjectCreationNode* oc, int line, bool emit);
    // The allocator type-arg of an `Owned<T, A>` box instance (its last generic arg); "" if `ty` is not an
    // `Owned` instance. Used to reject a bare `new` into a STATEFUL-allocator box (which would leak — the
    // block is malloc'd but the box's no-op `deallocate` never frees it).
    std::string boxAllocatorArg(const std::string& ty);
    // Validate a `new [(allocator: a)]` into an intrinsic INTERFACE box `ty` (M11d) and report whether the
    // allocator-aware emission path applies (the box's `allocType` is a stateful, non-Global allocator).
    // Rejects a stateful box built with a bare `new`, or a placement whose handle type != the box's declared
    // `A`. Pre-flight only (placementAllocator with emit=false) — the caller re-runs it with emit=true.
    bool ifaceNewAllocator(const std::string& ty, ObjectCreationNode* oc, int line);
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
    // A1: infer the concrete generic-variant instance of a value-producing variant ctor used as a `match`
    // subject (`Optional::Some(x)` -> the `Optional<int32>` instance node), binding each bare-type-param
    // payload field via `exprTypeNode(arg, localTys)`. `reg` registers the instance (discovery only). Returns
    // null when the callee isn't a `Type::Variant(args)` or a param can't be inferred (falls back to the error).
    SharedIdentifier inferInlineVariantInstance(InvocationNode* inv,
                                                std::map<std::string, SharedIdentifier>& localTys, bool reg);
    // The mangled tagged-union type of a value-producing `match` SUBJECT that is an inline variant ctor,
    // OR a variant-producing ternary / nested `match` over such ctors — resolved (and its instances
    // registered) at discovery so emitMatchSwitch, which can't re-infer it, has the subject's class. "" if none.
    std::string inferMatchSubjInst(SharedExpression subj);
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
    void registerInstColls();   // MCU 6b-1: register const-param-derived collection sizes (`InlineArray<T,(N+1)>`)

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
    // Reject a non-escaping borrow in a storage position. Contracts are always rejected; a `type view`
    // is rejected only when `alsoView` (the FIELD site) — a view MAY be returned (checked per-ReturnNode).
    void rejectStoredInterface(SharedIdentifier ty, const char* whereClause, int line, bool alsoView = false);
    std::string smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem = false);  // null the dtor's guard field
    // Cross-element smart-ptr UPCAST: widen a CONCRETE-element owning handle into a
    // CONTRACT-element (intrinsic fat) handle — the Liskov "is a" (`Shared<Shape> s = a;`
    // where `a: Shared<Sq>`). The concrete side is a library `Shared`/`Owned` struct (thin
    // `T*` + optional ctrl); the contract side is the intrinsic `{obj, vtbl, ctrl}`. `dst` is
    // the intrinsic type, `src` the library-owner lvalue.
    bool isSmartPtrUpcast(const std::string& dstTy, SharedExpression src);
    void emitSmartPtrUpcast(const std::string& nm, const std::string& dstTy,
                            SharedExpression src, int handoff, int depth, int line);
    // Base-class upcast: widen a `Shared`/`Owned` over a DERIVED class into one over a BASE
    // class (both thin library handles). Adjusts the pointer to the base subobject; a `Shared`
    // retains, an `Owned` moves. Safe because a `virtual class` has a virtual destructor
    // (drop dispatches to the most-derived via the vtable's `__dtor`).
    bool isSmartPtrBaseUpcast(const std::string& dstTy, SharedExpression src);
    void emitSmartPtrBaseUpcast(const std::string& nm, const std::string& dstTy,
                                SharedExpression src, int handoff, int depth, int line);
    // The pointee class of an owning handle — a library `Shared`/`Owned`/`Weak` or an intrinsic
    // contract handle; "" if `cls` isn't an owning handle.
    std::string ownerElem(const std::string& cls);
    // Both sides are owning handles over DIFFERENT elements, but it isn't a valid upcast (the
    // element isn't an `is a`, or the kinds differ) — used to give a clean diagnostic instead of
    // the misleading collection-hand-off message.
    bool isSmartPtrHandoffMismatch(const std::string& dstTy, SharedExpression src);
    // A move-only VALUE — a destructible class value that isn't a smart-ptr/collection/
    // extern struct. It MOVES on hand-off (its dtor is suppressed) and is never silently copied.
    bool isMoveOnlyValue(const std::string& cls) const;
    bool ownsByValue(const std::string& cls) const;   // move-only resource OR heap-owning collection/string (a by-value OWNING slot)
    // A move-only VALUE that opted into `Copyable` (a public nullary `copy` returning its
    // own type). Its presence makes the give/copy marker mandatory: bare hand-off = error, `copy`
    // deep-copies via copy(), `give` moves.
    bool isCopyable(const std::string& cls) const;
    bool satisfiesBound(const std::string& t, const std::string& bound) const;   // does concrete C-type `t` satisfy contract `bound`? (Copyable: value/primitive yes, resource iff it implements it)
    void markMoved(const std::string& cVar);                // state -> Moved
    void checkNotMoved(const std::string& cVar, int line);  // reject a use of a moved local
    // Move-state KEY for a drop-before-assign LHS: an unqualified local -> its name; a single-level
    // `local.field` member access -> "local.field"; anything else -> "". Lets the field-first-write
    // "release the old value" skip a not-yet-live (freshly zero-inited) move-only-value field slot.
    std::string lvalueMoveKey(SharedExpression lhs) const;
    // The source of a move hand-off: a bare move-only local -> its name (caller marks it moved);
    // a field/element/base member -> reject (moving out would leave the owner moved-from).
    std::string moveOnlySource(SharedExpression e, int line);
    // Reject a `give` whose source is an owning binding of a BORROWING `match (x)` arm (it aliases the
    // still-owned subject → double free). Returns true (and emits a hard error) when it fires.
    bool giveOfBorrowedBinding(SharedExpression e, int line);
    // Dispatch `recv.method(args)` on a smart-pointer receiver: an intrinsic
    // (lock/expired/valid) on the pointer itself, else auto-deref to the pointee.
    std::string emitSmartPtrCall(const std::string& cls, const std::string& recvExpr,
                                 const std::string& method, SharedArgumentList args, int srcLine,
                                 const IdentifierNode* site = nullptr);

    void linkBases();
    // Merge each contract's refined-parent methods (`type contract A implements B`) into its own `methods`
    // (transitive, cycle-safe), so a refining contract's vtable/conformance/dispatch include the parent slots.
    void linkContracts();
    void buildVtables();
    void computeDestructible();
    void computeReachesPointer();   // serialization mode gate — sibling of computeDestructible
    void computeReachesSharedWeak();   // channel-sendability gate — Shared|Weak-only sibling of reachesPointer
    void checkChannelSendability();    // reject a `channel<T>` whose T reaches a non-atomic shared refcount
    // M6.2: greatest-fixpoint dual of computeReachesPointer — mark every deeply/transitively immutable type
    // (the `immutable` qualifier verified) and error on a qualified type with a mutable part. A `Shared`/`Weak`
    // over such a T is sendable across isolates and uses the atomic refcount flavor.
    void computeDeeplyImmutable();
    bool deeplyImmutable(const std::string& cls) const;        // predicate: `cls` is a deeply-immutable class
    bool fieldTypeDeeplyImmutable(const SharedIdentifier& type) const;  // is a field/payload type immutable?
    bool isSharedOrWeakClass(const std::string& cls) const;   // an intrinsic/triad Shared or Weak (not Owned)
    bool isAtomicClass(const std::string& cls) const;         // an `Atomic<T>` instance (std::concurrent, M6)
    // By-value (tree) serialization intrinsic — direct C emission for a `@generate` struct (Phase C).
    void emitSerializeDefinition(ClassInfo& ci);
    void emitDeserializeDefinition(ClassInfo& ci);
    // `@generate(Format)` — the synthesized infallible field-dump `void T__format(T* self, Formatter* f)` and
    // its per-field writer (scalar -> a Formatter writeX, composite -> its own `__format`).
    void emitFormatDefinition(ClassInfo& ci);
    void emitFmtFieldWrite(SharedIdentifier ty, const std::string& access, int line);
    // `@generate(Equatable|Hashable)` — the derived memberwise `equals` / field-walked `hash`.
    void emitEqualsDefinition(ClassInfo& ci);
    void emitHashDefinition(ClassInfo& ci);
    std::string eqFieldTest(SharedIdentifier ty, const std::string& a, const std::string& b, int line);
    void emitFmtLiteral(const std::string& s);   // write a literal chunk via a kama_string temp + Formatter__writeStr
    // `@generate(of|zero)` bag ctors (M6): the C signature (`V V__of(f1…)` / `V V__zero(void)`) shared by the
    // prototype and the definition, and the synthesized memberwise/zero-init body. `which` is "of" or "zero".
    std::string bagCtorSig(const ClassInfo& ci, const std::string& which);
    void        emitBagCtorBody(ClassInfo& ci, const std::string& which);
    // A "bag" = a `value` whose every field is public — the only shape `of`/`zero` may be generated for.
    bool        isTransparentValue(const ClassInfo& ci) const;
    void emitEnumSerializeDefinition(ClassInfo& ci);     // externally-tagged {"tag":…[,"value":{…}]}
    void emitEnumDeserializeDefinition(ClassInfo& ci);
    void emitSerFieldWrite(SharedIdentifier ty, const std::string& access, int depth,
                           const std::string& resultCType);   // resultCType empty => graph-node/void context (sticky only)
    void emitDeFieldRead(SharedIdentifier ty, const std::string& dst, int depth,
                         const std::string& resultCType, const std::string& cleanup);
    std::string deReadExpr(SharedIdentifier ty);   // the `Deserializer` read expression for a field type
    bool isScalarDeType(SharedIdentifier ty);      // scalar/string field: bare sticky read (vs a fallible composite)
    // Graph (object-graph / pointer) serialization intrinsic — direct C emission (Phase D).
    void computeGraphNodeTypes();                   // closure over smart-ptr fields; sets isGraphNode + Shared<T> return
    void emitGraphNodeHelperProtos(ClassInfo& ci);  // T__serializeNode / T__allocShell / T__wireShell prototypes
    void emitGraphNodeHelpers(ClassInfo& ci);       // …their bodies
    void emitGraphSerializeDefinition(ClassInfo& ci);   // public serialize: {root,objects} envelope + drain
    void emitGraphDeserializeDefinition(ClassInfo& ci); // public deserialize: two-pass, returns Shared<T>
    // One field's graph-pointer classification (empty kind => not a smart-ptr edge).
    // kind: Shared/Weak/Owned. elemIsContract => the pointee is a contract (fat {obj,vtbl,ctrl} edge,
    // resolved to a concrete conformance vtable at both endpoints — see Phase E).
    struct GraphEdge { std::string kind; std::string elemC; bool optional = false; bool elemIsContract = false; };
    GraphEdge graphEdgeOf(SharedIdentifier ty);
    std::string graphWireName(const ClassInfo& ci);   // source type name for the wire `__type` tag
    void emitGraphRefRead(SharedIdentifier ty, const GraphEdge& e, const std::string& dst, int d);  // pass-2 wire one field
    void emitPolyContractResolvers();   // Phase E: per-contract nodeWriterFor / implVtbl dispatch helpers
    SharedIdentifier sharedTypeNode(SharedIdentifier elem);   // synth a `Shared<elem>` type node (for return types)
    SharedIdentifier optionalTypeNode(SharedIdentifier elem); // synth an `Optional<elem>` node (the `.as<T>()` result)
    SharedIdentifier ownedErrorTypeNode();                    // synth `Owned<Error>` (the boxed-error payload)
    SharedIdentifier resultOwnedErrorTypeNode(SharedIdentifier inner); // synth `Result<inner, Owned<Error>>` (the fallible-deserialize return type)
    SharedIdentifier resultUnitOwnedErrorTypeNode();          // synth `Result<Unit, Owned<Error>>` (the fallible-serialize return type)
    // box a sticky enum error (`DeError`/`SerError`) drawn from `errExpr` into an Owned<Error> (raw C); returns the temp.
    std::string emitStickyErrBox(int depth, const std::string& enumType = "DeError",
                                 const std::string& errExpr = "r.vtbl->errorCode(r.obj)");
    std::string emitAsDowncast(AsDowncastNode* ad);           // Model C `expr.as<T>()` -> Optional<T> (vtbl compare)
    std::string emitBitcast(BitcastNode* v);                  // `bitcast<T>(expr)` -> no-UB same-width union type-pun
    std::vector<std::string> _graphNodeOrder;       // graphNodeTypes in a stable order (for driver dispatch chains)
    // Contracts used as a graph edge element (`Shared<Shape>`): each gets a runtime-dispatch resolver pair.
    std::set<std::string> _polyContracts;
    // Poly-DISPATCH contracts (Model C, base `Error`): a contract that must support DYNAMIC dispatch +
    // boxing even for RETRO impls — the enum→interface capability. Set when an enum retro-implements a
    // contract (an enum can only implement via retro), and (P2+) when a contract is a `Result` E-arg or an
    // `Owned/Shared/Weak<C>` element. Distinct from `_polyContracts` (serialization graph edges). For such
    // a contract, `emitClassInterfaceVtables` emits `<Impl>__as_<C>` even for a retro impl (so an enum
    // gets a fat-pointer vtbl), and `rejectStoredInterface` treats a bare `C` in an owning slot as sugar.
    std::set<std::string> _polyDispatchContracts;
    bool isPolyDispatchContract(const std::string& c) const { return _polyDispatchContracts.count(c) != 0; }
    // The prelude triad's generic-TEMPLATE keys (`std::memory::{Shared,Owned,Weak}`), captured at collection.
    // A concrete-element triad instance (`Shared<Leaf>`) is an ordinary library generic instance (NOT
    // isIntrinsicColl), so pointer detection goes by template identity via `_genericTypeInstOf`.
    std::string _sharedTmpl, _ownedTmpl, _weakTmpl;
    // std::concurrent's channel-family generic-template keys, captured at collection (like the memory
    // triad above). Used by checkChannelSendability to find every `channel<T>` instantiation site.
    std::string _channelTmpl, _senderTmpl, _receiverTmpl;
    // std::concurrent's `Atomic<T>` generic-template key (M6). Captured at collection like the family above;
    // used to validate the element (integer/`Ptr` scalar only) and to exempt an `Atomic` from the
    // disjoint-borrow rule (several isolates may `ref`-borrow the SAME atomic cell — the sanctioned case).
    std::string _atomicTmpl;
    std::vector<ParamSig> paramSigsOf(SharedParameterList params);
    static bool isExtern(FunctionDeclarationNode* fn);
    static bool isExposed(FunctionDeclarationNode* fn);   // `expose fn` — kama→host C-ABI boundary

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
    ClassInfo* retroTargetInfo(const std::string& tkey);   // collection/primitive retro-conformance ClassInfo
    // AND over a `when [P: B, …]` gate: every gated param's concrete arg must satisfy its bound. `params`
    // are the template's type-param names, `concrete` the instance's args (index-aligned).
    bool whenConditionsHold(const std::vector<std::string>& whenParams,
                            const std::vector<std::string>& whenBounds,
                            const std::vector<std::string>& params,
                            const std::vector<SharedIdentifier>& concrete);
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
    // A non-escaping borrow: a contract (fat-ptr, borrows its object) OR a `type view` (borrows a raw
    // `Ptr<T>`). Both are rejected as a FIELD or COLLECTION ELEMENT — they'd dangle. (A view may still be
    // RETURNED when it borrows `this`/a `ref` param; that is checked per-ReturnNode, not here.)
    bool isNonEscapingBorrow(const std::string& name) const {
        if (isInterface(name)) return true;
        auto it = _classes.find(name);
        return it != _classes.end() && it->second.isBorrow;
    }
    std::string ifaceSlotSig(SharedParameterList params);     // "(void* self, T a, ...)"
    void emitInterfaceTypes(InterfaceInfo& ii);               // vtbl struct + fat-pointer struct
    void emitClassInterfaceVtables(ClassInfo& ci);            // the C__as_I instances
    // (I){ (void*)&<obj>, &<C>__as_I } — wrap a concrete lvalue as an interface value
    std::string fatPointer(const std::string& iface, const std::string& concrete, const std::string& addrExpr);
    // `site` is the method-NAME identifier at the call, for the reference index (M6 B3c) — the same
    // defaulted-trailing-parameter idiom emitDispatch/emitSmartPtrCall use.
    std::string emitInterfaceDispatch(const std::string& fatExpr, const std::string& iface,
                                      const std::string& method, SharedArgumentList args, int srcLine,
                                      const std::string& recvCType = "",
                                      const IdentifierNode* site = nullptr);

    // Declarations / top level
    bool paramByRef(FunctionParameterNode* p);
    bool paramIsOut(FunctionParameterNode* p);   // `out` — the write-only half of the by-pointer pair
    // `ownerCType` names the enclosing type when emitting a class member, so a `ref This`
    // SELF-borrow can be told apart from borrowing someone else's smart-pointer handle.
    std::string paramListC(SharedParameterList params, const char* selfType,
                           const char* ownerCType = nullptr);
    // `nameOverride`: emit under a supplied mangled name instead of the declared one
    // (used for generic instantiations, whose C name carries the concrete type args).
    void emitFunctionPrototype(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);
    void emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);

    // Classes
    bool isClass(const std::string& name) const { return _classes.count(name) != 0; }
    bool isBaseOf(const std::string& base, const std::string& derived) const;   // base in derived's chain
    std::string namespaceOfType(const std::string& value) const;  // `ns::path` of a registered type with bare name `value`, else "" (missing-import diagnostic)
    bool isTypeParamName(const std::string& n) const;              // `n` is a generic type-param (any template's, or an active binding)
    void checkTypeResolves(SharedIdentifier type, const std::string& cTypeResult,
                           const char* what, int line);  // unresolved type name -> missing-import / unknown-type diagnostic
    void checkDeclaredTypes(const std::vector<SharedCompilationUnit>& units);  // the same check over every DECLARED type (param/return/field)
    std::string ptrElemType(SharedExpression e);   // if `e` is a raw `this.field[i]` where field is Ptr<T>, the element C-type; else ""
    std::string ptrLocalElemType(SharedExpression e);  // if `e` is a bare-LOCAL `buf[i]` where buf is Ptr<T>, the element C-type; else "" (store-path only)
    std::string exprClass(SharedExpression e);          // class name of expr, "" if unknown/primitive
    std::string receiverScalarCType(SharedExpression e); // C scalar type of a primitive receiver place (`p.x`, `arr[i]`), "" if none
    bool exprIsChar(SharedExpression e);                // true iff `e`'s kama type is `char` (a char literal, local/param/foreach binding, or a char field)
    int holeBuiltinType(SharedExpression e);            // IDENTIFIER_*_VAL of an interp hole's numeric kama type (local/param/field/literal), 0 if unknown
    void emitHoleSpec(const std::string& fv, SharedExpression hole, const std::string& spec);  // format-specifier fast-path for `${x:spec}`
    void emitHoleInto(const std::string& fv, SharedExpression hole, SharedString spec);        // render one hole into Formatter `fv` (spec / char / Format dispatch); shared by plain + tagged interpolation
    std::string lvalueCType(SharedExpression e);        // C type of an lvalue local/param/field, KEEPING collection/string types
    bool exprIsString(SharedExpression e);              // true iff `e` statically has kama type `string` (kama_string)
    std::string hoistStringTemp(SharedExpression e);    // owned-string RVALUE -> a scope-dtor'd temp (frees it); "" for lvalue/literal/non-string
    void emitStruct(ClassInfo& ci);
    void emitVariantStruct(ClassInfo& ci);   // tag + union layout of a discriminated-union enum
    void emitClassPrototypes(ClassInfo& ci);
    void emitClassDefinitions(ClassInfo& ci);
    // Emits a method / operator / named-`ctor` body. There is no `isCtor` flag: a named `ctor` is a static
    // factory with no `self`, so it needs none of the instance-ctor prologue the flag used to select.
    void emitMethodOrCtorBody(const std::string& cName, const char* retType,
                              SharedParameterList params, SharedBlock body,
                              ClassInfo& owner, bool isConstMethod = false,
                              bool isStatic = false);
    std::string emitMemberAccess(MemberAccessNode* ma);
    std::string emitMethodCall(InvocationNode* call, MemberAccessNode* recv);
    bool        isTypeReceiver(MemberAccessNode* ma, std::string& outType);            // X.name -> X is a type?
    std::string newFactoryCall(const std::string& cls, ObjectCreationNode* oc, int lineNo);  // new Type.name(...) factory
    void        emitNewFactoryMove(const std::string& cls, const std::string& slotPtr,  // new Type.name(...) construct
                                   ObjectCreationNode* oc, int lineNo, int depth);
    bool        ctorIsFallible(ObjectCreationNode* oc);                                 // new Type.name(...) ctor returns Result?
    std::string emitFallibleNewBox(const std::string& target, const std::string& lval, // M4b: fallible new -> Result<Owned<T>,E>
                                   ObjectCreationNode* oc, int srcLine);
    std::string emitTryNewBox(const std::string& target, const std::string& lval,      // M-step5: try new -> Optional<Owned<T>>
                              ObjectCreationNode* oc, int srcLine);
    // Model C (P2): box an enum VALUE (`enumCType`, given by `enumValExpr`) into an `Owned<C>`/`Shared<C>`
    // fat handle (`ownedCType`, C a poly-dispatch contract), heap-copying the enum in. Emits the
    // malloc+move+vtbl[+ctrl] as a HOISTED statement (needs a statement slot) and returns the temp name.
    std::string emitEnumBoxIntoContract(const std::string& ownedCType, const std::string& enumCType,
                                        const std::string& enumValExpr, int srcLine);
    // The enum cType a variant literal names (`IoError::NotFound` / `IoError::Other(...)`), or "" if `e`
    // isn't a variant reference. `exprClass` returns "" for a variant literal (its type comes from context),
    // so Model-C error boxing resolves the source enum through this.
    std::string variantExprEnumCType(SharedExpression e);
    std::string emitDotOnTypeCtorCall(InvocationNode* call, MemberAccessNode* recv, const std::string& typeName);
    // Does an invocation (a free-fn / method call) return a PLACE (`fn ref T` — a borrow), not a fresh
    // owned value? Used to gate materialize-and-drop of an owned rvalue receiver: a place must NOT be
    // dropped (dropping a copy of a borrow would double-free). isPlaceReturn is the discriminator.
    bool invocationReturnsPlace(InvocationNode* iv);
    // Dispatch a call on a receiver of static class `clsName`, given the C pointer
    // expression `recvPtr` (e.g. "self" or "&(c)"): virtual -> via __vptr; else direct.
    // `site`, when non-null, is the source identifier the method was spelled at: the resolved method is
    // recorded against it for the reference index (M6 B3). Defaulted, like resolveUserName/resolveFunc,
    // so the compiler-internal callers with no user spelling in hand are unaffected.
    std::string emitDispatch(const std::string& clsName, const std::string& recvPtr,
                             const std::string& method, SharedArgumentList args, int srcLine,
                             const IdentifierNode* site = nullptr);
    // Nothing is constructible by default: reject a nameless `Type(...)`/`new Type(...)`, with advice that
    // offers `of`/`zero` only for a transparent value. True if it rejected. Exempts intrinsic/extern.
    bool rejectNamelessConstruction(const ClassInfo& ci, const std::string& disp, bool viaNew, int srcLine);
    void checkNamelessNewBanned(ObjectCreationNode* oc, int line);   // the `new` entry point into the above

    // Statements
    void emitStatement(SharedStatement stmt, int depth);
    void emitAsm(AsmNode* a, int depth);   // `asm("...")` -> `__asm__ __volatile__("..." : : : "memory")` (MCU 6a)
    // std::log v2 (M7): recognize a `logError/Warn/Info/Debug/Trace(tag:, msg:)` facade call statement and
    // lower it in place — compile-strip below the floor + a runtime `kama_log_enabled` guard with the message
    // built INSIDE it (zero cost when filtered). `logFacadeLevel` returns true + the level ordinal (0..4) iff
    // `iv` resolves to a std::log facade fn (in a log-importing program) with both `tag:`/`msg:` present.
    bool logFacadeLevel(InvocationNode* iv, int& level);
    void emitLogFacade(InvocationNode* iv, int level, int depth);
    // Bind a facade string argument to a stable `kama_string` lvalue and return its temp name (usable as a
    // span via `.data`/`.len` and by address via `&`); an owned rvalue (interpolation/concat) becomes a
    // scope-dtor'd temp (dropped via dropCondTemps at the guard/wrapper it was hoisted into), a literal/lvalue
    // is a borrow temp (no drop). Setup is flushed into `_hoisted` at `depth`.
    std::string logSpanOf(SharedExpression e, int depth);
    std::string isolatePrep(IsolateNode* iso, std::string& cls, std::string& val,
                            bool& isBorrow, bool borrowOK);   // shared front half (borrow = M4.2 `ref`)
    void emitIsolate(IsolateNode* iso, int depth);   // `spawn worker(p: give x);` — deferred-join scope child (M4)
    std::string emitIsolateExpr(IsolateNode* iso);   // `Isolate h = spawn worker(...)` — RAII handle form
    void emitScope(ScopeNode* sc, int depth);        // `scope { }` — structured concurrency + join barrier (M4)
    void emitParallelFor(ParallelForNode* pf, int depth);   // `parallel_for (ref T e in coll) { }` — disjoint-slice data-parallel (M6.3)
    SharedIdentifier parforViewType(SharedIdentifier elem);  // synthesize the `View<elem>` type node the loop iterates (M6.3)
    Scope* innermostTaskScope();                     // nearest enclosing `scope { }`, or null
    int    innermostTaskScopeIndex();                // its _scopes index, or -1
    int    findScopeDeclaring(const std::string& name);   // _scopes index that declares `name`, or -1 (a param)
    void emitBlock(BlockNode* block, int depth);
    void emitBlockScoped(BlockNode* block, int depth, bool loopBoundary, bool functionRoot);
    void emitBody(SharedStatement stmt, int depth, bool loopBoundary);  // brace-wrapped control-flow body
    std::string inlineStatement(SharedStatement stmt);         // for-clause form (no ; / newline)
    std::string emitForClause(SharedStatementList list);       // comma-joined inlineStatements

    // RAII cleanup
    void emitScopeCleanup(const Scope& s, int depth);          // reverse-order dtors for one scope
    void dropCondTemps(size_t preLoc, int depth);              // drop+unregister a condition's hoisted temps
    void emitUnwindToLoop(int depth);                          // break/continue: innermost..loop boundary
    void emitUnwindAll(int depth);                             // return: innermost..function root
    void recordDestructibleLocal(const std::string& cVar, const std::string& className);
    static bool stmtIsJump(SharedStatement s);                 // direct return/break/continue
    static bool bodyDiverges(SharedStatement s);               // body ends in return/break/continue
    void emitDtorDefinition(ClassInfo& ci);

    // Expressions -> C expression text
    static std::string cEscapeStringBody(const std::string& s);   // escape a string's bytes for a C `"..."` body (no quotes/wrapper)
    std::string emitExpression(SharedExpression expr);
    std::string emitInterpolation(InterpolatedStringNode* is);   // `"a ${x} b"` -> a hoisted Formatter build
    std::string emitTaggedInterpolation(InterpolatedStringNode* is);   // `tag"a ${x} b"` -> a Template + a `<tag>(ref Template)` call
    std::string emitInvocation(InvocationNode* call);
    std::string emitVariantConstruction(ClassInfo& ci, const std::string& variant,
                                        SharedArgumentList args, int srcLine);
    // If `e` is an inline construction for exactly `targetCType` in a hoist-enabled context,
    // materialize a preceding temp (ISO C, no `({…})`) and return its name; else "". Generalizes the
    // argument-position lowering to any value site (return, variant payload, …).
    std::string tryHoistInlineCtor(SharedExpression e, const std::string& targetCType, int srcLine);
    std::string tryHoistInlineNew(SharedExpression e, const std::string& targetCType, int srcLine);
    // Materialize `value` (unwrapping a give/copy marker; resolving an inline ctor/`new`/bare-generic-ctor
    // from `dstCType`) and assign it into the already-declared lvalue `dst` of type `dstCType`, applying the
    // give/copy matrix for an OWNED value (smart-ptr / resource / collection / bindable — move consumes the
    // source, copy duplicates). Shared by `return` and value-producing `match` arms (`:= give x` / `:= List()`).
    void emitOwnedValueInto(const std::string& dst, const std::string& dstCType,
                            SharedExpression value, int line, int depth);
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
    std::map<std::string, int64_t> _constLocalVals;           // 6b-2: local `const` name -> folded int (comptime uses: sizes/fills)
    std::map<std::string, int64_t> _moduleConsts;             // 6b-2: module `comptime` qualified name -> folded int
    std::set<std::string> _constStatics;                      // qualified names emitted as C `static const` (every `comptime` static)
    // 6b-2: a type-associated `comptime` constant (`Type::NAME`). Keyed "<qualifiedClass>::<name>".
    struct TypeConstInfo { bool hasValue; int64_t value; Visibility visibility; std::string owner; std::string cName; SharedIdentifier type; SharedExpression initializer; int line; };
    std::map<std::string, TypeConstInfo> _typeConsts;
    // const-eval 6b-3: `comptime fn` registry — compile-time-only functions the interpreter runs. Free
    // fns are keyed by qualified name; type-associated ones as "<qualifiedClass>::<name>". NOT in _funcs:
    // a comptime fn is never emitted as a C symbol (comptime-only model). Populated in collect passes.
    std::map<std::string, FunctionDeclarationNode*> _comptimeFns;
    // Type-associated `comptime fn` (`Type::name()`), keyed "<qualifiedClass>::<name>", with its visibility
    // (default private — a comptime fn is scoped/access-restricted by default) and owning type.
    struct ComptimeMethod { ClassMethodDeclarationNode* node; Visibility vis; std::string owner; };
    std::map<std::string, ComptimeMethod> _comptimeMethods;
    std::string _ctCurrentOwner;   // type whose comptime fn body is evaluating (for private-visibility checks)
    bool isComptimeFnName(const std::string& name, SharedStringList qualifier, std::string& outKey) const;  // runtime-call rejection
    // Purity is enforced structurally at evaluation time (the interpreter has no case for an impure
    // node → a clean "unsupported in comptime fn" diagnostic), the C++ constexpr model. See kama.comptime.cpp.

    // --- const-eval 6b-3: the comptime interpreter (kama.comptime.cpp) -----------------------------
    // A compile-time value the interpreter computes. Integers are held in int64 (like constValue) with a
    // width/sign tag so wrap happens on cast + typed store, exactly as the emitted C would (see ctTruncate).
    struct CTValue {
        enum Kind { Int, Float, Bool } kind = Int;
        int64_t i = 0;        // Int / Bool payload
        int     width = 32;   // Int declared width in bits (8/16/32/64) — drives wrap on store/cast
        bool    isSigned = true;
        double  f = 0.0;      // Float payload
        bool    isF32 = false;
        std::vector<CTValue> elems;   // Stage 3: InlineArray<T,N> element values (kind is unused when set)
        bool isArray = false;         // Stage 3: this value is a fixed array (elems holds the elements)
        std::string elemCType;        // Stage 3: element C type, for baking `static const T name[N] = {…}`
    };
    struct CTEnv { std::map<std::string, CTValue> vars; };   // one comptime-fn call frame (its locals)
    enum class CTFlow { Normal, Return, Break, Continue, Fail };
    long _ctSteps = 0;      // step budget consumed by the current top-level comptime evaluation
    int  _ctDepth = 0;      // comptime-fn call-recursion depth
    bool _ctFailed = false; // a comptime diagnostic was already emitted this evaluation
    static const long CT_STEP_BUDGET = 1000000;   // runaway guard (cf. C++ constexpr-step limit)
    static const int  CT_MAX_DEPTH   = 256;
    // Baked comptime-fn-derived constant values, keyed by qualified cName; consulted at the const emit site.
    std::map<std::string, CTValue> _comptimeConstVals;
    std::set<std::string> _ctErroredConsts;   // consts whose interpreter eval already errored (suppress a duplicate emit-time diagnostic)
    // Module `comptime` consts whose fold needed the interpreter (a `comptime fn` call), in declaration order.
    struct CTDeferredConst { std::string cName; SharedIdentifier type; SharedExpression init; NsCtx ctx; int line; };
    std::vector<CTDeferredConst> _ctDeferredConsts;

    void evalComptimeConsts();                                                    // the deferred-const evaluation pass
    bool ctEvalCall(FunctionDeclarationNode* fn, const std::vector<CTValue>& args, int line, CTValue& out);
    bool ctEvalBody(SharedParameterList params, SharedBlock body, SharedIdentifier retType,
                    const std::vector<CTValue>& args, int line, CTValue& out);    // shared core for free fns + methods
    bool ctEvalExpr(SharedExpression e, CTEnv& env, CTValue& out);
    CTFlow ctEvalStmt(SharedStatement s, CTEnv& env, CTValue& ret);
    bool ctResolveConst(SharedIdentifier id, CTValue& out);                       // module/type comptime const -> CTValue
    bool ctTypeInfo(SharedIdentifier type, CTValue& proto);                       // Kama scalar/array type -> CTValue shape
    bool ctArrayInfo(SharedIdentifier type, CTValue& elemProto, int64_t& n, std::string& elemCType);  // InlineArray<T,N> shape
    bool ctBuildArrayInit(SharedExpression init, CTEnv& env, const CTValue& elemProto, size_t n, std::vector<CTValue>& out);  // [v;N] / [a,b,c]
    void ctCoerce(const CTValue& proto, CTValue& v);                              // coerce v to proto's kind/width (typed store)
    void ctTruncate(CTValue& v);                                                  // wrap an Int to its declared width
    bool ctFail(const char* what, int line);                                      // emit a comptime diagnostic, mark failed
    std::string ctRender(const CTValue& v) const;                                 // scalar -> C initializer text
    static double ctAsF(const CTValue& v);
    static int64_t ctAsI(const CTValue& v);
    std::string rootBinding(SharedExpression e) const;        // the root identifier a write targets
    // View-return escape check (B4): the root a returned view ultimately BORROWS. `viewReturnRoot`
    // dispatches on the return form (view ctor / chained call / bare place); `borrowArgRoot` traces a
    // view-ctor's borrowed-pointer argument through `addr(of: …)` and a `recv.dataPtr()` call.
    std::string viewReturnRoot(SharedExpression e) const;
    std::string borrowArgRoot(SharedExpression e) const;
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
    // Never-null definite assignment for `Owned`/`Shared` fields (Stage 1): each must be set before the
    // ctor returns and never read before it is set. `Weak` is exempt (nullable). v1 = straight-line.
    std::string ctorFieldRef(SharedExpression e, ClassInfo& owner, const std::set<std::string>& locals);
    void        scanOwningReads(SharedExpression e, ClassInfo& owner, const std::set<std::string>& owning,
                                const std::set<std::string>& assigned, const std::set<std::string>& locals,
                                std::string& bad, int& badLine);
    void        analyzeCtorStmt(SharedStatement st, ClassInfo& owner, const std::set<std::string>& owning,
                                std::set<std::string>& assigned, std::set<std::string>& locals, bool topLevel);
    void        checkNamedCtorComplete(ClassInfo& owner, SharedBlock body);
    void        checkViewCtorEscape(ClassInfo& owner, ClassMethodDeclarationNode* mnode);   // a view ctor may only borrow its params
    // Construction-model M8b: a value field may be left unassigned in a ctor iff its type is DEFAULT-FILLABLE
    // (a primitive / raw `Ptr` — zero is a valid value; an intrinsic collection — zero is a valid empty; or a
    // type with an explicit `default` ctor). Otherwise it must be explicitly assigned. `concreteCType` is the
    // field's type ALREADY resolved to its concrete C name (under the active _typeSubst / per instance).
    bool        isDefaultFillable(const std::string& concreteCType);
    // Owning read-before-assign is a compile error. `params` (optional) brings the `out` parameters into
    // the analysis: they start UNASSIGNED, so reading one is an error and every return must have filled it.
    void        checkDefiniteAssignment(SharedBlock body, SharedParameterList params = SharedParameterList());
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
    // Field-wise init of an extern (C-POD) struct from NAMED args (`nm.f = e; …`). The struct is
    // already `= {0}`, so only provided fields are set; unknown field / positional arg = clean error.
    std::string externAggregateInit(const std::string& nm, ClassInfo& ci,
                                     SharedArgumentList args, int srcLine);

    // Helpers
    std::string cType(SharedIdentifier type);
    std::string cFunctionName(const std::string& kamaName);   // main -> kama_main
    std::string mangledFunctionName(FunctionDeclarationNode* fn, bool& isEntryPoint);
    std::string binaryOperator(int token);
    static bool isComparisonToken(int token);   // `==`/`!=`/`<`/`>`/`<=`/`>=` — the contract-driven six
    std::string assignmentOperator(int token);
    // Render an expression back to READABLE KAMA source text (not C) for diagnostic messages — the
    // auto-stringified condition in `assert(cond: …)`. Pure (no emitter side effects, never calls
    // emitExpression); covers the forms that appear in conditions and returns "" for anything else so
    // the caller degrades to a bare "assertion failed". Reusable for future compiler diagnostics.
    std::string unparseExpr(SharedExpression expr);
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
    std::string dotCtorFactoryClass(SharedExpression e); // the class if `e` is a dot-on-type ctor call `T.make(…)`, else "" (arg path only)
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
    void warning(const char* what, int srcLine);   // soft: reported, does NOT fail the build

    // MCU step 4: lower `@interrupt` / `@section(".x")` to a C `__attribute__((...))` prefix.
    // `fn` is null for a module static (which accepts `@section` only).
    std::string declAttrPrefix(const SharedAttributeList& attrs, FunctionDeclarationNode* fn, int line);
    bool fnHasNoHeap(FunctionDeclarationNode* fn) const;                 // does this fn carry `@noheap`?
    void rejectIfNoHeap(const char* what, int line);                    // the ONE no-heap gate (`--no-heap`/`@noheap`)

    // Multi-file: collect a whole program, then emit declarations (shared
    // header) and definitions (per module) separately.
    void collectProgram(const std::vector<SharedCompilationUnit>& units);
    void emitHeaderContent(const std::vector<SharedCompilationUnit>& units);  // typedefs/structs/protos/macros
    void emitModuleContent(SharedCompilationUnit unit);                       // this file's vtables + defs
    void emitModuleStaticDecl(ModuleVariableDeclaration* mv);                 // MCU step 1: module-level `static`
};

#endif // __KAMA_CEMIT_H__
