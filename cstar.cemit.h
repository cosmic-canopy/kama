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
#include "cstar.forward.h"

// A function parameter, in declared order. Named cstar arguments are matched
// against these to recover C's positional order at each call site.
struct ParamSig {
    std::string name;
    bool        byRef;        // ref/out => passed as a pointer (call site emits &arg)
    std::string className;    // class type (for ref upcast at call sites), "" if primitive
};

struct FuncSig {
    std::string            cName;    // mangled C name (e.g. main -> cstar_main)
    std::vector<ParamSig>  params;
};

// ---- Class model (M4) -----------------------------------------------------

struct FieldInfo {
    std::string      name;
    SharedIdentifier type;
    SharedExpression initializer;    // optional; applied in the constructor
};

struct MethodInfo {
    std::string                  cName;   // Class__method (declaring class)
    SharedIdentifier             returnType;
    std::vector<ParamSig>        params;
    ClassMethodDeclarationNode*  node;    // for body emission
    bool                         isVirtual  = false;  // virtual/override/abstract
    bool                         isOverride = false;
    bool                         isAbstract = false;  // null body
};

struct ClassInfo {
    std::string                       name;       // struct name (== cstar class name in M4)
    std::vector<FieldInfo>            fields;      // declaration order
    std::set<std::string>            fieldNames;
    std::map<std::string, MethodInfo> methods;    // by cstar method name
    bool                              hasCtor = false;
    std::vector<ParamSig>             ctorParams;
    ClassConstructorDeclarationNode*  ctorNode = nullptr;
    ClassDeclarationNode*             node    = nullptr;

    // RAII (M5)
    bool                              hasDtor = false;   // declares its own ~dtor
    ClassDestructorDeclarationNode*   dtorNode = nullptr;
    bool                              destructible = false; // own dtor OR a destructible field (transitive)

    // Inheritance + virtual dispatch (M6)
    std::string                       baseName;        // "" if no base
    ClassInfo*                        base = nullptr;  // resolved by linkBases()
    bool                              isAbstractClass = false;
    bool                              hasVtable = false;     // this or an ancestor has a virtual
    std::string                       vtableRoot;            // class owning the __vptr member
    std::map<std::string,std::string> slotImpl;             // virtual slot name -> impl cName (most-derived here)
};

class CEmitter {
public:
    CEmitter(std::ostream& out, const std::string& sourcePath, bool emitLineDirectives);

    // Emit a full translation unit. Returns the number of unsupported nodes
    // encountered (0 == fully lowered).
    int emit(SharedCompilationUnit unit);

private:
    std::ostream& _out;
    std::string   _sourcePath;       // absolute path, used in #line directives
    bool          _lines;            // whether to emit #line directives
    int           _unsupported;      // count of nodes we could not lower

    std::map<std::string, FuncSig> _funcs;   // cstar function name -> signature
    std::set<std::string> _refParams;        // by-ref params of the function being emitted

    std::map<std::string, ClassInfo>   _classes;     // class name -> info
    std::map<std::string, std::string> _localTypes;  // local/param -> class name ("" if primitive)
    ClassInfo*                         _currentClass = nullptr;  // when emitting a method/ctor

    // Virtual dispatch (M6): per-root union of vtable slots, in introduction order.
    struct VSlot { std::string name; std::string owner; ClassMethodDeclarationNode* node; };
    std::map<std::string, std::vector<VSlot>> _rootVtables;   // root class name -> slots

    // RAII scope stack (M5): live destructible locals per lexical scope.
    struct LiveLocal { std::string cVar; std::string className; };
    struct Scope { std::vector<LiveLocal> locals; bool isLoopBoundary = false; bool isFunctionRoot = false; };
    std::vector<Scope> _scopes;
    std::string        _currentReturnCType = "void";  // for return-temp
    int                _tempCounter = 0;

    void line(int srcLine);                          // emit a #line directive
    void indent(int depth);

    // Pre-pass
    void collectSignatures(SharedCompilationUnit unit);
    void collectClasses(SharedCompilationUnit unit);
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
                              ClassInfo& owner, bool isCtor);
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
    void emitDtorDefinition(ClassInfo& ci);

    // Expressions -> C expression text
    std::string emitExpression(SharedExpression expr);
    std::string emitInvocation(InvocationNode* call);
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
};

#endif // __CSTAR_CEMIT_H__
