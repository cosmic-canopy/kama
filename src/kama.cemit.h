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
#include <functional>  // the package resolver the driver installs (setPackageResolver)
#include <cstdint>     // fixed-width ints — not transitive on all libcs (e.g. Windows UCRT)
#include "kama.forward.h"
#include "kama.diagnostic.h"   // structured Diagnostic accumulated by unsupported() (query surface)
#include "kama.query.h"        // LSP query surface: SrcRange / DefSite / PosEntry / SymbolInfo / Location

// ---------------------------------------------------------------------------------------------
// INHERITANCE — a build-time feature switch, not a runtime one.
//
//     make                      # inheritance in
//     make KAMA_INHERITANCE=0   # a compiler built without it
//
// Two purposes, and only a preprocessor gate serves either:
//
//   1. ISOLATE THE SIZE DELTA. "What does inheritance cost the compiler?" is answerable only by
//      building the compiler both ways and subtracting. A runtime flag leaves every byte in place.
//   2. BE THE EXTRACTION POINT. kama is still deciding whether to keep inheritance at all
//      (docs/design/inheritance.md). If the answer becomes no, the `#if KAMA_INHERITANCE` blocks ARE
//      the deletion list — mechanical, complete, and already proven to compile without their contents.
//
// Purpose 2 is why the gate must stay honest: `tools/check-no-inheritance.sh` builds the KAMA_INHERITANCE=0
// compiler and exercises it, because an untested build variant rots within weeks.
//
// SCOPE: the emitter only. The grammar still PARSES `extends`/`virtual`/`base` in an inheritance-free
// build and the emitter answers with a real diagnostic, rather than the syntax error that removing the
// productions would give. That is also where the bytes are — the parser tables are generated either way,
// so gating the grammar would buy almost no delta for real bison risk (every `_opt` rule must set `$$`).
// Removing the grammar later is 5 tokens, 3 productions and 3 AST node types.
#ifndef KAMA_INHERITANCE
#define KAMA_INHERITANCE 1
#endif

// The CEILING on how many `extends` hops a class may sit below its root. Each `virtual`/`abstract class`
// still has to state its OWN budget (`virtual(maxDepth: N)`), which may not exceed this; a type whose
// budget reaches 0 must be written `final`. The compiler could infer that last step and deliberately does
// not — a designer should meet the limit as an intentional marker on the type they are writing, not as a
// surprise the first time they try to extend it once more.
//
// 2 — a root, a middle layer and a leaf — because that is what mainstream OOP designs actually use, and
// stopping short of it would push a shape people legitimately want into composition for no gain. Beyond
// it the failure modes turn asymmetric: too strict pushes a middle layer into COMPOSITION, which this
// design is happy with, while too loose grows the deep hierarchies the restriction exists to prevent.
//
// ⚠️ Objects do NOT depend on the Makefile, so changing this needs a clean rebuild of out/<platform>/
// (unlike KAMA_INHERITANCE=0, which gets its own directory).
#ifndef KAMA_INHERIT_DEPTH
#define KAMA_INHERIT_DEPTH 2
#endif
// ---------------------------------------------------------------------------------------------

// A function parameter, in declared order. Named kama arguments are matched
// against these to recover C's positional order at each call site.
struct ParamSig {
    std::string name;
    // DEFAULTED, and it has to be: a default-constructed `ParamSig` left this indeterminate, so a
    // synthesized signature built field-by-field passed `&i` where the runtime wanted a `size_t`. It
    // compiled clean on one host and failed on another, which is the worst way to find out.
    bool        byRef = false;   // ref/out => passed as a pointer (call site emits &arg)
    std::string className;    // class type (for ref upcast at call sites), "" if primitive
    // `out T x` — a WRITE-ONLY borrow: the callee must assign it on every path before returning, and may
    // not read the incoming value. Lowers identically to `ref` (a `T*`); the difference is entirely in the
    // rules, which is why the call site must SAY `out` (see emitReorderedCall) — the marker is what lets
    // the caller's definite-assignment analysis mark an otherwise-unassigned local live across the call.
    bool        isOut = false;
    bool        isConst = false;   // `const` param — emits `const T*` for FFI pointers
    bool        isHardware = false; // `hardware UnsafePtr<T>` param — emits `volatile T*` for MMIO
    // The parameter's declaration identifier — the SAME node registerBinding keys its DefSite on, which
    // is what lets a call-site LABEL be indexed as a reference to it (LSP M6 A2). Analysis-only; null for
    // the synthesized signatures of string/collection intrinsics, which have no source declaration.
    const IdentifierNode* declSite = nullptr;
    // The C type for the KIND rule alone. `className` cannot carry it for a SYNTHESIZED intrinsic
    // signature, because that field is overloaded: `ownsByValue(p.className)` is what makes a
    // by-value collection argument demand `give`/`copy`, and the read-only `kama_string__*` intrinsics
    // are deliberately exempt from that — they borrow. Naming their parameter `kama_string` to teach
    // the kind rule made 51 fixtures demand a hand-off marker for `s.contains(substring: t)`. So the
    // type checker gets a field that means exactly one thing. Empty => fall back to `className`,
    // which for every DECLARED parameter already IS the C type spelling (`paramSigsOf`).
    std::string kindCType;
};

struct FuncSig {
    std::string            cName;    // mangled C name (e.g. main -> kama_main)
    std::string            retCType; // resolved C return type (signature check)
    std::vector<ParamSig>  params;
    bool                   isPlaceReturn = false;  // `fn ref T …` — returns a place (T*), deref'd at the call site
    bool                   isUnsafe = false;       // `unsafe fn …` — the body may touch raw memory
    // The declaration site. Read by the LSP def-site table, and by `resolveFnPtrTarget` for the one
    // question a bind needs and `FuncSig` does not otherwise carry: whether the function is `@noheap`.
    FunctionDeclarationNode* node = nullptr;
    // The unit that DECLARED it. `node` carries a line but no file, so without this a diagnostic about
    // two declarations can only name one of them — which is what made "first declared at line 2" useless
    // once the two `main`s moved into different directories. Same record `ClassInfo::declFile` keeps for
    // a type and `_genericDeclFile` for a generic; filled from `_collectingUnitPath` at the same point.
    // It is also the file rung's key: visibility is per FILE, so "may this name reach here" is answered
    // by comparing this against the referencing unit (see `checkReach`).
    std::string            declFile;
};

// A function-pointer signature type: a bodiless `fn ret Name(params);`.
// Lowers to `typedef ret (*cName)(paramtypes);`. FunctionPtr<Name> spells `cName`.
struct SigInfo {
    std::string            cName;       // typedef name (namespace-mangled)
    std::string            retCType;    // resolved C return type (for the typedef)
    std::vector<ParamSig>  params;      // names (named-arg invoke) + C types (className)
    std::string            declFile;    // the unit that declared it — the file rung's key (see `checkReach`)
                         // `@noheap` on the `fnptr` declaration. Signature-level for the same reason
                         // `InterfaceMethod::noHeap` is: a call through the pointer is dispatched to a
                         // target the compiler cannot see, so inference stops at the slot. Declaring it
                         // is what lets the proof cross — the signature PROMISES the callee allocates
                         // nothing, every function bound to it is checked against that promise, and a
                         // `@noheap` caller may then call through it.
    bool                   noHeap = false;
                         // The THREADING contract of the C API this signature is handed to (ROADMAP row
                         // 1). A callback crossing to C must say which, and it is a declaration rather
                         // than an inference because the compiler genuinely cannot know: `kama_run_loop`
                         // is `while (tick(state)) { }` and CoreAudio's render callback is another
                         // thread, and nothing in either signature says so. `@foreignEntry` makes every
                         // function bound to this type a region the module-static check walks;
                         // `@callerThread` says the callee runs on the calling isolate and nothing
                         // changes.
    bool                   foreignEntry = false;
    bool                   callerThread = false;
};

// What a `fnptr`-typed destination is being bound to, as resolved by `resolveFnPtrTarget`. One record
// for the two paths that must agree — `emitFnPtrBind` lowers it, `checkFnPtrValueBind` judges it — so a
// bind position cannot be lowered by one and checked by neither.
struct FnPtrTarget {
    enum Kind { None, Function, Method, SigValue };
    Kind                    kind = None;
    std::string             cName;        // the C symbol that decays to the pointer (Function/Method)
    FuncSig                 sig;          // receiver-first signature, to compare against the fnptr
    std::string             display;      // "function 'dbl'" / "method 'Vec2::dot'" — diagnostic subject
    std::string             name;         // the bare spelling, for "declare `dbl` `@noheap` too"
    std::string             mismatchNote; // the extra clause a METHOD's shape error needs, else ""
    bool                    noHeap = false;   // the target's own `@noheap` declaration
    IdentifierNode*         id   = nullptr;   // the name node — LSP reference recording, EMIT path only
    ASTNode*                node = nullptr;   // ...and the declaration it refers to
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
    // The field's C type, resolved ONCE in the DECLARING class's scope by bakeFieldCTypes(). "" = not
    // baked (a type parameter, or a class registered after the pass), which `fieldCType` falls back on.
    // A field's type is a fact about its class, never about whoever reads it — see bakeFieldCTypes.
    // Sibling of ParamSig::className and ClassInfo::tagCType, which bake the same kind of answer.
    std::string      cTypeBaked;
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
// from a user `type <kind> Name` marker. `Neutral` is the kind for compiler-built types with no
// marker (collections, smart-ptrs, tagged-union enums); their ownership is driven by their own
// machinery (isIntrinsicColl/isSmartPtr/isVariant + destructibility), not the kind.
//
// NOTE `Neutral` is unrelated to the `type intrinsic <…> implements C` SURFACE syntax, which confers
// a contract on a primitive and creates no ClassInfo in `_classes` at all. It was spelled `Intrinsic`
// until that syntax existed; the two never named the same concept.
enum class TypeKind { Value, Resource, Contract, Neutral };

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
    bool                         noHeap = false;      // `@noheap …` — see InterfaceMethod::noHeap. On a
                                                      // VIRTUAL method it is the same promise across the
                                                      // same kind of blind slot, so an override inherits
                                                      // the obligation exactly as it inherits `const`.
    bool                         isUnsafe = false;    // `unsafe fn …` — the BODY may touch raw memory (C#'s
                                                      // meaning). Calling one is unrestricted; it is the
                                                      // signature, not the marker, that bounds the danger.
    Visibility                   visibility = Visibility::Private;
    bool                         isFinal = false;     // `final fn` — seals a virtual slot
    bool                         isStatic = false;    // `static fn` — no implicit `self`; called `Type::m(...)`
    bool                         isCtor = false;      // a named constructor (`ctor name(…)`) — a static factory
                                                      // returning the enclosing type (or `Result<This,E>`)
    bool                         isDefaultCtor = false; // `default ctor …()` — the canonical zero-arg ctor (M8b);
                                                        // the field-fill target for complete-init (Part 2). Explicit only.
    // The contract this method came from, empty for a method declared in the type's OWN body. It is what
    // separates an injected method from a native one sharing a ClassInfo — `string` carries an injected
    // `compareTo` beside its built-in `equals` — which is the discriminator the contract-scope rule needs,
    // and the one that says "supplied by a `type intrinsic` block", so the impl passes emit it rather than
    // the per-class proto/body loops.
    std::string                  fromContract;
    // Compiler-synthesized by-value serialization (a `@generate` tree struct with no hand impl). `node` is
    // null: the proto/body loops skip these and emit via emitSerializeDefinition/emitDeserializeDefinition.
    bool                         isSynthSer = false;  // synthesized `serialize(ref Serializer)`
    bool                         isSynthDe  = false;  // synthesized static `deserialize(Deserializer) -> This`
    // Compiler-synthesized `@generate(Formattable)` field-dump `format(ref Formatter)`. `node` is null: the
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
// `Simd` and `Mask` are here rather than beside `Fixed` for one reason worth stating: they are the only
// kinds whose C type is NOT a struct. It lowers to a `vector_size` typedef over a primitive, so it needs no forward
// `typedef struct`, has no by-value struct dependency, and its `_TYPE` goes out in the EARLY types pass
// where `Fixed`'s cannot. Every `collKind == Fixed` test in the struct-ordering passes is therefore a
// test these kinds must NOT accidentally join — and every `!= Fixed` test is one they must not join
// EITHER, which is what `isValueVectorKind` below is for. A `Mask` is a `Simd` in every structural
// respect; it is a separate kind so that `select` cannot be handed a data vector.
enum class CollKind { String, Owned, Shared, Weak, Bindable, Fixed, Simd, Mask };
// A `Simd` and a `Mask` are both bare `vector_size` typedefs over a primitive: no struct, no forward
// declaration, no by-value dependency, no heap, nothing to drop. Every rule that asks "is this an
// intrinsic collection that owns something" must answer NO for both, and asking it as `!= Fixed` — which
// several sites did — silently answered yes. Use this instead of naming the kinds.
inline bool isValueVectorKind(CollKind k) { return k == CollKind::Simd || k == CollKind::Mask; }

// Per-file namespace context. A file with `namespace X;` is public (scope
// = mangled X); a file without one is private (scope = "_F<file>"). Bare names
// resolve to the file's own scope, then its `using`s — never another file's
// private symbols (private-by-default).
struct NsCtx {
    std::string scope;        // mangle prefix: "Graphics" or "_F<file>"
    // The file this context belongs to. Visibility is per FILE, not per module, so "may this reference
    // reach that declaration" is answered by comparing this against the symbol's `declFile`. It travels
    // in NsCtx rather than being read from `diagFile()` because every pass already installs the right
    // NsCtx per unit — including generic instantiation, which restores the TEMPLATE's context so the
    // template body's references are judged from the file that wrote them, not the one that used them.
    // Empty for the prelude, which is compiler-owned and exempt.
    std::string unitPath;
    // The module this file is in, UNMANGLED (`std::collections`), as the driver names it. `scope` holds the
    // mangled form and cannot be turned back: a kama identifier may contain `__` (`_Hidden` -> `_F<file>___Hidden`),
    // so the join is not injective. Empty for a file in no module.
    std::string module;
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
    TypeKind                          kind = TypeKind::Neutral;   // set to value/resource for user types
    // `type view` — a non-escaping, stack-only borrow (C# `ref struct`): codegens like a `value`
    // (inline, owns nothing, no dtor) but the escape checker forbids it as a return/field/collection
    // element (like a `contract`). It borrows raw `UnsafePtr<T>` it does not own; see isNonEscapingBorrow.
    bool                              isBorrow = false;
    std::vector<FieldInfo>            fields;      // declaration order
    std::set<std::string>            fieldNames;
    std::set<std::string>            constFields;   // `const` data members — write-once in the ctor
    // `@generate(Serializable|Deserializable)` opt-in (pay-for-what-you-use): only set for a marked type; drives
    // emission of the reflective `__serialize`/`__deserialize` helpers (see emitSerialize/DeserializeDefinition).
    bool                              genSerialize = false;
    bool                              genDeserialize = false;
    // `@generate(of|zero)` — bag-only opt-in ctors on a TRANSPARENT value (all public fields, see
    // isTransparentValue). `of` = a synthesized memberwise ctor `V.of(f1: …, …)`; `zero` = a zero-init ctor
    // `V.zero()`. Both synthesize a named `ctor` (registered in `ctors`/`methods`) whose body is emitted by
    // emitBagCtorDefinitions. See the construction-model campaign (M6).
    bool                              genOf = false;
    bool                              genZero = false;
    // `@generate(Formattable)` — opt-in synthesized field-dump `Formattable` impl (`Type { f: v, … }`), infallible;
    // the display analog of genSerialize. Body emitted by emitFormatDefinition.
    bool                              genFormat = false;
    // `@generate(Equatable|Hashable)` — opt-in memberwise `equals` / field-walked `hash`, plus the nominal
    // conformance (so `==` lowers to it and a `<K: Hashable + Equatable>` bound is satisfied). Structural
    // equality stays a deliberate NON-default: you ask for it. Bodies: emitEqualsDefinition/emitHashDefinition.
    bool                              genEquatable = false;
    bool                              genHashable = false;
    // `@align(N)` / `@packed` — LAYOUT CONTROL, passed through to the C compiler as
    // `__attribute__((aligned(N)))` / `((packed))` on the emitted struct. kama does not own layout (it
    // emits C; the C compiler lays the struct out) and deliberately does not acquire a second source of
    // truth that could disagree per target — these state the constraint and `sizeof`/`alignof`/
    // `comptime assert` verify what the toolchain actually did. 0 / false = say nothing.
    int                               alignN = 0;
    bool                              packed = false;
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
    // An enum promoted to a variant ClassInfo has NO `ClassDeclarationNode` — its declaration site is an
    // `EnumDeclarationNode`. Anything reaching for a line number or a member list must consult this when
    // `node` is null, or it dereferences null (which `emitClassInterfaceVtables` did for an unknown
    // contract on an enum).
    EnumDeclarationNode*              enumNode = nullptr;
    int  declLine() const;   // out-of-line: both node types are only forward-declared here

    // RAII
    bool                              hasDtor = false;   // declares its own ~dtor
    ClassDestructorDeclarationNode*   dtorNode = nullptr;
    bool                              destructible = false; // own dtor OR a destructible field (transitive)
    // Serialization mode gate (the tighter sibling of `destructible`): transitively reaches a
    // Shared/Weak/Owned pointer field. false => by-value/tree serialization; true => object-graph.
    // Recurses through collection elements + owned fields, but STOPS at a pointer (doesn't recurse
    // through it). Populated by computeReachesPointer(). Consumed by the serialization lowering (Phase C+).
    bool                              reachesPointer = false;
    // (Sendability is not a computed flag here: a type DECLARES `implements Sendable`, which lands in
    // `interfaces` like any conformance, and checkSendableDeclarations verifies the claim over its fields.)
    // `immutable value|resource T` — the greppable qualifier opting into cross-isolate sharing (M6.2). The
    // emitter VERIFIES deep/transitive immutability (computeDeeplyImmutable); a qualified type with a mutable
    // field is a compile error. Distinct from a `const` binding (which permits a mutable alias elsewhere).
    bool                              isImmutableQualified = false;
    // Deeply/transitively immutable (computed): `isImmutableQualified` AND every field/base/variant-payload/
    // element is itself deeply immutable, with no raw `UnsafePtr`/mutable-`Owned`/mutable collection. Populated by
    // computeDeeplyImmutable(). A `Shared<T>`/`Weak<T>` over such a `T` is sendable across isolates (its
    // control block uses the ATOMIC refcount flavor; see useAtomicRefcount) — `when [T: Immutable]` in std::memory.
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
    int                               maxDepth = 0;    // `virtual(maxDepth: N)`: levels that may still be added BELOW this type (0 = sealed, i.e. `final`)
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
    // Contracts supplied by a `type intrinsic <…> implements C { … }` block (a subset of `interfaces`).
    // These dispatch statically/monomorphized through the injected methods, so they get NO fat-pointer
    // interface vtable (a primitive target can't be boxed as one) — skipped in vtable emission.
    std::vector<std::string>          staticOnlyInterfaces;

    // Collections: a monomorphized Coll<T> is a synthetic ClassInfo whose
    // method bodies come from a C-template macro (not kama AST).
    bool                              isIntrinsicColl = false;
    CollKind                          collKind = CollKind::String;   // arbitrary: only read when isIntrinsicColl
    std::string                       collElemClass;         // element class name ("" if primitive)
    bool                              isGenericInst = false; // a specialized generic-type instance (Box_int32)
    // An OPAQUE TYPE PARAMETER: the synthetic type that stands in for a template's `T` while
    // `checkUninstantiatedTemplates` walks a body nobody instantiated. Its methods are exactly what the
    // parameter's declared bounds promise, which is what makes `x.compareTo(…)` resolvable — and what
    // makes a call the bounds do NOT promise a diagnosable error at the declaration instead of a surprise
    // at every consumer's use site. Never emitted: the probe erases it before anything can read it as a
    // real type (see probeSandboxEnd).
    bool                              isOpaqueParam = false;
    // A synthetic ClassInfo for a PRIMITIVE target of `type intrinsic <int32> implements C` — it carries
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
    // The declaring file, for a diagnostic raised about this type OUTSIDE per-module emission — where
    // `_sourcePath` is whatever module is being written and, in a multi-file build, is still "" (the
    // driver constructs that emitter with no path; the real ones arrive per module inside emitProgram).
    // Collection knows the unit; a late whole-program check does not, so it is recorded here.
    std::string                       declFile;

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
    // Fixed<T,N> only: the element TYPE NODE, kept because registerFixedViews needs to build `View<T>`
    // later — after every unit is collected — and the C spellings above cannot be turned back into one.
    SharedIdentifier elem;
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
                         // `const fn` on the member. Unlike `unsafe` — which marks a BODY, and so is
                         // rejected on a bodiless contract member — `const` constrains what a CALLER may
                         // pass as receiver, so it is signature-level and the contract may demand it: an
                         // implementation of a `const fn` member must itself be `const fn`.
                         bool isConst = false;
                         // `static fn` on the member — a contract CAN require one, and `Hasher` does:
                         // `static fn uint64 finish(uint64 raw)` is zero-state, so `H::finish(raw)`
                         // monomorphizes to a direct call with no receiver. It is therefore part of the
                         // signature in BOTH directions: an instance implementation of a static member has
                         // no `H::` form, and a static implementation of an instance member is handed a
                         // receiver it never declared, which shifts every argument by one.
                         bool isStatic = false;
                         // The declaration's NAME identifier, for the reference index (M6 B3c). Null for
                         // the operator arm, which has no name node — as MethodInfo::node already is.
                         SharedIdentifier nameId;
                         // `@noheap` on the member. Signature-level for the same reason `isConst` is, and
                         // for a sharper one: a contract call is dispatched through a vtbl slot, so the
                         // compiler cannot see which implementation runs. Inference stops at that slot.
                         // Declaring it is what lets the proof cross — the contract PROMISES the member
                         // allocates nothing, every implementation is checked against that promise, and a
                         // `@noheap` caller may then dispatch through it. Last in the struct so the two
                         // brace-initialised push_backs keep working; set explicitly beside them.
                         bool noHeap = false; };
// The kinds that may `implements` a contract — its `for` clause (`type contract C for value, view`).
// A BITMASK, not a set<string>: the gate is a test in the inner loop of a `_classes × interfaces`
// sweep, the domain is CLOSED at five, and the clause has to render back into a diagnostic in a
// FIXED order — a set would print it alphabetically and silently reorder under any widening.
//
// `contract` is deliberately absent. A contract implementing a contract is REFINEMENT — a different
// axis, spelled by `implements` on the contract itself and handled by linkContracts().
enum ImplKind : unsigned {
    IK_Value     = 1u << 0,
    IK_Resource  = 1u << 1,
    IK_View      = 1u << 2,
    IK_Enum      = 1u << 3,
    IK_Intrinsic = 1u << 4,
};
// The ONE word<->bit table, read forward by the name gate (word -> bit, 0 for a non-kind word) and
// backward by the diagnostics (mask -> "value, view"), both in declaration order. A single-bit mask
// therefore renders as the bare kind noun, and a clause renders exactly as it should be written.
// Defined in kama.cemit.cpp.
unsigned    kamaImplKindBit(const std::string& word);
std::string kamaImplKindListText(unsigned mask);

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
    // Kind gate (`for value, resource, view, enum, intrinsic`): which kinds may `implements` this
    // contract, as ImplKind bits. 0 means the clause was missing or malformed — ALREADY DIAGNOSED at
    // the declaration, so every enforcement site reads 0 as "say nothing". That one convention is what
    // stops a single bad contract from making each of its implementers report a second, invented reason.
    unsigned                     implKinds = 0;
    // Refined parent contracts (`type contract Animated implements Drawable`) — resolved names. Their methods
    // are merged into `methods` by linkContracts() so vtable/conformance/dispatch see the full slot set.
    std::vector<std::string>     refines;
    // `type contract Job implements Sendable` — every implementor must declare `Sendable` (and is verified),
    // which is what lets the type-erased box `Owned<Job>` cross an isolate boundary. Not a parent in
    // `refines`: the marker has no members to merge, only a requirement to pass on.
    bool                         requiresSendable = false;
    // A specialized generic-contract instance (`Iterator_int32`) — emitted under a bound _typeSubst so
    // its `T`-typed method sigs resolve; the template itself lives in _genericContracts, not here.
    bool                         isGenericInst = false;
    // `type contract C<T is This>` — the parameter PINNED to the implementing type, or -1 for none. It is
    // what lets a self-typed contract be a contract VALUE at all: `This` is a substitution with nothing to
    // substitute into under erasure, while a pinned parameter is a real type argument that resolves the
    // same way in the vtbl slot and in the concrete function. At most one, and it must come first.
    int                          pinnedParam = -1;
    std::string                  templateKey;   // the generic contract this specializes (e.g. "Iterator")
    std::vector<SharedIdentifier> typeArgs;      // the concrete args (e.g. [int32])
    // `@viewable type contract C { … }` — the MINT GRANT. A `type view`'s constructor is private, because
    // a view is a bidirectional relationship: it does not exist without a type to view, so it may be born
    // only inside the view itself or inside the type it views. This flag is how a type DECLARES that it
    // views something — implementing a member of a marked contract lets that member's body mint the view
    // it returns. Copied into every specialization by the _genericContracts template path, so marking
    // `Iterable<T>` marks `Iterable_int32` too.
    bool                         isViewable = false;
    std::string                  declFile;   // the unit that declared it — a late whole-program check scopes diagFile() from it
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
    std::string declFile;                       // the unit that declared it — the file rung's key (see `checkReach`)
};

// True for a name the BUILD CONFIGURATION owns (DEBUG/RELEASE/HOSTED and the OS_/ARCH_/ABI_ namespaces
// derived from the target triple) rather than a project's own `flags` entry. Shared by the emitter's
// strict `@compileFor` validation and the driver's manifest check, so the rule has one definition.
// Defined in kama.cemit.cpp. See docs/targets.md.
bool kamaIsBuildConfigFlag(const std::string& name);

// Does a `@compileFor(...)` attribute list hold for this flag set? The whole gate rule — bare `FLAG`,
// `!FLAG`, comma = AND, strict-mode validation of every name — in one place, because it has two callers
// on two diagnostic channels: the emitter gating a DECLARATION (CEmitter::compileForActive), and the
// driver gating a whole FILE (`file @compileFor(FLAG);`, fileGateActive in kama.driver.cpp), which runs
// before any emitter exists. `report` receives a rejection (an undeclared flag under strict mode, or an
// argument that is neither `FLAG` nor `!FLAG`); the caller turns it into its own kind of diagnostic.
// Defined in kama.cemit.cpp.
bool kamaCompileForActive(const SharedAttributeList& attrs,
                          const std::set<std::string>& active,
                          const std::set<std::string>& declared,
                          bool strict,
                          const std::function<void(const std::string&)>& report);

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // The implicit prelude (library sum types Optional/Result). Collected before user code
    // with a global namespace, so its templates register but emit nothing unless instantiated.
    // `srcPath` is where that source ACTUALLY lives on disk (the driver resolves it; "" when there is no
    // such file, as in a `--no-std` install). It travels ALONGSIDE the unit's synthetic `<prelude>` name
    // rather than replacing it — see DefSite::file and builtinSourcePath for the four passes that read
    // the `<` sentinel and would change behaviour if the name became a path. Its one reader is
    // go-to-definition.
    void setPrelude(SharedCompilationUnit u, const std::string& srcPath = std::string())
    {
        _preludeUnit = u;
        if (u && !srcPath.empty()) noteBuiltinFile(u.get(), srcPath);
    }

    // `--no-heap` (MCU step 5): reject every emitter-visible heap allocation program-wide (the no-heap
    // subset — also serves game-engine hot paths / real-time audio, not just bare metal). Composes with
    // `--target embedded`. Per-region `@noheap` on a fn is handled per-body; both funnel through
    // `rejectIfNoHeap`. Set from the driver before emission.
    void setNoHeap(bool on) { _noHeapProgram = on; }
    // M5a: measure the strict-numeric migration. Hidden, off by default, deleted when that rule
    // lands — its whole job is to answer "how big is the corpus change" with a count instead of a
    // guess. See `noteNumericHandoff`.
    void setStrictNumeric(bool on) { _strictNumericScan = on; }
    // Measure what `checkUninstantiatedTemplates` reaches. Hidden, off by default, same shape and same
    // reason as `--strict-numeric`: one TSV row per probed template on stdout, reporting the diagnostics
    // DEFERRED because a type was unknown rather than wrong. That is the point — it is the half of an
    // uninstantiated body this pass cannot see, and a measurement that hid its own blind spot would be
    // worse than no measurement.
    //
    // Row: `probe` (a generic FUNCTION) or `probe-type` (a generic TYPE or `enum`), then key file line
    // #params errors deferred resolved, then one column per DeferKind in enum order.
    //
    // ⚠️ It said here that it would be DELETED when the work it sized landed. That work has landed —
    // deferrals went 87 -> 0 for functions, and the type half walks 35 templates and resolves 265 sites —
    // and it is staying, for the reason it was built: **a pass that reports "checked" while meaning
    // "checked except two sites" is the failure mode this instrument exists to prevent.** Two sites still
    // defer (`opaque-scalar`: `cast<T>` inside `Atomic<T>`, which no bound in the language can license),
    // and reach can regress silently in a way no fixture would catch. Same standing as `--strict-numeric`,
    // which was kept for the same reason and sized this campaign.
    void setProbeReport(bool on) { _probeReport = on; }


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
    // Maps a unit's source path to the manifest of the package that owns it ("" when nothing does, and
    // for the synthetic prelude units). Supplied by the driver — resolving it is filesystem work, and it
    // is consulted only when a diagnostic has to say which package a conformance came from.
    void setPackageResolver(std::function<std::string(const std::string&)> r) { _packageResolver = r; }
    // Maps a unit's source path to the MODULE that owns it — `std::collections`, or a bare project name
    // for a file in the project root module, and "" for a loose file with no `kama.json` above it. This is
    // where a file's identity comes from (SPEC.md § Modules): the path plus the project's
    // `modules` map, never the `namespace` line the file happens to declare. Supplied by the driver
    // because the answer is filesystem work — walking to the owning manifest and reading its module tree.
    // §2c's rung, answered by the driver: may a file of `importer` see `imported`'s surface? Both are full
    // module names; "" is a file in no module. Absent (or unset) means allow, so a unit tree built without
    // a driver — a test harness, a future front end — is not silently locked down.
    void setModuleVisible(std::function<bool(const std::string&, const std::string&)> f) { _moduleVisible = std::move(f); }
    void setModuleResolver(std::function<std::string(const std::string&)> r) { _moduleResolver = r; }

    // A namespaced built-in module (the smart-pointer triad, std::memory) — collected before user
    // code under its own `namespace`/`export`, plus an implicit `using` so its names are always in
    // scope. Like the prelude, its generic templates emit nothing unless instantiated.
    // The built-in documentation file (prelude/builtin.kama) and where each registered name is written in
    // it. `int32`/`string`/`isize` are reserved words rather than declarations, so unlike the prelude
    // there is no source to resolve — this is a written PLACE for them, the shape Go's `builtin.go` and
    // Rust's `primitive_docs.rs` use. The driver scans the file (it owns filesystem work); the emitter
    // only turns the table into def-sites. Empty path = no such file, and then a built-in keeps answering
    // "no definition", which stays the honest answer.
    void setBuiltinDoc(const std::string& path, const std::map<std::string, SrcRange>& index)
    {
        _builtinDocFile = path;
        _builtinDocIndex = &index;
    }

    void addPreludeModule(SharedCompilationUnit u, const std::string& srcPath = std::string())
    {
        if (!u) return;
        _preludeModuleUnits.push_back(u);
        if (!srcPath.empty()) noteBuiltinFile(u.get(), srcPath);     // see setPrelude
    }

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
    // Where an auto-import's edit goes in `uri` — the first `import` entry's start when the file has a
    // block, else column 0 of the line a new block belongs on. `hasBlock` (out) says which. An empty
    // range means the index holds no unit for this path. See LspImportInsertion (kama.lsp.h) for the two
    // texts a server writes at it, and why one position is enough.
    SrcRange importInsertionAt(const std::string& uri, bool& hasBlock) const;
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
    // Its compiler-owned sibling: a prelude/built-in-module decl -> the file it was embedded from. A
    // SEPARATE map on purpose — see buildDeclUnits for why these must not make `DefSite::unit` non-null.
    std::map<const ASTNode*, std::string> _declBuiltinFile;
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
    // The file that owns the body being emitted, when it is NOT the module currently being written: a
    // generic INSTANCE is emitted from the header pass, before any module's path is current. Scoped by
    // emitGenericInst / emitGenericTypeInst; read by diagFile(). `_refUnit` cannot serve — it is null for
    // a prelude/std template on purpose (that is how reference recording is disabled for them), and those
    // are exactly the templates a user program instantiates most.
    std::string _emitDeclFile;
    bool _analysis = false;                      // analysis-mode ctor => record references; a build records none
    void recordRef(const std::string& key, const IdentifierNode* site);  // pure append; no diagnostics, no cType
    // ...for the built-in spellings `cType` short-circuits before the resolver sees them (usize/isize/
    // UnsafePtr). A no-op when there is no doc file, so nothing indexes a location that does not exist.
    void recordBuiltinRef(const std::string& name, const IdentifierNode* site);
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
    void addBuiltinDefSites();                   // ...plus the C++-registered names, from prelude/builtin.kama
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
                    const std::string& display, const std::string& container,
                    // For a MEMBER of a compiler-owned type, whose node is not a top-level declaration and
                    // so is in no map: its type's file, already resolved by the caller. See DefSite::file.
                    const std::string& builtinFile = std::string());  // one _defSites entry
    const CompilationUnit* unitOfDecl(const ASTNode* topLevelDecl) const;  // _declUnit lookup (nullptr => prelude/std)
    // The file a compiler-owned top-level declaration was embedded from, or "" (a user decl, or no such
    // file on disk). The `unitOfDecl` sibling for the half `_declUnit` deliberately cannot express.
    std::string builtinFileOfDecl(const ASTNode* topLevelDecl) const;

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
    // `stmtOnly` walks the DECLARATION statements alone, skipping the `match`-arm half: a payload binding
    // carries a derived type rather than a source spelling, and its resolution replay does not belong in a
    // check pass. `checkDeclaredTypes` is that caller.
    void collectBindings(SharedStatement s, std::vector<QueryBinding>& out, bool stmtOnly = false) const;
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
    // (file, line, message) already reported. A generic type's member body is emitted ONCE PER
    // INSTANTIATION, so one mistake in it was counted and printed once per instantiation. Per-emitter,
    // and every program gets its own CEmitter, so this never reaches across programs.
    std::set<std::string> _reportedDiags;
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
    // Compiler-owned unit -> the file it was embedded FROM. Filled by setPrelude / addPreludeModule; read
    // by buildDeclUnits, to give those declarations' DefSites a `file` they can be opened at.
    std::map<const CompilationUnit*, std::string> _builtinUnitFile;
    // The same answer keyed by the unit's synthetic NAME, which is what diagFile() deals in — a
    // diagnostic knows the file it belongs to as a string, never as a unit pointer. See reportPath().
    std::map<std::string, std::string> _builtinFileByName;
    // Record both keyings at once. Out of line because `CompilationUnit` is incomplete here and the
    // unit's NAME has to be read off it.
    void noteBuiltinFile(const CompilationUnit* u, const std::string& srcPath);

public:
    // The file name to SHOW for a unit: a compiler-owned unit's real source when the install has it and
    // the driver has verified it is still the text this binary compiled (builtinSourcePath), else the
    // synthetic name unchanged. Applied only where a position leaves the compiler — a diagnostic's
    // `file`, an instrument's row — never to `_collectingUnitPath` or `declFile`, which the `<` sentinel
    // is read out of by checkReach, CEmitter::line, setPackageResolver and moduleOfUnit.
    std::string reportPath(const std::string& unit) const
    {
        if (unit.empty() || unit[0] != '<') return unit;
        auto it = _builtinFileByName.find(unit);
        return it == _builtinFileByName.end() ? unit : it->second;
    }
private:
    // prelude/builtin.kama and its name -> span table, both owned by the driver (the table is a process
    // -wide static, scanned once). See setBuiltinDoc.
    std::string                            _builtinDocFile;
    const std::map<std::string, SrcRange>* _builtinDocIndex = nullptr;
    // Does the program use serde at all? Set in collectProgram from a `@generate` type or a Serializer/
    // Deserializer backend — the only ways to (de)serialize anything. When false we emit NONE of the serde
    // machinery: the prelude's primitive Serializable/Deserializable conformances are skipped, and a collection's
    // conditional `when [T: Serializable]` serde (serialize/serKey/…) is dropped via whenConditionsHold. Purely a
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
    // Set while the emitter dispatches a call IT synthesized (string interpolation lowering `${x}` to
    // `x.format(f:)`). The contract-scope gate is a rule about SOURCE — what a primitive'''s API looks
    // like to an author — so a compiler lowering is exempt from it.
    bool                               _inSynthDispatch = false;
    std::string                        _thisType;                // C name `This` resolves to (the class being emitted, or the contract type inside its vtbl slot)
    bool                               _basesLinked = false;     // linkBases() has run, so an empty ClassInfo::baseName means "no base" rather than "not resolved yet"
    bool                               _inBaseInstall = false;   // emitting the RHS of `this.base = …`: the one place an `abstract` type's ctor may be CALLED

    // Virtual dispatch: per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots
    std::set<std::pair<std::string,std::string>> _overriddenSlots;  // (vtableRoot, slot) overridden somewhere -> keep dynamic

    // Contract conformances of a PRIMITIVE (`type intrinsic <int32> implements Hashable`). Kept OUT of
    // `_classes` (an entry there would make every "user type?" test treat the primitive as a struct).
    // Keyed by `primKey` — the KAMA type name (`int32`, `char`), NOT the cType: `cType` is not injective,
    // and `char` and `uint32` both emit `uint32_t`. The ClassInfo's `name` is still the C type, because it
    // is what `This` resolves to and how the `self` parameter is spelled; the key and the name differ, and
    // the two char/uint32 entries deliberately share a name while holding different method cNames.
    std::map<std::string, ClassInfo>     _primConformances;
    // The ONE way in. Every read/write of `_primConformances` goes through these three, so the key's
    // identity lives in exactly one place.
    ClassInfo*       primConformance(const std::string& key)
                     { auto it = _primConformances.find(key); return it == _primConformances.end() ? nullptr : &it->second; }
    const ClassInfo* primConformance(const std::string& key) const
                     { auto it = _primConformances.find(key); return it == _primConformances.end() ? nullptr : &it->second; }
    ClassInfo&       primConformanceFor(const std::string& key) { return _primConformances[key]; }   // creates
    std::map<std::string, InterfaceInfo> _interfaces;        // contract name -> info
    // Who first claimed a (type, contract) pair, as the declaring file's path. Read only when a SECOND
    // claim arrives: a duplicate that crosses a package boundary is the one kind neither the user nor
    // either author can fix from one side, so that message has to name both packages. Two packages that
    // have never heard of each other can each conform `int32` to a contract one of them owns.
    std::map<std::pair<std::string, std::string>, std::string> _conformanceOrigin;
    std::string _collectingUnitPath;    // the unit whose declarations are being collected right now
    // The view type the CURRENT method body is allowed to mint (its C name), or empty. Set on entry to
    // every method body when the owner implements a `@viewable` contract declaring a member of that name
    // and the method returns a view — see emitMethodOrCtorBody and emitDotOnTypeCtorCall.
    std::string _mintGrant;
    std::function<std::string(const std::string&)> _packageResolver;   // unit path -> owning manifest, from the driver
    std::function<std::string(const std::string&)> _moduleResolver;
    std::function<bool(const std::string&, const std::string&)> _moduleVisible;   // (importer, imported) -> §2c
    // Pre-scanned conformances: target `primKey` -> the contracts a `type intrinsic` block grants it.
    // Populated before the collection pass so a generic-type-arg bound check that fires during
    // collection (e.g. `Map<string, V>` needing `string: Hashable`) isn't a false negative — the methods
    // themselves are injected later in applyIntrinsicImpl, which also validates completeness/coherence.
    std::map<std::string, std::set<std::string>> _intrinsicConformances;
    std::map<std::string, EnumInfo>      _enums;             // enum name -> info
    // Every enum's decl node, keyed by qualified name. Its remaining consumer is the LSP/query def-site
    // table (kama.query.cpp), which is the ONLY place either kind of enum gets a def-site: a tagged enum
    // is lowered to a variant ClassInfo and never reaches `_enums`, and the `_classes` loop skips variant
    // backings. (It also fed the lazy Model-C promotion, which is gone — an enum now declares its
    // conformance, so it is promoted at its declaration and needs no rebuild.)
    std::map<std::string, EnumDeclarationNode*> _enumDeclNodes;
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
    std::map<std::string, std::string> _genericDeclFile;            // template cName -> declaring file (diagFile)
    std::map<std::string, NsCtx>                    _genericCtx;    // template cName -> home namespace ctx
    std::map<std::string, GenericInst>              _genericInsts;  // mangled name -> instantiation (dedup)
    // The type params of the template `checkUninstantiatedTemplates` is currently probing. A generic
    // FUNCTION's params live nowhere else: `_genericTypeParams` holds generic classes'/enums' only, and
    // `_typeSubst` is deliberately empty during a probe (that is what keeps `T` symbolic). Without this
    // every probed `T` would reach `checkTypeResolves` as an unknown type. Read by `isTypeParamName`.
    std::set<std::string>                           _probeTypeParams;
    // The same params' declared contract bounds (`<T: Comparable<T>, C: Order<T>>` -> T:[Comparable], …),
    // resolved names, empty vector = unbounded. Only the CLASSIFIER reads it: a deferred call on a bare
    // `T` receiver means something different when `T` carries a bound (a future opaque parameter will
    // resolve it from the bound) than when it does not (the body is calling something `T` never promised,
    // and closing that needs a bound ADDED at the declaration — a source change, not a compiler one).
    std::map<std::string, std::vector<std::string>>  _probeParamBounds;
    bool                                            _probingTemplate = false;  // inside the probe walk
    long                                            _probeDeferred   = 0;      // the blind-spot tally
    long                                            _probeResolved   = 0;      // its denominator: sites fully checked
    bool                                            _probeReport     = false;  // `--probe-templates`

    // A diagnostic that fires because a type is UNKNOWN, not because something is concretely wrong.
    //
    // The four that exist ("method call on unresolved receiver", the turbofish and scope-qualified-call
    // rejections, and "cannot tell which `X` to construct") are all sound at an INSTANTIATION, where every
    // type is bound — and all four are unsound during a probe, where `T` is symbolic on purpose. Measured
    // before this existed: they rejected 81 of 640 corpus fixtures, every one of them valid generic code.
    //
    // So a probe DEFERS them, to the instantiation that will resolve the type and re-raise them properly,
    // and counts what it deferred. The tally is the point: it is precisely the half of an uninstantiated
    // body this pass cannot see, and reporting it is what keeps the pass from reading as "checked" when it
    // means "checked the concrete half" (`--probe-templates` prints it per template).
    //
    // Note what does NOT come through here: a receiver whose class IS resolved and lacks the method, an
    // `int32` initialized with a string, a call to a name that exists nowhere. Those are wrong at every
    // instantiation, so the probe reports them, which is the whole reason the pass exists.
    //
    // WHY THE TALLY IS BUCKETED. One number says how much the pass gave up on; it does not say who can
    // close it, and those are different projects. A receiver spelled `View<T>` waits on a compiler change
    // (bind the parameter to a synthetic type and the instance is ordinary); a receiver spelled `T` with
    // no bound waits on a SOURCE change in every generic that does it. Sizing the second from the first
    // is how the 5 %-reach estimate went wrong the last time — so the instrument reports the split.
    enum DeferKind {
        DK_RecvGeneric = 0,  // receiver's type is `Foo<T>` — a generic type mentioning a probed param
        DK_RecvBound,        // receiver's type IS a probed param, and that param declares a bound
        DK_RecvUnbound,      // receiver's type IS a probed param with NO bound  <- the source-migration cost
        DK_RecvUnknown,      // receiver's type node is unrecoverable (a collection element, an index)
        DK_Turbofish,        // `sortWith::<T, C>` — forwards the enclosing params, no instance to route to
        DK_ScopeQual,        // `Natural<T>::compare(…)` — qualifier names a generic instance that has none
        DK_DotCtor,          // `DynamicArray<T>.empty()` — no instance to construct until `T` is bound
        DK_OpaqueScalar,     // `cast<T>(…)` — kama has NO bound that says "an integer primitive"
        DK_ConstParam,       // `return N;` — a `comptime` param read as a value; a probe binds no argument to it
        DK_Count
    };
    static const char* deferKindName(int k);
    // Mint one opaque type per type parameter of `templateKey` and bind it into `_typeSubst`, so a probe
    // walk resolves `T` instead of stepping around it. Returns the synthetic names, index-parallel to
    // `typeParams` — empty for a const param, which stands for a VALUE and has no type to synthesize.
    // Two passes internally: every parameter is registered bare before any bound is projected, because a
    // bound may name a sibling parameter (`<T, C: Order<T>>`) and `Order<T>` cannot mangle until `T` is
    // a type.
    std::vector<std::string> buildOpaqueParams(const std::string& templateKey,
                                               SharedStringList typeParams,
                                               SharedBoundsList typeBounds,
                                               SharedIdentifierList constTypes);
    // Everything a probe registers is an artifact of a body that was never instantiated, and must not
    // outlive the walk: `analyze()` builds the query index AFTER the probe, so a `View___opq_f_T` left in
    // `_classes` becomes a type the LSP offers. Begin snapshots the key sets; End erases every key that
    // appeared in between. Erasing from a `std::map` keeps references to the surviving elements valid,
    // which is what lets resolved `ClassInfo::base` pointers stay good across the cleanup.
    void probeSandboxBegin();
    void probeSandboxEnd();
    // opaque class name -> the parameter's SOURCE name (`__opq_..._T` -> `T`). A user must never read
    // `__opq` in a diagnostic: the mistake is in their generic, and the type they wrote there is `T`.
    // Consulted by `demangleForDisplay`, which is the one place every message passes through.
    std::map<std::string, std::string> _opaqueDisplay;
    struct ProbeSnapshot {
        std::set<std::string> classes, interfaces, typeInsts, typeInstOf, typeInstCtx,
                              collections, contractInsts, contractInstCtx, primConf, fnInsts;
        // Keyed by AST node, not by name: a turbofish resolved during the walk records its callee per
        // call site, and those entries name an instance the walk is about to erase.
        std::set<const InvocationNode*> callInsts;
        size_t typeInstOrder = 0, collectionOrder = 0;
    };
    ProbeSnapshot _probeSnap;
    long _probeDeferBy[DK_Count] = {0};
    bool deferUnknownWhileProbing(DeferKind k)
    { if (!_probingTemplate) return false; ++_probeDeferred; ++_probeDeferBy[k]; return true; }
    // Which bucket a deferred call on `recv` belongs to — the DK_Recv* split above. Reads the receiver's
    // declared kama type NODE (`receiverTypeNode`), never its C type: `Foo<T>` and `T` lower to nothing
    // distinguishable once the parameter is unbound, and the distinction is the whole measurement.
    DeferKind classifyDeferredReceiver(SharedExpression recv);
    bool typeMentionsProbedParam(const SharedIdentifier& t) const;
    // A member missing from an OPAQUE PARAMETER is a different claim from a member missing from a real
    // type, and it deserves a different message. "`DynamicArray<T>` has no `get`" names a type that does
    // not have a method. "`T` has no bound providing `fromStr`" names a PROMISE that was never made — the
    // fix is a bound, not a method — so this says so, and lists the bounds `T` does carry. Reports and
    // returns true when it applies; false leaves the caller's own diagnostic to fire.
    bool rejectUnprovenBound(const std::string& cls, const std::string& member, int line);
    // True when `cls` is an opaque parameter, so a rule that needs to know whether it is a SCALAR cannot
    // decide. There is no contract in kama that means "an integer primitive" — `Atomic<T>`'s element
    // restriction is enforced by the compiler at the instantiation, not by a bound — so a template that
    // casts through its own parameter has no way to promise what it needs, and demanding one would be
    // demanding a bound the language cannot spell. Deferred and counted, not waved through.
    bool opaqueScalarUnknown(const std::string& cls)
    {
        if (!_probingTemplate) return false;
        auto it = _classes.find(cls);
        if (it == _classes.end() || !it->second.isOpaqueParam) return false;
        return deferUnknownWhileProbing(DK_OpaqueScalar);
    }
    // generic call site -> (enclosing type-substitution signature -> instantiation mangled name). A call
    // inside a generic TYPE's member is ONE AST node serving every instantiation of that type, so the node
    // alone cannot identify the callee: `Pair<int32>.first()` and `Pair<int64>.first()` route to different
    // specializations of the same generic function. The signature is empty everywhere else.
    std::map<const InvocationNode*, std::map<std::string, std::string>> _callInst;
    // Call sites discovery LOOKED AT and could not resolve — it diagnosed them itself (inferGenericInst /
    // explicitGenericInst report before returning false). The emit-side fail-closed rule below reads this
    // to tell "discovery said no" from "discovery never came here", and stays silent about the first, so
    // one mistake keeps producing one diagnostic.
    std::set<const InvocationNode*>                 _genericInferFailed;
    std::string substSig();      // the active _typeSubst as a stable key ("" outside a generic instance)
    std::string callInstOf(const InvocationNode* call);   // the instantiation for `call` here, or ""
    std::map<std::string, SharedIdentifier>         _typeSubst;     // type-param name -> concrete (only while emitting an instantiation)
    // A bound comptime param: its folded value AND the integral type it was DECLARED with. The two
    // travel together in one map on purpose — the value alone was enough while a const param could only
    // be a size, but reading it as a value needs the width, and a second parallel map would be one
    // missed `clear()` away from a stale binding silently retyping an unrelated name.
    struct ConstBinding { int64_t value = 0; int kind = 0; };   // kind = the declared type's builtInVal
    std::map<std::string, ConstBinding>             _comptimeSubst;    // const-param name (`const N: int`) -> binding (parallel to _typeSubst)
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
    // template name -> per-param DECLARED integral type for a `const N: int32` param (null entry = a
    // type param). The first reader of ClassDeclarationNode/EnumDeclarationNode::comptimeParams' data:
    // both nodes carried const-param info that nothing consumed, which is why a const param on a TYPE
    // was parse-only plumbing while the same spelling on a function worked.
    std::map<std::string, std::vector<SharedIdentifier>> _genericTypeConstTypes;
    std::map<std::string, NsCtx>              _genericTypeCtx;      // template name -> home namespace ctx
    std::map<std::string, NsCtx>              _genericTypeInstCtx;  // instance -> registration (use-site) ctx, so a
                                                                    // prelude template's user-type args resolve at emit time
    std::map<std::string, GenericTypeInst>    _genericTypeInsts;    // mangled name -> instantiation (dedup)
    std::map<std::string, std::string>        _genericTypeInstOf;   // mangled name -> template name (construction)
    std::vector<std::string>                  _genericTypeInstOrder;// registration order (inner-first; struct-typedef emit)
    // Instances (a generic type's, or a generic function's) whose type argument was REFUSED by a bound.
    // Their shape still registers — an unregistered instance turns one rejection into a second, unrelated
    // "unknown type" cascade — but their member BODIES are never emitted, so a template cannot go on to
    // report the consequences of an argument it already refused. Rust's `ty::Error` poisoning, and what
    // C++20 concepts do; walking the body anyway is the pre-concepts C++ template-error vomit.
    std::set<std::string>                     _boundFailedInsts;
    bool                                      _emitStaticClass = false;  // prefix `static` on specialized class fns (header ODR)
    // Inside emitHeaderContent: the text being written belongs to no module, so `#line` reads only
    // `_emitDeclFile` (a generic instance's template) and stays silent for everything else. See line().
    bool                                      _inHeaderPass = false;
    bool                                      _emitStaticInlineFn = false;// prefix `static inline` on a free fn (prelude helper body emitted in the header)
    bool                                      _strictNumericScan = false;  // `--strict-numeric`: TALLY numeric hand-offs, reject nothing
    std::set<std::string>                     _strictNumericSeen;          // dedupe: a template body is emitted once per instantiation
    bool                                      _noHeapProgram = false;    // `--no-heap`: reject every heap allocation program-wide
    bool                                      _release = false;          // `--release`: strip `debugAssert`
    bool                                      _noHeapActive  = false;    // inside a `@noheap` fn: reject heap allocation in this body

    // --- `@noheap` transitivity -------------------------------------------------------------------
    // `@noheap` used to gate ONE body and stop, so an `@noheap` fn calling an un-annotated kama helper
    // that did `new` compiled clean and SPEC's "a real-time audio callback allocates nothing" was false
    // after one level of indirection. The proof is now transitive, and these three maps are how.
    //
    // They are filled DURING emission, on purpose. The detector is `rejectIfNoHeap` itself — the same
    // gate that rejects a direct allocation also records one — so detection can never drift from the
    // gate, because it IS the gate. A separate AST walker was the obvious alternative and is the wrong
    // shape twice over: two of the seven allocation sites (boxing a primitive / an error into an owning
    // contract handle) are TYPE-directed, not syntactic, so no walker over the source can see them; and
    // a second walker obliged to know every allocating node kind is exactly how `scanExprForGenerics`
    // drifted by three node kinds and started failing open.
    //
    // Everything is keyed by the mangled C name, which is what makes the analysis precise for free:
    // a generic instance is its own node, so `DynamicArray<int32, BumpAllocator>.add` and the
    // `GlobalAllocator` one are different functions, and an arena-backed container stays legal inside a
    // `@noheap` region while a heap-backed one does not. Monomorphization is doing the work a
    // whole-program analysis would otherwise have to approximate.
    struct AllocSite { std::string what; int line = 0; std::string file; bool indirect = false; };
    struct CallEdge  { int line = 0; };
    // `fromFlag` distinguishes a body the AUTHOR annotated from one `--no-heap` seeded. Both are roots of
    // the same walk, but they are two different claims and must not borrow each other's sentence: telling
    // an author their function "is `@noheap`" when they wrote no attribute names a cause that is not there.
    struct NoHeapFn  { std::string display; int line = 0; std::string file; bool fromFlag = false; };
    std::map<std::string, AllocSite> _allocSites;   // C name -> why it allocates DIRECTLY
    // caller -> callee -> the FIRST call site. A map rather than a list so a body that calls the same
    // helper fifty times contributes one edge, and so iteration order is the callee name — the walk below
    // reports a chain, and a chain that changed between builds would be a diagnostic nobody could pin.
    std::map<std::string, std::map<std::string, CallEdge>> _callEdges;
    // C name -> every root of the transitive walk: every `@noheap` body, and under `--no-heap` every USER
    // body as well (`isUserBody` — the prelude and the stdlib are excluded, or the flag would report a
    // defect against code the author never wrote and cannot change).
    std::map<std::string, NoHeapFn>  _noHeapFns;
    // Record one call edge out of the body being emitted. A no-op outside a body, and self-edges are
    // dropped (direct recursion cannot make a function allocate that did not already).
    void recordCallEdge(const std::string& callee, int line);
    // The ONE spelling of a deep copy (`T__copy(&(x))`), so its call edge is recorded in one place.
    std::string copyCall(const std::string& cls, const std::string& lvalue);
    // The fixpoint + the report. Runs after ALL emission on both entry points — see the .cpp.
    void checkNoHeapTransitive();
    // Is this body one the AUTHOR wrote, as opposed to the prelude or the stdlib? Asked only by
    // `--no-heap`, which seeds the transitive walk with every user body — see the block comment on the
    // definition for why neither half of the test is sufficient alone.
    bool isUserBody(const std::string& declFile, const std::string& display) const;
    // ROADMAP row 1 — the check `@foreignEntry` exists for. A body that may run on a thread kama did not
    // create sees FRESH module statics (`KAMA_ISOLATE_LOCAL` is `_Thread_local`): the declared initialiser,
    // never a value another isolate assigned. So inside a foreign-entry REGION — the root and everything it
    // reaches over `_callEdges` — reading a static that is assigned anywhere OUTSIDE the region is an
    // error: that read can only ever see the initialiser. Same shape as the no-heap proof: facts at the
    // two funnels (the identifier arm for reads, checkConstWrite for writes), roots from the attribute and
    // from every bind to a `@foreignEntry` signature (checkFnPtrBind), the walk after emission.
    struct ForeignEntryFn { std::string display; int line = 0; std::string file; std::string via; };   // `via`: the signature it was bound to, or ""
    struct StaticRead     { std::string name; int line = 0; std::string file; };
    std::map<std::string, ForeignEntryFn>                    _foreignEntryFns;   // C name -> root of the walk
    std::map<std::string, std::map<std::string, StaticRead>> _staticReads;      // fn -> static key -> first read
    std::map<std::string, std::set<std::string>>             _staticWriters;    // static key -> every fn that assigns it
    std::string                                              _staticWriteLhs;   // the static a plain `=` is storing to: its LHS mention is not a read
    void recordStaticRead(const std::string& key, const std::string& name, int line);
    void recordStaticWrite(SharedExpression target);
    void checkForeignEntryStatics();
    // ROADMAP row 2 — `@onPanic(recover: <literal>)`: a `@noheap` body whose panics (bounds, arithmetic,
    // `panic`, `assert`, …) longjmp back to its prologue and return the literal, instead of aborting the
    // process from a thread the player cannot see. Gated so the longjmp skips no destructor: the region —
    // the root plus everything it reaches over `_callEdges` — may own no destructible local (facts recorded
    // at recordDestructibleLocal and the two parameter sites, walked after emission like the no-heap proof).
    struct OnPanicFn        { std::string display; int line = 0; std::string file; };
    struct DestructibleSite { std::string name; std::string className; int line = 0; std::string file; };
    std::map<std::string, OnPanicFn>        _onPanicFns;          // C name -> root of the walk
    std::map<std::string, DestructibleSite> _destructibleOwners;  // fn -> the first destructible local it owns
    bool        _usesOnPanic   = false;   // any unit declares a region: the TU includes <setjmp.h> and defines KAMA_ONPANIC
    bool        _onPanicArmed  = false;   // the body being emitted is a region (prologue + disarm on every exit)
    std::string _onPanicRecover;          // the literal the landing pad returns ("" for a `void` region)
    bool        unitsUseOnPanic(const std::vector<SharedCompilationUnit>& units);
    void        emitOnPanicPrologue(int depth);
    void        recordDestructibleOwner(const std::string& cVar, const std::string& className);
    void        checkOnPanicRegions();
    std::set<std::string>                     _activeFlags;              // `@compileFor`: active build flags (membership gate)
    std::set<std::string>                     _declaredFlags;            // `kama.json` declared user-flag universe (strict validation)
    std::set<std::string>                     _prunedNames;              // decls `@compileFor` dropped in THIS build — so an
                                                                         // export manifest / import naming one says "not in this
                                                                         // configuration" instead of "no such declaration".
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
    // A PRIMITIVE widened to a contract value — (conformance key, contract C name), e.g. ("int32","Hashable").
    // A primitive has no `_classes` entry and its methods take `self` BY VALUE, so a widening needs a vtbl
    // and a deref thunk per method that no class needs. Collected in the SCAN pass rather than at the use
    // site because the vtbl must be declared before the C that names it — and emitted only for pairs a
    // program actually widens, since the alternative is ~100 vtables in every binary for a rare feature.
    std::set<std::pair<std::string, std::string>>   _primWidenings;
    std::string                                     _derefContract;         // resolved name of the prelude `Deref` contract ("" if none in scope) — gates auto-deref
    std::string                                     _heapOwnerContract;     // resolved name of the prelude `HeapOwner` contract — `new` placement-constructs into a type implementing it
    std::string                                     _movableContract;       // resolved name of the prelude `Movable` marker (implicit on every resource; `!Movable` subtracts it)
    std::string                                     _copyableContract;      // resolved name of the prelude `Copyable` marker
    std::string                                     _sendableContract;      // resolved name of the prelude `Sendable` marker (declared, verified, required at a crossing)
    int                                             _silentBounds = 0;      // >0 while a bound-refused instance registers its shape: nested bound failures are consequences, not reported

    // Namespaces: current-file scope + the helpers that mangle/resolve names.
    NsCtx _nsCtx;
    std::set<std::string> _namespaces;
    // Mangled module scope -> the unmangled name, for the visibility rung: a resolved symbol key carries the
    // mangled prefix and §2c is asked in real module names. Complete by construction — every module in the
    // compilation has at least one unit, and this is filled from the same loop that builds `_unitCtx`.
    std::map<std::string, std::string> _moduleNames;   // registered public namespaces (mangled)
    // The file-private scopes this compilation minted (`_F<file>`, one per unit no module owns). A private
    // scope names nothing a user could write, so `demangleForDisplay` strips it from a message — and since
    // §2e.26 made the spelling name-derived rather than `_F<digits>`, there is no PATTERN left to recognise
    // one by. This registry is what it consults instead. Filled from the same loop that builds `_unitCtx`.
    std::set<std::string> _privateScopes;
    std::set<std::string> _exported;     // mangled names of `export`ed top-level decls (module public surface)
    std::set<std::string> _externNames;  // FFI: literal C names of extern structs
    // THE FILE RUNG, FFI SIDE. Every other symbol is judged by the ONE file that declares it
    // (`declFileOf`), but an extern keeps its literal C spelling and so collapses onto a single table
    // entry no matter how many files declare it — 39 declare `malloc`. Repeating the declaration is the
    // idiom SPEC prescribes, so the answer is a SET of declaring files per C name rather than one
    // `declFile`, and `checkReach` asks whether the referencing file is in it.
    std::map<std::string, std::set<std::string>> _externDeclSites;   // literal C name -> files declaring it
    void emitIncludes(const std::vector<SharedCompilationUnit>& units);  // FFI #include directives
    std::map<const CompilationUnit*, NsCtx> _unitCtx;   // each file's context (for emit)
    NsCtx ctxOf(SharedCompilationUnit unit);                     // build a file's NsCtx
    static std::string qualifiedName(SharedIdentifier id);       // dotted "a.b.c" from value+qualifier
    static std::string mangleNs(const std::string& ns);          // "a.b" -> "a__b"
    std::string qualify(const std::string& name) const;          // scope-prefix a declared name
    // `site`, when non-null, is the source identifier this name was spelled at: in analysis mode the
    // resolved key is recorded against it for the M3 reference index (recordRef). Defaulted, so the ~68
    // call sites that have no identifier in hand (or don't want the use recorded) are unaffected.
    std::string resolveUserName(const std::string& value, SharedStringList qualifier,
                                const IdentifierNode* site = nullptr);                  // class/enum/iface ref
    bool rejectRootedPath(SharedStringList qualifier, const IdentifierNode* site);
    std::string resolveFunc(const std::string& name, SharedStringList qualifier,
                            const IdentifierNode* site = nullptr);                      // function ref
    std::string resolveUserNameImpl(const std::string& value, SharedStringList qualifier);  // the search itself
    std::string resolveFuncImpl(const std::string& name, SharedStringList qualifier);       // the search itself
    std::string resolveModuleVar(const std::string& name, SharedStringList qualifier);      // module `static`/`comptime`
    bool moduleVarExported(ModuleVariableDeclaration* mv);   // does it publish any name? (header vs unit)
    bool isNamespace(const std::string& name) const;             // a known public namespace (or alias)

    // RAII scope stack: live destructible locals per lexical scope.
    struct LiveLocal { std::string cVar; std::string className; };
    struct Scope { std::vector<LiveLocal> locals; std::vector<std::string> declaredNames;
                   bool isLoopBoundary = false; bool isFunctionRoot = false;
                   // Structured concurrency (M4): a `scope { }` is a task scope. `taskChildren` are the C
                   // names of the `kama_isolate_t` handles `spawn`ed inside it; emitScopeCleanup joins them
                   // ALL before dropping any local (join-before-drop), on every exit path. `borrowedPlaces`
                   // are the places its children `ref`-borrow (M4.2) — a second child borrowing an
                   // OVERLAPPING place is rejected (no two tasks share a cell). A place, not a root, so
                   // two children may take two disjoint fields of one local; overlap is the same prefix
                   // test the view model uses (`placesConflict`), which is why this is a vector and not a
                   // set — membership is not the question, conflict is.
                   // A child is either ONE handle (a bare `spawn`) or a GROUP — a `kama_isolate_t[]`
                   // plus the count actually spawned into it (`parallel_spawn`, whose K is a runtime
                   // `length()`). `count` empty means the single-handle form.
                   struct TaskChild { std::string handle; std::string count; };
                   bool isTaskScope = false; std::vector<TaskChild> taskChildren;
                   std::vector<std::vector<std::string>> borrowedPlaces;
                   // `borrow h.mint() as v { … }` — the host PLACE, frozen for the extent of the block.
                   // A view is live over that storage, so growing or reseating it would leave the alias
                   // dangling. Conflict is the same prefix test the rest of the model uses, which is what
                   // leaves a DISJOINT sibling field fully mutable inside the window. Scope-shaped rather
                   // than emitter-shaped so nesting, loops and a `return` out of the block all unwind for
                   // free, and so a frozen place can never leak past the function (`_scopes` is cleared
                   // per function).
                   // `fromBorrow` separates a `borrow` window from a `foreach` one. Only a `borrow`
                   // introduces an ALIAS that names the view, so only it can be reseated; a `foreach`
                   // binding names an ELEMENT, and writing through it is the point of `ref` iteration.
                   struct FrozenPlace { std::vector<std::string> place; std::string alias; int line = 0;
                                        bool fromBorrow = true; };
                   std::vector<FrozenPlace> frozen;
                   // View locals whose root is already lifetime-bounded, so a DERIVE off one is bounded
                   // too (`View<T> mid = whole.slice(…)` where `whole` came from a window).
                   std::set<std::string> boundedViews;
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
    bool               _inUnsafe = false;             // the ENCLOSING FUNCTION is an `unsafe fn`
    bool               _inNamedCtorBody = false;       // emitting a named `ctor` factory body (const fields of the built local are writable)
    bool               _inStaticMethod = false;        // emitting a `static` method body (no `self`/`this`)
    // A `ctor` names the value it is building with `this`, but a ctor is a static factory with no `self`
    // PARAMETER — so the storage is synthesized at ctor entry and `self` points at it. Set the moment the
    // body first mentions `this` (explicitly, or implicitly through a bare field name), which is what
    // decides whether that prologue is emitted at all: the field-default fill can CALL a field's `default`
    // ctor, so emitting it for a ctor that never names `this` would construct a whole object for nothing.
    bool               _ctorSelfUsed = false;

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
    // `<T is This>` pins a parameter to the IMPLEMENTING type, so it means something only where an
    // implementer exists — a `type contract`. This is SEMANTIC, not syntactic: a grammar cannot see which
    // kind it is attached to, and making it a parse error would put tree-sitter permanently out of step
    // with the compiler (tools/check-treesitter.sh partitions on exactly that split).
    void rejectPinOutsideContract(SharedIdentifierList pins, SharedStringList params,
                                  const char* what, int line);
    void collectEnums(SharedCompilationUnit unit);
    ClassInfo buildVariantClassInfo(EnumDeclarationNode* ed, const std::string& name);   // tagged-union ClassInfo
    void emitEnum(EnumInfo& ei);
    bool isEnum(const std::string& name) const { return _enums.count(name) != 0; }
    void collectClasses(SharedCompilationUnit unit);

    // Contract-conformance plumbing, shared by every path that grants a type a contract —
    // `type enum X implements C` and `type intrinsic <…> implements C`. That sharing is the whole reason
    // they are functions rather than an inline loop in `collectProgram`.
    //
    // A recorded conformance dispatches STATICALLY (it lands in `staticOnlyInterfaces`, so no fat-pointer
    // vtable is emitted for it). `isPrimitive` gates the serde-return collection scan: a primitive's
    // `Result<scalar, Owned<Error>>` monomorph only matters when serde is used.
    // `type enum E implements C { A, B; …members… }` — promote, inject, record, check. Between
    // linkContracts() (needs contractMethods) and buildVtables().
    void collectEnumConformances(const std::vector<SharedCompilationUnit>& units);

    // `type intrinsic <…> implements C { … }` — contract conformance for a PRIMITIVE.
    SharedIdentifier intrinsicContract(IntrinsicImplNode* n) const;   // the one declared contract, or null
    // The members that serve ONE target: the block's shared bodies, with any `<…> { … }` section that
    // names this target overriding them method-for-method.
    SharedClassMemberDeclarationList intrinsicMembersFor(IntrinsicImplNode* n, SharedIdentifier target);
    void applyIntrinsicImpl(IntrinsicImplNode* n);   // validate + inject, once per target
    std::string implMethodCName(ClassInfo& tci, const std::string& method);   // the minted symbol, not a re-derivation
    // The "…and package B claims it too" clause on a duplicate conformance; "" unless the two claims
    // genuinely come from different packages.
    std::string duplicateOriginNote(const std::string& tkey, const std::string& contract);
    // One (target, members) pair per thing an impl block contributes — a `type intrinsic` set gives one
    // per target. The three emission passes (prototypes, prelude bodies,
    // module bodies) all walk exactly this set, so they share it instead of re-deriving it three times.
    struct ImplEmit { ClassInfo* target; SharedClassMemberDeclarationList members; };
    std::vector<ImplEmit> implEmitsOf(SharedCompilationUnit u);
    bool serdeGatedOff(SharedIdentifier contract) const;   // an ungated primitive Serializable/Deserializable
    void emitEnumMemberBodies(ClassInfo& eci, EnumDeclarationNode* ed);   // bodies of a `type enum`'s own methods
    void injectImplMethods(ClassInfo& tci, SharedClassMemberDeclarationList members,
                           const std::string& contract, const std::string& tkey, bool isPrimitive);
    // The impl must supply every method the contract requires.
    void checkImplCompleteness(ClassInfo& tci, const std::string& contract,
                               const std::string& tkey, int line);
    // Resolve a parallel (names, AST nodes) interface list in place: qualify each name against the
    // current ns context, and mangle a generic contract to its specialized instance (Iterator ->
    // Iterator_int32) using the generic args that only survive on the node.
    void resolveInterfaceNames(std::vector<std::string>& names, SharedIdentifierList nodes);

    // Collections: discover used Coll<T> instantiations, register a synthetic
    // ClassInfo + CollectionInfo for each, and emit the C-template macro lines.
    void collectCollections(SharedCompilationUnit unit);
    void scanStmtForCollections(SharedStatement s);
    void scanExprForCollections(SharedExpression e);
    void scanTypeForCollections(SharedIdentifier t);
    bool isCollectionType(SharedIdentifier t) const;
    std::string mangleElem(SharedIdentifier elem);
    void registerCollection(SharedIdentifier collType);
    void registerFixed(SharedIdentifier fixedType);   // InlineArray<T,N> — the comptime-sized value array
    void registerSimd(SharedIdentifier simdType);     // Simd<T,N> — the lane batch (a `vector_size` typedef)
    // `v.shuffle(pattern: […])` / `a.blend(rhs:, pattern: […])` — folds the pattern to literal lane
    // indices and emits `__builtin_shufflevector`. Not an ordinary call: the indices must be integer
    // CONSTANT expressions, because the CPU encodes the permutation in the instruction.
    std::string emitSimdShuffle(const std::string& cls, const std::string& method,
                                const std::string& recvPtr, SharedArgumentList args, int srcLine);
    // Const generics: the compile-time integer value of a const argument/param expression (an
    // integer literal, or a const-param identifier bound in the current instantiation via _comptimeSubst).
    bool constValue(SharedExpression e, int64_t& out);   // returns false if not a resolvable const int
    // Bake a named compile-time size into a declared type, at COLLECT time, so a reader never has to
    // import the constant. See the definition for why it is not done at the read site.
    void bakeConstSizes(SharedIdentifier t, const SharedStringList& shadowed);
    bool constArgN(SharedIdentifier arg, int64_t& out);  // same, for a type-arg node (literal or bound param)
    bool scalarByteSize(SharedIdentifier type, int64_t& out);  // `sizeof(T)` for a fixed-width scalar T
    // The inclusive value range of a FIXED-WIDTH integral type. False for anything whose range this
    // compiler has no business asserting — a float, a class, and deliberately `usize`/`isize`, whose
    // width is the target's, not ours. Drives both the constant-cast check and `constValue`'s fold.
    bool primIntRange(SharedIdentifier type, int64_t& lo, int64_t& hi);
    // The same table, reached from a LOWERED C type — the four hand-off positions that hold no type node.
    // `primIntRange` routes through it, so the numbers live in one place.
    bool primIntRangeC(const std::string& cType, int64_t& lo, int64_t& hi);
    // The one message for a constant that provably does not fit its cast target. Two paths reach it:
    // the emit walk, and the const folder — a `comptime` constant is resolved only in the folder.
    void rejectConstCastOverflow(SharedIdentifier target, int64_t v, int64_t lo, int64_t hi, int line);
    // The RUNTIME half of the same rule: the checked C expression for a `cast<T>(x)` whose value is only
    // knowable at runtime, or "" when the conversion provably cannot fail. Gated on a numeric target, so
    // `cast<UnsafePtr<T>>` never reaches the check.
    std::string narrowCheck(const std::string& dstCType, SharedExpression value);
    // 5b-A. Is a folded constant PROVABLY outside its destination? The only answer that diagnoses; every
    // uncertainty is "no". `srcCType` is the SOURCE's lowered type and may be "" — it is what tells a
    // genuinely negative value apart from `constValue`'s int64 reinterpretation of a magnitude above
    // INT64_MAX, and "" means neither can be ruled out, so nothing is reported.
    bool constOutOfRange(const std::string& dstCType, int64_t v, const std::string& srcCType);
    void rejectConstOutOfRange(const std::string& dstCType, SharedExpression value,
                               const char* what, bool isInit, int line);
    // 5b-B. A hand-off position has a destination, so every `wideUnsuffixed` literal reachable from the
    // value it is about to emit is CLAIMED — the fits-check above judges whether it actually fits. One
    // that no hand-off ever claims is reported by `emitExpression`, which is what stops a literal's type
    // from following its magnitude. Recorded on the SIDE rather than on the node: the emit walk runs once
    // per instantiation and again for a build after an analyze, and mutating shared AST across those
    // passes would make the second one silent.
    void governWideLiterals(SharedExpression e);
    std::string moduleStaticCTypeRaw(SharedExpression e);   // a module static's type, NOT filtered by isClass
    std::set<const void*> _litGoverned;
    // M7 `comptime assert(cond:, msg:)` — one surface, two lowerings (see the block above its definition).
    void emitComptimeAssert(ComptimeAssertNode* a);
    void emitComptimeAssertsIn(ClassDeclarationNode* cd);      // the type-member form, under the live binding
    bool ctaNeedsCLowering(SharedExpression e);                // predicate turns on a layout fact kama can't fold
    bool ctaRenderC(SharedExpression e, std::string& out);     // -> a C constant expression for _Static_assert
    std::set<std::string> _staticAsserts;                      // emitted-text dedupe (a monomorph is re-walked)
    void rejectUnfoldableConstArg(const std::string& param, SharedIdentifier arg);  // a VALUE arg that won't fold
    std::set<const void*> _badConstArgs;   // arg nodes already reported — binding is re-run per discovery pass
    // Bind one instantiation's parameters: a const param binds a VALUE (+ its declared width) in
    // _comptimeSubst, every other param binds a type in _typeSubst. Clears both first — this IS the
    // binding, not an addition to one. `constTypes` is parallel to `params` (null = a type param).
    void bindInstParams(const SharedStringList& params, const SharedIdentifierList& constTypes,
                        const std::vector<SharedIdentifier>& args);
    // ADD a generic TYPE/enum instantiation's const params to _comptimeSubst. Additive on purpose: a
    // generic type keeps EVERY param in _typeSubst, which mangleElem hops through and deepSubstType /
    // argCarriesUnboundParam walk — moving const params out of it would break all three.
    void bindInstConstParams(const std::string& tmplKey, const std::vector<SharedIdentifier>& args);
    // A bound const param spelled as a C VALUE — the integer cast to its declared type. The cast is
    // load-bearing: a bare `8` is a C `int`, so a `const F: uint32`/`int8` would promote and compare
    // differently from a real local of the type the author wrote. "" if `name` is not bound.
    std::string constParamCValue(const std::string& name);
    // A `Fixed<T,N>` intrinsic instance (a value-semantics collection). Its indexing/foreach reuse the
    // collection machinery, but it is carved out of ownership (never destructible, copies freely).
    bool isFixedColl(const std::string& cls) const;
    bool isSimdColl(const std::string& cls) const;
    bool isMaskColl(const std::string& cls) const;
    std::string emitArrayLiteral(ArrayLiteralNode* al);   // `[a,b,c]` / `[v; N]` -> a Fixed value
    void registerSmartPtr(CollKind kind, SharedIdentifier elem, const std::string& customName = "");   // Owned/Shared/Weak (customName: a library `Box<Contract>` routed here)
    void registerOptionalOfShared(SharedIdentifier elem);          // Optional<Shared<elem>> for Weak.tryUpgrade
    void registerOptionalOfName(const std::string& sharedName);    // Optional<sharedName> — a library `Rc_<elem>` partner
    void emitWeakTryUpgrade(const CollectionInfo& info);           // the tryUpgrade wrapper (builds the Optional)
    void emitStringFind(const CollectionInfo& info);
    void emitFixedView(const CollectionInfo& info);                // InlineArray<T,N>.view() -> View<T>               // `.find()` wrapper: kama_string__find_raw -> Optional<usize>
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
    // The node-returning twin: deepSubstType(typeNode) under `inCls`'s type args, so a caller can scope
    // the binding to one resolution rather than hold it open across a construct.
    SharedIdentifier deepSubstInInstance(const std::string& inCls, SharedIdentifier typeNode);
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
    // template's ctx without the arg's home mangle being stripped. Primitives/`UnsafePtr`/`This` pass through.
    SharedIdentifier absolutizeType(SharedIdentifier t);
    // Generic TYPES: discover `Box<Arg>` uses, build one specialized ClassInfo each, emit under subst.
    void scanTypeForGenericTypes(SharedIdentifier t);
    void registerGenericTypeInst(const std::string& tmpl, SharedIdentifierList args);
    // True iff EVERY type-param of a generic template carries a default — so it may be named BARE
    // (zero type args), like an all-defaulted `BitSet<A = GlobalAllocator>` written just `BitSet`.
    // Its defaults then fill in at genericTypeMangle / registerGenericTypeInst (empty args).
    bool allTypeParamsDefaulted(const std::string& tmpl) const;
    void registerFixedViews();                  // late pass: InlineArray<T,N> gains view() + the Viewable grant
    SharedIdentifier viewQualifiedNode(const std::string& tmplKey);   // "a__b__View" -> `a::b::View` node
    SharedIdentifier findMethodReturn(ClassInfo& ci, const std::string& member);   // one method's return type
    const std::string& viewTemplateKey();       // the stdlib `type view View<T>` template key (cached)
    std::string _viewTmplKey;                   // "" until looked up, and "" if the stdlib has no View
    bool        _viewTmplLookedUp = false;
    // True iff a (post-substitution) type arg still carries an UNBOUND type-parameter — a bare name resolving
    // to no known type (nor a primitive / This / UnsafePtr / usize / isize), recursing into nested generic args.
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
    // Record a primitive->contract widening. Recorded OPTIMISTICALLY (the scan runs before `type intrinsic`
    // blocks are applied, so no primitive has a conformance yet); the emission point filters.
    void scanPrimWidening(SharedIdentifier declType, SharedExpression init);
    std::string scanPrimKeyOf(SharedExpression e);
    // The emit-time authority: the conformance key when `e` is a primitive declaring contract `ct`, else "".
    std::string primWidenKey(SharedExpression e, const std::string& ct);
    void emitPrimWidenVtables();
    std::string emitPrimBoxIntoContract(const std::string& ownedCType, const std::string& primKey_,
                                        const std::string& valExpr, int srcLine);
    void registerGenericContractInst(const std::string& tmpl, SharedIdentifierList args);
    // A contract's method-prototype list, from _interfaces (concrete/instance) or _genericContracts
    // (a template). Bound-checking matches by method NAME, which is type-parameter-independent, so it
    // reads either table through this one accessor. Returns nullptr for an unknown name.
    const std::vector<InterfaceMethod>* contractMethods(const std::string& name);
    // A resolved contract's `for`-clause mask. Same two-table shape as contractMethods(), and for the
    // same reason: a generic contract's INSTANCE is minted lazily, so a lookup can run before the mint
    // and has to reach the template. A MANGLED instance name (`Real_double`) is a key in neither table
    // until then, hence `tmplHint` — the pre-mangle base the caller already has. 0 = not kind-gated.
    unsigned implKindsOf(const std::string& contract, const std::string& tmplHint = std::string());
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
    bool ifaceNewAllocator(const std::string& ty, ObjectCreationNode* oc, int line,
                           const char* verb = "new");   // `try new` wants its own spelling in the advice
    // If `cls` implements the prelude `HeapOwner<T>` contract, the owned element `T` (so `new T(args)` can
    // placement-construct into `cls` via its `adopt(UnsafePtr<T>)`); "" otherwise. Inert when no HeapOwner in scope.
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
    // `seed` pre-binds the parameters a use site wrote explicitly, so the rest can still be inferred;
    // null means infer everything. See explicitGenericInst for who seeds and why.
    bool inferGenericInst(FunctionDeclarationNode* tmpl, const std::string& key, SharedArgumentList args,
                          std::map<std::string, SharedIdentifier>& localTys, int line, GenericInst& out,
                          const std::map<std::string, SharedIdentifier>* seed = nullptr);
    // Turbofish and/or `#(…)`: bind a generic function's params from what the use site wrote explicitly
    // (bypassing argument inference — reaches return-only generics inference can't). Arity + bounds are
    // checked PER GROUP, and an omitted-but-inferable group falls through to inferGenericInst.
    // `nTypeArgs` is the callee name's group split (-1 = synthesized, i.e. every slot written).
    bool explicitGenericInst(FunctionDeclarationNode* tmpl, const std::string& key, SharedIdentifierList typeArgs,
                             int nTypeArgs, SharedArgumentList callArgs,
                             std::map<std::string, SharedIdentifier>* localTys, int line, GenericInst& out);
    void emitGenericInst(const GenericInst& gi, bool prototypeOnly);
    void registerInstColls();   // MCU 6b-1: register const-param-derived collection sizes (`InlineArray<T,(N+1)>`)
    void registerInstGenerics(); // discover generic-fn calls inside a generic TYPE's members (per instantiation)

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
    void rejectMintProtocolValue(SharedIdentifier ty, const char* what, int line);   // a `@viewable` contract is not a value
    std::string smartPtrInvalidate(const std::string& expr, CollKind kind, bool ifaceElem = false);  // null the dtor's guard field
    std::string moveNullStmt(const std::string& cls, const std::string& expr) const;   // the moved-FROM intrinsic's reset, by shape
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
    std::string pinnedInstanceName(const std::string& bare, const std::string& t) const;
    // The name a COMPILER-SYNTHESIZED conformance must record — `pinnedInstanceName`, plus minting the
    // instance, since there is no `implements` clause to drive the usual path. See its definition.
    std::string synthConformanceName(const std::string& bare, const std::string& typeName);
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
    // Resolve every field's declared type in ITS OWN class's scope, once, before any body is walked.
    // Runs immediately before computeDestructible, whose context install it reuses. See the definition
    // for why a read site must not do this itself.
    void bakeFieldCTypes();
    // A field's C type: the baked answer, else resolve it now (an instance minted after the bake).
    std::string fieldCType(const std::string& ownerCls, const FieldInfo& f);
    void computeDestructible();
    void computeReachesPointer();   // serialization mode gate — sibling of computeDestructible
    void computeAtomicRefcount();      // M6.2: a `Shared`/`Weak` instance over a deeply-immutable T takes the atomic ctrl-block flavor
    // ...and the SAME gate on the other crossing: a `spawn` bundle. Recorded during emission (a `spawn`
    // lives in a body, which the collect-time channel pass cannot see) and checked after it.
    // `what` names the crossing for the diagnostic: the bundle itself, or a `parallel_for` element / capture.
    struct SpawnBundle { std::string cls; int line = 0; std::string file; std::string what = "as the bundle"; };
    std::vector<SpawnBundle> _spawnBundles;
    void checkSpawnBundleSendability();
    // Sendability is DECLARED (`implements Sendable`) and VERIFIED (checkSendableDeclarations); a crossing
    // requires the declaration. `unsendableReason` is the one predicate, asked "why not" — "" means Sendable.
    void        checkSendableDeclarations();
    bool        isSendableClass(const std::string& cls);
    std::string unsendableReason(const std::string& cls);
    bool        contractRequiresSendable(const std::string& name) const;   // `type contract C implements Sendable`
    void        enterClassCtx(const ClassInfo& ci);   // resolve as the class's own body does; caller saves/restores _nsCtx/_typeSubst
    // A `type view` may not implement a contract whose `ctor` slot constructs the implementer out of
    // parameters that carry no borrow — such a view could only borrow a constructor local. Rejected at the
    // `implements`, because no body can satisfy it.
    void checkViewContractCtors();
    // An `implements` clause promises the contract's SIGNATURES — return type, parameter types, arity —
    // and until this check it promised nothing: a vtable slot is filled with a cast, so a mismatch
    // reached the C compiler at the use sites (or, on a direct call, ran and truncated silently).
    // Detected on the C spelling (ground truth after aliases/imports/substitution); reported in kama.
    struct ConfSig;
    ConfSig contractSigOf(InterfaceInfo& ii, const InterfaceMethod& m);
    ConfSig implSigOf(ClassInfo& tci, ClassInfo* owner, MethodInfo* mi);
    std::string kamaTypeText(SharedIdentifier t);
    void checkConformanceSignature(ClassInfo& tci, const std::string& contract,
                                   const std::string& tkey, int line);
    void checkConformanceSignatures();   // the `_classes` sweep; enums/intrinsics come via checkImplCompleteness
    void checkOverrideSignatures();      // the same promise one axis over: a derived `override` vs its base
    void reportSigMismatch(const ConfSig& want, const ConfSig& have, const std::string& lead,
                           const std::string& authority, const std::string& rule, int at);
    // One report per (type-or-template, contract, member): a generic type's conformance is checked on
    // its INSTANCES, so a template-level mismatch would otherwise repeat per instantiation.
    std::set<std::string> _conformanceSigChecked;
    void checkViewableContracts();   // a `@viewable` contract must have a member that could mint
    bool paramCanCarryBorrow(FunctionParameterNode* p, const std::string& selfParam) const;
    // The `UnsafePtr` containment rule (the unsafe seam). `namesUnsafePtr` is TRUE when a type node IS
    // `UnsafePtr` or CONTAINS one in a generic argument — `Optional<UnsafePtr>` is the shape that made a
    // token-based rule leak, since `match (a.allocate(…)) { case Some(value: p): … }` binds an `UnsafePtr`
    // and never spells it.
    //
    // Keyed on the SOURCE spelling, deliberately, exactly as `paramCanCarryBorrow` and `_viewTypeNames`
    // are: a rule that read the SUBSTITUTED type would make `DynamicArray<UnsafePtr<int32>>` force every
    // method of `DynamicArray` unsafe at that one instantiation and not at others — a diagnostic that
    // depends on monomorphization, reported at a declaration the author of the instantiation never wrote.
    // Holding a raw pointer is legal (an `UnsafePtr` FIELD is legal by design); it is naming one in a
    // signature, and producing or handling one in a body, that the marker exists to make greppable.
    static bool namesUnsafePtr(SharedIdentifier type);
    // Diagnose an expression position whose type is a raw pointer outside an `unsafe fn`.
    // No-op inside one. Returns true if it rejected.
    bool grantedMint(const ClassInfo& ci, const std::string& member) const;  // `member` is a nullary member of a `@viewable` contract `ci` implements
    bool declaresViewable(const ClassInfo& ci) const;   // the host of a `parallel_for` declared it hands out a view — `grantedMint(ci, "view")`
    bool rejectRawOutsideUnsafe(const char* what, int line);
    // The signature half: a declaration NAMING a raw pointer (return type or any parameter) must be
    // `unsafe`. Applied only where a body exists — an `abstract` member and a `contract` member are
    // bodiless conduits, forced instead by the types their implementer and caller must handle.
    bool namesViewType(SharedIdentifier t) const;   // the SOURCE spelling names a `type view`
    void rejectViewByRef(FunctionParameterNode* p, const std::string& owner, int line);
    void checkViewRefParams();                      // the `ref`/`out` view ban — a late whole-program pass
    // By-`ref` parameters awaiting the view test, staged during `collectSignatures` (which runs before
    // `collectClasses`, so no view name is known yet) with the file that declared them.
    struct PendingViewByRef { FunctionParameterNode* param; std::string owner; int line; std::string file; };
    std::vector<PendingViewByRef> _pendingViewByRef;
    void checkSignatureRawPtr(bool isUnsafe, SharedIdentifier ret, SharedParameterList params,
                              const std::string& name, int line);
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
    // `@generate(Formattable)` — the synthesized infallible field-dump `void T__format(T* self, Formatter* f)` and
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
    // True (and diagnosed) for a PRIMITIVE serde field with no wire form — `isize`/`usize`, whose width
    // is platform-varying. Both directions funnel through it; see the definition for why.
    bool serdeRejectsPrimitive(SharedIdentifier ty, const std::string& access, bool writing, int line);
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
    // boxing even for a STATIC-ONLY conformance — the enum→interface capability. Set when a variant target
    // implements a contract, and (P2+) when a contract is a `Result` E-arg or an `Owned/Shared/Weak<C>`
    // element. Distinct from `_polyContracts` (serialization graph edges). For such a contract,
    // `emitClassInterfaceVtables` emits `<Impl>__as_<C>` even for a static-only conformance (so an enum
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
    // used to validate the element (integer/`UnsafePtr` scalar only) and to exempt an `Atomic` from the
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
    // Deep immutability, as the derived `Immutable` bound asks — reads the qualifier too (ordering).
    bool isImmutableType(const ClassInfo& ci) const;
    bool classSatisfiesBound(ClassInfo* ci, const std::string& contract);
    ClassInfo* implTargetInfo(const std::string& tkey);   // collection/primitive impl-conformance ClassInfo
    // AND over a `when [P: B, …]` gate: every gated param's concrete arg must satisfy its bound. `params`
    // are the template's type-param names, `concrete` the instance's args (index-aligned).
    // The contract a `when [P: B]` gate names, RESOLVED — a conformance list holds resolved names.
    std::string resolveWhenBound(const SharedIdentifier& b);
    bool whenConditionsHold(const std::vector<std::string>& whenParams,
                            const std::vector<std::string>& whenBounds,
                            const std::vector<std::string>& params,
                            const std::vector<SharedIdentifier>& concrete);
    // Does `ci` NOMINALLY `implements` a contract whose template is `tmpl` (any instantiation)? Checks the
    // recorded `interfaces` list (a plain name == tmpl, or a generic instance whose `templateKey` == tmpl).
    bool implementsContractTemplate(ClassInfo* ci, const std::string& tmpl);
    // Verify a concrete type arg satisfies each contract bound on a type parameter (else diagnose).
    // False means REFUSED — the caller poisons the instance so the template's body is never walked with
    // an argument the bound rejected. A deferral (a not-yet-concrete arg) answers true, not false.
    bool checkBounds(const std::string& paramName, SharedIdentifier concreteArg,
                     SharedIdentifierList bounds, int line, const std::string& templateKey);
    // Resolve a generic's BOUNDS under the template's home namespace rather than the call site's, so a
    // bound need not be imported by every caller. RAII — restores `_nsCtx` on scope exit.
    struct BoundCtxScope {
        BoundCtxScope(CEmitter* e, const std::string& templateKey);
        ~BoundCtxScope();
        CEmitter* _e; NsCtx _saved;
    };
    std::string basePathTo(ClassInfo* from, ClassInfo* to);   // "__base." chain from `from` down to `to`
    std::string vptrPrefix(ClassInfo* ci);                    // "__base." * (hops to vtableRoot)
    void checkDerivedPublicSurface();                         // decision A: no widening, no `implements`
    void emitVtableType(ClassInfo& ci);                       // only when ci is its own vtableRoot
    void emitVtableInstance(ClassInfo& ci);                   // for every class with hasVtable
    std::string vtableSlotSig(const VSlot& s);                // "(Owner* self, T a, ...)"

    // Contracts
    bool isInterface(const std::string& name) const { return _interfaces.count(name) != 0; }
    // A non-escaping borrow: a contract (fat-ptr, borrows its object) OR a `type view` (borrows a raw
    // `UnsafePtr<T>`). Both are rejected as a FIELD or COLLECTION ELEMENT — they'd dangle. (A view may still be
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
    // `selfByValue` is the target's `ClassInfo::isScalarRecv` — a PRIMITIVE conformance takes `this` as the
    // scalar itself (`int32_t self`), not a pointer. It is passed in rather than derived from `selfType`
    // because the conformance registry is keyed by the KAMA type name while `selfType` is the C one.
    std::string paramListC(SharedParameterList params, const char* selfType,
                           const char* ownerCType = nullptr, bool selfByValue = false);
    // `nameOverride`: emit under a supplied mangled name instead of the declared one
    // (used for generic instantiations, whose C name carries the concrete type args).
    void emitFunctionPrototype(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);
    void emitFunction(FunctionDeclarationNode* fn, const std::string* nameOverride = nullptr);

    // Classes
    bool isClass(const std::string& name) const { return _classes.count(name) != 0; }
    bool isBaseOf(const std::string& base, const std::string& derived) const;   // base in derived's chain
    std::string namespaceOfType(const std::string& value) const;  // `ns::path` of a registered type with bare name `value`, else "" (missing-import diagnostic)
    bool isTypeParamName(const std::string& n) const;              // `n` is a generic type-param (any template's, or an active binding)
    // The VALUE-position twin of checkTypeResolves: a bare or `::`-qualified name that no binding table,
    // function, module static, enum, type constant or type resolved. Says what the name IS when it is a
    // type, a variant of some enum, a member reached with the wrong operator — and "cannot resolve" only
    // when it is nothing at all. Never called for a name a table bound.
    void rejectUnresolvedName(IdentifierNode* v, const std::string& nm);
    // `nm` is a `comptime` parameter of the function or type being emitted, read while nothing binds it —
    // the uninstantiated-template probe leaves every const param unbound on purpose, and a real
    // instantiation whose argument did not fold was already reported at its call site. Neither is a defect
    // of the body, so the identifier arm defers (probe) or stays silent (cascade) instead of rejecting.
    bool isComptimeParamHere(const std::string& nm) const;
    void checkTypeResolves(SharedIdentifier type, const std::string& cTypeResult,
                           const char* what, int line);  // unresolved type name -> missing-import / unknown-type diagnostic
    void checkDeclaredTypes(const std::vector<SharedCompilationUnit>& units);  // the same check over every DECLARED type (param/return/field)
    // Every generic template NOBODY instantiates, walked once for its diagnostics alone.
    //
    // `analyze()` IS `emit()`, and the emit walk SKIPS a template body (`emitModuleContent`'s
    // `fn->typeParams` continues) — so every rule in this file is invisible inside an uninstantiated
    // generic BY CONSTRUCTION, and four distinct errors in one built clean. The fix is not another rule:
    // it is to stop skipping the body. This re-emits each such body into a throwaway sink with the type
    // params left SYMBOLIC (`_typeSubst` empty — `emitGenericInst` minus `bindInstParams`), so the whole
    // rule set applies to the concrete half of the body and keeps applying as rules are added.
    //
    // A `T`-typed expression goes quiet on its own: `cType` hands an unresolved name back unchanged, so it
    // lands in the `exprClass == ""` bucket ~40 callers already read as "not a class, take the raw-C path".
    // That is the pass's REACH, not a gap it hides — `--probe-templates` reports it as a column.
    //
    // Runs LAST, after every real emission: instantiation discovery is finished by then, so the walk
    // cannot register work the program does not use, and nothing downstream reads what it touches.
    void checkUninstantiatedTemplates();
    // The same walk for a generic TYPE or `enum` nobody instantiates. It could not exist before opaque
    // parameters: a `_genericTypes` entry is a SHAPE AWAITING SPECIALIZATION, not a class — its `when`
    // gates are unevaluated and its `ctors` map is not the one an instance gets — so handing it straight
    // to `emitClassDefinitions` failed 628 of 641 corpus fixtures when it was tried. What turns a shape
    // into a class is `registerGenericTypeInst`, and that needs real arguments; distinct synthetic ones
    // are exactly what an opaque parameter is. So this registers a probe instance and hands it to
    // `emitGenericTypeInst`, the very function a real instantiation goes through.
    void checkUninstantiatedTypeTemplates();
    SharedIdentifier probeConstArg();   // the placeholder a probe puts in a `const N: int32` slot
    long _probeTypesWalked = 0;    // generic types given a probe instance
    // THE FILE RUNG, one predicate for every position: a reference to a symbol declared in ANOTHER file
    // requires that file to have `export`ed it. See the definition for the three deliberate blind spots.
    void checkReach(const std::string& key, const std::string& spelled, const char* what, int line,
                    const std::string& refFile, bool qualified);
    std::string declFileOf(const std::string& key) const;   // declaring unit of a resolved symbol, or ""
    std::string refFilePath() const;                        // the file a reference is written in, or ""
    // False until `_exported` is filled. Collection resolves names too, and the rung cannot be answered
    // against an empty export set — every reference would be rejected.
    bool _exportedReady = false;
    // A type spelling reaches no further than an `import` would: reject one naming a symbol its declaring
    // file does not `export`. Split out of `checkDeclaredTypes` because a LOCAL declaration gets this
    // clause alone, without the resolution half. Caller owns `_nsCtx`.
    void checkQualifiedExport(const SharedIdentifier& t, const char* what);
    // `null` into a slot whose declared type is a safe kama type — rejected. The sibling of the `== null`
    // rule, for the STORE direction. `whereClause` completes "so ___ cannot be `null`".
    void rejectNullInit(SharedIdentifier declType, SharedExpression init, const char* what, int line);
    // The first expression type check kama has ever had. KINDS only — the four families a value can
    // belong to, not its width. `Unknown` is the whole design: it is what everything the classifier
    // cannot answer becomes, and it never diagnoses. Widening/narrowing WITHIN a kind is milestone 6.
    enum class TKind { Unknown, Num, Bool, Str, Aggregate };
    TKind kindOfCType(const std::string& ct);            // kind of an already-lowered C type name
    TKind declTypeKind(SharedIdentifier type);           // kind of a DECLARED kama type node
    TKind exprKind(SharedExpression e);                  // kind of an expression; Unknown unless certain
    // The LOWERED C type of any expression, "" when not certain. Total, and "" NEVER diagnoses — it is
    // `TKind::Unknown` one level down. This is the resolver every finer rule composes from: the kind
    // rule above reads it through `kindOfCType`, and width/conversion checking will read it directly.
    std::string typeOfExpr(SharedExpression e);
    std::string classifierCType(SharedIdentifier type);  // cType, but "" wherever cType would DIAGNOSE
    // A saved `_localTypes` / `_localTypeNodes` entry, so a scoped binding can be undone. Both the match
    // emitter and the classifier bind arm payloads; only the emitter may diagnose, so they share the
    // save/restore shape rather than the install.
    struct SavedLocalType { std::string name; bool had; std::string prev; bool hadNode; SharedIdentifier prevNode; };
    void restoreLocalTypeBindings(const std::vector<SavedLocalType>& saved);
    // The `match` subject's variant class, QUIETLY — the same four-step recovery `emitMatchSwitch` does
    // (`exprClass`, through a `give` hand-off, the discovery-stashed inline instance, the qualified
    // variant name), with no diagnostic on failure. "" when it is not a resolvable variant class, which
    // is the only answer a total classifier may give. `inlineSubj` (optional) reports steps 3–4.
    std::string matchSubjectClassQuiet(MatchNode* m, bool* inlineSubj = nullptr);
    // Install one arm's payload bindings for the duration of classifying that arm's value. Binds the
    // subject instance's type args itself, per resolution — the caller must NOT hold them open.
    std::vector<SavedLocalType> bindArmPayloadTypes(const ClassInfo& ci, const SharedMatchArm& a);
    void noteNumericHandoff(const std::string& dstCType, SharedExpression value,
                            const char* what, int line);   // M5a measurement; silent unless the flag is on
    void noteNumericOperands(int opToken, SharedExpression lhs, SharedExpression rhs,
                             const char* posWhat, int line);   // the seventh position; same flag, same rows
    // Type IDENTITY, which the kind rule above cannot express. `TKind` has five buckets, so two DISTINCT
    // kama types that land in the same bucket are interchangeable at every hand-off — and the numeric
    // rules cannot catch them either, because they bail on any C spelling that is not a name they know.
    // A family is a set of types whose members are mutually non-interchangeable AND recognizable from an
    // already-lowered C type. `None` is silent, exactly as `TKind::Unknown` is.
    enum class IdFamily { None, Enum, Num, Sig, Char };
    IdFamily idFamilyOf(const std::string& ct);
    static const char* idFamilyName(IdFamily f);
    void noteTypeIdentity(const std::string& dstCType, SharedExpression value,
                          const char* what, int line);     // the identity measurement; same hidden flag
    void noteTypeIdentityOperands(int opToken, SharedExpression lhs, SharedExpression rhs,
                                  const char* opName, int line);
    std::string idTypeName(const std::string& ct);       // the name a diagnostic gives an identity-bearing type
    bool classDeclaresContract(const std::string& cls, const std::string& itf);   // itf on `cls` or any base
    bool valueReachesContract(SharedExpression e, const std::string& c, const std::string& itf);
    // A concrete value bound to a contract it does not implement. One-sided: silent unless certain.
    void rejectContractNonConformance(const std::string& itf, SharedExpression value,
                                      const char* what, int line);
    std::string sigShapeNote(const std::string& srcCType, const std::string& dstCType);
    // Two distinct types that share a C spelling, at a hand-off. The identity peer of
    // `rejectNumericConversion`, for the families it and the kind rule both skip.
    void rejectTypeIdentityMismatch(const std::string& dstCType, SharedExpression value,
                                    const char* what, bool isInit, int line);
    void rejectNumericConversion(const std::string& dstCType, SharedExpression value,
                                 const char* what, bool isInit, int line);   // M6: no implicit conversion
    void rejectMixedOperands(int opToken, SharedExpression lhs, SharedExpression rhs,
                             const std::string& opName, int line);   // M6: the seventh position
    std::string indexElemTypeRaw(SharedExpression e);    // `a[i]`'s element type, class OR primitive
    static const char* kindName(TKind k);                // the word a diagnostic uses for a kind
    // Initializer whose KIND cannot be the declared type's. `what` completes "so ___ cannot be …".
    void rejectInitKindMismatch(SharedIdentifier declType, SharedExpression init, const char* what, int line);
    // The type `e` is declared as, when that type can never BE null; "" when `null` is legitimate there.
    // Resolves a bare local through its DECLARED type node before falling back to `exprClass`, which is
    // empty for a primitive and so cannot tell an `int32` from an `UnsafePtr`.
    std::string neverNullType(SharedExpression e);
    // Fall-off-the-end analysis: a non-void function must return on every path (or diverge).
    void checkReturns(FunctionDeclarationNode* fn, ClassMethodDeclarationNode* md, const char* what);
    bool alwaysExits(const SharedStatement& s) const;   // provably returns or diverges (one-sided: no => "cannot prove")
    bool exprDiverges(const ASTNode* n) const;          // a `panic(...)` call
    bool hasLoopBreak(const SharedStatement& s) const;  // a `break` escaping THIS loop
    bool isLiteralTrue(const SharedExpression& e) const;
    // A ctor call spelled on a generic TEMPLATE name (`Box.make(…)`) — resolved from the assignment
    // target, so the two assignment paths share this one predicate. See the definition.
    bool isGenericDotCtorCall(ASTNode* r);
    std::string ptrElemType(SharedExpression e);   // if `e` is a raw `this.field[i]` where field is UnsafePtr<T>, the element C-type; else ""
    std::string ptrLocalElemType(SharedExpression e);  // if `e` is a bare-LOCAL `buf[i]` where buf is UnsafePtr<T>, the element C-type; else "" (store-path only)
    std::string exprClass(SharedExpression e);          // class name of expr, "" if unknown/primitive
    std::string receiverScalarCType(SharedExpression e); // C scalar type of a primitive receiver place (`p.x`, `arr[i]`), "" if none
    // The exact KAMA type node behind a place expression (local/param/foreach binding, or a field through an
    // instance) — the only channel that keeps `char` apart from `uint32`, which share the C type `uint32_t`.
    SharedIdentifier receiverTypeNode(SharedExpression e);
    bool exprIsChar(SharedExpression e);                // true iff `e`'s kama type is `char` (a char literal, local/param/foreach binding, or a char field)
    int holeBuiltinType(SharedExpression e);            // IDENTIFIER_*_VAL of an interp hole's numeric kama type (local/param/field/literal), 0 if unknown
    void emitHoleSpec(const std::string& fv, SharedExpression hole, const std::string& spec);  // format-specifier fast-path for `${x:spec}`
    void emitHoleInto(const std::string& fv, SharedExpression hole, SharedString spec);        // render one hole into Formatter `fv` (spec / char / Formattable dispatch); shared by plain + tagged interpolation
    std::string lvalueCType(SharedExpression e);        // C type of an lvalue local/param/field, KEEPING collection/string types
    bool exprIsString(SharedExpression e);              // true iff `e` statically has kama type `string` (kama_string)
    std::string hoistStringTemp(SharedExpression e);    // owned-string RVALUE -> a scope-dtor'd temp (frees it); "" for lvalue/literal/non-string
    void emitStruct(ClassInfo& ci);
    void emitVariantStruct(ClassInfo& ci);   // tag + union layout of a discriminated-union enum
    void emitClassPrototypes(ClassInfo& ci);
    void emitClassDefinitions(ClassInfo& ci);
    // Emits a method / operator / named-`ctor` body. There is no `isCtor` flag: a named `ctor` is a static
    // factory with no `self`, so it needs none of the instance-ctor prologue the flag used to select.
    // `memberName` is the KAMA name of the member being emitted (not the mangled `cName`) — the mint grant
    // asks whether a `@viewable` contract declares a member by that name, and the mangled form cannot be
    // split back apart reliably once a generic instance is in it.
    void emitMethodOrCtorBody(const std::string& cName, const char* retType,
                              SharedParameterList params, SharedBlock body,
                              ClassInfo& owner, bool isConstMethod = false,
                              bool isStatic = false, bool isUnsafe = false,
                              const char* memberName = nullptr,
                              SharedAttributeList attrs = SharedAttributeList());
    // The `__attribute__((...))` a member attribute lowers to, computed by emitMethodOrCtorBody and
    // consumed by the signature line inside it. A member is not on declAttrPrefix's two function paths.
    std::string _memberAttrPrefix;
    // Bring zero-inited storage of class `ty` (named `nm` in C) up to a valid empty state — field
    // initializers, each field's `default` ctor, and the vtable pointer. Shared by the bare class-local
    // declaration path and by a `ctor`'s implicit `this` storage, which must agree exactly.
    // `moveKey` is the prefix under which the fill seeds per-field MOVE STATE; it defaults to `nm`
    // but differs where the C name differs from the source name (a ctor's storage: `__self` / `this`).
    void emitAggregateFill(const std::string& nm, const std::string& ty, int lineNo, int depth,
                           const std::string& moveKey = std::string(),
                           SharedExpression baseInit = SharedExpression());
    // `this` in a ctor is `self`, a `T*`. True where the destination wants the `T` BY VALUE (a return temp,
    // a variant payload) and the pointer must therefore be dereferenced.
    bool ctorThisAsValue(SharedExpression e, const std::string& dstCType) const;
    std::string emitMemberAccess(MemberAccessNode* ma);
    void             rejectBaseMember(MemberAccessNode* ma);
    int              readMaxDepth(const SharedModifier& mod, int line);
    bool             isThisBase(SharedExpression e);
    bool             exprMentionsThis(SharedExpression e);
    SharedExpression baseInstallOf(SharedStatement st);
    void             checkBaseInstall(ClassInfo& owner, SharedBlock body, int ctorLine);
    std::string emitMethodCall(InvocationNode* call, MemberAccessNode* recv);
    bool        isTypeReceiver(MemberAccessNode* ma, std::string& outType);            // X.name -> X is a type?
    std::string dotOnTypeInstance(MemberAccessNode* recv, const std::string& typeName); // X::<A>.name -> the instance
    // What a member reached through a TYPE actually is, and the one sentence that names its right spelling.
    // Shared by the call path (`V.f(...)`, emitDotOnTypeCtorCall) and the read path (`V.f`,
    // rejectDotOnTypeRead) so the two can never disagree on how the `::`/`.` split is written.
    enum class DotMemberKind { Field, Const, Static, Ctor, Method };
    std::string dotOnTypeAdvice(DotMemberKind k, const std::string& disp, const std::string& typeName,
                                const std::string& member);
    // `nm` is bound as a VALUE here (local, param, field of the enclosing type, module static, comptime
    // param, function) — the set a `.` head resolves through before it could mean a type. A live binding
    // WINS over a same-spelled type, the precedence isTypeReceiver uses.
    bool isValueName(const std::string& nm, SharedStringList qualifier);
    // `X.name` with no call, where `X` is a TYPE: an enum variant, a field, a `comptime` constant, a static
    // or a method reached with the constructor spelling. Reports and returns true; false when `X` is not
    // a type at all (the identifier arm then says what `X` is not).
    bool rejectDotOnTypeRead(MemberAccessNode* ma, IdentifierNode* head, const std::string& field);
    std::string newFactoryCall(const std::string& cls, ObjectCreationNode* oc, int lineNo);  // new Type.name(...) factory
    void        emitNewFactoryMove(const std::string& cls, const std::string& slotPtr,  // new Type.name(...) construct
                                   ObjectCreationNode* oc, int lineNo, int depth);
    bool        ctorIsFallible(ObjectCreationNode* oc);                                 // new Type.name(...) ctor returns Result?
    std::string emitFallibleNewBox(const std::string& target, const std::string& lval, // M4b: fallible new -> Result<Owned<T>,E>
                                   ObjectCreationNode* oc, int srcLine);
    std::string emitTryNewBox(const std::string& target, const std::string& lval,      // M-step5: try new -> Optional<Owned<T>>
                              ObjectCreationNode* oc, int srcLine);
    std::string emitTryCast(const std::string& target, const std::string& lval,        // try cast<T> -> Optional<T>
                            CastNode* cst, int srcLine);
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
    void emitBorrow(BorrowNode* bn, int depth);      // `borrow h as v { }` — the lexical window a view is minted into
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
    // The no-heap fact a destructible local carries — shared with the by-value smart-ptr PARAMETER path.
    void noteDestructibleOwner(const std::string& className);
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
    std::string tryHoistInlineValue(SharedExpression e, const std::string& targetCType, int srcLine);
    // Materialize `value` (unwrapping a give/copy marker; resolving an inline ctor/`new`/bare-generic-ctor
    // from `dstCType`) and assign it into the already-declared lvalue `dst` of type `dstCType`, applying the
    // give/copy matrix for an OWNED value (smart-ptr / resource / collection / bindable — move consumes the
    // source, copy duplicates). Shared by `return` and value-producing `match` arms (`:= give x` / `:= List()`).
    void emitOwnedValueInto(const std::string& dst, const std::string& dstCType,
                            SharedExpression value, int line, int depth,
                            const char* what = "this value");
    // The kind rule against an ALREADY-LOWERED destination C type, for the hand-off sites that have one
    // (a return value, a `match` arm, a call argument) rather than a declared type node.
    // Two user classes that are not the same class, not an upcast, and not a widening. See the block
    // comment on the definition — the `ref` path always checked this and the by-value path never did.
    bool plainUserClass(const std::string& ct) const;
    void rejectClassIdentityMismatch(const std::string& dstCType, SharedExpression value,
                                     const char* what, int line);
    void rejectValueKindMismatch(const std::string& dstCType, SharedExpression value,
                                 const char* what, int line);
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
    // Validate one `@align(N)`/`@packed` into a type's layout state. See the definition for why N is
    // held to a power of two rather than passed through.
    void        readLayoutAttr(const AttributeNode& at, int& alignN, bool& packed, int line);
    // The trailing `__attribute__((packed, aligned(N)))` on a struct definition; "" when unannotated.
    std::string layoutAttrSuffix(const ClassInfo& ci) const;

    // The `default:` arm closing an exhaustive match's switch. `break` unless the tag is a pinned integer,
    // where the arm diverges instead — see the definition for why the dead edge is a compile error (an
    // unassigned temp in the value form, a missing return in the statement form) and not merely lost.
    void        emitMatchDefaultArm(bool hasWildcard, bool pinnedTag, const std::string& what, int depth);

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
    // THE FILE RUNG FOR MODULE-SCOPE VARIABLES. Qualified name -> the file that declared it. Without this
    // `declFileOf` returns "" for a module `comptime`/`static`, so `checkReach` waves through every
    // cross-file reference and the mistake only surfaces as a clang "use of undeclared identifier" — a
    // check/build divergence, and the reason KB-3 read as "a comptime cannot be exported" when the real
    // state was that module-scope variables had never been wired into the module system at all.
    std::map<std::string, std::string> _moduleVarFile;
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
    // The PLACE a expression designates: the base binding plus its chain of field names. This is the
    // whole safety core of the view model — see `placesConflict`. Not `const`: resolving a bare field
    // name to `this.<name>` needs `findFieldOwner`.
    std::vector<std::string> placePath(SharedExpression e);
    static bool placesConflict(const std::vector<std::string>& a, const std::vector<std::string>& b);
    static std::string placeText(const std::vector<std::string>& p);   // "this.inner.buf", for diagnostics
    // The innermost enclosing `borrow` whose frozen host overlaps `p`, or null. Walks `_scopes` back to
    // front, exactly as `markMoved` does for its own scope-shaped question.
    const Scope::FrozenPlace* frozenConflict(const std::vector<std::string>& p) const;
    const std::vector<std::string>* frozenAliasRoot(const std::string& name) const;  // the place a `borrow` ALIAS views
    std::vector<std::string> viewRootPlace(SharedExpression e);   // the place a VIEW expression borrows
    bool viewLocalBounded(SharedExpression init);   // the window rule: is this view's root lifetime-bounded?
    SharedIdentifier mintReturnTypeNode(SharedExpression host, std::map<std::string, SharedIdentifier>& localTys);
    bool rejectFrozenWrite(SharedExpression target, int line);   // one sentence for every write shape
    // View-return escape check (B4): the root a returned view ultimately BORROWS. `viewReturnRoot`
    // dispatches on the return form (view ctor / chained call / bare place); `borrowArgRoot` traces a
    // view-ctor's borrowed-pointer argument through `addr(of: …)` and a `recv.dataPtr()` call.
    std::string viewReturnRoot(SharedExpression e) const;
    std::string borrowArgRoot(SharedExpression e) const;
    // The root a view CONSTRUCTOR call borrows, matched BY ARGUMENT NAME against the ctor's declared
    // parameters — every one of them that can carry a borrow, not just the first one written.
    std::string viewCtorBorrowRoot(IdentifierNode* typeId, const std::string& method,
                                   SharedArgumentList args) const;
    // Does a borrowed root outlive the call? (`this`, a `ref` param, or a view param.) The single
    // predicate behind the B4 return check and the ctor-argument check, so the two cannot drift.
    bool        isSafeViewRoot(const std::string& root) const;
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
    void        checkConstPlaceReturn(bool isConst, bool isRef, const std::string& m, int line);
    // error if a body-level BINDER (a local, a `foreach` variable, a `match` payload binding) takes the
    // name of a comptime param bound in this instantiation — see the definition for why.
    void        checkConstParamBinder(const std::string& nm, const char* kind, int srcLine);
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
    // (a primitive / raw `UnsafePtr` — zero is a valid value; an intrinsic collection — zero is a valid empty; or a
    // type with an explicit `default` ctor). Otherwise it must be explicitly assigned. `concreteCType` is the
    // field's type ALREADY resolved to its concrete C name (under the active _typeSubst / per instance).
    bool        isDefaultFillable(const std::string& concreteCType);
    // Owning read-before-assign is a compile error. `params` (optional) brings the `out` parameters into
    // the analysis: they start UNASSIGNED, so reading one is an error and every return must have filled it.
    void        checkDefiniteAssignment(SharedBlock body, SharedParameterList params = SharedParameterList());
    std::string emitFnPtrBind(const std::string& sigCName, SharedExpression init, int line);
    // The one resolver behind every `fnptr` bind, and the two judgements over it. See the block comment
    // on `resolveFnPtrTarget` for why the split exists (only a local initializer used to be checked).
    bool resolveFnPtrTarget(SharedExpression init, FnPtrTarget& out);
    void checkFnPtrBind(const std::string& sigCName, const FnPtrTarget& t, int line);
    void checkFnPtrValueBind(const std::string& dstCType, SharedExpression value, int line);
    // A kama function crossing to C — the seam must DECLARE the callee's threading contract (row 1).
    void checkForeignCrossing(const std::string& calleeCName, const ParamSig& p,
                              SharedExpression argExpr, int line);
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
    // The conformance-registry key: the KAMA spelling for a scalar primitive, `cType` for everything else.
    // `cType` is not injective (`char` and `uint32` both emit `uint32_t`), and a conformance must be.
    std::string primKey(SharedIdentifier type);
    std::string primKeyOfCType(const std::string& ct);   // the same, recovered from a C type (`uint32_t` -> `uint32`)
    static bool isScalarPrimKey(const std::string& k);   // true iff `k` names a scalar primitive, not a C type
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

    // Both render their message through demangleForDisplay first, so an internal mangled name
    // (`_F<file>__Plain`, `std__collections__Map_int32_..._GlobalAllocator`) can never reach the user or the LSP.
    const std::string& diagFile() const;   // the file a diagnostic belongs to — see the definition
    void unsupported(const char* rawWhat, int srcLine);
    // ...and the form that names the SYMBOL the defect is about, for the sites an editor can offer a fix
    // on (an unimported name, an unknown type). See Diagnostic::subject.
    void unsupported(const char* rawWhat, int srcLine, const std::string& subject);
    // Mangled -> source spelling, applied at the single point a message becomes visible (see the .cpp).
    std::string demangleForDisplay(const std::string& msg, int depth = 0) const;
    // A call's resolved return type, UNFILTERED (class, plain enum or primitive). exprClass keeps the
    // classes; exprEnumType keeps the enums. See the .cpp.
    std::string callReturnTypeRaw(InvocationNode* inv);
    // The extern-call gate. CALLING a C function is the unsafe act — the declaration is bodiless, so it
    // carries no marker of its own. No scalar exemption: see the definition.
    void gateExternCall(const FuncSig& sig, const std::string& name, int line);

    // Where an attribute was written. It used to be inferred from `fn` being null, which could say only
    // "function or static" — a member has no FunctionDeclarationNode either, so it would have been read
    // as a static and rejected with the wrong noun. The site is named explicitly instead.
    enum class AttrSite { Function, ModuleStatic, Member };

    // MCU step 4: lower `@interrupt` / `@section(".x")` to a C `__attribute__((...))` prefix.
    // `fn` is null at every site but Function (it carries `@interrupt`'s return/param/expose probes).
    std::string declAttrPrefix(const SharedAttributeList& attrs, FunctionDeclarationNode* fn, int line,
                               AttrSite site = AttrSite::Function);
    bool fnHasNoHeap(FunctionDeclarationNode* fn) const;                 // does this fn carry `@noheap`?
    bool hasNoHeapAttr(const SharedAttributeList& attrs) const;          // ...same question, node-free
    bool hasAttr(const SharedAttributeList& attrs, const char* name) const;   // ...for any attribute name
    void rejectIfNoHeap(const char* what, int line);                    // the ONE no-heap gate (`--no-heap`/`@noheap`)
    void rejectNoHeapIndirect(const char* what, int line);              // ...and its half for an unresolvable call
#if !KAMA_INHERITANCE
    void rejectInheritance(const char* what, int line);                 // the ONE gate for KAMA_INHERITANCE=0
#endif

    // Multi-file: collect a whole program, then emit declarations (shared
    // header) and definitions (per module) separately.
    void collectProgram(const std::vector<SharedCompilationUnit>& units);
    void emitHeaderContent(const std::vector<SharedCompilationUnit>& units);  // typedefs/structs/protos/macros
    void emitModuleContent(SharedCompilationUnit unit);                       // this file's vtables + defs
    // MCU step 1: module-level `static`. `declOnly` emits the header's `extern` declaration of a MUTABLE
    // static (nothing for a `comptime`); the default emits the definition — see the definition for why
    // a mutable static has external linkage.
    void emitModuleStaticDecl(ModuleVariableDeclaration* mv, bool declOnly = false);
};

#endif // __KAMA_CEMIT_H__
