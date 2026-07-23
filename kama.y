%{
#define YYERROR_VERBOSE
#define YYDEBUG 1
#include <memory>
#include <string>
#include <cstdlib>
#include "kama.parser.hpp"
#include "kama.ast.h"
#include "kama.context.h"


struct LexerInstanceData*  yyget_extra ( yyscan_t scanner );
extern int yylex(YYSTYPE * yylval_param, yyscan_t scanner);

int yyerror(yyscan_t scanner, const char *msg);
SharedExpression createIntegerLiteralNode(CodeGenContext& context, int base, const std::string& str);
SharedStatement makeTypeDeclaration(CodeGenContext& context, SharedAttributeList attributes,
    SharedModifierList modifiers, SharedString typeKind, SharedIdentifier head, SharedStringList forKinds,
    SharedClassBaseDeclaration base, SharedClassMemberDeclarationList body);

#define SCANNER_CODEGENCONTEXT *(yyget_extra(scanner)->codeGenContext)

%}

%code requires {

#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#include "kama.ast.h"
#include "kama.context.h"

#ifndef KAMA_LEXER_INSTANCE_DATA
#define KAMA_LEXER_INSTANCE_DATA

//#define KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE 1
//#define KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE 0

struct LexerInstanceData {
   int next_line_number;
   int next_column_number;
   SharedString workString;

   SharedCodeGenContext codeGenContext;
   SharedCompilationUnit compilationUnit;

   /* Nesting depth of open generic `<…>` (type contexts only). The grammar bumps it on each
      generic `<` and drops it on the matching `>`; while >0 the lexer splits a `>>` into two `>`
      (so `DynamicArray<Shared<Circle>>` needs no space). 0 in expression context, so `a >> b` stays a shift. */
   int genericDepth = 0;

   /* Set true when a `${` interpolation hole is seen inside the current string literal, so its closing
      `"` emits a tail `ISTR_CHUNK` (interpolated) rather than a plain `STRING_LITERAL`. Strings don't
      nest (holes carry only identifier/member/index tokens), so a single flag suffices. */
   bool strInterp = false;
};

struct kamayystype {
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
  SharedMatchArm matcharm;
  SharedArgument argument;
  SharedAttribute attribute;
  SharedEnumMemberDeclaration enummemberdecl;
  SharedFunctionDeclaration functiondecl;
  SharedClassBaseDeclaration classbasedecl;
  SharedClassMemberDeclaration classmemberdecl;
  SharedClassOperatorDeclarator operatordeclarator;
  SharedClassConstructorDeclarator constructordeclarator;
  SharedClassConstructorInitializer constructorinitializer;

  SharedStringList strings;
  SharedUsingDeclarationList usingdeclarationlist;
  SharedImportDeclaration importdeclaration;
  SharedImportDeclarationList importdeclarationlist;
  SharedStatementList statementlist;
  SharedIdentifierList identifierlist;
  SharedModifierList modifierlist;
  SharedParameterList parameterlist;
  SharedVariableDeclaratorList variabledeclaratorlist;
  SharedConstVariableDeclaratorList constvariabledeclaratorlist;
  SharedMatchArmList matcharmlist;
  SharedArgumentList argumentlist;
  SharedAttributeList attributelist;
  SharedExpressionList expressionlist;
  SharedEnumMemberDeclarationList enummemberdecllist;
  SharedFunctionDeclarationList functiondecllist;
  SharedClassMemberDeclarationList classmemberdecllist;
  
  SharedString string;
  SharedInterpolatedString interpstring;
  int token;
};
#define YYSTYPE kamayystype

#endif

}

/* Options */
%expect 1
%defines
%define api.pure full
/* Better syntax errors: "syntax error, unexpected X, expecting Y" (using the token string-aliases below,
   e.g. "when" / "[" / ":="). Zero runtime cost (the parser isn't in the runtime); the message is built
   only on an error. LAC makes the expected-set exact — its per-token parse cost is negligible next to
   codegen + the C compiler invocation, so it's on. */
%define parse.error detailed
%define parse.lac full
%lex-param   { yyscan_t scanner }
%parse-param { yyscan_t scanner }



/* Tokens */
%token <string> IDENTIFIER 
%token <string> FLOAT_LITERAL_NO_SUFFIX FLOAT_LITERAL_32 FLOAT_LITERAL_64 CHARACTER_LITERAL STRING_LITERAL
%token <string> ISTR_CHUNK   /* a literal chunk of an interpolated string: head, mid, or tail (between holes) */
%token <string> ISTR_SPEC    /* the raw format-spec text of a hole `${expr:SPEC}` (e.g. `.2`, `0x`); parsed at codegen */
%token <string> STRING_TAG   /* a tag name immediately preceding a string literal: `sql"…"`, `html"…"` (Campaign 2) */
%token <string> DEC_LITERAL_NO_SUFFIX HEX_LITERAL_NO_SUFFIX OCT_LITERAL_NO_SUFFIX BASED_LITERAL_NO_SUFFIX 
%token <string> DEC_LITERAL HEX_LITERAL OCT_LITERAL BASED_LITERAL

/* KEYWORDS */ 
%token <string> ABSTRACT BASE BOOL BREAK
%token <string> CASE CAST CONST CONTINUE CTOR DEFAULT
%token <string> AS CHAR DO DOUBLE ELSE ENUM EXPORT EXPOSE EXTERN EXTENDS IMPLEMENTS IMPORT
%token <string> FALSE FINAL FLOAT32 FLOAT64
%token <string> FN FNPTR FOR FOREACH IF IN
%token <string> INT INT8 INT16 INT32 INT64 SPAWN SCOPE
%token <string> MATCH
%token <string> NAMESPACE
%token <string> NEW NULL_LITERAL OPERATOR OUT SIZEOF ALIGNOF
%token <string> OVERRIDE PRIVATE PROTECTED PUBLIC FRIEND
%token <string> REF RETURN STATIC STRING
%token <string> THIS TRUE TYPE
%token <string> UINT8 UINT16 UINT32 UINT64
%token <string> UNSAFE VIRTUAL VOID
%token <string> VOLATILE WHILE

/* PUNCTUATION AND SINGLE CHARACTER OPERATORS */
%token <token> COMMA ","
%token <token> AT "@"
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
%token <token> ELLIPSIS "..."
%token <string> GIVE COPY
%token <token> WHEN "when"
%token <token> SLASH "/"
%token <token> COLONCOLON "::"
%token <token> WALRUS ":="
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
%type <token> assignment_operator overloadable_operator handoff_default
%type <strings> qualifier
%type <expression> expression expression_opt literal boolean_literal variable_initializer
%type <expression> parenthesized_expression constant_expression boolean_expression for_condition_opt
%type <expression> for_condition unary_expression variable_reference primary_expression_no_parenthesis array_literal
%type <expression> postfix_expression cast_expression sizeof_expression member_access element_access this_access
%type <expression> as_downcast_expression interp_expr interp_hole
%type <interpstring> interp_body
%type <expressionlist> interp_index
%type <expression> base_access primary_expression multiplicative_expression additive_expression
%type <expression> shift_expression relational_expression equality_expression and_expression
%type <expression> exclusive_or_expression inclusive_or_expression conditional_and_expression
%type <expression> conditional_expression conditional_or_expression
%type <expressionlist> expression_list
%type <statement> compilation_unit code_declaration type_declaration function_declaration statement
%type <statement> declaration_statement local_variable_declaration embedded_statement local_constant_declaration
%type <statement> empty_statement selection_statement iteration_statement jump_statement if_statement
%type <statement> while_statement do_statement for_statement foreach_statement
%type <statement> break_statement continue_statement return_statement enum_declaration
%type <statement> marked_type_declaration unsafe_statement spawn_statement scope_statement arm_value_statement retroactive_impl_declaration
%type <statementlist> code_opt code_declarations statement_list statement_list_opt
%type <statementlist> for_initializer_opt for_initializer for_iterator_opt for_iterator statement_expression_list
%type <namespacedeclaration> namespace_opt
%type <usingdeclaration> import_symbol
%type <usingdeclarationlist> import_symbols
%type <importdeclaration> import_directive
%type <importdeclarationlist> import_directives_opt import_directives
%type <strings> import_path export_manifest_opt export_name_list for_kinds_opt kind_name_list
%type <identifier> basic_identifier qualified_identifier type_name type non_array_type simple_type function_return_type type_or_value_arg generic_turbofish_name
%type <identifier> primitive_type numeric_type integral_type floating_point_type class_type qualified_identifier_no_generic
%type <identifier> type_param type_param_default_opt type_decl_head enum_underlying_opt implements_entry method_when_opt when_clause when_cond_list
%type <identifierlist> friend_member_list interface_type_list type_arg_list type_param_list bound_list type_params_opt
%type <modifier> modifier function_modifier_opt parameter_modifier_opt
%type <modifierlist> modifiers modifiers_opt
%type <parameter> parameter
%type <parameterlist> parameter_list parameter_list_opt
%type <block> block method_body operator_body constructor_body
%type <variabledeclarator> variable_declarator
%type <variabledeclaratorlist> variable_declarators
%type <constvariabledeclarator> constant_declarator
%type <constvariabledeclaratorlist> constant_declarators
%type <expressionstatement> expression_statement statement_expression assignment invocation_expression match_expression
%type <matcharm> match_arm match_pattern
%type <matcharmlist> match_arms
%type <strings> match_bindings
%type <expressionstatement> object_creation_expression new_expression post_increment_expression post_decrement_expression
%type <expressionstatement> pre_increment_expression pre_decrement_expression
   /* %type <unaryexpression> unary_expression */
   /*%type <binaryexpression>*/
%type <argument> argument attr_arg
%type <argumentlist> argument_list_opt argument_list attr_arg_list
%type <attribute> attribute
%type <attributelist> attribute_list
%type <enummemberdecl> enum_member_declaration
%type <enummemberdecllist> enum_body enum_member_declarations_opt enum_member_declarations
%type <classbasedecl> class_base_opt class_base
%type <classmemberdecl> class_member_declaration constant_declaration field_declaration method_declaration friend_declaration
%type <classmemberdecl> operator_declaration constructor_declaration destructor_declaration
%type <classmemberdecllist> class_body class_member_declarations_opt class_member_declarations
%type <operatordeclarator> operator_declarator overloadable_operator_declarator
%type <constructordeclarator> constructor_declarator
%type <constructorinitializer> constructor_initializer_opt constructor_initializer
%type <string> const_opt method_name

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
  : namespace_opt import_directives_opt export_manifest_opt code_opt  { yyget_extra(scanner)->compilationUnit = CreateCompilationUnit( SCANNER_CODEGENCONTEXT, yyget_extra(scanner)->codeGenContext->getModuleName(), $1, $2, $3, $4); }
  ;

/* The module's PUBLIC SURFACE, declared once at the top: `export { A, B, C };`. A name here must be a
   top-level declaration IN THIS FILE; everything unlisted is module-private. Declarations themselves carry
   no visibility modifier (so `type`/`fn` syntax stays uniform). Mirrors `import a::b::{A, B}`. */
export_manifest_opt
  : /* Nothing */   { $$ = std::make_shared<StringList>(); }
  | EXPORT LEFT_BRACE export_name_list RIGHT_BRACE SEMICOLON   { $$ = $3; }
  ;
export_name_list
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | export_name_list COMMA IDENTIFIER   { $1->push_back($3); $$ = $1; }
  ;

namespace_opt
  : /* Nothing */  { $$ = SharedNamespaceDeclaration(); }
  | NAMESPACE qualified_identifier_no_generic SEMICOLON  { $$ = std::make_shared<NamespaceDeclarationNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;

/* Module imports (`::`-path resolved to a source file by the driver). Four forms:
     import a::b;                 -- load; qualified-only access (a::b::X)
     import a::b as m;            -- load + module alias (m::X)
     import a::b::{X, Y as Z};    -- load + per-symbol, unqualified (Y bound as Z)                       */
import_directives_opt
  : /* Nothing */   { $$ = std::make_shared<ImportDeclarationList>(); }
  | import_directives
  ;
import_directives
  : import_directive   { $$ = std::make_shared<ImportDeclarationList>(); $$->push_back($1); }
  | import_directives import_directive   { $1->push_back($2); $$ = $1; }
  ;
import_directive
  : IMPORT import_path SEMICOLON
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<UsingDeclarationList>(), SharedString()); }
  | IMPORT import_path AS IDENTIFIER SEMICOLON
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<UsingDeclarationList>(), $4); }
  | IMPORT import_path COLONCOLON LEFT_BRACE import_symbols RIGHT_BRACE SEMICOLON
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $5, SharedString()); }
  ;
import_path
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | import_path COLONCOLON IDENTIFIER   { $1->push_back($3); $$ = $1; }
  ;
import_symbols
  : import_symbol   { $$ = std::make_shared<UsingDeclarationList>(); $$->push_back($1); }
  | import_symbols COMMA import_symbol   { $1->push_back($3); $$ = $1; }
  ;
import_symbol
  : IDENTIFIER   { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | IDENTIFIER AS IDENTIFIER   { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); }
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
  | retroactive_impl_declaration
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

  /* strtof/strtod (not std::stof/stod): a float literal at/above the type max (e.g. FLT_MAX) makes the
     std:: versions THROW std::out_of_range, which was uncaught and terminated the compiler. strtof/strtod
     saturate to ±inf on overflow (C-idiomatic) instead — no crash on a boundary literal. */
  | FLOAT_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Float64Node>(SCANNER_CODEGENCONTEXT, strtod ($1->c_str(), nullptr)); }
  | FLOAT_LITERAL_32   { $$ = std::make_shared<Float32Node>(SCANNER_CODEGENCONTEXT, strtof ($1->substr(0,$1->length() - 3).c_str(), nullptr)); }
  | FLOAT_LITERAL_64   { $$ = std::make_shared<Float64Node>(SCANNER_CODEGENCONTEXT, strtod ($1->substr(0,$1->length() - 3).c_str(), nullptr)); }
  | CHARACTER_LITERAL   { $$ = std::make_shared<CharNode>(SCANNER_CODEGENCONTEXT, (uint32_t)strtoul($1->c_str(), NULL, 10)); }
  | STRING_LITERAL   { $$ = std::make_shared<StringNode>(SCANNER_CODEGENCONTEXT, $1); }
  | interp_expr
  | STRING_TAG STRING_LITERAL   { auto n = std::make_shared<InterpolatedStringNode>(SCANNER_CODEGENCONTEXT); n->parts.push_back($2); n->tag = $1; $$ = n; }   /* a tagged plain string: one part, no holes */
  | STRING_TAG interp_expr   { std::static_pointer_cast<InterpolatedStringNode>($2)->tag = $1; $$ = $2; }   /* a tagged interpolation */
  | NULL_LITERAL   { $$ = std::make_shared<NullNode>(SCANNER_CODEGENCONTEXT); }
  ;

/* String interpolation `"a ${x} b ${y} c"`. The lexer emits: a literal chunk (ISTR_CHUNK) before each hole
   and a final tail chunk, with the hole's identifier/member/index tokens in between. `interp_body` gathers
   CHUNK hole (CHUNK hole)* (parts.size()==holes.size()); `interp_expr` appends the trailing tail chunk so
   parts.size()==holes.size()+1. Holes are restricted to an identifier with `.field`/`[index]` accessors. */
interp_expr
  : interp_body ISTR_CHUNK   { $1->parts.push_back($2); $$ = $1; }
  ;
interp_body
  : ISTR_CHUNK interp_hole
      { $$ = std::make_shared<InterpolatedStringNode>(SCANNER_CODEGENCONTEXT); $$->parts.push_back($1); $$->holes.push_back($2); $$->specs.push_back(nullptr); }
  | ISTR_CHUNK interp_hole ISTR_SPEC
      { $$ = std::make_shared<InterpolatedStringNode>(SCANNER_CODEGENCONTEXT); $$->parts.push_back($1); $$->holes.push_back($2); $$->specs.push_back($3); }
  | interp_body ISTR_CHUNK interp_hole
      { $1->parts.push_back($2); $1->holes.push_back($3); $1->specs.push_back(nullptr); $$ = $1; }
  | interp_body ISTR_CHUNK interp_hole ISTR_SPEC
      { $1->parts.push_back($2); $1->holes.push_back($3); $1->specs.push_back($4); $$ = $1; }
  ;
interp_hole
  : IDENTIFIER   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | interp_hole DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); }
  | interp_hole LEFT_BRACKET interp_index RIGHT_BRACKET   { $$ = std::make_shared<ElementAccessNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  ;
interp_index
  : IDENTIFIER              { $$ = std::make_shared<ExpressionList>(); $$->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | DEC_LITERAL_NO_SUFFIX   { $$ = std::make_shared<ExpressionList>(); $$->push_back(std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, strtol($1->c_str(), NULL, 10))); }
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
  : IDENTIFIER COLONCOLON { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | qualifier IDENTIFIER COLONCOLON { $1->push_back($2); }
  ;
basic_identifier
  : IDENTIFIER   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | IDENTIFIER LT { yyget_extra(scanner)->genericDepth++; } type_arg_list GT   {
        /* The mid-rule bumped genericDepth on the opening `<` so the lexer splits a nested `>>`
           close (see kama.l); drop it back now that this `>` closed the list. */
        yyget_extra(scanner)->genericDepth--;
        /* `Name<A, B, …>` — the type args are a LIST. `genericArg` mirrors [0] so every
           single-arg consumer (Ptr/collections/guards) is untouched; multi-arg sites read genericArgs. */
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<StringList>(), (*$4)[0]);
        id->genericArgs = $4;
        $$ = id;
    }
  ;
/* Comma-separated type arguments: `int32`, `int32, string`, `int32, DynamicArray<int>` — mirrors
   interface_type_list. Always length ≥ 1 (the `<…>` syntax requires at least one). */
type_arg_list
  : type_or_value_arg   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | type_arg_list COMMA type_or_value_arg   { $1->push_back($3); $$ = $1; }
  ;
/* A generic argument is a type, or an integer VALUE for a const param (`InlineArray<float, 4>`). A value
   arg is wrapped in an IdentifierNode carrying `constArgValue` (value name is null). */
type_or_value_arg
  : type
  | IDENTIFIER COLON type   { $3->argName = $1; $$ = $3; }   /* NAMED override: `A: Arena` skips an earlier default */
  | literal   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, SharedString()); id->constArgValue = $1; $$ = id; }
  ;

qualified_identifier_no_generic
  : IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | qualifier IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2, $1); }
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
  | CHAR   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, IDENTIFIER_CHAR_VAL); }
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
  : enum_declaration
  | marked_type_declaration
  ;

/* `type <kind> Name { … }` — the ownership-model declaration. The kind word
   (`value`/`resource`/`contract`) is an ordinary IDENTIFIER checked by the emitter, so it
   is never reserved. `type` marks every type declaration (greppable, like `fn`). All three
   kinds share the class body; the emitter routes `contract` to the fat-pointer vtable path. */
marked_type_declaration
  : TYPE modifiers_opt IDENTIFIER type_decl_head for_kinds_opt class_base_opt class_body semicolon_opt
    { $$ = makeTypeDeclaration(SCANNER_CODEGENCONTEXT, SharedAttributeList(), $2, $3, $4, $5, $6, $7); }
  | attribute_list TYPE modifiers_opt IDENTIFIER type_decl_head for_kinds_opt class_base_opt class_body semicolon_opt
    { $$ = makeTypeDeclaration(SCANNER_CODEGENCONTEXT, $1, $3, $4, $5, $6, $7, $8); }   /* `@generate(...) type …` */
  ;

/* `implements C for T { …methods… }` — RETROACTIVE contract conformance: an external top-level block that
   gives an existing type T (a primitive/stdlib/foreign type) the methods of contract C. Distinct from the
   `implements` CLAUSE inside a type declaration (which lists contracts a type opts into on its own line).
   Unambiguous at top level — nothing else here begins with IMPLEMENTS. Coherence (orphan rule) is enforced
   by the emitter. */
retroactive_impl_declaration
  : IMPLEMENTS type_name FOR type class_body semicolon_opt   /* target is `type` so a primitive (string/int32) is accepted */
    { $$ = std::make_shared<RetroactiveImplNode>(SCANNER_CODEGENCONTEXT, $2, $4, $5); }
  ;

/* Kind-gate on a `type contract`: `for value | resource | both` (also `value, resource`). MANDATORY on a
   contract (enforced by the emitter), forbidden on value/resource. Kind words are contextual identifiers. */
for_kinds_opt
  : /* Nothing */   { $$ = std::make_shared<StringList>(); }
  | FOR kind_name_list   { $$ = $2; }
  ;
kind_name_list
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | kind_name_list COMMA IDENTIFIER   { $1->push_back($3); $$ = $1; }
  ;
/* The NAME + type-parameter list in a type DECLARATION — decoupled from the type-USE production
   (`basic_identifier`, whose `type_arg_list` can't carry bounds). `Foo` or `Foo<K: I + J, V>`. */
type_decl_head
  : IDENTIFIER   { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | IDENTIFIER LT { yyget_extra(scanner)->genericDepth++; } type_param_list GT   {
        yyget_extra(scanner)->genericDepth--;
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1);
        id->genericArgs = $4;   /* each element is a type_param node carrying its name + bounds */
        $$ = id;
    }
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
  | FINAL   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | STATIC   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | DEFAULT   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }   /* `default ctor` — the canonical zero-arg ctor */
  | VIRTUAL   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  | VOLATILE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }
  ;

friend_declaration
  : FRIEND qualified_identifier LEFT_BRACKET friend_member_list RIGHT_BRACKET SEMICOLON
      { $$ = std::make_shared<FriendGrantNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  | FRIEND qualified_identifier LEFT_BRACKET ELLIPSIS RIGHT_BRACKET SEMICOLON
      { $$ = std::make_shared<FriendGrantNode>(SCANNER_CODEGENCONTEXT, $2, nullptr); }   // [...] => all privates
  ;
friend_member_list
  : IDENTIFIER   { $$ = std::make_shared<IdentifierList>(); $$->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | friend_member_list COMMA IDENTIFIER   { $1->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); $$ = $1; }
  ;

function_modifier_opt
  : /* Nothing */   { $$ = SharedModifier(); }
  | EXPOSE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }   /* kama→host C-ABI boundary: bare exported symbol */
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
  | function_modifier_opt FN function_return_type IDENTIFIER type_params_opt LPAREN parameter_list_opt RPAREN block   {
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $7, $9 );
      /* Split `<T, K: I + J>` into parallel typeParams (names) + typeBounds (contract lists). */
      if ($5 && !$5->empty()) {
          fn->typeParams = std::make_shared<StringList>();
          fn->typeBounds = std::make_shared<BoundsList>();
          fn->constParams = std::make_shared<StringList>();
          for (auto& p : *$5) if (p && p->value) {
              fn->typeParams->push_back(p->value);
              fn->typeBounds->push_back(p->bounds ? p->bounds : std::make_shared<IdentifierList>());
              if (p->isConstParam) fn->constParams->push_back(p->value);
          }
      }
      $$ = fn;
  }
  | function_modifier_opt FN REF type IDENTIFIER type_params_opt LPAREN parameter_list_opt RPAREN block   {
      /* `fn ref T f(ref …)` — a place-returning FREE function (mirrors the `fn ref T` method form).
         The returned place must borrow a `ref`/`out` param (a free fn has no `this`); the escape
         check at the ReturnNode place path enforces it. */
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $8, $10 );
      fn->isRef = true;
      if ($6 && !$6->empty()) {
          fn->typeParams = std::make_shared<StringList>();
          fn->typeBounds = std::make_shared<BoundsList>();
          fn->constParams = std::make_shared<StringList>();
          for (auto& p : *$6) if (p && p->value) {
              fn->typeParams->push_back(p->value);
              fn->typeBounds->push_back(p->bounds ? p->bounds : std::make_shared<IdentifierList>());
              if (p->isConstParam) fn->constParams->push_back(p->value);
          }
      }
      $$ = fn;
  }
  | FNPTR function_return_type IDENTIFIER LPAREN parameter_list_opt RPAREN SEMICOLON   {
      /* `fnptr ret Name(params);` — an explicit function-pointer TYPE.
         A null body marks it as a signature type (collectSignatures -> _sigs). */
      $$ = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  SharedModifier(), $2, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, SharedBlock() );
  }
  ;
/* Generic type parameters on a fn declaration: `fn max<T, U>(...)` / `fn sort<T: Comparable>(...)`
   with contract bounds. Yields a list of type-param IdentifierNodes (each carrying its `bounds`);
   the fn/type decl action splits it into names (typeParams) + contract lists (typeBounds). */
type_params_opt
  : /* Nothing */   { $$ = SharedIdentifierList(); }
  | LT { yyget_extra(scanner)->genericDepth++; } type_param_list GT   { yyget_extra(scanner)->genericDepth--; $$ = $3; }
  ;
type_param_list
  : type_param   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | type_param_list COMMA type_param   { $1->push_back($3); $$ = $1; }
  ;
type_param
  : IDENTIFIER type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); id->defaultArg = $2; $$ = id; }
  | IDENTIFIER COLON bound_list type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); id->bounds = $3; id->defaultArg = $4; $$ = id; }
  | CONST IDENTIFIER COLON integral_type type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2); id->isConstParam = true; id->defaultArg = $5; $$ = id; }   /* `const N: int` — a compile-time value param */
  ;
/* Optional `= DefaultType` (or `= literal` for a const param) on a trailing type parameter. */
type_param_default_opt
  : /* Nothing */   { $$ = SharedIdentifier(); }
  | EQ type_or_value_arg   { $$ = $2; }
  ;
/* Contract bounds on a type parameter: `IHashable` or `IHashable + IComparable` (`+` = AND).
   Each bound is a `type_name`, so a generic contract bound (`IFoo<int>`) parses + gets genericDepth. */
bound_list
  : type_name   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | bound_list PLUS type_name   { $1->push_back($3); $$ = $1; }
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
  : const_opt parameter_modifier_opt type IDENTIFIER   { auto p = std::make_shared<FunctionParameterNode>(SCANNER_CODEGENCONTEXT, $2, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4)); p->isConst = ($1 != nullptr); $$ = p; }
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
    /* `Isolate h = spawn worker(p: give x);` — the handle form: spawn now, returning an RAII `Isolate`
       whose drop = join. Only valid as a declaration initializer (after `=`), where `spawn` is
       unambiguous (a keyword, never an expression start), so this adds no conflict. */
  | SPAWN invocation_expression   { $$ = std::make_shared<IsolateNode>(SCANNER_CODEGENCONTEXT, $2); }
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
  | IDENTIFIER                          { $$ = std::make_shared<ConstVariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression()); }
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
  | arm_value_statement
  | unsafe_statement
  | spawn_statement
  | scope_statement
  ;
arm_value_statement
    /* `:= expr;` — the value a match arm's block produces (assigned out to whatever the match is bound
       to). A distinct statement (not a jump); the emitter requires it be the arm block's last statement. */
  : WALRUS expression SEMICOLON   { $$ = std::make_shared<ArmValueNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
unsafe_statement
  : UNSAFE block   { $$ = std::make_shared<UnsafeNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
  /* `spawn worker(p: give x);` — spawn a top-level fn on a fresh OS thread with a MOVED-in arg
     bundle, then join (fused). Reuses `invocation_expression` for named-arg / `give` parsing; the
     emitter validates the callee is a bare top-level fn (no receiver) → shared-nothing. */
spawn_statement
  : SPAWN invocation_expression SEMICOLON   { $$ = std::make_shared<IsolateNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
  /* `scope { ... }` — a structured-concurrency block (M4): bare `spawn`s inside it are deferred-join
     children joined at the closing brace, before any local dtor. Block-bodied keyword, exactly like
     `unsafe`. */
scope_statement
  : SCOPE block   { $$ = std::make_shared<ScopeNode>(SCANNER_CODEGENCONTEXT, $2); }
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
  | match_expression   /* `match (…) { … };` as a statement (trailing `;`, value discarded) */
  ;
selection_statement
  : if_statement
  ;
if_statement
  : IF LPAREN boolean_expression RPAREN embedded_statement   { $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, SharedStatement()); }
  | IF LPAREN boolean_expression RPAREN embedded_statement ELSE embedded_statement   { $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, $7); }
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
/* `match (subject) { case Variant(bindings): expr; … case _: expr; }` — a single
   value-producing construct. Wired into both statement_expression (value discarded) and
   primary_expression_no_parenthesis (lifted to a temp). Each arm's body is one expression. */
match_expression
  : MATCH LPAREN expression RPAREN LEFT_BRACE match_arms RIGHT_BRACE
    { $$ = std::make_shared<MatchNode>(SCANNER_CODEGENCONTEXT, $3, $6); }
  ;
match_arms
  : match_arm   { $$ = std::make_shared<MatchArmList>(); $$->push_back($1); }
  | match_arms match_arm   { $1->push_back($2); $$ = $1; }
  ;
match_arm
  : CASE match_pattern COLON expression SEMICOLON   { $2->body = $4; $$ = $2; }
  | CASE match_pattern COLON block   { $2->block = $4; $$ = $2; }   /* block arm (multi-statement) */
  ;
match_pattern
  : IDENTIFIER
    { auto a = std::make_shared<MatchArmNode>(SCANNER_CODEGENCONTEXT); a->variantName = $1; $$ = a; }
  | IDENTIFIER LPAREN match_bindings RPAREN
    { auto a = std::make_shared<MatchArmNode>(SCANNER_CODEGENCONTEXT); a->variantName = $1; a->bindings = $3; $$ = a; }
  ;
match_bindings
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); }
  | match_bindings COMMA IDENTIFIER   { $1->push_back($3); $$ = $1; }
  ;
foreach_statement
  : FOREACH LPAREN type IDENTIFIER IN expression RPAREN embedded_statement   { $$ = std::make_shared<ForEachNode>(SCANNER_CODEGENCONTEXT,  $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8); }
  | FOREACH LPAREN REF type IDENTIFIER IN expression RPAREN embedded_statement   { auto n = std::make_shared<ForEachNode>(SCANNER_CODEGENCONTEXT,  $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $9); n->isRef = true; $$ = n; }   /* `foreach (ref T e in …)` — mutate elements in place */
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
  | GIVE variable_reference   { $$ = std::make_shared<HandoffNode>(SCANNER_CODEGENCONTEXT, true,  $2); }   // move
  | COPY variable_reference   { $$ = std::make_shared<HandoffNode>(SCANNER_CODEGENCONTEXT, false, $2); }   // duplicate
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
  | match_expression   { $$ = $1; }   /* value-producing `match` in expression position */
  | array_literal
  | as_downcast_expression   { $$ = $1; }
  ;

/* Model C: `expr.as<T>()` — runtime downcast of a boxed poly-dispatch error to a concrete enum `T`, yielding
   `Optional<T>`. Mirrors cast_expression's genericDepth mid-rules so the `<…>` parses without spaces and a
   trailing `>>` stays a shift. `.as` (DOT AS) is distinct from `.member` (DOT IDENTIFIER) — no conflict. */
as_downcast_expression
  : primary_expression DOT AS LT { yyget_extra(scanner)->genericDepth++; } type GT { yyget_extra(scanner)->genericDepth--; } LPAREN RPAREN
    { $$ = std::make_shared<AsDowncastNode>(SCANNER_CODEGENCONTEXT, $1, $6); }
  | qualified_identifier_no_generic DOT AS LT { yyget_extra(scanner)->genericDepth++; } type GT { yyget_extra(scanner)->genericDepth--; } LPAREN RPAREN
    { $$ = std::make_shared<AsDowncastNode>(SCANNER_CODEGENCONTEXT, std::static_pointer_cast<ExpressionNode>($1), $6); }
  ;

/* fixed-array value literal initializing a `InlineArray<T,N>`: `[a, b, c]` (elements) or `[v; N]` (fill).
   A leading `[` is a primary here; a `[` after a primary is postfix indexing — unambiguous. */
array_literal
  : LEFT_BRACKET expression_list RIGHT_BRACKET
    { $$ = std::make_shared<ArrayLiteralNode>(SCANNER_CODEGENCONTEXT, $2); }
  | LEFT_BRACKET expression SEMICOLON constant_expression RIGHT_BRACKET
    { $$ = std::make_shared<ArrayLiteralNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  ;
parenthesized_expression
  : LPAREN expression RPAREN   { $$ = $2; }
  ;
member_access
  : primary_expression DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); }
  | qualified_identifier_no_generic DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), std::static_pointer_cast<ExpressionNode>($1)); }
  | class_type DOT IDENTIFIER   { $$ = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); }
  ;
invocation_expression
  : primary_expression_no_parenthesis LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | qualified_identifier_no_generic LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
    /* Turbofish: explicit type arguments on a generic function call — `make::<int32>()`. The `::`
       before `<` is unambiguous (a qualifier is always followed by an identifier, never `<`), so no
       comparison-operator clash. The `IDENTIFIER::<…>` prefix is factored into `generic_turbofish_name`
       (shared with the on-type ctor form below) so the genericDepth mid-rule actions aren't duplicated —
       duplicated mid-rule actions become distinct empty nonterminals and reduce/reduce-conflict. */
  | generic_turbofish_name LPAREN argument_list_opt RPAREN {
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, $1, $3);
    }
    /* Receiver turbofish: `r.deserialize::<T>()` — explicit type args on a member call (only `deserialize`
       is generic today). The `::` before `<` disambiguates from `<` as less-than (unlike `.as<T>()`, which
       rides the `as` keyword — `deserialize` is a plain IDENTIFIER). The type args ride the method
       IdentifierNode's `genericArgs`, mirroring the free-fn turbofish above; two receiver forms mirror the
       `.as<T>()`/member_access pair (a bare ident reduces via qualified_identifier_no_generic). */
  | primary_expression DOT IDENTIFIER COLONCOLON LT { yyget_extra(scanner)->genericDepth++; } type_arg_list GT { yyget_extra(scanner)->genericDepth--; } LPAREN argument_list_opt RPAREN {
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3, std::make_shared<StringList>(), (*$7)[0]);
        id->genericArgs = $7;
        auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, id, $1);
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, ma, $11);
    }
  | qualified_identifier_no_generic DOT IDENTIFIER COLONCOLON LT { yyget_extra(scanner)->genericDepth++; } type_arg_list GT { yyget_extra(scanner)->genericDepth--; } LPAREN argument_list_opt RPAREN {
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3, std::make_shared<StringList>(), (*$7)[0]);
        id->genericArgs = $7;
        auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, id, std::static_pointer_cast<ExpressionNode>($1));
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, ma, $11);
    }
    /* On-type turbofish `Map::<int32,int32>.empty()` — the CANONICAL generic-constructor spelling: the
       enclosing type's args ride the RECEIVER (`::<…>` on the type), not the ctor. Reuses the shared
       `generic_turbofish_name` prefix; differs from the free-fn form only in the trailing `DOT IDENTIFIER`.
       The ctor's own turbofish slot (`.make::<…>` above) stays free for a ctor with its OWN generics. */
  | generic_turbofish_name DOT IDENTIFIER LPAREN argument_list_opt RPAREN {
        auto method = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3);
        auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, method, std::static_pointer_cast<ExpressionNode>($1));
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, ma, $5);
    }
  ;
/* Shared `IDENTIFIER::<type_args>` prefix — a type name (or generic free-fn name) carrying explicit type
   args in turbofish form. Factored out so the genericDepth mid-rule actions live in exactly one place. */
generic_turbofish_name
  : IDENTIFIER COLONCOLON LT { yyget_extra(scanner)->genericDepth++; } type_arg_list GT { yyget_extra(scanner)->genericDepth--; } {
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<StringList>(), (*$5)[0]);
        id->genericArgs = $5;
        $$ = id;
    }
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
/* `@name` / `@name(args)` — declaration attributes (serialization metadata + codegen trigger). A BARE arg
   (`@generate(Serialize)`) is an identifier with no value (name set, expression null); a NAMED arg
   (`@field(name: "x")`) is `key: expr`. A NON-EMPTY attribute_list is a distinct alternative on the type/
   field decl (never an empty prefix), keeping it free of shift/reduce conflicts with the plain
   modifiers_opt forms. */
attribute_list
  : attribute                  { $$ = std::make_shared<AttributeList>(); $$->push_back($1); }
  | attribute_list attribute   { $1->push_back($2); $$ = $1; }
  ;
attribute
  : AT IDENTIFIER                              { $$ = std::make_shared<AttributeNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<ArgumentList>()); }
  | AT IDENTIFIER LPAREN attr_arg_list RPAREN  { $$ = std::make_shared<AttributeNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  ;
attr_arg_list
  : attr_arg                       { $$ = std::make_shared<ArgumentList>(); $$->push_back($1); }
  | attr_arg_list COMMA attr_arg   { $1->push_back($3); $$ = $1; }
  ;
attr_arg
  : IDENTIFIER                     { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), SharedExpression()); }
  | IDENTIFIER COLON expression    { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), $3); }
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
  | NEW LPAREN argument_list RPAREN type LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT,  $5, $7, $3 ); }
  | NEW type DOT IDENTIFIER LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $2, $6); n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4); $$ = n; }
  | NEW LPAREN argument_list RPAREN type DOT IDENTIFIER LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $5, $9, $3); n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $7); $$ = n; }
    /* On-type turbofish through `new`: `new BTreeNode::<K,V,A>.make(...)` — the uniform construction spelling
       (explicit type args always ride the type as `::<…>`). Reuses `generic_turbofish_name` (the type carries
       its args), mirroring the plain-call on-type turbofish in invocation_expression. */
  | NEW generic_turbofish_name DOT IDENTIFIER LPAREN argument_list_opt RPAREN {
        auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $2, $6);
        n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4);
        $$ = n;
    }
  ;
unary_expression
  : postfix_expression
  | EXCLAMATION unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | TILDE unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | cast_expression
  | sizeof_expression
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
    /* The mid-rules track genericDepth across the cast's `<…>` so `cast<DynamicArray<int>>(x)` needs no space;
       the `--` fires before `( expression )` so a `>>` shift inside the cast body stays a shift. The
       operand is a full `expression` (like a parenthesized primary) so `cast<T>(a + b)` needs no inner
       parens — the surrounding `( … )` already delimits it. */
  : CAST LT { yyget_extra(scanner)->genericDepth++; } type GT { yyget_extra(scanner)->genericDepth--; } LPAREN expression RPAREN   { $$ = std::make_shared<CastNode>(SCANNER_CODEGENCONTEXT,  $4, $8 ); }
  ;
sizeof_expression
  /* `sizeof(T)` / `alignof(T)` — the compile-time byte size / alignment of a type as a `usize`
     (`type` self-manages its own `<…>` genericDepth, so `sizeof(InlineArray<int32,4>)` parses too). */
  : SIZEOF LPAREN type RPAREN   { $$ = std::make_shared<SizeofNode>(SCANNER_CODEGENCONTEXT, $3); }
  | ALIGNOF LPAREN type RPAREN   { auto s = std::make_shared<SizeofNode>(SCANNER_CODEGENCONTEXT, $3); s->isAlign = true; $$ = s; }
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

class_base_opt
  : /* Nothing */   { $$ = SharedClassBaseDeclaration(); }
  | class_base
  ;
class_base
  : EXTENDS type_name   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<IdentifierList>()); }
  | IMPLEMENTS interface_type_list   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, SharedIdentifier(), $2); }
  | EXTENDS type_name IMPLEMENTS interface_type_list   { $$ = std::make_shared<ClassBaseDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  ;
/* The `implements <contract>[, …]` list. A `Copyable` entry carries a mandatory contract parameter
   `(bare: give|copy)` — the bare-hand-off default (the first instance of contract parameters/metadata). */
interface_type_list
  : implements_entry   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | interface_type_list COMMA implements_entry   { $1->push_back($3); }
  ;
implements_entry
  : type_name   { $$ = $1; }
  | type_name LPAREN IDENTIFIER COLON handoff_default RPAREN   { $1->bareDefault = $5; $$ = $1; }
  | type_name when_clause
      { $1->whenParams = $2->whenParams; $1->whenBounds = $2->whenBounds; $$ = $1; }
  | type_name LPAREN IDENTIFIER COLON handoff_default RPAREN when_clause
      { $1->bareDefault = $5; $1->whenParams = $7->whenParams; $1->whenBounds = $7->whenBounds; $$ = $1; }
  ;
/* `when [P1: B1, P2: B2, …]` — a conditional gate (on a method or an implements entry). Square brackets
   are mandatory (one condition or many); the AND of all conditions must hold for the impl/method to exist.
   Carried on a synthetic holder identifier as parallel whenParams/whenBounds lists. */
when_clause
  : WHEN LEFT_BRACKET when_cond_list RIGHT_BRACKET   { $$ = $3; }
  ;
when_cond_list
  : IDENTIFIER COLON type_name
      { auto h = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, SharedString());
        h->whenParams = std::make_shared<IdentifierList>();
        h->whenBounds = std::make_shared<IdentifierList>();
        h->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1));
        h->whenBounds->push_back($3); $$ = h; }
    /* `when [A: default]` — a STRUCTURAL bound: the arg bound to A must have a `default` ctor
       (checked via isDefaultFillable). `default` is a keyword, so it can't reduce as a type_name. */
  | IDENTIFIER COLON DEFAULT
      { auto h = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, SharedString());
        h->whenParams = std::make_shared<IdentifierList>();
        h->whenBounds = std::make_shared<IdentifierList>();
        h->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1));
        h->whenBounds->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); $$ = h; }
  | when_cond_list COMMA IDENTIFIER COLON type_name
      { $1->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3));
        $1->whenBounds->push_back($5); $$ = $1; }
  | when_cond_list COMMA IDENTIFIER COLON DEFAULT
      { $1->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3));
        $1->whenBounds->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5)); $$ = $1; }
  ;
handoff_default
  : GIVE   { $$ = GIVE; }
  | COPY   { $$ = COPY; }
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
  | friend_declaration   { $$ = $1; }
  ;
constant_declaration
  : modifiers_opt CONST type constant_declarators SEMICOLON   { $$ = std::make_shared<ClassConstDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $4); }
  ;
field_declaration
  : modifiers_opt type variable_declarators SEMICOLON   { $$ = std::make_shared<ClassFieldDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  | attribute_list modifiers_opt type variable_declarators SEMICOLON   { auto f = std::make_shared<ClassFieldDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $3, $4); f->attributes = $1; $$ = f; }   /* `@field … type name;` */
  ;
method_declaration
  : modifiers_opt const_opt FN type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $10); m->isConst = ($2 != nullptr); if ($9) { m->whenParams = $9->whenParams; m->whenBounds = $9->whenBounds; } $$ = m; }
  | modifiers_opt const_opt FN VOID method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4, IDENTIFIER_VOID_VAL), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $10); m->isConst = ($2 != nullptr); if ($9) { m->whenParams = $9->whenParams; m->whenBounds = $9->whenBounds; } $$ = m; }
  | modifiers_opt const_opt FN REF type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), $8, $11); m->isConst = ($2 != nullptr); m->isRef = true; if ($10) { m->whenParams = $10->whenParams; m->whenBounds = $10->whenBounds; } $$ = m; }   /* `fn ref T at(…)` — a place-returning method */
  ;
/* `fn … when [T: Bound, …]` — a method present only when every gated type-param satisfies its bound (the
   value `iterator()` needs a Copyable element; `Map.copy()` needs both K AND V Copyable). The holder
   carries the parallel whenParams/whenBounds lists. */
method_when_opt
  : /* Nothing */   { $$ = SharedIdentifier(); }
  | when_clause     { $$ = $1; }
  ;
/* A method name is an identifier — but `copy`/`give` are hand-off markers only in expression
   position, so we let them name a member too (contextual keywords). This is what lets a `resource`
   opt into the `Copyable` contract with a method literally named `copy`. */
method_name
  : IDENTIFIER   { $$ = $1; }
  | COPY         { $$ = $1; }
  | GIVE         { $$ = $1; }
  ;

/* `const fn …` — an optional const qualifier on a method. A dedicated slot
   (not a general modifier) so it can't collide with the const-field / const-local
   declaration forms that also begin with CONST. */
const_opt
  : /* Nothing */   { $$ = SharedString(); }
  | CONST           { $$ = $1; }
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
  : REF type OPERATOR LEFT_BRACKET RIGHT_BRACKET LPAREN type IDENTIFIER RPAREN   { auto d = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $2, LEFT_BRACKET, $7, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $8), SharedIdentifier(), SharedIdentifier()); d->refReturn = true; $$ = d; }   /* `ref T operator[](usize i)` — a place-returning index operator */
  | type OPERATOR overloadable_operator LPAREN RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, SharedIdentifier(), SharedIdentifier(), SharedIdentifier(), SharedIdentifier()); }   /* 0-param unary: `Vec2 operator-()` = `-this` */
  | type OPERATOR overloadable_operator LPAREN type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), SharedIdentifier(), SharedIdentifier()); }
  | type OPERATOR overloadable_operator LPAREN type IDENTIFIER COMMA type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), $8, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $9) ); }
  ;
overloadable_operator
  : PLUS
  | MINUS
  | EXCLAMATION
  | TILDE
  | PLUSPLUS
  | MINUSMINUS
  /* `true`/`false` conversion operators are not overloadable (they aliased token codes 0/1) */
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
    /* Construction-model: a NAMED constructor — sugar for a static factory returning the enclosing type
       (infallible, no return type written) or `Result<This,E>` (fallible, leading type like `fn`). Lowered
       through the static-method pipeline; the emitter fills the infallible return type = the enclosing type. */
  | modifiers_opt CTOR method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body
    { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, SharedIdentifier(), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, $8); m->isCtor = true; if ($7) { m->whenParams = $7->whenParams; m->whenBounds = $7->whenBounds; } $$ = m; }
  | modifiers_opt CTOR type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body
    { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $9); m->isCtor = true; if ($8) { m->whenParams = $8->whenParams; m->whenBounds = $8->whenBounds; } $$ = m; }
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

/* `enum Name<T> : IntType { A, B(payload…) }`. The head reuses `type_decl_head` (so
   generic enums parse exactly like generic types); an optional `: IntType` pins the underlying
   integer / tag width; members may carry a named payload (below) making the enum a tagged union. */
enum_declaration
  : modifiers_opt ENUM type_decl_head enum_underlying_opt enum_body semicolon_opt
    { auto n = std::make_shared<EnumDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5);
      n->underlyingType = $4;
      /* Same param/bounds capture as marked_type_declaration: strip the head's genericArgs so the
         enum NAME stays bare `Optional`, keeping names + bounds on the node. */
      if ($3->genericArgs && !$3->genericArgs->empty()) {
          n->typeParams = std::make_shared<StringList>();
          n->typeBounds = std::make_shared<BoundsList>();
          n->constParams = std::make_shared<StringList>();
          n->typeDefaults = std::make_shared<IdentifierList>();
          for (auto& a : *$3->genericArgs) if (a && a->value) {
              n->typeParams->push_back(a->value);
              n->typeBounds->push_back(a->bounds ? a->bounds : std::make_shared<IdentifierList>());
              n->typeDefaults->push_back(a->defaultArg);
              if (a->isConstParam) n->constParams->push_back(a->value);
          }
          $3->genericArgs = SharedIdentifierList();
          $3->genericArg  = SharedIdentifier();
      }
      $$ = n; }
  | attribute_list modifiers_opt ENUM type_decl_head enum_underlying_opt enum_body semicolon_opt
    { auto n = std::make_shared<EnumDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $4, $6);
      n->underlyingType = $5;
      n->attributes = $1;   /* `@generate(Serialize, Deserialize) enum …` */
      if ($4->genericArgs && !$4->genericArgs->empty()) {
          n->typeParams = std::make_shared<StringList>();
          n->typeBounds = std::make_shared<BoundsList>();
          n->constParams = std::make_shared<StringList>();
          n->typeDefaults = std::make_shared<IdentifierList>();
          for (auto& a : *$4->genericArgs) if (a && a->value) {
              n->typeParams->push_back(a->value);
              n->typeBounds->push_back(a->bounds ? a->bounds : std::make_shared<IdentifierList>());
              n->typeDefaults->push_back(a->defaultArg);
              if (a->isConstParam) n->constParams->push_back(a->value);
          }
          $4->genericArgs = SharedIdentifierList();
          $4->genericArg  = SharedIdentifier();
      }
      $$ = n; }
  ;
enum_underlying_opt
  : /* Nothing */        { $$ = SharedIdentifier(); }
  | COLON integral_type  { $$ = $2; }
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
  | IDENTIFIER LPAREN parameter_list RPAREN   { auto m = std::make_shared<EnumMemberDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression() ); m->payload = $3; $$ = m; }   /* tagged-union variant with a named payload */
  ;

/*------------------------------------------------------------------------------ 
                              Interface 
------------------------------------------------------------------------------*/

%%

// Build a `ClassDeclarationNode` from the shared parts of a `type` declaration (with or without a leading
// `@…` attribute list). Captures generic params/bounds off the head and strips them so the class NAME stays
// bare (`Pair`/`Map`). Factored out so the attributed and un-attributed alternatives share one action.
SharedStatement makeTypeDeclaration(CodeGenContext& context, SharedAttributeList attributes,
    SharedModifierList modifiers, SharedString typeKind, SharedIdentifier head, SharedStringList forKinds,
    SharedClassBaseDeclaration base, SharedClassMemberDeclarationList body)
{
    auto n = std::make_shared<ClassDeclarationNode>(context, modifiers, head, base, body);
    n->typeKind   = typeKind;
    n->forKinds   = forKinds;   // `for value|resource|both` — mandatory on a `type contract`, else empty
    n->attributes = attributes; // `@generate(...)` etc. (null when the un-attributed alternative was used)
    if (head->genericArgs && !head->genericArgs->empty()) {
        n->typeParams   = std::make_shared<StringList>();
        n->typeBounds   = std::make_shared<BoundsList>();
        n->constParams  = std::make_shared<StringList>();
        n->typeDefaults = std::make_shared<IdentifierList>();
        for (auto& a : *head->genericArgs) if (a && a->value) {
            n->typeParams->push_back(a->value);
            n->typeBounds->push_back(a->bounds ? a->bounds : std::make_shared<IdentifierList>());
            n->typeDefaults->push_back(a->defaultArg);   // null when this param has no `= Default`
            if (a->isConstParam) n->constParams->push_back(a->value);
        }
        head->genericArgs = SharedIdentifierList();
        head->genericArg  = SharedIdentifier();
    }
    return n;
}

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
    // The width's FIRST digit is at index-1 ('6' of "…64", '1' of "…16", '3' of "…32"); index-2 is the
    // type char ('i'/'u'), so reading it here left every `i16`/`i64` literal parsed as 32-bit — truncating
    // any value that didn't fit int32 (`2654435761i64` → -1640531535). Unsigned masked it (values fit uint32).
    char firstDigit = str[index-1];
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
  
  // Extract the numeric digit substring and its radix (base==0 => an `N_xxx` based literal). An unsigned
  // literal is parsed with strtoull so the full 64-bit range (values above INT64_MAX like `…615ui64`) is
  // preserved; a signed literal keeps strtoll. Using strtoll for BOTH previously saturated any unsigned
  // literal past LLONG_MAX to 9223372036854775807 — a silent truncation.
  std::string digits;
  int realBase = base;
  if(base == 0)
  {
    // based type
    std::string::size_type underscoreIndex = str.find('_');
    std::string::size_type subStrLength = str.length() - underscoreIndex - suffixSize;
    realBase = strtol(str.substr(underscoreIndex + 1, subStrLength).c_str(), NULL, 10);
    digits = str.substr(0, underscoreIndex - 1);
  }
  else
  {
    digits = str.substr(0, str.length() - suffixSize);
  }

  long long val = 0;
  unsigned long long uval = 0;
  if(signedVal) val  = strtoll( digits.c_str(), NULL, realBase);
  else          uval = strtoull(digits.c_str(), NULL, realBase);
  SharedExpression rtn;

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
        rtn = std::make_shared<UInt8Node>(context, (uint8_t)uval);
      break;
      case 16:
        rtn = std::make_shared<UInt16Node>(context, (uint16_t)uval);
      break;
      case 64:
        rtn = std::make_shared<UInt64Node>(context, (uint64_t)uval);
      break;
      case 32:
      default:
        rtn = std::make_shared<UInt32Node>(context, (uint32_t)uval);
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

