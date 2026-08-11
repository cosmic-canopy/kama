%{
#define YYERROR_VERBOSE
#define YYDEBUG 1

/* Parse-stack depth (LSP M5.3). YYSTYPE below is a plain struct of shared_ptrs, NOT a %union, so Bison
 * never defines YYSTYPE_IS_TRIVIAL, so its stack-relocation path is compiled out entirely and the stack
 * CANNOT GROW: depth was hard-capped at Bison's YYINITDEPTH default of 200 and YYMAXDEPTH was moot.
 * 40 nested `if` blocks hit it and reported "memory exhausted", which is both a real limit for generated
 * code and decision tables and a diagnostic that tells the user nothing true.
 *
 * Raising the constant is the ONLY correct fix here. Do NOT "fix" it by defining YYSTYPE_IS_TRIVIAL or
 * by providing yyoverflow: both enable the relocation path, whose YYCOPY is __builtin_memcpy under
 * Clang/GCC — over a stack of shared_ptrs. Memcpying those is a use-after-free waiting to happen.
 *
 * The cost is paid per yyparse() call, which in `kama lsp` means per keystroke: 1200 x sizeof(YYSTYPE)
 * (~648 B) of C++ stack, default-constructed and destructed. Measured with KAMA_TIMING on a 363-line
 * buffer, buffer-parse moved 11.6 -> 12.1 ms — real, not noise, and worth 0.5 ms out of an 85 ms
 * analysis to stop lying to anyone with deeply nested code. Raise it deliberately, not to a huge
 * number: 1200 clears 200 nested ifs with room to spare (verified), which is far past hand-written
 * code and into generated-code territory. */
#define YYINITDEPTH 1200
#include <memory>
#include <string>
#include <cstdlib>
#include "kama.parser.hpp"
#include "kama.ast.h"
#include "kama.context.h"


struct LexerInstanceData*  yyget_extra ( yyscan_t scanner );
extern int yylex(YYSTYPE * yylval_param, YYLTYPE * yylloc_param, yyscan_t scanner);

int yyerror(YYLTYPE* llocp, yyscan_t scanner, const char *msg);
SharedExpression createIntegerLiteralNode(CodeGenContext& context, int base, const std::string& str);
SharedStatement makeTypeDeclaration(CodeGenContext& context, SharedAttributeList attributes,
    SharedModifierList modifiers, SharedString typeKind, SharedIdentifier head, SharedStringList forKinds,
    SharedClassBaseDeclaration base, SharedClassMemberDeclarationList body);
SharedStatement makeEnumDeclaration(CodeGenContext& context, SharedAttributeList attributes,
    SharedModifierList modifiers, SharedIdentifier head, SharedIdentifier underlying,
    SharedClassBaseDeclaration base, SharedEnumBody body);

/* MANDATORY BRACES — every branch and loop body must be a block.
 *
 * A bare body is where `goto fail;`-shaped bugs live: a later edit adds a second statement, it indents
 * like it belongs to the branch, and it does not. kama has no whitespace rule to fall back on, so the
 * brace is the only thing that can carry that meaning. Requiring it makes the bug unrepresentable.
 *
 * Checked HERE, at parse time, and not in the analysis walk: analysis never visits an uninstantiated
 * generic or a closure-pruned unit, so a bare body would survive in exactly the code nobody compiles
 * today. A parse check is total. It is a hand-raised yyerror inside the action rather than a grammar
 * restriction (`IF LPAREN … RPAREN block`) because bison's generic "expecting LEFT_BRACE" cannot say
 * WHICH body or why — the punctuation tokens carry no string aliases. Same shape as the `is` check in
 * type_parameter below.
 *
 * A body is braced iff it is a BlockNode, which is sound because the parser NEVER synthesizes one — a
 * bare body is stored raw on the parent, and the braces appear only at emission (emitBody in
 * kama.cemit.cpp). A NULL body is `if (x);`: empty_statement has an empty action, so $$ stays
 * default-constructed. That is the purest form of the bug, so it is rejected, not skipped.
 *
 * `else` additionally accepts an `if`. `else if (…) { … }` is one branch chain — the `if` IS the body's
 * brace — and forcing `else { if (…) { … } }` would nest every chain in the corpus for no safety gain. */
static void requireBraced(const SharedStatement& body, YYLTYPE* loc, yyscan_t scanner,
                          const char* what, const char* shape, bool elseArm = false)
{
    if (std::dynamic_pointer_cast<BlockNode>(body)) return;
    if (elseArm && std::dynamic_pointer_cast<IfNode>(body)) return;   /* an `else if` chain link */
    yyerror(loc, scanner, (std::string("the body of `") + what + "` must be braced -- write `"
                           + shape + "`").c_str());
}

/* A FUNCTION's type parameter takes no `= Default`. The grammar shares `type_param` with types and
 * enums, where a trailing default is a real feature (`Map<string, int32>` filling in a hasher and an
 * allocator), so `fn f<T = int32>()` parsed — and the three `fn` arms then dropped the default on the
 * floor, because FunctionDeclarationNode has no `typeDefaults` to put it in. The call failed afterwards
 * with `cannot infer type parameter 'T'`, which names the symptom of the silently discarded default.
 *
 * Rejected rather than implemented: a default fills in a type argument the use site OMITTED, and a
 * function's type arguments are not written at the use site to begin with — inference reads them off the
 * arguments, or a turbofish spells them. There is nothing for a default to fill in. Zero uses anywhere in
 * the tree. Same shape as `requireBraced` above and the `is` check in type_param: a hand-raised yyerror,
 * because bison's generic "unexpected =" cannot say which parameter or why. */
static void rejectFnTypeParamDefault(const SharedIdentifierList& params, YYLTYPE* loc, yyscan_t scanner)
{
    if (!params) return;
    for (auto& p : *params)
        if (p && p->defaultArg)
            yyerror(loc, scanner, (std::string("type parameter `") + (p->value ? *p->value : "?")
                                   + "` of a function may not have a default -- a default fills in an "
                                     "argument the use site omitted, and a function's type arguments come "
                                     "from inference or a turbofish. Defaults belong on a `type`.").c_str());
}

#define SCANNER_CODEGENCONTEXT *(yyget_extra(scanner)->codeGenContext)

/* LSP source spans (M0). With %locations the lexer stamps each token's true [start,end) into yylloc, so
 * Bison computes an accurate span (@$) for every rule from its RHS symbols' positions — free of the
 * one-token lookahead skew that plagued the old "read the lexer counter at reduce time" scheme. This
 * override does the standard span computation AND stashes @$ into the CodeGenContext, so the existing
 * ASTNode(context) ctor (its 90-odd construction sites) picks up the accurate rule position with no
 * per-site edits. A node built mid-action from a raw IDENTIFIER token (a decl name) still gets its
 * container's start line here; the few such name sites are stamped precisely via STAMP_LOC below. */
#define YYLLOC_DEFAULT(Cur, Rhs, N)                                          \
  do {                                                                       \
    if (N) {                                                                 \
      (Cur).first_line   = YYRHSLOC(Rhs, 1).first_line;                      \
      (Cur).first_column = YYRHSLOC(Rhs, 1).first_column;                    \
      (Cur).last_line    = YYRHSLOC(Rhs, N).last_line;                       \
      (Cur).last_column  = YYRHSLOC(Rhs, N).last_column;                     \
    } else {                                                                 \
      (Cur).first_line = (Cur).last_line = YYRHSLOC(Rhs, 0).last_line;       \
      (Cur).first_column = (Cur).last_column = YYRHSLOC(Rhs, 0).last_column; \
    }                                                                        \
    CodeGenContext* _ctx = yyget_extra(scanner)->codeGenContext.get();       \
    _ctx->line = (Cur).first_line;  _ctx->col = (Cur).first_column;          \
    _ctx->endLine = (Cur).last_line; _ctx->endCol = (Cur).last_column;       \
  } while (0)

/* Stamp an already-built identifier node with a token's precise @N span (for decl names constructed inside
 * a container action from a raw IDENTIFIER token, where @$ would span the whole container). */
#define STAMP_LOC(idExpr, Loc)  do { auto _sid = (idExpr); if (_sid) {        \
    _sid->line = (Loc).first_line;   _sid->column = (Loc).first_column;       \
    _sid->endLine = (Loc).last_line; _sid->endColumn = (Loc).last_column; } } while (0)

/* A `::`-separated name list (a qualifier, an import path, an export manifest) is a list of plain STRINGS,
 * so a segment has no node to carry its position. STAMP_SEG records the token's span beside the list, in
 * CodeGenContext::listSegPos (see there for why it is keyed by the list object); TAKE_SEGS moves the whole
 * vector onto the owning node at the CONSUMING reduction. M6 B3f. */
/* NB the parentheses: SCANNER_CODEGENCONTEXT expands to a `*deref`, and `.` binds tighter than unary `*`. */
#define STAMP_SEG(listExpr, Loc)  do { auto _sl = (listExpr);                              \
    (SCANNER_CODEGENCONTEXT).stampSeg(_sl.get(), (Loc).first_line, (Loc).first_column,     \
                                      (Loc).last_line, (Loc).last_column); } while (0)
#define TAKE_SEGS(dst, listExpr)  do { auto _sl = (listExpr);                              \
    (dst) = (SCANNER_CODEGENCONTEXT).takeSegs(_sl.get()); } while (0)

%}

%code requires {

#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#include "kama.ast.h"
#include "kama.context.h"
#include <unordered_set>

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

   /* Every identifier-token spelling seen in this file, recorded by the lexer (RECORD_IDENT in kama.l)
      and moved onto the CompilationUnit at reduction. It is the reference side of closure pruning: a
      same-namespace sibling is reachable with NO import at all (`priority_queue.kama` declares
      `DynamicArray<T, A> data;` and imports nothing), so an import-edge closure under-computes.
      Unordered on purpose — this is one insert per identifier token on a ~14 ms parse. */
   std::unordered_set<std::string> identTokens;
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
  SharedEnumBody enumbody;   // an enum body's two lists (variants + post-`;` class members)
  SharedIntrinsicBody intrinsicbody;   // a `type intrinsic` body (shared members + per-target sections)
  SharedIdentifierList intrinsictargets;   // the primitive set in `<int8, int16, …>`
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
%locations
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
%token <string> CASE CAST BITCAST COMPTIME CONST CONTINUE CTOR DEFAULT
%token <string> AS CHAR DO DOUBLE ELSE ENUM EXPORT EXPOSE EXTERN EXTENDS IMPLEMENTS IMPORT
%token <string> FALSE FINAL FLOAT32 FLOAT64
%token <string> FN FNPTR FOR FOREACH HARDWARE IF IMMUTABLE IN
%token <string> INT INT8 INT16 INT32 INT64 SPAWN SCOPE PARALLEL_FOR
%token <string> MATCH
%token <string> NAMESPACE
%token <string> NEW NULL_LITERAL OPERATOR OUT SIZEOF ALIGNOF TRY ASM
%token <string> OVERRIDE PRIVATE PROTECTED PUBLIC FRIEND
%token <string> REF RETURN SLOT STATIC STRING
%token <string> THIS TRUE TYPE
%token <string> UINT8 UINT16 UINT32 UINT64
%token <string> UNSAFE VIRTUAL VOID
%token <string> WHILE

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
%type <expression> postfix_expression cast_expression bitcast_expression sizeof_expression member_access element_access this_access
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
%type <statement> marked_type_declaration unsafe_statement spawn_statement scope_statement parallel_for_statement arm_value_statement asm_statement
%type <statement> module_variable_declaration
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
%type <argumentlist> match_bindings
%type <argument> match_binding
%type <expressionstatement> object_creation_expression new_expression post_increment_expression post_decrement_expression
%type <expressionstatement> pre_increment_expression pre_decrement_expression
   /* %type <unaryexpression> unary_expression */
   /*%type <binaryexpression>*/
%type <argument> argument attr_arg
%type <argumentlist> argument_list_opt argument_list attr_arg_list
%type <attribute> attribute
%type <attributelist> attribute_list
%type <enummemberdecl> enum_member_declaration
%type <enummemberdecllist> enum_member_declarations_opt enum_member_declarations
%type <enumbody> enum_class_body
%type <intrinsicbody> intrinsic_body intrinsic_members
%type <intrinsictargets> intrinsic_target_list
%type <statement> marked_intrinsic_declaration
%type <classbasedecl> class_base_opt class_base
%type <classmemberdecl> class_member_declaration constant_declaration field_declaration method_declaration friend_declaration
%type <classmemberdecl> operator_declaration constructor_declaration destructor_declaration
%type <classmemberdecllist> class_body class_member_declarations_opt class_member_declarations
%type <operatordeclarator> operator_declarator overloadable_operator_declarator
%type <constructordeclarator> constructor_declarator
%type <constructorinitializer> constructor_initializer_opt constructor_initializer
%type <string> const_opt hardware_opt method_name

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
  : namespace_opt import_directives_opt export_manifest_opt code_opt  { yyget_extra(scanner)->compilationUnit = CreateCompilationUnit( SCANNER_CODEGENCONTEXT, yyget_extra(scanner)->codeGenContext->getModuleName(), $1, $2, $3, $4); TAKE_SEGS(yyget_extra(scanner)->compilationUnit->exportListPos, $3);
      /* Closure-pruning facts, harvested HERE because this is the one reduction every parse goes through,
         and because it is before any emitter exists to rewrite the decl list (see CompilationUnit). */
      harvestUnitFacts(yyget_extra(scanner)->compilationUnit);
      yyget_extra(scanner)->compilationUnit->identTokens.insert(yyget_extra(scanner)->identTokens.begin(),
                                                                yyget_extra(scanner)->identTokens.end()); }
  ;

/* The module's PUBLIC SURFACE, declared once at the top: `export { A, B, C };`. A name here must be a
   top-level declaration IN THIS FILE; everything unlisted is module-private. Declarations themselves carry
   no visibility modifier (so `type`/`fn` syntax stays uniform). Mirrors `import a::b::{A, B}`. */
export_manifest_opt
  : /* Nothing */   { $$ = std::make_shared<StringList>(); }
  | EXPORT LEFT_BRACE export_name_list RIGHT_BRACE SEMICOLON   { $$ = $3; }
  ;
export_name_list
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); STAMP_SEG($$, @1); }
  | export_name_list COMMA IDENTIFIER   { $1->push_back($3); $$ = $1; STAMP_SEG($$, @3); }
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
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<UsingDeclarationList>(), SharedString()); TAKE_SEGS($$->modulePathPos, $2); }
  | IMPORT import_path AS IDENTIFIER SEMICOLON
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, std::make_shared<UsingDeclarationList>(), $4); TAKE_SEGS($$->modulePathPos, $2); }
  | IMPORT import_path COLONCOLON LEFT_BRACE import_symbols RIGHT_BRACE SEMICOLON
      { $$ = std::make_shared<ImportDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $5, SharedString()); TAKE_SEGS($$->modulePathPos, $2); }
  ;
import_path
  : IDENTIFIER   { $$ = std::make_shared<StringList>(); $$->push_back($1); STAMP_SEG($$, @1); }
  | import_path COLONCOLON IDENTIFIER   { $1->push_back($3); $$ = $1; STAMP_SEG($$, @3); }
  ;
import_symbols
  : import_symbol   { $$ = std::make_shared<UsingDeclarationList>(); $$->push_back($1); }
  | import_symbols COMMA import_symbol   { $1->push_back($3); $$ = $1; }
  ;
import_symbol
  : IDENTIFIER   { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | IDENTIFIER AS IDENTIFIER   { $$ = std::make_shared<UsingDeclarationNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); STAMP_LOC($$->identifier, @1); STAMP_LOC($$->alias, @3); }
  ;

code_opt
  : /* Nothing */   { $$ = std::make_shared<StatementList>(); }
  | code_declarations
  ;
code_declarations
  : code_declaration   { $$ = std::make_shared<StatementList>(); $$->push_back($1); }
  | code_declarations code_declaration   { $1->push_back($2); }
  /* Error recovery, LAST RESORT (LSP M5.3). Only reached when the statement- and member-level arms
   * above could not resync — an unbalanced brace, typically. Unlike those, this one loses a whole
   * `type` or `fn`, which makes every reference to it read as undeclared: that is the one recovery
   * outcome that carpets a file with false semantic errors. Hence the counter — the LSP publishes
   * semantic diagnostics from a partial parse EXCEPT when this arm fired (see lspAnalyze).
   *
   * Note this cannot recover garbage as the very first token of the file: with `parse.lac full` the
   * header nonterminals have not reduced yet at the initial state, so nothing on the stack carries an
   * `error` action. A file whose first character is garbage has nothing worth recovering anyway. */
  | error   { $$ = std::make_shared<StatementList>(); yyget_extra(scanner)->codeGenContext->droppedTopLevelDecl++; }
  | code_declarations error   { $$ = $1; yyget_extra(scanner)->codeGenContext->droppedTopLevelDecl++; }
  ;
code_declaration
  : function_declaration
  | type_declaration
  | module_variable_declaration
  ;

/* Module-level mutable static (MCU campaign step 1). `STATIC` is a unique prefix at top level
   (fn/type don't start with it), so no conflict. Reuses `variable_declarators`;
   const-init + value/Ptr/InlineArray legality are enforced semantically in the emitter. */
module_variable_declaration
  : STATIC hardware_opt type variable_declarators SEMICOLON   { auto mv = std::make_shared<ModuleVariableDeclaration>(SCANNER_CODEGENCONTEXT, $3, $4); mv->isHardware = ($2 != nullptr); $$ = mv; }
  | attribute_list STATIC hardware_opt type variable_declarators SEMICOLON   { auto mv = std::make_shared<ModuleVariableDeclaration>(SCANNER_CODEGENCONTEXT, $4, $5); mv->isHardware = ($3 != nullptr); mv->attributes = $1; $$ = mv; }   /* `@section(".x") static …` */
  | COMPTIME type variable_declarators SEMICOLON   { auto mv = std::make_shared<ModuleVariableDeclaration>(SCANNER_CODEGENCONTEXT, $2, $3); mv->isComptime = true; $$ = mv; }   /* `comptime NAME = <expr>` — a named compile-time constant (6b-2) */
  | attribute_list COMPTIME type variable_declarators SEMICOLON   { auto mv = std::make_shared<ModuleVariableDeclaration>(SCANNER_CODEGENCONTEXT, $3, $4); mv->isComptime = true; mv->attributes = $1; $$ = mv; }   /* `@section(".flash") comptime …` */
  ;

/*------------------------------------------------------------------------------ 
                              Literals 
------------------------------------------------------------------------------*/

literal
  : boolean_literal
  /* ⚠️ strtoLL, not strtol: `long` is 64-bit on Unix and 32-BIT ON WINDOWS (LLP64), so strtol SATURATES
     an unsuffixed literal above 2147483647 to LONG_MAX there and silently returns a different number
     than the same source produces on macOS or Linux. `att.depthSlice = 4294967295` — WebGPU's
     DEPTH_SLICE_UNDEFINED — compiled to 2147483647, and wgpu rejected the render pass with "Depth slice
     was provided but the color attachment's view is not 3D": the native examples/webgpu triangle drew
     nothing on Windows and was fine everywhere else. strtoll is 64-bit on every platform, so the
     narrowing to Int32Node's int32_t wraps identically on all of them, which is the point.
     The suffixed forms below already learned this (see createIntegerLiteralNode's note on strtoull). */
  | DEC_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, (int32_t)strtoll( $1->c_str(), NULL, 10)); }
  | HEX_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, (int32_t)strtoll( $1->c_str(), NULL, 16)); }
  | OCT_LITERAL_NO_SUFFIX   { $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, (int32_t)strtoll( $1->substr(2).c_str(), NULL, 8)); }
  | BASED_LITERAL_NO_SUFFIX   { std::string::size_type underscoreIndex = $1->find('_');
    int base = (int)strtoll($1->substr(underscoreIndex + 1).c_str(), NULL, 10);
    $$ = std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, (int32_t)strtoll( $1->substr(2, underscoreIndex - 3).c_str(), NULL, base));
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
  | interp_hole DOT IDENTIFIER   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); STAMP_LOC(ma->identifier, @3); $$ = ma; }
  | interp_hole LEFT_BRACKET interp_index RIGHT_BRACKET   { $$ = std::make_shared<ElementAccessNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  ;
interp_index
  : IDENTIFIER              { $$ = std::make_shared<ExpressionList>(); $$->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | DEC_LITERAL_NO_SUFFIX   { $$ = std::make_shared<ExpressionList>(); $$->push_back(std::make_shared<Int32Node>(SCANNER_CODEGENCONTEXT, (int32_t)strtoll($1->c_str(), NULL, 10))); }
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
  | qualifier basic_identifier   { $$ = $2; $$->setQualifier($1); TAKE_SEGS($$->qualifierPos, $1); }
  ;
qualifier
  : IDENTIFIER COLONCOLON { $$ = std::make_shared<StringList>(); $$->push_back($1); STAMP_SEG($$, @1); }
  | qualifier IDENTIFIER COLONCOLON { $1->push_back($2); STAMP_SEG($1, @2); }
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
        STAMP_LOC(id, @1);   /* the NAME only — rename must not swallow `<A, B, …>` */
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
  | LPAREN expression RPAREN   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, SharedString()); id->constArgValue = $2; $$ = id; }   /* MCU 6b-1: const arithmetic size `InlineArray<T, (N+1)>` — parens keep `>`/`>>` unambiguous vs generic close; folded by constValue() at instantiation */
  ;

qualified_identifier_no_generic
  : IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); }
    /* The name only, NOT `Ns::Name` — this is the production an `Enum::Member` read or a `mod::fn` call
       reduces through, and rename REPLACES the range: a whole-production span would eat the qualifier. */
  | qualifier IDENTIFIER  { $$ = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2, $1); STAMP_LOC($$, @2); TAKE_SEGS($$->qualifierPos, $1); }
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
  | marked_intrinsic_declaration
  ;

/* `type <kind> Name { … }` — the ownership-model declaration. The kind word
   (`value`/`resource`/`contract`) is an ordinary IDENTIFIER checked by the emitter, so it
   is never reserved. `type` marks every type declaration (greppable, like `fn`). All three
   kinds share the class body; the emitter routes `contract` to the fat-pointer vtable path. */
marked_type_declaration
  : TYPE modifiers_opt IDENTIFIER type_decl_head for_kinds_opt class_base_opt class_body semicolon_opt
    {      $$ = makeTypeDeclaration(SCANNER_CODEGENCONTEXT, SharedAttributeList(), $2, $3, $4, $5, $6, $7); }
  | attribute_list TYPE modifiers_opt IDENTIFIER type_decl_head for_kinds_opt class_base_opt class_body semicolon_opt
    {      $$ = makeTypeDeclaration(SCANNER_CODEGENCONTEXT, $1, $3, $4, $5, $6, $7, $8); }   /* `@generate(...) type …` */
  ;

/* `type intrinsic <int8, int16, …> implements C { …methods… <int8> { …methods… } }` — conformance for a
   PRIMITIVE. The kind word stays a positional bare IDENTIFIER (the emitter checks it is `intrinsic`), so
   `intrinsic` is never reserved and `int32 intrinsic = 1;` keeps working.

   NO CONFLICT with `marked_type_declaration`, even though both begin `TYPE modifiers_opt IDENTIFIER`: its
   `type_decl_head` continues with the NAME, an IDENTIFIER, while this one continues with `<`. One token of
   lookahead separates them, and `type intrinsic <…>` was a parse error before, which is what left the slot
   free. The target list is `simple_type` — every one of its first tokens (INT8…UINT64, FLOAT32, FLOAT64,
   BOOL, CHAR, STRING) is RESERVED, so it cannot collide with an IDENTIFIER either. That is also why the
   legal target set is exactly the primitives: it falls out of the grammar rather than being checked.

   No `genericDepth` mid-rule action after the `<`: the list holds only primitives, so it can never nest,
   so `>` can never lex as `>>`. (A duplicated mid-rule action becomes its own empty nonterminal and
   reduce/reduce-conflicts — see the note on parameter_modifier_opt.) */
marked_intrinsic_declaration
  : TYPE modifiers_opt IDENTIFIER LT intrinsic_target_list GT class_base_opt intrinsic_body semicolon_opt
    { auto n = std::make_shared<IntrinsicImplNode>(SCANNER_CODEGENCONTEXT, $2, $5, $7, $8);
      n->kindWord = $3;
      $$ = n; }
  ;
intrinsic_target_list
  : simple_type   { $$ = std::make_shared<IdentifierList>(); $$->push_back($1); }
  | intrinsic_target_list COMMA simple_type   { $1->push_back($3); $$ = $1; }
  ;
/* A member is either shared by every target, or inside a `<…> { … }` SECTION that overrides it for the
   targets it names. A section can only begin with `<`, which no class member can, so the two are
   distinguishable with no lookahead trickery. */
intrinsic_body
  : LEFT_BRACE RIGHT_BRACE   { $$ = std::make_shared<IntrinsicBody>();
                               $$->members  = std::make_shared<ClassMemberDeclarationList>();
                               $$->sections = std::make_shared<IntrinsicSectionList>(); }
  | LEFT_BRACE intrinsic_members RIGHT_BRACE   { $$ = $2; }
  ;
intrinsic_members
  : class_member_declaration
    { $$ = std::make_shared<IntrinsicBody>();
      $$->members  = std::make_shared<ClassMemberDeclarationList>();
      $$->sections = std::make_shared<IntrinsicSectionList>();
      $$->members->push_back($1); }
  | LT intrinsic_target_list GT LEFT_BRACE class_member_declarations_opt RIGHT_BRACE
    { $$ = std::make_shared<IntrinsicBody>();
      $$->members  = std::make_shared<ClassMemberDeclarationList>();
      $$->sections = std::make_shared<IntrinsicSectionList>();
      auto sec = std::make_shared<IntrinsicSection>(); sec->targets = $2; sec->members = $5;
      $$->sections->push_back(sec); }
  | intrinsic_members class_member_declaration   { $1->members->push_back($2); $$ = $1; }
  | intrinsic_members LT intrinsic_target_list GT LEFT_BRACE class_member_declarations_opt RIGHT_BRACE
    { auto sec = std::make_shared<IntrinsicSection>(); sec->targets = $3; sec->members = $6;
      $1->sections->push_back(sec); $$ = $1; }
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
        STAMP_LOC(id, @1);      /* the NAME only — without this a rename REPLACES `Box<T>`, eating `<T>` */
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
    /* `virtual(maxDepth: 2)` / `abstract(maxDepth: 1)` — how many levels may still be added BELOW this
       type. Reuses `argument_list` so the spelling is kama's ordinary named-argument one; the emitter
       checks the name and the value, per the usual permissive-grammar/diagnosing-emitter split. The
       modifier list is shared with methods, so `virtual(maxDepth: 1) fn` parses too and is rejected
       there — the same way a class-named ctor still parses so it can be answered with a sentence. */
  | VIRTUAL LPAREN argument_list RPAREN   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | ABSTRACT LPAREN argument_list RPAREN   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1, $3); }
  | IMMUTABLE   { $$ = std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1); }   /* `immutable value T` — deeply-immutable, shareable across isolates (M6.2) */
  ;

friend_declaration
  : FRIEND qualified_identifier LEFT_BRACKET friend_member_list RIGHT_BRACKET SEMICOLON
      { $$ = std::make_shared<FriendGrantNode>(SCANNER_CODEGENCONTEXT, $2, $4); }
  | FRIEND qualified_identifier LEFT_BRACKET ELLIPSIS RIGHT_BRACKET SEMICOLON
      { $$ = std::make_shared<FriendGrantNode>(SCANNER_CODEGENCONTEXT, $2, nullptr); }   // [...] => all privates
  ;
friend_member_list
  : IDENTIFIER   { $$ = std::make_shared<IdentifierList>(); $$->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1)); }
  | friend_member_list COMMA IDENTIFIER   { $1->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3)); STAMP_LOC($1->back(), @3); $$ = $1; }
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
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $1), $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, SharedBlock() );
      STAMP_LOC(fn->name, @4);
      $$ = fn;
   }
  | COMPTIME FN function_return_type IDENTIFIER LPAREN parameter_list_opt RPAREN block   {
      /* `comptime fn T name(…)` — a compile-time-only free function (const-eval 6b-3). Monomorphic in v1
         (no type/const params, no `expose` — it is never emitted as a C symbol). */
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  SharedModifier(), $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8 );
      STAMP_LOC(fn->name, @4);
      fn->isComptime = true;
      $$ = fn;
  }
  | function_modifier_opt FN function_return_type IDENTIFIER type_params_opt LPAREN parameter_list_opt RPAREN block   {
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $7, $9 );
      STAMP_LOC(fn->name, @4);   /* precise name span (an unmodified fn's @$ starts at the previous token) */
      /* Split `<T, K: I + J>` into parallel typeParams (names) + typeBounds (contract lists). */
      rejectFnTypeParamDefault($5, &@5, scanner);
      if ($5 && !$5->empty()) {
          fn->typeParams = std::make_shared<StringList>();
          fn->typeBounds = std::make_shared<BoundsList>();
          fn->typePins  = std::make_shared<IdentifierList>();
          fn->constParams = std::make_shared<StringList>();
          fn->constTypes  = std::make_shared<IdentifierList>();
          for (auto& p : *$5) if (p && p->value) {
              fn->typeParams->push_back(p->value);
              fn->typeBounds->push_back(p->bounds ? p->bounds : std::make_shared<IdentifierList>());
              fn->typePins->push_back(p->pin);   // `<T is This>` — only a contract has an implementer
              fn->constTypes->push_back(p->isConstParam ? p->constType : SharedIdentifier());
              if (p->isConstParam) fn->constParams->push_back(p->value);
          }
      }
      $$ = fn;
  }
  | attribute_list function_modifier_opt FN function_return_type IDENTIFIER type_params_opt LPAREN parameter_list_opt RPAREN block   {
      /* `@interrupt`/`@section(".x")` fn — MCU codegen attributes (mirrors the attributed-TYPE form). */
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  $2, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $8, $10 );
      STAMP_LOC(fn->name, @5);
      fn->attributes = $1;
      rejectFnTypeParamDefault($6, &@6, scanner);
      if ($6 && !$6->empty()) {
          fn->typeParams = std::make_shared<StringList>();
          fn->typeBounds = std::make_shared<BoundsList>();
          fn->typePins  = std::make_shared<IdentifierList>();
          fn->constParams = std::make_shared<StringList>();
          fn->constTypes  = std::make_shared<IdentifierList>();
          for (auto& p : *$6) if (p && p->value) {
              fn->typeParams->push_back(p->value);
              fn->typeBounds->push_back(p->bounds ? p->bounds : std::make_shared<IdentifierList>());
              fn->typePins->push_back(p->pin);   // `<T is This>` — only a contract has an implementer
              fn->constTypes->push_back(p->isConstParam ? p->constType : SharedIdentifier());
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
      STAMP_LOC(fn->name, @5);
      fn->isRef = true;
      rejectFnTypeParamDefault($6, &@6, scanner);
      if ($6 && !$6->empty()) {
          fn->typeParams = std::make_shared<StringList>();
          fn->typeBounds = std::make_shared<BoundsList>();
          fn->typePins  = std::make_shared<IdentifierList>();
          fn->constParams = std::make_shared<StringList>();
          fn->constTypes  = std::make_shared<IdentifierList>();
          for (auto& p : *$6) if (p && p->value) {
              fn->typeParams->push_back(p->value);
              fn->typeBounds->push_back(p->bounds ? p->bounds : std::make_shared<IdentifierList>());
              fn->typePins->push_back(p->pin);   // `<T is This>` — only a contract has an implementer
              fn->constTypes->push_back(p->isConstParam ? p->constType : SharedIdentifier());
              if (p->isConstParam) fn->constParams->push_back(p->value);
          }
      }
      $$ = fn;
  }
  | FNPTR function_return_type IDENTIFIER LPAREN parameter_list_opt RPAREN SEMICOLON   {
      /* `fnptr ret Name(params);` — an explicit function-pointer TYPE.
         A null body marks it as a signature type (collectSignatures -> _sigs). */
      auto fn = std::make_shared<FunctionDeclarationNode>(SCANNER_CODEGENCONTEXT,  SharedModifier(), $2, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, SharedBlock() );
      STAMP_LOC(fn->name, @3);
      $$ = fn;
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
  : IDENTIFIER type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); id->defaultArg = $2; STAMP_LOC(id, @1); $$ = id; }
    /* `<T is This>` — an IDENTITY constraint, pinning the parameter to the implementing type. Deliberately
       NOT a `bound_list` entry: a bound holds a CONTRACT (a non-contract there is already a hard error),
       so admitting `This` would cost an exception plus a hand-rejection of `T: This + Contract`. Its own
       slot keeps that rule intact and makes `T is This + Contract` ungrammatical rather than diagnosed.
       `is` is a CONTEXTUAL identifier, like the `for value|resource|both` kind words — it reserves nothing.
       Two IDENTIFIERs in a row are unambiguous here: every other arm takes `:`, `=`, `,` or `>` next. */
  | IDENTIFIER IDENTIFIER type_name
      { if (*$2 != "is") yyerror(&@2, scanner, "expected `is` or `:` after a type parameter name");
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); id->pin = $3;
        STAMP_LOC(id, @1); $$ = id; }
  | IDENTIFIER COLON bound_list type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); id->bounds = $3; id->defaultArg = $4; STAMP_LOC(id, @1); $$ = id; }
  | CONST IDENTIFIER COLON integral_type type_param_default_opt   { auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2); id->isConstParam = true; id->constType = $4; id->defaultArg = $5; STAMP_LOC(id, @2); $$ = id; }   /* `const N: int` — a compile-time value param */
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
  : const_opt hardware_opt parameter_modifier_opt type IDENTIFIER   { auto p = std::make_shared<FunctionParameterNode>(SCANNER_CODEGENCONTEXT, $3, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5)); p->isConst = ($1 != nullptr); p->isHardware = ($2 != nullptr); STAMP_LOC(p->identifier, @5); $$ = p; }
  ;
parameter_modifier_opt
  : /* Nothing */ { $$ = SharedModifier(); }
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
    /* `slot T x;` — a declared HOLE: storage with no value yet, illegal to read until definitely
       assigned, and with no destructor emitted while it stays unassigned. A leading keyword, exactly like
       `const`/`comptime` above, so it adds no conflict (the decision is made on the first token). */
  | SLOT type variable_declarators   { auto d = std::make_shared<LocalVariableDeclaration>(SCANNER_CODEGENCONTEXT, $2, $3); d->isSlot = true; $$ = d; }
  ;
variable_declarators
  : variable_declarator   { $$ = std::make_shared<VariableDeclaratorList>(); $$->push_back($1); }
  | variable_declarators COMMA variable_declarator   { $1->push_back($3); }
  ;
variable_declarator
  : IDENTIFIER   { $$ = std::make_shared<VariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression() ); }
  | IDENTIFIER EQ variable_initializer   { $$ = std::make_shared<VariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3); STAMP_LOC($$->name, @1); }
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
  | COMPTIME type constant_declarators   { auto d = std::make_shared<ConstLocalVariableDeclaration>(SCANNER_CODEGENCONTEXT, $2, $3); d->isComptime = true; $$ = d; }   /* `comptime T NAME = <expr>` — explicit compile-time local (6b-2) */
  ;
constant_declarators
  : constant_declarator   { $$ = std::make_shared<ConstVariableDeclaratorList>(); $$->push_back($1); }
  | constant_declarators COMMA constant_declarator   { $1->push_back($3); }
  ;
constant_declarator
  : IDENTIFIER EQ constant_expression   { $$ = std::make_shared<ConstVariableDeclarator>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3); STAMP_LOC($$->name, @1); }
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
  /* Error recovery, innermost grain (LSP M5.3). This is the load-bearing arm: Bison pops to the
   * NEAREST state with an `error` action, so a broken statement discards one statement and leaves the
   * enclosing function — its signature, its other locals, its remaining statements — intact. That is
   * what makes the partial AST worth having: `P p; p.<cursor>` keeps `p` in the index, which
   * declaration-level recovery alone would throw away along with the whole function.
   *
   * Both a leading and a trailing arm are needed. With only the trailing one, a block whose FIRST
   * statement is broken has no statement_list on the stack, so no state carries an `error` action and
   * the parse aborts — and "the first thing I typed isn't finished yet" is the commonest editing state
   * there is.
   *
   * The arms contribute NO node, deliberately. Downstream passes dereference every element
   * unconditionally, so a half-built node with a null body is a shape the emitter has never been asked
   * to survive, whereas a SHORTER well-formed list is one it already handles on every build — that is
   * exactly what pruneInactiveDecls produces for `@compileFor`.
   *
   * No %destructor is needed and none should be added: YYSTYPE is a plain struct rather than a %union,
   * so the value stack is a real array of default-constructed YYSTYPEs and a discarded subtree simply
   * stays alive until yyparse returns. `yyerrok` is deliberately absent too — Bison's 3-token
   * suppression window is what stops one broken statement from reporting an error per discarded token. */
  | error   { $$ = std::make_shared<StatementList>(); }
  | statement_list error   { $$ = $1; }
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
  | parallel_for_statement
  | asm_statement
  ;
arm_value_statement
    /* `:= expr;` — the value a match arm's block produces (assigned out to whatever the match is bound
       to). A distinct statement (not a jump); the emitter requires it be the arm block's last statement. */
  : WALRUS expression SEMICOLON   { $$ = std::make_shared<ArmValueNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
unsafe_statement
  : UNSAFE block   { $$ = std::make_shared<UnsafeNode>(SCANNER_CODEGENCONTEXT, $2); }
  ;
  /* `asm("wfi");` — inline assembly (MCU step 6a). One string-literal operand; lowers to
     `__asm__ __volatile__("<text>" : : : "memory")`. The emitter requires an enclosing `unsafe { }`
     (the single greppable raw-operation seam) and always emits the volatile + memory-clobber form. */
asm_statement
  : ASM LPAREN STRING_LITERAL RPAREN SEMICOLON   { $$ = std::make_shared<AsmNode>(SCANNER_CODEGENCONTEXT, $3); }
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
  /* `parallel_for (ref T e in coll) { ... }` — disjoint-slice data-parallel loop (M6.3): splits `coll`
     into K non-overlapping sub-Views, one per worker, mutates each in place, and joins them ALL at its
     own closing brace (self-joining barrier). `ref` is mandatory (disjoint mutable is the whole point);
     the body is a `block` because those braces ARE the barrier (like `scope { }`). */
parallel_for_statement
  : PARALLEL_FOR LPAREN REF type IDENTIFIER IN expression RPAREN block   { auto n = std::make_shared<ParallelForNode>(SCANNER_CODEGENCONTEXT, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $9); STAMP_LOC(n->name, @5); $$ = n; }
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
/* MANDATORY BRACES. Every branch and loop body below — if / else / while / do / for / foreach — must be a
   `block`. The body is spelled `embedded_statement` here on purpose: the restriction is enforced in the
   semantic action (requireBraced, in src/kama.y's prologue) rather than in the CFG, so the diagnostic can name the
   construct and point at the offending body instead of degrading to bison's "expecting LEFT_BRACE". A body
   that is not a `block` is a PARSE error; so is an empty `;` body. The single exemption is the `else` arm,
   which also accepts an `if` — that is an `else if` chain link, not a bare body. Rule + rationale:
   docs/SPEC.md "Control flow"; rejections guarded by tests/xfail/brace_*.kama. */
selection_statement
  : if_statement
  ;
if_statement
  : IF LPAREN boolean_expression RPAREN embedded_statement
      { requireBraced($5, &@5, scanner, "if", "if (...) { ... }");
        $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, SharedStatement()); }
  | IF LPAREN boolean_expression RPAREN embedded_statement ELSE embedded_statement
      { requireBraced($5, &@5, scanner, "if", "if (...) { ... }");
        requireBraced($7, &@7, scanner, "else", "else { ... }", /*elseArm*/ true);
        $$ = std::make_shared<IfNode>(SCANNER_CODEGENCONTEXT, $3, $5, $7); }
  ;
iteration_statement
  : while_statement
  | do_statement
  | for_statement
  | foreach_statement
  ;
while_statement
  : WHILE LPAREN boolean_expression RPAREN embedded_statement
      { requireBraced($5, &@5, scanner, "while", "while (...) { ... }");
        $$ = std::make_shared<WhileNode>(SCANNER_CODEGENCONTEXT,  $3, $5 ); }
  ;
do_statement
  : DO embedded_statement WHILE LPAREN boolean_expression RPAREN SEMICOLON
      { requireBraced($2, &@2, scanner, "do", "do { ... } while (...);");
        $$ = std::make_shared<DoWhileNode>(SCANNER_CODEGENCONTEXT,  $5, $2 ); }
  ;
for_statement
  : FOR LPAREN for_initializer_opt SEMICOLON for_condition_opt SEMICOLON for_iterator_opt RPAREN embedded_statement   {
      requireBraced($9, &@9, scanner, "for", "for (...; ...; ...) { ... }");
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
    { auto a = std::make_shared<MatchArmNode>(SCANNER_CODEGENCONTEXT); a->variantName = $1;
      a->variantId = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); STAMP_LOC(a->variantId, @1); $$ = a; }
  | IDENTIFIER LPAREN match_bindings RPAREN
    { auto a = std::make_shared<MatchArmNode>(SCANNER_CODEGENCONTEXT); a->variantName = $1;
      a->variantId = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1); STAMP_LOC(a->variantId, @1);
      /* The bindings arrive as nodes (for the LSP index); mirror their names into the string list every
         existing reader uses, so nothing downstream changes. */
      a->bindings   = std::make_shared<StringList>();
      a->labels     = std::make_shared<StringList>();
      a->bindingIds = std::make_shared<IdentifierList>();
      a->labelIds   = std::make_shared<IdentifierList>();
      for (auto& arg : *$3) {
          a->labels->push_back(arg->name->value);
          a->labelIds->push_back(arg->name);
          auto bid = std::static_pointer_cast<IdentifierNode>(arg->expression);
          a->bindings->push_back(bid->value);
          a->bindingIds->push_back(bid);
      }
      $$ = a; }
  ;
/* A payload pattern NAMES its fields, exactly as a call names its parameters:
   `case Rect(w: width, h: height)`. There is no positional form. Binding by position is how a
   two-field swap compiles clean and silently returns the wrong values — the bug class named arguments
   exist to remove, and kama does not exempt arity-1 from a label at a call site either. The label is
   the FIELD; the identifier after it is the local it binds. Reuses ArgumentNode because a pattern
   binding is the same shape as an argument: a name, a colon, a thing. */
match_bindings
  : match_binding   { $$ = std::make_shared<ArgumentList>(); $$->push_back($1); }
  | match_bindings COMMA match_binding   { $1->push_back($3); $$ = $1; }
  ;
match_binding
  : IDENTIFIER COLON IDENTIFIER
    { auto b = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3); STAMP_LOC(b, @3);
      $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT,
               std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), b);
      STAMP_LOC($$->name, @1); }
  ;
foreach_statement
  : FOREACH LPAREN type IDENTIFIER IN expression RPAREN embedded_statement   { requireBraced($8, &@8, scanner, "foreach", "foreach (T e in ...) { ... }"); auto n = std::make_shared<ForEachNode>(SCANNER_CODEGENCONTEXT,  $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $8); STAMP_LOC(n->name, @4); $$ = n; }
  | FOREACH LPAREN REF type IDENTIFIER IN expression RPAREN embedded_statement   { requireBraced($9, &@9, scanner, "foreach", "foreach (ref T e in ...) { ... }"); auto n = std::make_shared<ForEachNode>(SCANNER_CODEGENCONTEXT,  $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $9); n->isRef = true; STAMP_LOC(n->name, @5); $$ = n; }   /* `foreach (ref T e in …)` — mutate elements in place */
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
  : primary_expression DOT IDENTIFIER   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); STAMP_LOC(ma->identifier, @3); $$ = ma; }
  | qualified_identifier_no_generic DOT IDENTIFIER   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), std::static_pointer_cast<ExpressionNode>($1)); STAMP_LOC(ma->identifier, @3); $$ = ma; }
  | class_type DOT IDENTIFIER   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); STAMP_LOC(ma->identifier, @3); $$ = ma; }
    /* `T.default()` — call the ctor the type ELECTED with `default ctor …()` (M8b), without having to
       know the name it chose (`empty`/`zero`/…). `default` is a keyword, so it cannot arrive as the
       IDENTIFIER the rules above expect; this is the only extra production it needs. It matters most in
       GENERIC code: `fn f<A: default>()` states the bound and, until now, had no way to use it — the
       election was reachable only by the compiler's own field-fill loop. */
  | qualified_identifier_no_generic DOT DEFAULT   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), std::static_pointer_cast<ExpressionNode>($1)); STAMP_LOC(ma->identifier, @3); $$ = ma; }
    /* `this.base = Base.<ctor>(…)` — a derived constructor installs its base part by building it with the
       base's OWN constructor. `base` is a keyword for the same reason `default` is, so it needs the same
       two extra productions. The SECOND one is not decoration: it makes `d.base` reduce, so the emitter
       can answer a bad receiver with a sentence instead of `syntax error, unexpected BASE`.
       Note the receiver is `primary_expression`, NOT `this_access` — the parser must reduce
       `this_access -> primary_expression` on lookahead DOT before it can ever see BASE. */
  | primary_expression DOT BASE   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $1); STAMP_LOC(ma->identifier, @3); $$ = ma; }
  | qualified_identifier_no_generic DOT BASE   { auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), std::static_pointer_cast<ExpressionNode>($1)); STAMP_LOC(ma->identifier, @3); $$ = ma; }
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
        STAMP_LOC(id, @3);      /* the method NAME only — not the turbofish */
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
        STAMP_LOC(method, @3);
        auto ma = std::make_shared<MemberAccessNode>(SCANNER_CODEGENCONTEXT, method, std::static_pointer_cast<ExpressionNode>($1));
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, ma, $5);
    }
    /* On-type turbofish through `::` — `Box::<int32>::tag()`, a STATIC on a generic type. The sibling of
       the ctor form above, and the one that makes the spelling rule uniform: after a type, `.` is
       construction and `::` is scope resolution, generic or not. The turbofish is MANDATORY here (unlike
       for a ctor, which can infer from its arguments): a static has no receiver and its parameters need
       not mention `T`, so there is nothing to infer the monomorph from. Builds the SAME shape the plain
       `Type::name(...)` resolver already consumes — a qualified IdentifierNode with a null `expression` —
       with the type args parked in `qualifierGenericArgs`, since `qualifier` is a bare StringList. */
  | generic_turbofish_name COLONCOLON IDENTIFIER LPAREN argument_list_opt RPAREN {
        auto q = std::make_shared<StringList>();
        q->push_back($1->value);
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3, q);
        id->qualifierGenericArgs = $1->genericArgs;
        STAMP_LOC(id, @3);      /* the method NAME only — not the turbofish */
        $$ = std::make_shared<InvocationNode>(SCANNER_CODEGENCONTEXT, id, $5);
    }
  ;
/* Shared `IDENTIFIER::<type_args>` prefix — a type name (or generic free-fn name) carrying explicit type
   args in turbofish form. Factored out so the genericDepth mid-rule actions live in exactly one place. */
generic_turbofish_name
  : IDENTIFIER COLONCOLON LT { yyget_extra(scanner)->genericDepth++; } type_arg_list GT { yyget_extra(scanner)->genericDepth--; } {
        auto id = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<StringList>(), (*$5)[0]);
        id->genericArgs = $5;
        STAMP_LOC(id, @1);      /* the NAME only — shared by every turbofish call form */
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
  : IDENTIFIER COLON expression   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), $3); STAMP_LOC($$->name, @1); }
  | IDENTIFIER COLON REF variable_reference   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $3), $4); STAMP_LOC($$->name, @1); }
  | IDENTIFIER COLON OUT variable_reference   { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), std::make_shared<ModifierNode>(SCANNER_CODEGENCONTEXT, $3), $4); STAMP_LOC($$->name, @1); }
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
  | EXCLAMATION IDENTIFIER         { auto flag = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $2); STAMP_LOC(flag, @2); $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, SharedIdentifier(), SharedModifier(), std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, flag)); }   /* negated flag, e.g. @compileFor(!RELEASE) */
  | IDENTIFIER COLON expression    { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedModifier(), $3); STAMP_LOC($$->name, @1); }
  | STRING_LITERAL                 { $$ = std::make_shared<ArgumentNode>(SCANNER_CODEGENCONTEXT, SharedIdentifier(), SharedModifier(), std::make_shared<StringNode>(SCANNER_CODEGENCONTEXT, $1)); }   /* bare string, e.g. @section(".isr_vector") */
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
  : BASE DOT IDENTIFIER   { auto ba = std::make_shared<BaseAccessNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3) ); STAMP_LOC(ba->identifier, @3); $$ = ba; }
  | BASE LEFT_BRACKET expression_list RIGHT_BRACKET   { $$ = std::make_shared<BaseAccessNode>(SCANNER_CODEGENCONTEXT, $3); }
  ;
new_expression
  : object_creation_expression
  ;
object_creation_expression
  : NEW type LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT,  $2, $4 ); }
    /* `try new T(...)` / `try new T.name(...)` (M-step5): the ONE non-panic construction entry — yields
       `Optional<Owned<T>>`, `None` on OOM instead of trapping. No placement variant (a follow-on). The named
       form is the norm under the M8 construction model (a named-ctor type rejects the bare `new`). */
  | TRY NEW type LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $3, $5); n->isTry = true; $$ = n; }
  | TRY NEW type DOT IDENTIFIER LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $3, $7); n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5); STAMP_LOC(n->ctorName, @5); n->isTry = true; $$ = n; }
  | NEW LPAREN argument_list RPAREN type LPAREN argument_list_opt RPAREN   { $$ = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT,  $5, $7, $3 ); }
  | NEW type DOT IDENTIFIER LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $2, $6); n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4); STAMP_LOC(n->ctorName, @4); $$ = n; }
  | NEW LPAREN argument_list RPAREN type DOT IDENTIFIER LPAREN argument_list_opt RPAREN   { auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $5, $9, $3); n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $7); STAMP_LOC(n->ctorName, @7); $$ = n; }
    /* On-type turbofish through `new`: `new BTreeNode::<K,V,A>.make(...)` — the uniform construction spelling
       (explicit type args always ride the type as `::<…>`). Reuses `generic_turbofish_name` (the type carries
       its args), mirroring the plain-call on-type turbofish in invocation_expression. */
  | NEW generic_turbofish_name DOT IDENTIFIER LPAREN argument_list_opt RPAREN {
        auto n = std::make_shared<ObjectCreationNode>(SCANNER_CODEGENCONTEXT, $2, $6);
        n->ctorName = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4);
        STAMP_LOC(n->ctorName, @4);
        $$ = n;
    }
  ;
unary_expression
  : postfix_expression
  | EXCLAMATION unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | TILDE unary_expression   { $$ = std::make_shared<SimpleUnaryExpressionNode>(SCANNER_CODEGENCONTEXT, $1, $2); }
  | cast_expression
  | bitcast_expression
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
bitcast_expression
    /* `bitcast<T>(expr)` — a same-width bit reinterpret, parsed exactly like `cast<T>(...)` (the mid-rules
       track genericDepth across the `<…>`); the equal-width + numeric-scalar checks are enforced at emit. */
  : BITCAST LT { yyget_extra(scanner)->genericDepth++; } type GT { yyget_extra(scanner)->genericDepth--; } LPAREN expression RPAREN   { $$ = std::make_shared<BitcastNode>(SCANNER_CODEGENCONTEXT,  $4, $8 ); }
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
        STAMP_LOC(h->whenParams->back(), @1);   /* the bound is an already-built type_name; it needs none */
        h->whenBounds->push_back($3); $$ = h; }
    /* `when [A: default]` — a STRUCTURAL bound: the arg bound to A must have a `default` ctor
       (checked via isDefaultFillable). `default` is a keyword, so it can't reduce as a type_name. */
  | IDENTIFIER COLON DEFAULT
      { auto h = std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, SharedString());
        h->whenParams = std::make_shared<IdentifierList>();
        h->whenBounds = std::make_shared<IdentifierList>();
        h->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1));
        STAMP_LOC(h->whenParams->back(), @1);
        h->whenBounds->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3));
        STAMP_LOC(h->whenBounds->back(), @3); $$ = h; }
  | when_cond_list COMMA IDENTIFIER COLON type_name
      { $1->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3));
        STAMP_LOC($1->whenParams->back(), @3);
        $1->whenBounds->push_back($5); $$ = $1; }
  | when_cond_list COMMA IDENTIFIER COLON DEFAULT
      { $1->whenParams->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3));
        STAMP_LOC($1->whenParams->back(), @3);
        $1->whenBounds->push_back(std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5));
        STAMP_LOC($1->whenBounds->back(), @5); $$ = $1; }
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
  /* Error recovery, member grain (LSP M5.3) — one bad member must not hide its siblings, and the TYPE
   * stays declared either way, which is what keeps a half-typed member from cascading "undeclared type"
   * over every use of it. Same shape and same reasoning as the statement_list arms above. */
  | error   { $$ = std::make_shared<ClassMemberDeclarationList>(); }
  | class_member_declarations error   { $$ = $1; }
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
  | modifiers_opt COMPTIME type constant_declarators SEMICOLON   { auto k = std::make_shared<ClassConstDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, $4); k->isComptime = true; $$ = k; }   /* `comptime T NAME` — a type-associated compile-time constant, read `Type::NAME` (6b-2) */
  ;
field_declaration
  : modifiers_opt type variable_declarators SEMICOLON   { $$ = std::make_shared<ClassFieldDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $2, $3); }
  | attribute_list modifiers_opt type variable_declarators SEMICOLON   { auto f = std::make_shared<ClassFieldDeclarationNode>(SCANNER_CODEGENCONTEXT, $2, $3, $4); f->attributes = $1; $$ = f; }   /* `@field … type name;` */
  ;
method_declaration
  : modifiers_opt COMPTIME FN type method_name LPAREN parameter_list_opt RPAREN block   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $9); STAMP_LOC(m->name, @5); m->isComptime = true; $$ = m; }   /* `comptime fn T name(…)` — a type-associated compile-time-only function (6b-3), read `Type::name()` */
  | modifiers_opt const_opt FN type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $4, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $10); STAMP_LOC(m->name, @5); m->isConst = ($2 != nullptr); if ($9) { m->whenParams = $9->whenParams; m->whenBounds = $9->whenBounds; } $$ = m; }
  | modifiers_opt const_opt FN VOID method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4, IDENTIFIER_VOID_VAL), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $5), $7, $10); STAMP_LOC(m->name, @5); m->isConst = ($2 != nullptr); if ($9) { m->whenParams = $9->whenParams; m->whenBounds = $9->whenBounds; } $$ = m; }
  | modifiers_opt const_opt FN REF type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body   { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT,  $1, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), $8, $11); STAMP_LOC(m->name, @6); m->isConst = ($2 != nullptr); m->isRef = true; if ($10) { m->whenParams = $10->whenParams; m->whenBounds = $10->whenBounds; } $$ = m; }   /* `fn ref T at(…)` — a place-returning method */
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
/* `hardware T` — the MCU/MMIO qualifier (emits C `volatile`). A dedicated slot (not a general
   modifier) so it appears only where it is meaningful: a module `static` and a `Ptr<T>` parameter. */
hardware_opt
  : /* Nothing */   { $$ = SharedString(); }
  | HARDWARE        { $$ = $1; }
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
  : REF type OPERATOR LEFT_BRACKET RIGHT_BRACKET LPAREN type IDENTIFIER RPAREN   { auto d = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $2, LEFT_BRACKET, $7, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $8), SharedIdentifier(), SharedIdentifier()); d->refReturn = true; STAMP_LOC(d->param1Name, @8); $$ = d; }   /* `ref T operator[](usize i)` — a place-returning index operator */
  | type OPERATOR overloadable_operator LPAREN RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, SharedIdentifier(), SharedIdentifier(), SharedIdentifier(), SharedIdentifier()); }   /* 0-param unary: `Vec2 operator-()` = `-this` */
  | type OPERATOR overloadable_operator LPAREN type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), SharedIdentifier(), SharedIdentifier()); STAMP_LOC($$->param1Name, @6); }
  | type OPERATOR overloadable_operator LPAREN type IDENTIFIER COMMA type IDENTIFIER RPAREN   { $$ = std::make_shared<ClassOperatorDeclaratorNode>(SCANNER_CODEGENCONTEXT, $1, $3, $5, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $6), $8, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $9) ); STAMP_LOC($$->param1Name, @6); STAMP_LOC($$->param2Name, @9); }
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
    { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, SharedIdentifier(), std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $5, $8); STAMP_LOC(m->name, @3); m->isCtor = true; if ($7) { m->whenParams = $7->whenParams; m->whenBounds = $7->whenBounds; } $$ = m; }
  | modifiers_opt CTOR type method_name LPAREN parameter_list_opt RPAREN method_when_opt method_body
    { auto m = std::make_shared<ClassMethodDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, $3, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $4), $6, $9); STAMP_LOC(m->name, @4); m->isCtor = true; if ($8) { m->whenParams = $8->whenParams; m->whenBounds = $8->whenBounds; } $$ = m; }
  ;
constructor_declarator
  : IDENTIFIER LPAREN parameter_list_opt RPAREN constructor_initializer_opt   { $$ = std::make_shared<ClassConstructorDeclaratorNode>(SCANNER_CODEGENCONTEXT, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3, $5); STAMP_LOC($$->constructorName, @1); }
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
    /* `$$` is the BASE handle here (%type <classmember>), so the name is unreachable through it — hence a
       named local. Worst span on the checklist: @$ runs from the modifiers through the entire body. */
  : modifiers_opt TILDE IDENTIFIER LPAREN RPAREN block   { auto d = std::make_shared<ClassDestructorDeclarationNode>(SCANNER_CODEGENCONTEXT, $1, std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $3), $6); STAMP_LOC(d->destructorName, @3); $$ = d; }
  ;

/*------------------------------------------------------------------------------ 
                              Enum 
------------------------------------------------------------------------------*/

/* `type enum Name<T> : IntType implements C { A, B(payload…); …methods… }`. The head reuses
   `type_decl_head` (so generic enums parse exactly like generic types); an optional `: IntType` pins the
   underlying integer / tag width; members may carry a named payload (below) making the enum a tagged
   union. Like every other kind an enum takes `class_base_opt`, so it declares its conformance inline —
   and the methods satisfying that contract follow the variants after a `;`.

   The `type` marker is MANDATORY — GOALS #3c says every declaration is `type <kind> Name`, and an enum
   is a kind like any other. `enum` is a RESERVED token (unlike the contextual kind words
   `value`/`resource`/`view`/`contract`), so `TYPE ENUM …` is its own production rather than another arm
   of `marked_type_declaration`; that is also what keeps the two apart with no conflict. */
enum_declaration
  : TYPE modifiers_opt ENUM type_decl_head enum_underlying_opt class_base_opt enum_class_body semicolon_opt
    {      $$ = makeEnumDeclaration(SCANNER_CODEGENCONTEXT, SharedAttributeList(), $2, $4, $5, $6, $7); }
  | attribute_list TYPE modifiers_opt ENUM type_decl_head enum_underlying_opt class_base_opt enum_class_body semicolon_opt
    {      $$ = makeEnumDeclaration(SCANNER_CODEGENCONTEXT, $1, $3, $5, $6, $7, $8); }   /* `@generate(...) type enum …` */
  ;
enum_underlying_opt
  : /* Nothing */        { $$ = SharedIdentifier(); }
  | COLON integral_type  { $$ = $2; }
  ;
/* Four explicit alternatives rather than an `_opt` tail, for two reasons, both LALR(1):
   - the `;` separator is MANDATORY before class members. Without it `{ Foo bar; }` is ambiguous at one
     token of lookahead — `Foo` reduces as a variant, or begins a `Foo bar;` field. With it, class
     members are reachable only after SEMICOLON and no state admits both.
   - spelling the trailing comma out (rather than a `comma_opt` nonterminal) keeps the "is this comma a
     list separator or the body terminator?" decision inside one production, exactly as the original
     `enum_body` did. */
enum_class_body
  : LEFT_BRACE enum_member_declarations_opt RIGHT_BRACE
    { $$ = std::make_shared<EnumBody>(); $$->variants = $2; }
  | LEFT_BRACE enum_member_declarations COMMA RIGHT_BRACE
    { $$ = std::make_shared<EnumBody>(); $$->variants = $2; }
  | LEFT_BRACE enum_member_declarations SEMICOLON class_member_declarations_opt RIGHT_BRACE
    { $$ = std::make_shared<EnumBody>(); $$->variants = $2; $$->members = $4; }
  | LEFT_BRACE enum_member_declarations COMMA SEMICOLON class_member_declarations_opt RIGHT_BRACE
    { $$ = std::make_shared<EnumBody>(); $$->variants = $2; $$->members = $5; }
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
  | IDENTIFIER EQ constant_expression   { $$ = std::make_shared<EnumMemberDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), $3 ); STAMP_LOC($$->identifier, @1); }
  | IDENTIFIER LPAREN parameter_list RPAREN   { auto m = std::make_shared<EnumMemberDeclarationNode>(SCANNER_CODEGENCONTEXT,  std::make_shared<IdentifierNode>(SCANNER_CODEGENCONTEXT, $1), SharedExpression() ); m->payload = $3; STAMP_LOC(m->identifier, @1); $$ = m; }   /* tagged-union variant with a named payload */
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
        n->constTypes   = std::make_shared<IdentifierList>();
        n->typeDefaults = std::make_shared<IdentifierList>();
        n->typePins     = std::make_shared<IdentifierList>();
        for (auto& a : *head->genericArgs) if (a && a->value) {
            n->typeParams->push_back(a->value);
            n->typeBounds->push_back(a->bounds ? a->bounds : std::make_shared<IdentifierList>());
            n->typeDefaults->push_back(a->defaultArg);   // null when this param has no `= Default`
            n->typePins->push_back(a->pin);              // null when this param has no `is <T>`
            n->constTypes->push_back(a->isConstParam ? a->constType : SharedIdentifier());
            if (a->isConstParam) n->constParams->push_back(a->value);
        }
        head->genericArgs = SharedIdentifierList();
        head->genericArg  = SharedIdentifier();
    }
    return n;
}

// Build an `EnumDeclarationNode` from the shared parts of every enum spelling (attributed or not,
// `type`-marked or not). Captures generic params/bounds off the head and strips them so the enum NAME
// stays bare (`Optional`/`Result`) — the same contract `makeTypeDeclaration` honors, and previously
// copy-pasted into each enum alternative.
SharedStatement makeEnumDeclaration(CodeGenContext& context, SharedAttributeList attributes,
    SharedModifierList modifiers, SharedIdentifier head, SharedIdentifier underlying,
    SharedClassBaseDeclaration base, SharedEnumBody body)
{
    auto n = std::make_shared<EnumDeclarationNode>(context, modifiers, head,
                 body ? body->variants : SharedEnumMemberDeclarationList());
    n->underlyingType = underlying;
    n->attributes     = attributes;
    n->baseTypes      = base;
    n->members        = body ? body->members : SharedClassMemberDeclarationList();
    if (head->genericArgs && !head->genericArgs->empty()) {
        n->typeParams   = std::make_shared<StringList>();
        n->typeBounds   = std::make_shared<BoundsList>();
        n->constParams  = std::make_shared<StringList>();
        n->constTypes   = std::make_shared<IdentifierList>();
        n->typeDefaults = std::make_shared<IdentifierList>();
        n->typePins     = std::make_shared<IdentifierList>();
        for (auto& a : *head->genericArgs) if (a && a->value) {
            n->typeParams->push_back(a->value);
            n->typeBounds->push_back(a->bounds ? a->bounds : std::make_shared<IdentifierList>());
            n->typeDefaults->push_back(a->defaultArg);
            n->typePins->push_back(a->pin);   // `<T is This>` — only a contract has an implementer
            n->constTypes->push_back(a->isConstParam ? a->constType : SharedIdentifier());
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

int yyerror(YYLTYPE* llocp, yyscan_t scanner, const char *msg)
{
    LexerInstanceData* data = yyget_extra(scanner);
    // The 10-error budget in CodeGenContext existed from the beginning and was DEAD CODE — nothing
    // could ever reach it, because without recovery the parse stopped at error one. M5.3's recovery
    // arms make it reachable, and reachable it must be: a buffer mid-refactor can otherwise emit an
    // error per line, per keystroke, straight into publishDiagnostics. Past the budget the errors are
    // still COUNTED (so every caller's `errorCount() > 0` gate still fails the parse) but no longer
    // reported — the first 10 are the ones a human reads anyway.
    if (data->codeGenContext->isErrorLimitReached()) {
        data->codeGenContext->countErrorOnly();
        return 1;
    }
    // %locations gives the error's precise start (the offending token), better than the lexer counter.
    int line = llocp ? llocp->first_line : data->codeGenContext->line;
    int col  = llocp ? llocp->first_column : data->codeGenContext->col;
    return data->codeGenContext->handleError(line, col, "Parse", msg);
}

