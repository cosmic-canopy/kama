#ifndef __KAMA_FORWARD_H__
#define __KAMA_FORWARD_H__

#include <vector>
#include <memory>
#include <map>

class ASTNode;
class CodeGenContext;
class CompilationUnit;
class StatementNode;
class ExpressionNode;
class InterpolatedStringNode;
class IdentifierNode;
class UsingDeclarationNode;
class ImportDeclarationNode;
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
class ArgumentNode;
class AttributeNode;
class InvocationNode;
class MemberAccessNode;
class ElementAccessNode;
class ArrayLiteralNode;
class SizeofNode;
class ZeroValueNode;
class ForEachNode;
class ThisAccessNode;
class ObjectCreationNode;
class AsDowncastNode;
class EnumMemberDeclarationNode;
class EnumDeclarationNode;
class MatchNode;
class MatchArmNode;
class ClassDeclarationNode;
class ClassBaseDeclarationNode;
class ClassMemberDeclarationNode;
class ClassFieldDeclarationNode;
class ClassMethodDeclarationNode;
class ClassConstructorDeclarationNode;
class ClassDestructorDeclarationNode;
class ClassOperatorDeclaratorNode;
class ClassOperatorDeclarationNode;
class BinaryExpressionNode;
class ClassConstructorDeclaratorNode;
class ClassConstructorInitializerNode;

typedef std::shared_ptr<ASTNode> SharedAST;
typedef std::shared_ptr<std::string> SharedString;
typedef std::shared_ptr<StatementNode> SharedStatement;
typedef std::shared_ptr<ExpressionNode> SharedExpression;
typedef std::shared_ptr<InterpolatedStringNode> SharedInterpolatedString;
typedef std::shared_ptr<IdentifierNode> SharedIdentifier;
typedef std::shared_ptr<NamespaceDeclarationNode> SharedNamespaceDeclaration;
typedef std::shared_ptr<ModifierNode> SharedModifier;
typedef std::shared_ptr<UsingDeclarationNode> SharedUsingDeclaration;
typedef std::shared_ptr<ImportDeclarationNode> SharedImportDeclaration;
typedef std::shared_ptr<FunctionParameterNode> SharedParameter;
typedef std::shared_ptr<FunctionDeclarationNode> SharedFunctionDeclaration;
typedef std::shared_ptr<BlockNode> SharedBlock;
typedef std::shared_ptr<VariableDeclarator> SharedVariableDeclarator;
typedef std::shared_ptr<LocalVariableDeclaration> SharedLocalVariableDeclaration;
typedef std::shared_ptr<ConstVariableDeclarator> SharedConstVariableDeclarator;
typedef std::shared_ptr<ConstLocalVariableDeclaration> SharedConstLocalVariableDeclaration;
typedef std::shared_ptr<ExpressionStatementNode> SharedExpressionStatement;
typedef std::shared_ptr<MatchNode> SharedMatch;
typedef std::shared_ptr<MatchArmNode> SharedMatchArm;
typedef std::shared_ptr<ArgumentNode> SharedArgument;
typedef std::shared_ptr<AttributeNode> SharedAttribute;
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
typedef std::vector<SharedImportDeclaration> ImportDeclarationList;
typedef std::vector<SharedIdentifier> IdentifierList;
typedef std::vector<SharedModifier> ModifierList;
typedef std::vector<SharedParameter> ParameterList;
typedef std::vector<SharedFunctionDeclaration> FunctionDeclarationList;
typedef std::vector<SharedVariableDeclarator> VariableDeclaratorList;
typedef std::vector<SharedConstVariableDeclarator> ConstVariableDeclaratorList;
typedef std::vector<SharedMatchArm> MatchArmList;
typedef std::vector<SharedArgument> ArgumentList;
typedef std::vector<SharedAttribute> AttributeList;
typedef std::vector<SharedEnumMemberDeclaration> EnumMemberDeclarationList;
typedef std::vector<SharedFunctionDeclaration> FunctionDeclarationList;
typedef std::vector<SharedClassMemberDeclaration> ClassMemberDeclarationList;

typedef std::shared_ptr<FunctionDeclarationList> SharedFunctionDeclarationList;
typedef std::shared_ptr<ParameterList> SharedParameterList;
typedef std::shared_ptr<StatementList> SharedStatementList;
typedef std::shared_ptr<StringList> SharedStringList;
typedef std::shared_ptr<UsingDeclarationList> SharedUsingDeclarationList;
typedef std::shared_ptr<ImportDeclarationList> SharedImportDeclarationList;
typedef std::shared_ptr<IdentifierList> SharedIdentifierList;
// Contract bounds — one contract list per type parameter (parallel to a decl's typeParams;
// an entry is empty for an unbounded param). `Map<K: Hashable + Comparable, V>` -> bounds[0] =
// [Hashable, Comparable], bounds[1] = [].
typedef std::vector<SharedIdentifierList> BoundsList;
typedef std::shared_ptr<BoundsList> SharedBoundsList;
typedef std::shared_ptr<ModifierList> SharedModifierList;
typedef std::shared_ptr<VariableDeclaratorList> SharedVariableDeclaratorList;
typedef std::shared_ptr<ConstVariableDeclaratorList> SharedConstVariableDeclaratorList;
typedef std::shared_ptr<MatchArmList> SharedMatchArmList;
typedef std::shared_ptr<ArgumentList> SharedArgumentList;
typedef std::shared_ptr<AttributeList> SharedAttributeList;
typedef std::shared_ptr<ExpressionList> SharedExpressionList;
typedef std::shared_ptr<EnumMemberDeclarationList> SharedEnumMemberDeclarationList;
typedef std::shared_ptr<FunctionDeclarationList> SharedFunctionDeclarationList;
typedef std::shared_ptr<ClassMemberDeclarationList> SharedClassMemberDeclarationList;

typedef std::shared_ptr<CompilationUnit> SharedCompilationUnit;
typedef std::shared_ptr<CodeGenContext> SharedCodeGenContext;

#define CreateCompilationUnit std::make_shared<CompilationUnit>
#define CreateCodegenContext std::make_shared<CodeGenContext>

#define KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE 1
#define KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE 0

#endif //__KAMA_FORWARD_H__