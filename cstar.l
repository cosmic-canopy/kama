%{
#include <string>
#include <memory>
#include "cstar.parser.hpp"

#define SAVE_TOKEN_FROM_BUFFER(l)  yyextra->next_column_number += l; \
                                   yylval->string = yyextra->workString; \
                                   yyextra->workString = NULL;

#define SAVE_TOKEN(src, leng, l)  yyextra->codeGenContext->line = yyextra->next_line_number; \
                                  yyextra->codeGenContext->col = yyextra->next_column_number; \
                                  yyextra->next_column_number += l; \
                                  yylval->string = std::make_shared<std::string>(src, leng);

#define RETURN_TOKEN(t, l)   yyextra->codeGenContext->line = yyextra->next_line_number; \
                             yyextra->codeGenContext->col = yyextra->next_column_number; \
                             yyextra->next_column_number += l; \
                             return yylval->token = t;

#define YY_EXTRA_TYPE struct LexerInstanceData *

static int isValidChar(const char );
static int getToken(const char *);
char getSimpleEscape(const char* input, int index);
std::string hexToUTF8(int cp);

void error(const char *, yyscan_t scanner);
%}

%option extra-type="struct LexerInstanceData *"
%option 8bit reentrant bison-bridge
%option never-interactive
%option warn nodefault
%option outfile="cstar.lexer.cpp"
%option header-file="cstar.lexer.hpp"
%option stack
%option noyywrap nounistd nounput noinput noyy_top_state


new_line                    \n
white_space_1               " "
white_space_2               [\t\r]

reserved_preprocessor       ^[ \t]*#.*

single_line_comment         "//".*
multi_line_comment_start    "/*"
multi_line_comment_end      "*/"

dec_digit                   [0-9]
hex_digit                   [0-9A-Fa-f]
oct_digit                   [0-7]
based_digit                 [0-9A-Za-z]
int_suffix                  u?i(8|16|32|64)
dec_literal_no_suffix       {dec_digit}+
hex_literal_no_suffix       0[xX]{hex_digit}+
oct_literal_no_suffix       0[oO]{oct_digit}+
based_literal_no_suffix     0[bB]{based_digit}+_([1-2][0-9]|[3][0-2]|[2-9])
dec_literal                 {dec_literal_no_suffix}{int_suffix}
hex_literal                 {hex_literal_no_suffix}{int_suffix}
oct_literal                 {oct_literal_no_suffix}{int_suffix}
based_literal               {based_literal_no_suffix}{int_suffix}

exponent                    [eE][+\-]?{dec_digit}+
float_exp                   {dec_digit}\.{dec_digit}+{exponent}
float_normal                {dec_digit}+\.{dec_digit}+
float_literal_no_suffix     ({float_exp}|{float_normal})
float_literal32             {float_literal_no_suffix}f(32)
float_literal64             {float_literal_no_suffix}f(64)

single_char                 [^\\\']
simple_esc_seq              \\[\'\"\\0abfnrtv]
simple_esc_seq_literal      \'{simple_esc_seq}\'
single_char_literal         \'{single_char}\'

  /* Does not make sense for single char as we use UTF-8.  Range: U+0000 - U+10FFFF */
uni_codepoint_esc_seq       \\u\{{hex_digit}{4,6}\} 
single_string_char          [^\\\"]
regular_string_start        \"
regular_string_end          \"

single_verbatim_char        [^\"]
quote_esc_seq               \"\"
verbatim_string_start       @\"
verbatim_string_end         \"

letter_char                 [A-Za-z]
ident_char                  {dec_digit}|{letter_char}|"_"
identifier                  ({letter_char}|"_"){ident_char}*

%x IN_COMMENT IN_VERBATIM_STRING IN_REGULAR_STRING

%%

  /* Reserved for preprocessor use. For now we will assume #region and #endregion as in C# to allow IDE folding. */
{reserved_preprocessor}                        { /* ignore */ }
{white_space_1}                                { ++(yyextra->next_column_number); }
{white_space_2}                                { yyextra->next_column_number += 2; }
{new_line}                                     { ++(yyextra->next_line_number); yyextra->next_column_number = CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE; }

{single_line_comment}                          { ; /* ignore */ }

{multi_line_comment_start}                     { yy_push_state(IN_COMMENT, yyscanner); yyextra->next_column_number += 2; }
<IN_COMMENT>.                                  { ++(yyextra->next_column_number); }
<IN_COMMENT>{new_line}                         { ++(yyextra->next_line_number); yyextra->next_column_number = CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE; }
<IN_COMMENT>{multi_line_comment_end}           { yy_pop_state(yyscanner); yyextra->next_column_number += 2; }
<IN_COMMENT><<EOF>>                            { 
                                                 error("unexpected end of file lexing multi-line comment", yyscanner); 
                                               }

{verbatim_string_start}                        { 
                                                 yy_push_state(IN_VERBATIM_STRING, yyscanner); 
                                                 yyextra->codeGenContext->col = yyextra->next_column_number;
                                                 yyextra->next_column_number += 2; 
                                                 yyextra->workString = std::make_shared<std::string>();
                                               }
<IN_VERBATIM_STRING>{new_line}                 { 
                                                 ++(yyextra->next_line_number); 
                                                 yyextra->next_column_number = CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE; 
                                                 yyextra->workString->append("\n");
                                               }
<IN_VERBATIM_STRING>{single_verbatim_char}     { 
                                                 ++(yyextra->next_column_number);
                                                 yyextra->workString->append(yytext);
                                               }
<IN_VERBATIM_STRING>{quote_esc_seq}            {
                                                 yyextra->next_column_number += 2;
                                                 yyextra->workString->append("\"");
                                               }
<IN_VERBATIM_STRING>{verbatim_string_end}      { 
                                                 yy_pop_state(yyscanner);
                                                 SAVE_TOKEN_FROM_BUFFER(1);
                                                 return STRING_LITERAL; 
                                               }
<IN_VERBATIM_STRING><<EOF>>                    { 
                                                  error("unexpected end of file lexing verbatim string", yyscanner); 
                                               }

{dec_literal_no_suffix}                        { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return DEC_LITERAL_NO_SUFFIX; }
{hex_literal_no_suffix}                        { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return HEX_LITERAL_NO_SUFFIX; }
{oct_literal_no_suffix}                        { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return OCT_LITERAL_NO_SUFFIX; }
{based_literal_no_suffix}                      { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return BASED_LITERAL_NO_SUFFIX; }
{dec_literal}                                  { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return DEC_LITERAL; }
{hex_literal}                                  { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return HEX_LITERAL; }
{oct_literal}                                  { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return OCT_LITERAL; }
{based_literal}                                { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return BASED_LITERAL; }

{float_literal_no_suffix}                      { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return FLOAT_LITERAL_NO_SUFFIX; }
{float_literal32}                              { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return FLOAT_LITERAL_32; }
{float_literal64}                              { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return FLOAT_LITERAL_64; }

{simple_esc_seq_literal}                       { char c = getSimpleEscape(yytext, 2); SAVE_TOKEN(&c, 1, yyleng); return CHARACTER_LITERAL; }
{single_char_literal}                          { char c = yytext[1]; SAVE_TOKEN(&c, 1, yyleng); return CHARACTER_LITERAL; }

{regular_string_start}                         {
                                                 yy_push_state(IN_REGULAR_STRING, yyscanner); 
                                                 yyextra->codeGenContext->col = yyextra->next_column_number;
                                                 yyextra->next_column_number += 1; 
                                                 yyextra->workString = std::make_shared<std::string>();
                                               }
<IN_REGULAR_STRING>{single_string_char}        {
                                                 ++(yyextra->next_column_number);
                                                 yyextra->workString->append(yytext);
                                               }
<IN_REGULAR_STRING>{simple_esc_seq}            {
                                                 yyextra->next_column_number += 2;
                                                 char c = getSimpleEscape(yytext, 1);
                                                 yyextra->workString->append(&c, 1);
                                               }
<IN_REGULAR_STRING>{uni_codepoint_esc_seq}     {
                                                 yyextra->next_column_number += yyleng;
                                                 std::string hexString(&(yytext[3]));
                                                 hexString.pop_back();

                                                 int hexVal = strtol( hexString.c_str(), NULL, 16);
                                                 yyextra->workString->append(hexToUTF8(hexVal));
                                               }
<IN_REGULAR_STRING>{regular_string_end}        {
                                                 yy_pop_state(yyscanner);
                                                 SAVE_TOKEN_FROM_BUFFER(1);
                                                 return STRING_LITERAL; 
                                               }
<IN_REGULAR_STRING>.                           { error("unexpected character lexing string", yyscanner); }
<IN_REGULAR_STRING><<EOF>>                     {
                                                  error("unexpected end of file lexing string", yyscanner);
                                               }

{identifier}                                   { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return getToken(yytext); }

","     { RETURN_TOKEN(COMMA, 1); }
"["     { RETURN_TOKEN(LEFT_BRACKET, 1); }
"]"     { RETURN_TOKEN(RIGHT_BRACKET, 1); }

"++"    { RETURN_TOKEN(PLUSPLUS, 2); }
"--"    { RETURN_TOKEN(MINUSMINUS, 2); }

"&&"    { RETURN_TOKEN(ANDAND, 2); }
"||"    { RETURN_TOKEN(OROR, 2); }

"=="    { RETURN_TOKEN(EQEQ, 2); }
"!="    { RETURN_TOKEN(NOTEQ, 2); }
"<="    { RETURN_TOKEN(LEQ, 2); }
">="    { RETURN_TOKEN(GEQ, 2); }

"*="    { RETURN_TOKEN(STAREQ, 2); }
"/="    { RETURN_TOKEN(DIVEQ, 2); }
"+="    { RETURN_TOKEN(PLUSEQ, 2); }
"-="    { RETURN_TOKEN(MINUSEQ, 2); }
"%="    { RETURN_TOKEN(MODEQ, 2); }

"<<"    { RETURN_TOKEN(LTLT, 2); }
">>"    { RETURN_TOKEN(GTGT, 2); }
">>="   { RETURN_TOKEN(GTGTEQ, 3); }
"<<="   { RETURN_TOKEN(LTLTEQ, 3); }
"&="    { RETURN_TOKEN(ANDEQ, 2); }
"|="    { RETURN_TOKEN(OREQ, 2); }
"^="    { RETURN_TOKEN(XOREQ, 2); }

"{"     { RETURN_TOKEN(LEFT_BRACE, 1); }
"}"     { RETURN_TOKEN(RIGHT_BRACE, 1); }
"!"     { RETURN_TOKEN(EXCLAMATION, 1); }
"%"     { RETURN_TOKEN(PERCENT, 1); }
"&"     { RETURN_TOKEN(AMP, 1); }
"("     { RETURN_TOKEN(LPAREN, 1); }
")"     { RETURN_TOKEN(RPAREN, 1); }
"*"     { RETURN_TOKEN(STAR, 1); }
"+"     { RETURN_TOKEN(PLUS, 1); }
"-"     { RETURN_TOKEN(MINUS, 1); }
"."     { RETURN_TOKEN(DOT, 1); }
"/"     { RETURN_TOKEN(SLASH, 1); }
":"     { RETURN_TOKEN(COLON, 1); }
";"     { RETURN_TOKEN(SEMICOLON, 1); }
"<"     { RETURN_TOKEN(LT, 1); }
"="     { RETURN_TOKEN(EQ, 1); }
">"     { RETURN_TOKEN(GT, 1); }
"?"     { RETURN_TOKEN(QUESTION, 1); }
"|"     { RETURN_TOKEN(BAR, 1); }
"~"     { RETURN_TOKEN(TILDE, 1); }
"^"     { RETURN_TOKEN(CARET, 1); }

.       {
//          if (isValidChar(yytext[0])) 
//          {
//            return RETURN_TOKEN(*yytext, 1);
//          }
//          else 
//          {
              error("invalid token", yyscanner); 
//          }
        }
%%

struct name_value {
  const char *name;
  int value;
};
static struct name_value keywords [] = { 
 /* This list must remain sorted!!! */
    {"abstract",    ABSTRACT},
    {"base",        BASE},
    {"bool",        BOOL},
    {"break",       BREAK},
    {"case",        CASE},
    {"cast",        CAST},
    {"class",       CLASS},
    {"const",       CONST},
    {"continue",    CONTINUE},
    {"default",     DEFAULT},
    {"do",          DO},
    {"double",      DOUBLE},
    {"else",        ELSE},
    {"enum",        ENUM},
    {"export",      EXPORT},
    {"extends",     EXTENDS},
    {"extern",      EXTERN},
    {"false",       FALSE},
    {"final",       FINAL},
    {"float32",     FLOAT32},
    {"float64",     FLOAT64},
    {"for",         FOR},
    {"foreach",     FOREACH},
    {"friend",      FRIEND},
    {"if",          IF},
    {"implements",  IMPLEMENTS},
    {"in",          IN},
    {"int",         INT},
    {"int16",       INT16},
    {"int32",       INT32},
    {"int64",       INT64},
    {"int8",        INT8},
    {"interface",   INTERFACE},
    {"namespace",   NAMESPACE},
    {"new",         NEW},
    {"null",        NULL_LITERAL},
    {"operator",    OPERATOR},
    {"out",         OUT},
    {"override",    OVERRIDE},
    {"private",     PRIVATE},
    {"protected",   PROTECTED},
    {"public",      PUBLIC},
    {"ref",         REF},
    {"return",      RETURN},
    {"static",      STATIC},
    {"string",      STRING},
    {"switch",      SWITCH},
    {"this",        THIS},
    {"true",        TRUE},
    {"uint16",      UINT16},
    {"uint32",      UINT32},
    {"uint64",      UINT64},
    {"uint8",       UINT8},
    {"using",       USING},
    {"virtual",     VIRTUAL},
    {"void",        VOID},
    {"volatile",    VOLATILE},
    {"while",       WHILE},
  };


/* Conduct a binary search for lexeme in the keywords array
 * between indices start (inclusive) and finish (exclusive)
 */
static int bin_search(const char *lexeme, int start, int finish)
{
  if (start >= finish)  /* Not found */
      return IDENTIFIER;
  else {
    int mid = (start+finish)/2;
    int cmp = strcmp(lexeme,keywords[mid].name);
    if (cmp == 0) 
      return keywords[mid].value;
    else if (cmp < 0)
      return bin_search(lexeme,start,mid);
    else
      return bin_search(lexeme,mid+1,finish);
  }
}

char getSimpleEscape(const char* input, int index)
{
  char rtn = 0;
  switch(input[index])
  {
    case '\'':
      rtn = '\'';
    break;
    case '"':
      rtn = '"';
    break;
    case '\\':
      rtn = '\\';
    break;
    case 0:
      rtn = 0;
    break;
    case 'a':
      rtn = '\a';
    break;
    case 'b':
      rtn = '\b';
    break;
    case 'f':
      rtn = '\f';
    break;
    case 'n':
      rtn = '\n';
    break;
    case 'r':
      rtn = '\r';
    break;
    case 't':
      rtn = '\t';
    break;
    case 'v':
      rtn = '\v';
    break;
  }

  return rtn;
}

static int getToken(const char *lexeme)
{

  static int num_keywords = sizeof(keywords) / sizeof(name_value);
  int token = bin_search(lexeme,0,num_keywords);
  if (token == -1)
    token = IDENTIFIER;
  return token;
}

static int isValidChar(const char c)
{
  static char table [] = {
    /* low part: 33-63 decimal */
    '!', 0, 0, 0, '%', '&', 0, '(', ')', '*',
    '+', 0, '-', '.', '/', 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, ':', ';', '<', '=', '>',
    '?',

    /* high part: 123-126 decimal */
    /* '{', '|', '}', '~', */

    /* special: 94 decimal */
    /* '^', */
  };

  int index = ((int)c) - 33;

  if (index >= 0) 
  {
    if (index <= 30)
    {
      return table[index];
    }
    else if((index >= (123-33) && (index <= (126-33))) || (index == (94-33)))
    {
      return 1;
    }
  }
  return 0;
}

std::string hexToUTF8(int cp)
{
    char c[5]={ 0x00,0x00,0x00,0x00,0x00 };
    if     (cp<=0x7F) { c[0] = cp;  }
    else if(cp<=0x7FF) { c[0] = (cp>>6)+192; c[1] = (cp&63)+128; }
    else if(0xd800<=cp && cp<=0xdfff) {} //invalid block of utf8
    else if(cp<=0xFFFF) { c[0] = (cp>>12)+224; c[1]= ((cp>>6)&63)+128; c[2]=(cp&63)+128; }
    else if(cp<=0x10FFFF) { c[0] = (cp>>18)+240; c[1] = ((cp>>12)&63)+128; c[2] = ((cp>>6)&63)+128; c[3]=(cp&63)+128; }
    return std::string(c);
}

void error(const char *msg, yyscan_t scanner)
{
    LexerInstanceData* data = yyget_extra(scanner);
    std::string msgString(msg);
    std::string tokenString(" [");
    tokenString.append(yyget_text( scanner ));
    tokenString.append("]");
    data->codeGenContext->handleError(data->codeGenContext->line, data->codeGenContext->col, "Lexical", msgString.append( tokenString ) );
}
