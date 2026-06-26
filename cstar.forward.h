#ifndef __CSTAR_FORWARD_H__
#define __CSTAR_FORWARD_H__

#include <vector>
#include <memory>
#include <map>

class ASTNode;
class CodeGenContext;
class CompilationUnit;
class StatementNode;
class ExpressionNode;
class IdentifierNode;
class UsingDeclarationNode;
class NamespaceDeclarationNode;
class CodeDeclarationNode;
class ModifierNode;
class FunctionDeclarationNode;
class FunctionParameterNode;
class BlockNode;
class VariableDeclarator;
class LocalVariableDeclaration;
class ConstVariableDeclarator;
class ConstLocalVariableDeclaration;
class ExpressionStatementNode;
class SwitchSectionNode;
class SwitchLabelNode;
class ArgumentNode;
class EnumMemberDeclarationNode;
class ClassBaseDeclarationNode;
class ClassMemberDeclarationNode;
class ClassOperatorDeclaratorNode;
class ClassConstructorDeclaratorNode;
class ClassConstructorInitializerNode;

typedef std::shared_ptr<ASTNode> SharedAST;
typedef std::shared_ptr<std::string> SharedString;
typedef std::shared_ptr<StatementNode> SharedStatement;
typedef std::shared_ptr<ExpressionNode> SharedExpression;
typedef std::shared_ptr<IdentifierNode> SharedIdentifier;
typedef std::shared_ptr<NamespaceDeclarationNode> SharedNamespaceDeclaration;
typedef std::shared_ptr<ModifierNode> SharedModifier;
typedef std::shared_ptr<UsingDeclarationNode> SharedUsingDeclaration;
typedef std::shared_ptr<FunctionParameterNode> SharedParameter;
typedef std::shared_ptr<FunctionDeclarationNode> SharedFunctionDeclaration;
typedef std::shared_ptr<BlockNode> SharedBlock;
typedef std::shared_ptr<VariableDeclarator> SharedVariableDeclarator;
typedef std::shared_ptr<LocalVariableDeclaration> SharedLocalVariableDeclaration;
typedef std::shared_ptr<ConstVariableDeclarator> SharedConstVariableDeclarator;
typedef std::shared_ptr<ConstLocalVariableDeclaration> SharedConstLocalVariableDeclaration;
typedef std::shared_ptr<ExpressionStatementNode> SharedExpressionStatement;
typedef std::shared_ptr<SwitchSectionNode> SharedSwitchSection;
typedef std::shared_ptr<SwitchLabelNode> SharedSwitchLabel;
typedef std::shared_ptr<ArgumentNode> SharedArgument;
typedef std::shared_ptr<EnumMemberDeclarationNode> SharedEnumMemberDeclaration;
typedef std::shared_ptr<ClassBaseDeclarationNode> SharedClassBaseDeclaration;
typedef std::shared_ptr<ClassMemberDeclarationNode> SharedClassMemberDeclaration;
typedef std::shared_ptr<ClassOperatorDeclaratorNode> SharedClassOperatorDeclarator;
typedef std::shared_ptr<ClassConstructorDeclaratorNode> SharedClassConstructorDeclarator;
typedef std::shared_ptr<ClassConstructorInitializerNode> SharedClassConstructorInitializer;

typedef std::vector<SharedString> StringList;
typedef std::vector<SharedStatement> StatementList;
typedef std::vector<SharedExpression> ExpressionList;
typedef std::vector<SharedUsingDeclaration> UsingDeclarationList;
typedef std::vector<SharedIdentifier> IdentifierList;
typedef std::vector<SharedModifier> ModifierList;
typedef std::vector<SharedParameter> ParameterList;
typedef std::vector<SharedFunctionDeclaration> FunctionDeclarationList;
typedef std::vector<SharedVariableDeclarator> VariableDeclaratorList;
typedef std::vector<SharedConstVariableDeclarator> ConstVariableDeclaratorList;
typedef std::vector<SharedSwitchSection> SwitchSectionList;
typedef std::vector<SharedSwitchLabel> SwitchLabelList;
typedef std::vector<SharedArgument> ArgumentList;
typedef std::vector<SharedEnumMemberDeclaration> EnumMemberDeclarationList;
typedef std::vector<SharedFunctionDeclaration> FunctionDeclarationList;
typedef std::vector<SharedClassMemberDeclaration> ClassMemberDeclarationList;

typedef std::shared_ptr<FunctionDeclarationList> SharedFunctionDeclarationList;
typedef std::shared_ptr<ParameterList> SharedParameterList;
typedef std::shared_ptr<StatementList> SharedStatementList;
typedef std::shared_ptr<StringList> SharedStringList;
typedef std::shared_ptr<UsingDeclarationList> SharedUsingDeclarationList;
typedef std::shared_ptr<IdentifierList> SharedIdentifierList;
typedef std::shared_ptr<ModifierList> SharedModifierList;
typedef std::shared_ptr<VariableDeclaratorList> SharedVariableDeclaratorList;
typedef std::shared_ptr<ConstVariableDeclaratorList> SharedConstVariableDeclaratorList;
typedef std::shared_ptr<SwitchSectionList> SharedSwitchSectionList;
typedef std::shared_ptr<SwitchLabelList> SharedSwitchLabelList;
typedef std::shared_ptr<ArgumentList> SharedArgumentList;
typedef std::shared_ptr<ExpressionList> SharedExpressionList;
typedef std::shared_ptr<EnumMemberDeclarationList> SharedEnumMemberDeclarationList;
typedef std::shared_ptr<FunctionDeclarationList> SharedFunctionDeclarationList;
typedef std::shared_ptr<ClassMemberDeclarationList> SharedClassMemberDeclarationList;

typedef std::shared_ptr<CompilationUnit> SharedCompilationUnit;
typedef std::shared_ptr<CodeGenContext> SharedCodeGenContext;
typedef llvm::Value* CodeGenRtn;

class CodeGenBlock;
class SymbolEntry;

typedef std::shared_ptr<SymbolEntry> SharedSymbolEntry;
typedef std::shared_ptr<CodeGenBlock> SharedCodeGenBlock;
typedef std::map<std::string, SharedSymbolEntry > NameValueMap;
typedef std::vector<SharedCodeGenBlock > CodeGenBlockList;

#define CreateCompilationUnit std::make_shared<CompilationUnit>
#define CreateCodegenContext std::make_shared<CodeGenContext>

#define CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE 1
#define CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE 0

#endif //__CSTAR_FORWARD_H__