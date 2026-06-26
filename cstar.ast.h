#ifndef __CSTAR_AST_H__
#define __CSTAR_AST_H__

#include <iostream>
#include <llvm/IR/Value.h>
#include <llvm/ADT/STLExtras.h>
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
    CodeGenRtn codeGen(CodeGenContext& context);
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context) = 0;
    void debugPrint(CodeGenContext& context, const llvm::Twine& prefix){ debugPrintInternal(std::cout, context, prefix); }
    void debugPrintPart(CodeGenContext& context, SharedAST node, const llvm::Twine& prefix);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix) = 0;
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class NamespaceDeclarationNode : public StatementNode {
public:
    SharedIdentifier name;
    NamespaceDeclarationNode(CodeGenContext& context, SharedIdentifier name)
        : ASTNode(context),  StatementNode(context), name(name) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class UsingDeclarationNode : public StatementNode {
public:
    SharedIdentifier identifier;
    SharedIdentifier alias;
    UsingDeclarationNode(CodeGenContext& context, SharedIdentifier identifier)
        : ASTNode(context),  StatementNode(context), identifier(identifier) { }
    UsingDeclarationNode(CodeGenContext& context, SharedIdentifier identifier, SharedIdentifier alias)
        : ASTNode(context),  StatementNode(context), identifier(identifier), alias(alias) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

//------------------------------------------------------------------------------ 
//                              Primitive Types
//------------------------------------------------------------------------------

class Int8Node : public ExpressionNode {
public:
    int8_t value;
    Int8Node(CodeGenContext& context, int8_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class Int16Node : public ExpressionNode {
public:
    int16_t value;
    Int16Node(CodeGenContext& context, int16_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class Int32Node : public ExpressionNode {
public:
    int32_t value;
    Int32Node(CodeGenContext& context, int32_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class Int64Node : public ExpressionNode {
public:
    int64_t value;
    Int64Node(CodeGenContext& context, int64_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class UInt8Node : public ExpressionNode {
public:
    uint8_t value;
    UInt8Node(CodeGenContext& context, uint8_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class UInt16Node : public ExpressionNode {
public:
    uint16_t value;
    UInt16Node(CodeGenContext& context, uint16_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class UInt32Node : public ExpressionNode {
public:
    uint32_t value;
    UInt32Node(CodeGenContext& context, uint32_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class UInt64Node : public ExpressionNode {
public:
    uint64_t value;
    UInt64Node(CodeGenContext& context, uint64_t value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class Float32Node : public ExpressionNode {
public:
    float value;
    Float32Node(CodeGenContext& context, float value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class Float64Node : public ExpressionNode {
public:
    double value;
    Float64Node(CodeGenContext& context, double value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class StringNode : public ExpressionNode {
public:
    SharedString value;
    StringNode(CodeGenContext& context, SharedString value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class BooleanNode : public ExpressionNode {
public:
    bool value;
    BooleanNode(CodeGenContext& context, bool value) : ASTNode(context),  ExpressionNode(context), value(value) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class NullNode : public ExpressionNode {
public:
    NullNode(CodeGenContext& context) : ASTNode(context),  ExpressionNode(context) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    SharedString generic;
    void setQualifier(SharedStringList qualifier){ qualifier = qualifier; }

    IdentifierNode(CodeGenContext& context, SharedString value, int builtInVal = IDENTIFIER_NONE_VAL)
        : ASTNode(context),  ExpressionNode(context), builtInVal(builtInVal), value(value), qualifier( std::make_shared<StringList>() ), generic( SharedString() ) { }
    IdentifierNode(CodeGenContext& context, SharedString value, SharedStringList qualifier, SharedString generic)
        : ASTNode(context),  ExpressionNode(context), builtInVal(IDENTIFIER_NONE_VAL), value(value), qualifier(qualifier), generic(generic) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    FunctionDeclarationNode(CodeGenContext& context,  SharedModifier modifier, SharedIdentifier returnType, SharedIdentifier name, 
                            SharedParameterList parameters, SharedBlock block ) 
        : ASTNode(context),  StatementNode(context)
        , modifier(modifier)
        , returnType(returnType)
        , name(name)
        , parameters(parameters)
        , block(block) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class FunctionParameterNode : public ExpressionNode {
public:
    SharedModifier modifier;
    SharedIdentifier type;
    SharedIdentifier identifier;
    FunctionParameterNode(CodeGenContext& context, SharedModifier modifier, SharedIdentifier type, SharedIdentifier identifier) 
        : ASTNode(context),  ExpressionNode(context), modifier(modifier), type(type), identifier(identifier) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

//------------------------------------------------------------------------------ 
//                              Statements
//------------------------------------------------------------------------------

class BlockNode : public StatementNode {
public:
    SharedStatementList statements;
    BlockNode(CodeGenContext& context, SharedStatementList statements)
        : ASTNode(context),  StatementNode(context), statements(statements) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class VariableDeclarator : public StatementNode {
public:
    SharedIdentifier name;
    SharedExpression initializer;
    VariableDeclarator(CodeGenContext& context, SharedIdentifier name, SharedExpression initializer)
        : ASTNode(context),  StatementNode(context), name(name), initializer(initializer) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ConstVariableDeclarator : public StatementNode {
public:
    SharedIdentifier name;
    SharedExpression initializer;
    ConstVariableDeclarator(CodeGenContext& context, SharedIdentifier name, SharedExpression initializer)
        : ASTNode(context),  StatementNode(context), name(name), initializer(initializer) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context){ /* No Op */  return NULL; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class LocalVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedVariableDeclaratorList variables;
    LocalVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
    virtual SymbolType symbolType() { return SymbolType::VARIABLE; }
};

class ConstLocalVariableDeclaration : public StatementNode {
public:
    SharedIdentifier type;
    SharedConstVariableDeclaratorList variables;
    ConstLocalVariableDeclaration(CodeGenContext& context, SharedIdentifier type, SharedConstVariableDeclaratorList variables)
        : ASTNode(context),  StatementNode(context), type(type), variables(variables) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class SwitchNode : public StatementNode {
public:
    SharedExpression expression;
    SharedSwitchSectionList switchsections;
    SwitchNode(CodeGenContext& context, SharedExpression expression, SharedSwitchSectionList switchsections)
        : ASTNode(context),  StatementNode(context), expression(expression), switchsections(switchsections) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class SwitchSectionNode : public StatementNode {
public:
    SharedSwitchLabelList labels;
    SharedStatementList statementList;
    SwitchSectionNode(CodeGenContext& context, SharedSwitchLabelList labels, SharedStatementList statementList)
        : ASTNode(context),  StatementNode(context), labels(labels)
    , statementList(statementList) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class SwitchLabelNode : public StatementNode {
public:
    SharedExpression constantExpression;
    SwitchLabelNode(CodeGenContext& context, SharedExpression constantExpression)
        : ASTNode(context),  StatementNode(context), constantExpression(constantExpression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    bool isDefault(){ return !constantExpression; }
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class WhileNode : public StatementNode {
public:
    SharedExpression booleanExpression;
    SharedStatement whileStatement;
    WhileNode(CodeGenContext& context,  SharedExpression booleanExpression, SharedStatement whileStatement)
        : ASTNode(context),  StatementNode(context)
        , booleanExpression(booleanExpression)
        , whileStatement(whileStatement) {}
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class DoWhileNode : public StatementNode {
public:
    SharedExpression booleanExpression;
    SharedStatement doWhileStatement;
    DoWhileNode(CodeGenContext& context,  SharedExpression booleanExpression, SharedStatement doWhileStatement)
        : ASTNode(context),  StatementNode(context)
        , booleanExpression(booleanExpression)
        , doWhileStatement(doWhileStatement) {}
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ForEachNode : public StatementNode {
public:
    SharedIdentifier type;
    SharedIdentifier name;
    SharedExpression expression;
    SharedStatement body;
    ForEachNode(CodeGenContext& context,  SharedIdentifier type,
                SharedIdentifier name,
                SharedExpression expression,
                SharedStatement body)
    : ASTNode(context),  StatementNode(context)
    , type(type)
    , name(name)
    , expression(expression)
    , body(body) {}
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class BreakNode : public StatementNode {
public:
    BreakNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ContinueNode : public StatementNode {
public:
    ContinueNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ReturnNode : public StatementNode {
public:
    SharedExpression expression;
    ReturnNode(CodeGenContext& context, SharedExpression expression) : ASTNode(context),  StatementNode(context), expression(expression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ThisAccessNode : public ExpressionNode {
public:
    ThisAccessNode(CodeGenContext& context) : ASTNode(context),  ExpressionNode(context) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class BaseAccessNode : public ExpressionNode {
public:
    SharedIdentifier identifier;
    SharedExpressionList expressionlist;
    BaseAccessNode(CodeGenContext& context, SharedIdentifier identifier) : ASTNode(context),  ExpressionNode(context), identifier(identifier) { }
    BaseAccessNode(CodeGenContext& context, SharedExpressionList expressionlist) : ASTNode(context),  ExpressionNode(context), expressionlist(expressionlist) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class SimpleUnaryExpressionNode : public ExpressionNode {
public:
    int token;
    SharedExpression expression;
    SimpleUnaryExpressionNode(CodeGenContext& context, int token, SharedExpression expression) 
        : ASTNode(context),  ExpressionNode(context), token(token), expression(expression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class CastNode : public ExpressionNode {
public:
    SharedIdentifier type;
    SharedExpression unaryExpression;
    CastNode(CodeGenContext& context, SharedIdentifier type, SharedExpression unaryExpression)
        : ASTNode(context),  ExpressionNode(context)
        , type(type)
        , unaryExpression(unaryExpression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class BinaryExpressionNode : public ExpressionNode {
public:
    int token;
    SharedExpression LHS;
    SharedExpression RHS;
    BinaryExpressionNode(CodeGenContext& context, int token, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), token(token), LHS(LHS), RHS(RHS) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class LogicalAndOrNode : public ExpressionNode {
public:
    int token;
    SharedExpression LHS;
    SharedExpression RHS;
    LogicalAndOrNode(CodeGenContext& context, int token, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), token(token), LHS(LHS), RHS(RHS) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class TernaryExpressionNode : public ExpressionNode {
public:
    SharedExpression condition;
    SharedExpression LHS;
    SharedExpression RHS;
    TernaryExpressionNode(CodeGenContext& context, SharedExpression condition, SharedExpression LHS, SharedExpression RHS)
        : ASTNode(context),  ExpressionNode(context), condition(condition), LHS(LHS), RHS(RHS) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ObjectCreationNode : public ExpressionStatementNode {
public:
    SharedIdentifier type;
    SharedArgumentList args;
    ObjectCreationNode(CodeGenContext& context, SharedIdentifier type, SharedArgumentList args)
        : ASTNode(context),  ExpressionStatementNode(context)
        , type(type)
        , args(args) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class PreIncrDecrNode : public ExpressionStatementNode {
public:
    int token;
    SharedExpression expression;
    PreIncrDecrNode(CodeGenContext& context, int token, SharedExpression expression)
        : ASTNode(context),  ExpressionStatementNode(context)
        , token(token)
        , expression(expression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class PostIncrDecrNode : public ExpressionStatementNode {
public:
    int token;
    SharedExpression expression;
    PostIncrDecrNode(CodeGenContext& context, int token, SharedExpression expression)
        : ASTNode(context),  ExpressionStatementNode(context)
        , token(token)
        , expression(expression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    ClassDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
                        SharedIdentifier name,
                        SharedClassBaseDeclaration baseTypes,
                        SharedClassMemberDeclarationList members)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , name(name)
        , baseTypes(baseTypes)
        , members(members) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ClassBaseDeclarationNode : public StatementNode {
public:
    SharedIdentifier base;
    SharedIdentifierList interfaces;
    ClassBaseDeclarationNode(CodeGenContext& context, SharedIdentifier base, SharedIdentifierList interfaces)
        : ASTNode(context),  StatementNode(context)
        , base(base)
        , interfaces(interfaces) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ClassMemberDeclarationNode : public StatementNode {
public:
    ClassMemberDeclarationNode(CodeGenContext& context) : ASTNode(context),  StatementNode(context) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ClassMethodDeclarationNode : public ClassMemberDeclarationNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier returnType;
    SharedIdentifier name;
    SharedParameterList params;
    SharedBlock body;
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ClassOperatorDeclaratorNode : public StatementNode {
public:
    SharedIdentifier returnType;
    int opToken;
    SharedIdentifier param1Type;
    SharedIdentifier param1Name;
    SharedIdentifier param2Type;
    SharedIdentifier param2Name;
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class ClassConstructorInitializerNode : public StatementNode {
public:
    SharedArgumentList args;
    ClassConstructorInitializerNode(CodeGenContext& context, SharedArgumentList args) : ASTNode(context),  StatementNode(context), args(args) {}
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
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
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

//------------------------------------------------------------------------------ 
//                              Enums
//------------------------------------------------------------------------------

class EnumDeclarationNode : public StatementNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier identifier;
    SharedEnumMemberDeclarationList body;
    EnumDeclarationNode(CodeGenContext& context, SharedModifierList modifiers, SharedIdentifier identifier, SharedEnumMemberDeclarationList body)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , identifier(identifier)
        , body(body) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

class EnumMemberDeclarationNode : public StatementNode {
public:
    SharedIdentifier identifier;
    SharedExpression constantExpression;
    EnumMemberDeclarationNode(CodeGenContext& context, SharedIdentifier identifier, SharedExpression constantExpression)
        : ASTNode(context),  StatementNode(context), identifier(identifier), constantExpression(constantExpression) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};

//------------------------------------------------------------------------------ 
//                              Interface
//------------------------------------------------------------------------------

class InterfaceDeclarationNode : public StatementNode {
public:
    SharedModifierList modifiers;
    SharedIdentifier identifier;
    SharedIdentifierList baseTypes;
    SharedFunctionDeclarationList body;
    InterfaceDeclarationNode(CodeGenContext& context, SharedModifierList modifiers,
                        SharedIdentifier identifier,
                        SharedIdentifierList baseTypes,
                        SharedFunctionDeclarationList body)
        : ASTNode(context),  StatementNode(context)
        , modifiers(modifiers)
        , identifier(identifier)
        , baseTypes(baseTypes)
        , body(body) { }
    virtual CodeGenRtn codeGenInternal(CodeGenContext& context);
    virtual void debugPrintInternal(std::ostream& stream, CodeGenContext& context, const llvm::Twine& prefix);
};


#endif //__CSTAR_AST_H__