#ifndef __CSTAR_AST_H__
#define __CSTAR_AST_H__

#include <iostream>
#include <cstdint>      // int8_t … uint64_t (not transitively available on all libcs, e.g. Windows UCRT)
#include "cstar.forward.h"

enum SymbolType {
  UNDEFINED = 0,
  VARIABLE
};

class ASTNode {
public:
    int line;
    int column;
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
    SharedUsingDeclarationList usingDeclarationList;
    SharedStatementList codeDeclarationList;
    CompilationUnit(CodeGenContext& context, SharedString name, 
                    SharedNamespaceDeclaration nameSpace, 
                    SharedUsingDeclarationList usingDeclarationList,
                    SharedStatementList codeDeclarationList) 
        : ASTNode(context)
        , StatementNode(context)
        , name(name)
        , nameSpace(nameSpace)
        , usingDeclarationList(usingDeclarationList)
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


class IdentifierNode : public ExpressionNode {
public:
    int builtInVal;
    SharedString value;
    SharedStringList qualifier;
    SharedIdentifier genericArg;   // element type for Coll<T> (a full type); == genericArgs[0]
    SharedIdentifierList genericArgs;  // all type args for Pair<A,B> etc.; genericArg mirrors [0]
    SharedIdentifierList bounds;       // when this node is a type-PARAMETER (`K` in `<K: I + J>`),
                                       // its contract bounds [I, J]; empty/unset otherwise.
    bool isConstParam = false;         // const generic PARAMETER (`const N: int`) — a value, not a type
    SharedExpression constArgValue;    // const generic ARGUMENT that is a literal (`4` in `Fixed<T,4>`)
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
    ModifierNode(CodeGenContext& context, SharedString value)
        : ASTNode(context),  ExpressionNode(context), value(value), targets( std::make_shared<IdentifierList>() ) { }
    ModifierNode(CodeGenContext& context, SharedString value, SharedIdentifierList targets)
        : ASTNode(context),  ExpressionNode(context), value(value), targets(targets) { }
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
    SharedStringList constParams;  // names of const generic params (`const N: int`); subset of typeParams order
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
    LocalVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
    virtual SymbolType symbolType() { return SymbolType::VARIABLE; }
};

class ConstLocalVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedConstVariableDeclaratorList variables;
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

// `sizeof(T)` — the compile-time byte size of a type, a `usize` (lowers to C `sizeof(cType)`).
class SizeofNode : public ExpressionNode {
public:
    SharedIdentifier type;
    SizeofNode(CodeGenContext& context, SharedIdentifier type)
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
    ObjectCreationNode(CodeGenContext& context, SharedIdentifier type, SharedArgumentList args)
        : ASTNode(context),  ExpressionStatementNode(context)
        , type(type)
        , args(args) { }
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
    EnumDeclarationNode(CodeGenContext& context, SharedModifierList modifiers, SharedIdentifier identifier, SharedEnumMemberDeclarationList body)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , identifier(identifier)
        , body(body) { }
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
class MatchArmNode : public StatementNode {
public:
    SharedString     variantName;   // the variant matched; "_" = wildcard
    SharedStringList bindings;      // payload binding names in field order; null/empty if none
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


#endif //__CSTAR_AST_H__