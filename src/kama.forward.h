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
class IsolateNode;
class ScopeNode;
class ParallelForNode;
class AsmNode;
class ComptimeAssertNode;
class VariableDeclarator;
class LocalVariableDeclaration;
class ModuleVariableDeclaration;
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
class BitcastNode;
class EnumMemberDeclarationNode;
class EnumDeclarationNode;
class IntrinsicImplNode;
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

// An enum's body carries TWO lists: its variants, and — after the mandatory `;` separator — ordinary
// class members, the methods a `type enum X implements C` needs to satisfy C. A small holder rather
// than a node because the parser's `%union` is a struct of shared_ptrs and one production has to yield
// both lists at once. The `;` is what makes the body LALR(1): without it a bare `Foo` variant and a
// `Foo bar;` field are indistinguishable at one token of lookahead.
struct EnumBody {
    SharedEnumMemberDeclarationList  variants;
    SharedClassMemberDeclarationList members;
};
typedef std::shared_ptr<EnumBody> SharedEnumBody;

// One `<int8, int16> { … }` SPECIALIZATION SECTION inside a `type intrinsic` block: the members it
// carries replace the block's shared bodies, for those targets only. What it serves is a contract whose
// body genuinely cannot be shared across the set — `sqrt` needs `sqrtf` for float32 and `sqrt` for
// float64, and kama has no in-body type branching by design.
struct IntrinsicSection {
    SharedIdentifierList             targets;
    SharedClassMemberDeclarationList members;
};
typedef std::shared_ptr<IntrinsicSection> SharedIntrinsicSection;
typedef std::vector<SharedIntrinsicSection> IntrinsicSectionList;
typedef std::shared_ptr<IntrinsicSectionList> SharedIntrinsicSectionList;

// A `type intrinsic` body: bodies shared by every target in the set, plus any per-target sections.
struct IntrinsicBody {
    SharedClassMemberDeclarationList members;
    SharedIntrinsicSectionList      sections;
};
typedef std::shared_ptr<IntrinsicBody> SharedIntrinsicBody;

typedef std::shared_ptr<CompilationUnit> SharedCompilationUnit;
typedef std::shared_ptr<CodeGenContext> SharedCodeGenContext;

#define CreateCompilationUnit std::make_shared<CompilationUnit>
#define CreateCodegenContext std::make_shared<CodeGenContext>

#define KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE 1
#define KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE 0

// Defined in kama.l, over the lexer's own keyword table — so callers that must reject reserved words
// (the LSP's rename validation) can never drift from what the compiler actually reserves.
bool kamaIsKeyword(const char* word);
// The lexer's keyword table, for keyword completion (LSP M4.3) — one source of truth, as above.
size_t      kamaKeywordCount();
const char* kamaKeywordAt(size_t i);

#endif //__KAMA_FORWARD_H__