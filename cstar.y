%{
#define YYERROR_VERBOSE
#define YYDEBUG 1
#include <memory>
#include <string>
#include <cstdlib>
#include "cstar.parser.hpp"
#include "cstar.ast.h"
#include "cstar.context.h"


struct LexerInstanceData*  yyget_extra ( yyscan_t scanner );
extern int yylex(YYSTYPE * yylval_param, yyscan_t scanner);

int yyerror(yyscan_t scanner, const char *msg);
SharedExpression createIntegerLiteralNode(CodeGenContext& context, int base, const std::string& str);

#define SCANNER_CODEGENCONTEXT *(yyget_extra(scanner)->codeGenContext)

%}

%code requires {

#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#include "cstar.ast.h"
#include "cstar.context.h"

#ifndef CSTAR_LEXER_INSTANCE_DATA
#define CSTAR_LEXER_INSTANCE_DATA

//#define CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE 1
//#define CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE 0

struct LexerInstanceData {
   int next_line_number;
   int next_column_number;
   SharedString workString;

   SharedCodeGenContext codeGenContext;
   SharedCompilationUnit compilationUnit;
};

struct cstaryystype {
  SharedExpression expression;
  SharedStatement statement;
  SharedIdentifier identifier;
  SharedNamespaceDeclaration namespacedeclaration;
  SharedModifier modifier;
  SharedUsingDeclaration usingdeclaration;
  SharedParameter parameter;
  SharedBlock block;
  SharedVariableDeclarator variabledeclarator;
  SharedConstVariableDeclarator constvariabledeclarator;
  SharedExpressionStatement expressionstatement;
  SharedSwitchSection switchsection;
  SharedSwitchLabel switchlabel;
  SharedArgument argument;
  SharedEnumMemberDeclaration enummemberdecl;
  SharedFunctionDeclaration functiondecl;
  SharedClassBaseDeclaration classbasedecl;
  SharedClassMemberDeclaration classmemberdecl;
  SharedClassOperatorDeclarator operatordeclarator;
  SharedClassConstructorDeclarator constructordeclarator;
  SharedClassConstructorInitializer constructorinitializer;

  SharedStringList strings;
  SharedUsingDeclarationList usingdeclarationlist;
  SharedStatementList statementlist;
  SharedIdentifierList identifierlist;
  SharedModifierList modifierlist;
  SharedParameterList parameterlist;
  SharedVariableDeclaratorList variabledeclaratorlist;
  SharedConstVariableDeclaratorList constvariabledeclaratorlist;
  SharedSwitchSectionList switchsectionlist;
  SharedSwitchLabelList switchlabellist;
  SharedArgumentList argumentlist;
  SharedExpressionList expressionlist;
  SharedEnumMemberDeclarationList enummemberdecllist;
  SharedFunctionDeclarationList functiondecllist;
  SharedClassMemberDeclarationList classmemberdecllist;
  
  SharedString string;
  int token;
};
#define YYSTYPE cstaryystype

#endif

}

/* Options */
%expect 1
%defines
%define api.pure full
%lex-param   { yyscan_t scanner }
%parse-param { yyscan_t scanner }



/* Tokens */
%token <string> IDENTIFIER 
%token <string> FLOAT_LITERAL_NO_SUFFIX FLOAT_LITERAL_32 FLOAT_LITERAL_64 CHARACTER_LITERAL STRING_LITERAL
%token <string> DEC_LITERAL_NO_SUFFIX HEX_LITERAL_NO_SUFFIX OCT_LITERAL_NO_SUFFIX BASED_LITERAL_NO_SUFFIX 
%token <string> DEC_LITERAL HEX_LITERAL OCT_LITERAL BASED_LITERAL

/* KEYWORDS */ 
%token <string> ABSTRACT BASE BOOL BREAK
%token <string> CASE CAST CLASS CONST CONTINUE
%token <string> DEFAULT DO DOUBLE ELSE ENUM EXPORT EXTERN EXTENDS IMPLEMENTS
%token <string> FALSE FINAL FLOAT32 FLOAT64
%token <string> FN FOR FOREACH IF IN
%token <string> INT INT8 INT16 INT32 INT64
%token <string> INTERFACE NAMESPACE
%token <string> NEW NULL_LITERAL OPERATOR OUT
%token <string> OVERRIDE PRIVATE PROTECTED PUBLIC FRIEND
%token <string> REF RETURN STATIC STRING
%token <string> SWITCH THIS TRUE
%token <string> UINT8 UINT16 UINT32 UINT64
%token <string> UNSAFE USING VIRTUAL VOID
%token <string> VOLATILE WHILE

/* PUNCTUATION AND SINGLE CHARACTER OPERATORS */
%token <token> COMMA ","
%token <token> LEFT_BRACKET "["
%token <token> RIGHT_BRACKET "]"

%token <token> LEFT_BRACE "{"
%token <token> RIGHT_BRACE "}"
%token <token> EXCLAMATION "!"
%token <token> PERCENT "%"
%token <token> AMP "&"
%token <token> LPAREN "("
%token <token> RPAREN ")"
%token <token> STAR "*"
%token <token> PLUS "+"
%token <token> MINUS "-"
%token <token> DOT "."
%token <token> SLASH "/"
%token <token> COLON ":"
%token <token> SEMICOLON ";"
%token <token> LT "<"
%token <token> EQ "="
%token <token> GT ">"
%token <token> QUESTION "?"
%token <token> BAR "|"
%token <token> TILDE "~"
%token <token> CARET "^"

/* MULTI-CHARACTER OPERATORS */
%token <token> PLUSEQ MINUSEQ STAREQ DIVEQ MODEQ
%token <token> XOREQ  ANDEQ OREQ LTLT GTGT GTGTEQ LTLTEQ EQEQ NOTEQ
%token <token> LEQ GEQ ANDAND OROR PLUSPLUS MINUSMINUS

/* non-terminals */
%type <token> assignment_operator overloadable_operator
%type <strings> qualifier
%type <expression> expression expression_opt literal boolean_literal variable_initializer
%type <expression> parenthesized_expression constant_expression boolean_expression for_condition_opt
%type <expression> for_condition unary_expression variable_reference primary_expression_no_parenthesis
%type <expression> postfix_expression cast_expression member_access element_access this_access
%type <expression> base_access primary_expression multiplicative_expression additive_expression
%type <expression> shift_expression relational_expression equality_expression and_expression
%type <expression> exclusive_or_expression inclusive_or_expression conditional_and_expression
%type <expression> conditional_expression conditional_or_expression
%type <expressionlist> expression_list
%type <statement> compilation_unit code_declaration type_declaration function_declaration statement
%type <statement> declaration_statement local_variable_declaration embedded_statement local_constant_declaration
%type <statement> empty_statement selection_statement iteration_statement jump_statement if_statement
%type <statement> switch_statement while_statement do_statement for_statement foreach_statement
%type <statement> break_statement continue_statement return_statement enum_declaration interface_declaration
%type <statement> class_declaration unsafe_statement
%type <statementlist> code_opt code_declarations statement_list statement_list_opt
%type <statementlist> for_initializer_opt for_initializer for_iterator_opt for_iterator statement_expression_list
%type <namespacedeclaration> namespace_opt
%type <usingdeclaration> using_directive using_alias_directive
%type <usingdeclarationlist> using_directives_opt using_directives
%type <identifier> basic_identifier qualified_identifier type_name type non_array_type simple_type function_return_type
%type <identifier> primitive_type numeric_type integral_type floating_point_type class_type qualified_identifier_no_generic
%type <identifierlist> friend_list interface_base_opt interface_base interface_type_list
%type <modifier> modifier function_modifier_opt parameter_modifier_opt
%type <modifierlist> modifiers modifiers_opt
%type <parameter> parameter
%type <parameterlist> parameter_list parameter_list_opt
%type <block> block method_body operator_body constructor_body
%type <variabledeclarator> variable_declarator
%type <variabledeclaratorlist> variable_declarators
%type <constvariabledeclarator> constant_declarator
%type <constvariabledeclaratorlist> constant_declarators
%type <expressionstatement> expression_statement statement_expression assignment invocation_expression
%type <expressionstatement> object_creation_expression new_expression post_increment_expression post_decrement_expression
%type <expressionstatement> pre_increment_expression pre_decrement_expression
%type <switchsection> switch_section
%type <switchlabel> switch_label
%type <switchsectionlist> switch_block switch_sections_opt switch_sections
%type <switchlabellist> switch_labels
   /* %type <unaryexpression> unary_expression */
   /*%type <binaryexpression>*/
%type <argument> argument
%type <argumentlist> argument_list_opt argument_list
%type <enummemberdecl> enum_member_declaration
%type <enummemberdecllist> enum_body enum_member_declarations_opt enum_member_declarations
%type <functiondecl> interface_member_declaration interface_method_declaration
%type <functiondecllist> interface_body interface_member_declarations_opt interface_member_declarations
%type <classbasedecl> class_base_opt class_base
%type <classmemberdecl> class_member_declaration constant_declaration field_declaration method_declaration
%type <classmemberdecl> operator_declaration constructor_declaration destructor_declaration
%type <classmemberdecllist> class_body class_member_declarations_opt class_member_declarations
%type <operatordeclarator> operator_declarator overloadable_operator_declarator
%type <constructordeclarator> constructor_declarator
%type <constructorinitializer> constructor_initializer_opt constructor_initializer

%start compilation_unit

%%

/*------------------------------------------------------------------------------ 
                              Useful Debug Printing

{ fprintf(stdout,"line: %d column: %d (%s)\n", yyget_extra(scanner)->line_number, yyget_extra(scanner)->column_number, ($1)->c_str()); }
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------ 
                              Compilation Unit 
------------------------------------------------------------------------------*/

compilation_unit
  : namespace_opt using_directives_opt code_opt  { yyget_extra(scanner)->compilationUnit = CreateCompilationUnit( SCANNER_CODEGENCONTEXT, yyget_extra(scanner)->codeGenContext->getModuleName(), $1, $2, $3); }
  ;

namespace_opt
  : /* Nothing */  { $$ = SharedNamespaceDeclaration(); }
  | NAMESPACE qualified_identifier_no_generic SEMICOLON  { $$ = std::make_shared<NamespaceDeclarationNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;

using_directives_opt
  : /* Nothing */   { $$ = std::make_shared<UsingDeclarationList>(); }
  | using_directives
  ;
using_directives
  : using_directive  { $$ = std::make_shared<UsingDeclarationList>(); $$->push_back($1); }
  | using_directives using_directive  { $1->push_back($2); }
  ;
using_directive
  : USING qualified_identifier_no_generic SEMICOLON  { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, $2); }
  | using_alias_directive
  ;
using_alias_directive
  : USING IDENTIFIER EQ qualified_identifier_no_generic SEMICOLON  { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, $4,std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2)); }
  ;

code_opt
  : /* Nothing */   { $$ = std::make_shared<StatementList>(); }
  | code_declarations
  ;
code_declarations
  : code_declaration   { $$ = std::make_shared<StatementList>(); $$->push_back($1); }
  | code_declarations code_declaration   { $1->push_back($2); }
  ;
code_declaration
  : function_declaration
  | type_declaration
  ;

/*------------------------------------------------------------------------------ 
                              Literals 
------------------------------------------------------------------------------*/

literal
  : boolean_literal
  | DEC_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, strtol( $1->c_str(), NULL, 10)); }
  | HEX_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, strtol( $1->c_str(), NULL, 16)); }
  | OCT_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, strtol( $1->substr(2).c_str(), NULL, 8)); }
  | BASED_LITERAL_NO_SUFFIX   { std::string::size_type underscoreIndex = $1->find('_');
    int base = strtol($1->substr(underscoreIndex + 1).c_str(), NULL, 10);
    $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, strtol( $1->substr(2, underscoreIndex - 3).c_str(), NULL, base)); 
  }
  | DEC_LITERAL   { $$ = createIntegerLiteralNode(SCANNER_CODEGENCONTEXT,  10, *$1 ); }
  | HEX_LITERAL   { $$ = createIntegerLiteralNode(SCANNER_CODEGENCONTEXT,  16, $1->substr(2) ); }
  | OCT_LITERAL   { $$ = createIntegerLiteralNode(SCANNER_CODEGENCONTEXT,  8, $1->substr(2) ); }
  | BASED_LITERAL   { $$ = createIntegerLiteralNode(SCANNER_CODEGENCONTEXT,  0, $1->substr(2) ); }

  | FLOAT_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Float64Node>(SCANNER_CODEGENCONTEXT, std::stod (*$1)); }
  | FLOAT_LITERAL_32   { $$ = std::make_shared<Float32Node>(SCANNER_CODEGENCONTEXT, std::stof ($1->substr(0,$1->length() - 3))); }
  | FLOAT_LITERAL_64   { $$ = std::make_shared<Float64Node>(SCANNER_CODEGENCONTEXT, std::stod ($1->substr(0,$1->length() - 3))); }
  | CHARACTER_LITERAL   { $$ = std::make_shared<StringNode>(SCANNER_CODEGENCONTEXT, $1); }
  | STRING_LITERAL   { $$ = std::make_shared<StringNode>(SCANNER_CODEGENCONTEXT, $1); }
  | NULL_LITERAL   { $$ = std::make_shared<NullNode>(SCANNER_CODEGENCONTEXT); }
  ;
boolean_literal
  : TRUE   { $$ = std::make_shared<BooleanNode>(SCANNER_CODEGENCONTEXT, true); }
  | FALSE   { $$ = std::make_shared<BooleanNode>(SCANNER_CODEGENCONTEXT, false); }
  ;

/*------------------------------------------------------------------------------ 
                              Basic Identification 
------------------------------------------------------------------------------*/

qualified_identifier
  : basic_identifier
  | qualifier basic_identifier   { $$ = $2; $$->setQualifier($1); }
  ;
qualifier
  : IDENTIFIER DOT { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | qualifier IDENTIFIER DOT { $1->push_back($2); }
  ;
basic_identifier
  : IDENTIFIER   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | IDENTIFIER LT type GT   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<StringList>(), $3); }
  ;

qualified_identifier_no_generic
  : IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | qualifier IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2, $1, SharedString() ); }
  ;

type_name
  : qualified_identifier
  ;

/*------------------------------------------------------------------------------ 
                              Types 
------------------------------------------------------------------------------*/

type
  : non_array_type
  ;
non_array_type
  : simple_type
  | type_name
  ;
simple_type
  : primitive_type
  | class_type
  ;
primitive_type
  : numeric_type
  | BOOL   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_BOOL_VAL); }
  ;
numeric_type
  : integral_type
  | floating_point_type
  ;
integral_type
  : INT   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_INT32_VAL); }
  | UINT8   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_UINT8_VAL); }
  | UINT16   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_UINT16_VAL); }
  | UINT32   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_UINT32_VAL); }
  | UINT64   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_UINT64_VAL); }
  | INT8   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_INT8_VAL); }
  | INT16   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_INT16_VAL); }
  | INT32   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_INT32_VAL); }
  | INT64   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_INT64_VAL); }
  ;
floating_point_type
  : DOUBLE   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_FLOAT64_VAL); }
  | FLOAT32   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_FLOAT32_VAL); }
  | FLOAT64   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_FLOAT64_VAL); }
  ;
class_type
  : STRING   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_STRING_VAL); }
  ;

type_declaration
  : class_declaration
  | interface_declaration
  | enum_declaration
  ;

/*------------------------------------------------------------------------------ 
                              Modifiers 
------------------------------------------------------------------------------*/

modifiers_opt
  : /* Nothing */   { $$ = std::make_shared<ModifierList>(); }
  | modifiers
  ;
modifiers
  : modifier   { $$ = std::make_shared<ModifierList>(); $$->push_back($1); }
  | modifiers modifier  { $1->push_back($2); }
  ;
modifier
  : ABSTRACT   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | EXTERN   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | OVERRIDE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | PRIVATE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | PROTECTED   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | PUBLIC   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | FRIEND LPAREN friend_list RPAREN   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | FINAL   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | STATIC   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | VIRTUAL   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | VOLATILE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | EXPORT   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  ;

friend_list
  : qualified_identifier   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | friend_list qualified_identifier   { $1->push_back($2); }
  ;

function_modifier_opt
  : /* Nothing */   { $$ = SharedModifier(); }
  | EXPORT   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  ;

/*------------------------------------------------------------------------------ 
                              Functions 
------------------------------------------------------------------------------*/

function_declaration
  : EXTERN STRING_LITERAL SEMICOLON   {
      $$ = std::make_shared<IncludeNode>(SCANNER_CODEGENCONTEXT, $2);   /* extern "<header.h>"; (FFI #include) */
   }
  | EXTERN FN function_return_type IDENTIFIER LPAREN parameter_list_opt RPAREN SEMICOLON   {
      $$ = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1), $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, SharedBlock() );
   }
  | function_modifier_opt FN function_return_type IDENTIFIER LPAREN parameter_list_opt RPAREN block   {
      $$ = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8 );
  }
  ;
function_return_type
  : type
  | VOID   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_VOID_VAL); }
  ;
parameter_list_opt
  : /* Nothing */   { $$ = std::make_shared<ParameterList>(); }
  | parameter_list
  ;
parameter_list
  : parameter   { $$ = std::make_shared<ParameterList>(); $$->push_back($1); }
  | parameter_list COMMA parameter   { $1->push_back($3); }
  ;
parameter
  : parameter_modifier_opt type IDENTIFIER   { $$ = std::make_shared<FunctionParameterNode>(SCANNER_CODEGENCONTEXT, $1, $2, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); }
  ;
parameter_modifier_opt
  : /* Nothing */ {  }
  | REF   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | OUT   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  ;

/*------------------------------------------------------------------------------ 
                              Statements 
------------------------------------------------------------------------------*/

statement
  : declaration_statement
  | embedded_statement
  ;
declaration_statement
  : local_variable_declaration SEMICOLON   { $$ = $1; }
  | local_constant_declaration SEMICOLON   { $$ = $1; }
  ;
local_variable_declaration
  : type variable_declarators   { $$ = std::make_shared<LocalVariableDeclaration>(SCANNER_CODEGENCONTEXT, $1, $2); }
  ;
variable_declarators
  : variable_declarator   { $$ = std::make_shared<VariableDeclaratorList>(); $$->push_back($1); }
  | variable_declarators COMMA variable_declarator   { $1->push_back($3); }
  ;
variable_declarator
  : IDENTIFIER   { $$ = std::make_shared<VariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression() ); }
  | IDENTIFIER EQ variable_initializer   { $$ = std::make_shared<VariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3); }
  ;
variable_initializer
  : expression
  ;
local_constant_declaration
  : CONST type constant_declarators   { $$ = std::make_shared<ConstLocalVariableDeclaration>(SCANNER_CODEGENCONTEXT, $2, $3); }
  ;
constant_declarators
  : constant_declarator   { $$ = std::make_shared<ConstVariableDeclaratorList>(); $$->push_back($1); }
  | constant_declarators COMMA constant_declarator   { $1->push_back($3); }
  ;
constant_declarator
  : IDENTIFIER EQ constant_expression   { $$ = std::make_shared<ConstVariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3); }
  ;
block
  : LEFT_BRACE statement_list_opt RIGHT_BRACE   { $$ = std::make_shared<BlockNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
statement_list_opt
  : /* Nothing */   { $$ = std::make_shared<StatementList>(); }
  | statement_list
  ;
statement_list
  : statement   { $$ = std::make_shared<StatementList>(); $$->push_back($1); }
  | statement_list statement   { $1->push_back($2); }
  ;
embedded_statement
  : block   { $$ = $1; }
  | empty_statement
  | expression_statement   { $$ = $1; }
  | selection_statement
  | iteration_statement
  | jump_statement
  | unsafe_statement
  ;
unsafe_statement
  : UNSAFE block   { $$ = std::make_shared<UnsafeNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
empty_statement
  : SEMICOLON   {  }
  ;
expression_statement
  : statement_expression SEMICOLON  { $$ = $1; }
  ;
statement_expression
  : invocation_expression
  | object_creation_expression
  | assignment
  | post_increment_expression
  | post_decrement_expression
  | pre_increment_expression
  | pre_decrement_expression
  ;
selection_statement
  : if_statement
  | switch_statement
  ;
if_statement
  : IF LPAREN boolean_expression RPAREN embedded_statement   { $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, SharedStatement()); }
  | IF LPAREN boolean_expression RPAREN embedded_statement ELSE embedded_statement   { $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, $7); }
  ;
switch_statement
  : SWITCH LPAREN expression RPAREN switch_block   { $$ = std::make_shared<SwitchNode>(SCANNER_CODEGENCONTEXT, $3, $5); }
  ;
switch_block
  : LEFT_BRACE switch_sections_opt RIGHT_BRACE   { $$ = $2; }
  ;
switch_sections_opt
  : /* Nothing */   { $$ = std::make_shared<SwitchSectionList>(); }
  | switch_sections
  ;
switch_sections
  : switch_section   { $$ = std::make_shared<SwitchSectionList>(); $$->push_back($1); }
  | switch_sections switch_section   { $1->push_back($2); }
  ;
switch_section
  : switch_labels statement_list   { $$ = std::make_shared<SwitchSectionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  ;
switch_labels
  : switch_label   { $$ = std::make_shared<SwitchLabelList>(); $$->push_back($1); }
  | switch_labels switch_label   { $1->push_back($2); }
  ;
switch_label
  : CASE constant_expression COLON   { $$ = std::make_shared<SwitchLabelNode>(SCANNER_CODEGENCONTEXT, $2); }
  | DEFAULT COLON   { $$ = std::make_shared<SwitchLabelNode>(SCANNER_CODEGENCONTEXT, SharedExpression()); }
  ;
iteration_statement
  : while_statement
  | do_statement
  | for_statement
  | foreach_statement
  ;
while_statement
  : WHILE LPAREN boolean_expression RPAREN embedded_statement   { $$ = std::make_shared<WhileNode>(SCANNER_CODEGENCONTEXT,  $3, $5 ); }
  ;
do_statement
  : DO embedded_statement WHILE LPAREN boolean_expression RPAREN SEMICOLON   { $$ = std::make_shared<DoWhileNode>(SCANNER_CODEGENCONTEXT,  $5, $2 ); }
  ;
for_statement
  : FOR LPAREN for_initializer_opt SEMICOLON for_condition_opt SEMICOLON for_iterator_opt RPAREN embedded_statement   { 
      $$ = std::make_shared<ForNode>(SCANNER_CODEGENCONTEXT, $3, $5, $7, $9); }
  ;
for_initializer_opt
  : /* Nothing */   { $$ = std::make_shared<StatementList>(); }
  | for_initializer
  ;
for_condition_opt
  : /* Nothing */   { $$ = SharedExpression(); }
  | for_condition
  ;
for_iterator_opt
  : /* Nothing */   { $$ = std::make_shared<StatementList>(); }
  | for_iterator
  ;
for_initializer
  : local_variable_declaration   { $$ = std::make_shared<StatementList>(); $$->push_back($1); }
  | statement_expression_list   { $$ = $1; }
  ;
for_condition
  : boolean_expression
  ;
for_iterator
  : statement_expression_list   { $$ = $1; }
  ;
statement_expression_list
  : statement_expression   { $$ = std::make_shared<StatementList>(); $$->push_back($1); }
  | statement_expression_list COMMA statement_expression   { $1->push_back($3); }
  ;
foreach_statement
  : FOREACH LPAREN type IDENTIFIER IN expression RPAREN embedded_statement   { $$ = std::make_shared<ForEachNode>(SCANNER_CODEGENCONTEXT,  $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8); }
  ;
jump_statement
  : break_statement
  | continue_statement
  | return_statement
  ;
break_statement
  : BREAK SEMICOLON   { $$ = std::make_shared<BreakNode>(SCANNER_CODEGENCONTEXT); }
  ;
continue_statement
  : CONTINUE SEMICOLON   { $$ = std::make_shared<ContinueNode>(SCANNER_CODEGENCONTEXT); }
  ;
return_statement
  : RETURN expression_opt SEMICOLON   { $$ = std::make_shared<ReturnNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
expression_opt
  : /* Nothing */   { $$ = SharedExpression(); }
  | expression
  ;
semicolon_opt
  : /* Nothing */   {  }
  | SEMICOLON
  ;

/*------------------------------------------------------------------------------ 
                              Expressions
------------------------------------------------------------------------------*/

expression
  : conditional_expression
  | assignment   { $$ = $1; }
  ;
assignment
  : unary_expression assignment_operator expression   { $$ = std::make_shared<AssignmentNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  ;
assignment_operator
  : EQ
  | PLUSEQ
  | MINUSEQ
  | STAREQ
  | DIVEQ
  | MODEQ
  | XOREQ
  | ANDEQ
  | OREQ
  | GTGTEQ
  | LTLTEQ
  ;
primary_expression
  : parenthesized_expression
  | primary_expression_no_parenthesis
  ;
primary_expression_no_parenthesis
  : literal
  | member_access
  | invocation_expression   { $$ = $1; }
  | element_access
  | this_access
  | base_access
  | new_expression   { $$ = $1; }
  ;
parenthesized_expression
  : LPAREN expression RPAREN   { $$ = $2; }
  ;
member_access
  : primary_expression DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); }
  | class_type DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); }
  ;
invocation_expression
  : primary_expression_no_parenthesis LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | qualified_identifier_no_generic LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  ;
argument_list_opt
  : /* Nothing */   { $$ = std::make_shared<ArgumentList>(); }
  | argument_list
  ;
argument_list
  : argument   { $$ = std::make_shared<ArgumentList>(); $$->push_back($1); }
  | argument_list COMMA argument   { $1->push_back($3); }
  ;
argument
  : IDENTIFIER COLON expression   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), $3); }
  | IDENTIFIER COLON REF variable_reference   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $3), $4); }
  | IDENTIFIER COLON OUT variable_reference   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $3), $4); }
  ;
variable_reference
  : expression
  ;
element_access
  : primary_expression LEFT_BRACKET expression_list RIGHT_BRACKET   { $$ = std::make_shared<ElementAccessNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | qualified_identifier_no_generic LEFT_BRACKET expression_list RIGHT_BRACKET   { $$ = std::make_shared<ElementAccessNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  ;
this_access
  : THIS   { $$ = std::make_shared<ThisAccessNode>(SCANNER_CODEGENCONTEXT); }
  ;
base_access
  : BASE DOT IDENTIFIER   { $$ = std::make_shared<BaseAccessNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3) ); }
  | BASE LEFT_BRACKET expression_list RIGHT_BRACKET   { $$ = std::make_shared<BaseAccessNode>(SCANNER_CODEGENCONTEXT, $3); }
  ;
new_expression
  : object_creation_expression
  ;
object_creation_expression
  : NEW type LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT,  $2, $4 ); }
  ;
unary_expression
  : postfix_expression
  | EXCLAMATION unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | TILDE unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | cast_expression
  | PLUS unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | MINUS unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | pre_increment_expression   { $$ = $1; }
  | pre_decrement_expression   { $$ = $1; }
  ;
postfix_expression
  : primary_expression
  | qualified_identifier_no_generic   { $$ = $1; }
  | post_increment_expression   { $$ = $1; }
  | post_decrement_expression   { $$ = $1; }
  ;
pre_increment_expression
  : PLUSPLUS unary_expression   { $$ = std::make_shared<PreIncrDecrNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  ;
pre_decrement_expression
  : MINUSMINUS unary_expression   { $$ = std::make_shared<PreIncrDecrNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  ;
post_increment_expression
  : postfix_expression PLUSPLUS   { $$ = std::make_shared<PostIncrDecrNode>(SCANNER_CODEGENCONTEXT, $2, $1); }
  ;
post_decrement_expression
  : postfix_expression MINUSMINUS   { $$ = std::make_shared<PostIncrDecrNode>(SCANNER_CODEGENCONTEXT, $2, $1); }
  ;
cast_expression
  : CAST LT type GT LPAREN unary_expression RPAREN   { $$ = std::make_shared<CastNode>(SCANNER_CODEGENCONTEXT,  $3, $6 ); }
  ;
constant_expression
  : expression
  ;
boolean_expression
  : expression
  ;
expression_list
  : expression   { $$ = std::make_shared<ExpressionList>(); $$->push_back($1); }
  | expression_list COMMA expression   { $1->push_back($3); }
  ;

   /* TODO: simplify these with operator precedence rules? */

multiplicative_expression
  : unary_expression
  | multiplicative_expression STAR unary_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | multiplicative_expression SLASH unary_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | multiplicative_expression PERCENT unary_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
additive_expression
  : multiplicative_expression
  | additive_expression PLUS multiplicative_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | additive_expression MINUS multiplicative_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
shift_expression
  : additive_expression 
  | shift_expression LTLT additive_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | shift_expression GTGT additive_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
relational_expression
  : shift_expression
  | relational_expression LT shift_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | relational_expression GT shift_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | relational_expression LEQ shift_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | relational_expression GEQ shift_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
equality_expression
  : relational_expression
  | equality_expression EQEQ relational_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  | equality_expression NOTEQ relational_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
and_expression
  : equality_expression
  | and_expression AMP equality_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
exclusive_or_expression
  : and_expression
  | exclusive_or_expression CARET and_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
inclusive_or_expression
  : exclusive_or_expression
  | inclusive_or_expression BAR exclusive_or_expression   { $$ = std::make_shared<BinaryExpressionNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
conditional_and_expression
  : inclusive_or_expression
  | conditional_and_expression ANDAND inclusive_or_expression   { $$ = std::make_shared<LogicalAndOrNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
conditional_or_expression
  : conditional_and_expression
  | conditional_or_expression OROR conditional_and_expression   { $$ = std::make_shared<LogicalAndOrNode>(SCANNER_CODEGENCONTEXT, $2, $1, $3); }
  ;
conditional_expression
  : conditional_or_expression
  | conditional_or_expression QUESTION expression COLON expression   { $$ = std::make_shared<TernaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5); }
  ;

/*------------------------------------------------------------------------------ 
                              Class 
------------------------------------------------------------------------------*/

class_declaration
  : modifiers_opt CLASS basic_identifier class_base_opt class_body semicolon_opt   { $$ = std::make_shared<ClassDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $4, $5); }
  ;
class_base_opt
  : /* Nothing */   { $$ = SharedClassBaseDeclaration(); }
  | class_base
  ;
class_base
  : EXTENDS type_name   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<IdentifierList>()); }
  | IMPLEMENTS interface_type_list   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, SharedIdentifier(), $2); }
  | EXTENDS type_name IMPLEMENTS interface_type_list   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  ;
class_body
  : LEFT_BRACE class_member_declarations_opt RIGHT_BRACE   { $$ = $2; }
  ;
class_member_declarations_opt
  : /* Nothing */   { $$ = std::make_shared<ClassMemberDeclarationList>(); }
  | class_member_declarations
  ;
class_member_declarations
  : class_member_declaration   { $$ = std::make_shared<ClassMemberDeclarationList>(); $$->push_back($1); }
  | class_member_declarations class_member_declaration   { $1->push_back($2); }
  ;
class_member_declaration
  : constant_declaration   { $$ = $1; }
  | field_declaration   { $$ = $1; }
  | method_declaration   { $$ = $1; }
  | operator_declaration   { $$ = $1; }
  | constructor_declaration   { $$ = $1; }
  | destructor_declaration   { $$ = $1; }
  ;
constant_declaration
  : modifiers_opt CONST type constant_declarators SEMICOLON   { $$ = std::make_shared<ClassConstDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $4); }
  ;
field_declaration
  : modifiers_opt type variable_declarators SEMICOLON   { $$ = std::make_shared<ClassFieldDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  ;
method_declaration
  : modifiers_opt FN type IDENTIFIER LPAREN parameter_list_opt RPAREN method_body   { $$ = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8); }
  | modifiers_opt FN VOID IDENTIFIER LPAREN parameter_list_opt RPAREN method_body   { $$ = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3, IDENTIFIER_VOID_VAL), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8); }
  ;
method_body
  : block
  | SEMICOLON   { $$ = SharedBlock(); }
  ;

operator_declaration
  : modifiers_opt operator_declarator operator_body   { $$ = std::make_shared<ClassOperatorDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  ;
operator_declarator
  : overloadable_operator_declarator
  ;
operator_body
  : block
  | SEMICOLON   { $$ = SharedBlock(); }
  ;
overloadable_operator_declarator
  : type OPERATOR overloadable_operator LPAREN type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), SharedIdentifier(), SharedIdentifier()); }
  | type OPERATOR overloadable_operator LPAREN type IDENTIFIER COMMA type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), $8, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $9) ); }
  ;
overloadable_operator
  : PLUS
  | MINUS
  | EXCLAMATION
  | TILDE
  | PLUSPLUS
  | MINUSMINUS
  | TRUE   { $$ = 1; }
  | FALSE   { $$ = 0; }
  | STAR
  | SLASH
  | PERCENT
  | AMP
  | BAR
  | CARET
  | LTLT
  | GTGT
  | EQEQ
  | NOTEQ
  | GT
  | LT
  | GEQ
  | LEQ
  ;
constructor_declaration
  : modifiers_opt constructor_declarator constructor_body   { $$ = std::make_shared<ClassConstructorDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  ;
constructor_declarator
  : IDENTIFIER LPAREN parameter_list_opt RPAREN constructor_initializer_opt   { $$ = std::make_shared<ClassConstructorDeclaratorNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3, $5); }
  ;
constructor_initializer_opt
  : /* Nothing */   { $$ = SharedClassConstructorInitializer(); }
  | constructor_initializer
  ;
constructor_initializer
  : COLON BASE LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<ClassConstructorInitializerNode>(SCANNER_CODEGENCONTEXT, $4); }
  ;
constructor_body
  : block
  | SEMICOLON   { $$ = SharedBlock(); }
  ;
destructor_declaration
  : modifiers_opt TILDE IDENTIFIER LPAREN RPAREN block   { $$ = std::make_shared<ClassDestructorDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $6); }
  ;

/*------------------------------------------------------------------------------ 
                              Enum 
------------------------------------------------------------------------------*/

enum_declaration
  : modifiers_opt ENUM IDENTIFIER enum_body semicolon_opt   { $$ = std::make_shared<EnumDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $4 ); }
  ;
enum_body
  : LEFT_BRACE enum_member_declarations_opt RIGHT_BRACE   { $$ = $2; }
  | LEFT_BRACE enum_member_declarations COMMA RIGHT_BRACE   { $$ = $2; }
  ;
enum_member_declarations_opt
  : /* Nothing */   { $$ = std::make_shared<EnumMemberDeclarationList>(); }
  | enum_member_declarations
  ;
enum_member_declarations
  : enum_member_declaration   { $$ = std::make_shared<EnumMemberDeclarationList>(); $$->push_back($1); }
  | enum_member_declarations COMMA enum_member_declaration   { $1->push_back($3); }
  ;
enum_member_declaration
  : IDENTIFIER   { $$ = std::make_shared<EnumMemberDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression() ); }
  | IDENTIFIER EQ constant_expression   { $$ = std::make_shared<EnumMemberDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3 ); }
  ;

/*------------------------------------------------------------------------------ 
                              Interface 
------------------------------------------------------------------------------*/

interface_declaration
  : modifiers_opt INTERFACE basic_identifier interface_base_opt interface_body semicolon_opt   { $$ = std::make_shared<InterfaceDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $4, $5 ); }
  ;
interface_base_opt
  : /* Nothing */   { $$ = std::make_shared<IdentifierList>(); }
  | interface_base
  ;
interface_base
  : COLON interface_type_list   { $$ = $2; }
  ;
interface_type_list
  : type_name   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | interface_type_list COMMA type_name   { $1->push_back($3); }
  ;
interface_body
  : LEFT_BRACE interface_member_declarations_opt RIGHT_BRACE   { $$ = $2; }
  ;
interface_member_declarations_opt
  : /* Nothing */   { $$ = std::make_shared<FunctionDeclarationList>(); }
  | interface_member_declarations
  ;
interface_member_declarations
  : interface_member_declaration   { $$ = std::make_shared<FunctionDeclarationList>(); $$->push_back($1); }
  | interface_member_declarations interface_member_declaration   { $1->push_back($2); }
  ;
interface_member_declaration
  : interface_method_declaration
  ;
interface_method_declaration
  : FN type IDENTIFIER LPAREN parameter_list_opt RPAREN SEMICOLON   { $$ = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  SharedModifier(), $2, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, SharedBlock() ); }
  | FN VOID IDENTIFIER LPAREN parameter_list_opt RPAREN SEMICOLON   { $$ = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  SharedModifier(), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2, IDENTIFIER_VOID_VAL), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, SharedBlock() ); }
  ;

%%

SharedExpression createIntegerLiteralNode(CodeGenContext& context, int base, const std::string& str)
{
  bool signedVal = true;
  int bits = 32;

  int index = str.length()-1;
  if(str[index] == '8')
  {
    bits = 8;
    if(str[index - 2] == 'u')
    {
      signedVal = false;
    }
  }
  else
  {
    char firstDigit = str[index-2];
    if(firstDigit == '1')
    {
      bits = 16;
    }
    else if(firstDigit == '6')
    {
      bits = 64;
    }

    if(str[index - 3] == 'u')
    {
      signedVal = false;
    }
  }

  int suffixSize = (bits == 8) ? 2 : 3;
  suffixSize = (!signedVal) ? suffixSize + 1 : suffixSize;
  
  long long val = 0;
  SharedExpression rtn;
  if(base == 0)
  {
    // based type
    std::string::size_type underscoreIndex = str.find('_');
    std::string::size_type subStrLength = str.length() - underscoreIndex - suffixSize;
    int base = strtol(str.substr(underscoreIndex + 1, subStrLength).c_str(), NULL, 10);
    val = strtoll( str.substr(0, underscoreIndex - 1).c_str(), NULL, base);
  }
  else
  {
    val = strtoll( str.substr(0, str.length() - suffixSize).c_str(), NULL, base);
  }

  if(signedVal)
  {
    switch(bits)
    {
      case 8:
        rtn = std::make_shared<Int8Node>(context, (int8_t)val);
      break;
      case 16:
        rtn = std::make_shared<Int16Node>(context, (int16_t)val);
      break;
      case 64:
        rtn = std::make_shared<Int64Node>(context, (int64_t)val);
      break;
      case 32:
      default:
        rtn = std::make_shared<Int32Node>(context, (int32_t)val);
      break;
    }
  }
  else
  {
    switch(bits)
    {
      case 8:
        rtn = std::make_shared<UInt8Node>(context, (uint8_t)val);
      break;
      case 16:
        rtn = std::make_shared<UInt16Node>(context, (uint16_t)val);
      break;
      case 64:
        rtn = std::make_shared<UInt64Node>(context, (uint64_t)val);
      break;
      case 32:
      default:
        rtn = std::make_shared<UInt32Node>(context, (uint32_t)val);
      break;
    }
  }

  return rtn;
}

int yyerror(yyscan_t scanner, const char *msg) 
{
    LexerInstanceData* data = yyget_extra(scanner);
    return data->codeGenContext->handleError(data->codeGenContext->line, data->codeGenContext->col, "Parse", msg);
}

