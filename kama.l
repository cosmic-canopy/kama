%{
#include <string>
#include <memory>
#include "kama.parser.hpp"

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

static int getToken(const char *);
char getSimpleEscape(const char* input, int index);
std::string hexToUTF8(int cp);
unsigned decodeUTF8Codepoint(const char* bytes, int len);

void error(const char *, yyscan_t scanner);
%}

%option extra-type="struct LexerInstanceData *"
%option 8bit reentrant bison-bridge
%option never-interactive
%option warn nodefault
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
simple_esc_seq              \\[\'\"\\0abfnrtv$]
simple_esc_seq_literal      \'{simple_esc_seq}\'
single_char_literal         \'{single_char}\'

  /* Range: U+0000 - U+10FFFF (1-6 hex digits). Used in string literals and, as `'\u{…}'`, as a `char`. */
uni_codepoint_esc_seq       \\u\{{hex_digit}{1,6}\}
uni_codepoint_char_literal  \'{uni_codepoint_esc_seq}\'

  /* A single multibyte UTF-8 codepoint written directly as a `char` literal (`'é'`, `'😀'`): 2–4 bytes =
     one scalar value. Overlong / surrogate / out-of-range forms are decoded then rejected in the action. */
utf8_cont                   [\x80-\xBF]
utf8_multibyte              [\xC2-\xDF]{utf8_cont}|[\xE0-\xEF]{utf8_cont}{2}|[\xF0-\xF4]{utf8_cont}{3}
multibyte_char_literal      \'{utf8_multibyte}\'
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

%x IN_COMMENT IN_VERBATIM_STRING IN_REGULAR_STRING IN_INTERP_HOLE IN_INTERP_SPEC

%%

  /* Reserved for preprocessor use. For now we will assume #region and #endregion as in C# to allow IDE folding. */
{reserved_preprocessor}                        { /* ignore */ }
{white_space_1}                                { ++(yyextra->next_column_number); }
{white_space_2}                                { yyextra->next_column_number += 2; }
{new_line}                                     { ++(yyextra->next_line_number); yyextra->next_column_number = KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE; }

{single_line_comment}                          { ; /* ignore */ }

{multi_line_comment_start}                     { yy_push_state(IN_COMMENT, yyscanner); yyextra->next_column_number += 2; }
<IN_COMMENT>.                                  { ++(yyextra->next_column_number); }
<IN_COMMENT>{new_line}                         { ++(yyextra->next_line_number); yyextra->next_column_number = KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE; }
<IN_COMMENT>{multi_line_comment_end}           { yy_pop_state(yyscanner); yyextra->next_column_number += 2; }
<IN_COMMENT><<EOF>>                            { 
                                                 error("unexpected end of file lexing multi-line comment", yyscanner);
                                                 yyterminate();   /* stop — else flex re-matches <<EOF>> at EOF forever */
                                               }

{verbatim_string_start}                        { 
                                                 yy_push_state(IN_VERBATIM_STRING, yyscanner); 
                                                 yyextra->codeGenContext->col = yyextra->next_column_number;
                                                 yyextra->next_column_number += 2; 
                                                 yyextra->workString = std::make_shared<std::string>();
                                               }
<IN_VERBATIM_STRING>{new_line}                 { 
                                                 ++(yyextra->next_line_number); 
                                                 yyextra->next_column_number = KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE; 
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
                                                  yyterminate();   /* stop — else flex re-matches <<EOF>> at EOF forever */
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

  /* A `char` literal carries its Unicode scalar value (codepoint) as a decimal string; the parser
     builds a CharNode from it. ASCII `'a'` and simple escapes are one byte = one codepoint; `'\u{…}'`
     is the hex codepoint; a multibyte source char (`'é'`, `'😀'`) is decoded from its UTF-8 bytes.
     `char` is exactly one scalar value — a multi-codepoint grapheme is a string, not a char. */
{simple_esc_seq_literal}                       { unsigned cp = (unsigned char)getSimpleEscape(yytext, 2); std::string s = std::to_string(cp); SAVE_TOKEN(s.c_str(), s.size(), yyleng); return CHARACTER_LITERAL; }
{single_char_literal}                          { unsigned cp = (unsigned char)yytext[1]; std::string s = std::to_string(cp); SAVE_TOKEN(s.c_str(), s.size(), yyleng); return CHARACTER_LITERAL; }
{uni_codepoint_char_literal}                   { std::string hexString(&(yytext[4])); hexString.pop_back(); hexString.pop_back(); unsigned cp = (unsigned)strtol(hexString.c_str(), NULL, 16); std::string s = std::to_string(cp); SAVE_TOKEN(s.c_str(), s.size(), yyleng); return CHARACTER_LITERAL; }
{multibyte_char_literal}                       { unsigned cp = decodeUTF8Codepoint(&(yytext[1]), yyleng - 2); if (cp == 0xFFFFFFFFu) { error("invalid multibyte char literal (overlong, surrogate, or out of range)", yyscanner); } else { std::string s = std::to_string(cp); SAVE_TOKEN(s.c_str(), s.size(), yyleng); return CHARACTER_LITERAL; } }

{regular_string_start}                         {
                                                 yy_push_state(IN_REGULAR_STRING, yyscanner);
                                                 yyextra->codeGenContext->col = yyextra->next_column_number;
                                                 yyextra->next_column_number += 1;
                                                 yyextra->workString = std::make_shared<std::string>();
                                                 yyextra->strInterp = false;   // set true on the first interpolation hole
                                               }
  /* Interpolation hole opener: emit the literal chunk accumulated so far as an ISTR_CHUNK, then lex the hole
     in IN_INTERP_HOLE until the closer. Longest-match beats single_string_char. A lone dollar (not followed
     by a brace) and an escaped dollar stay literal via single_string_char / simple_esc_seq. */
<IN_REGULAR_STRING>"${"                         {
                                                 yyextra->strInterp = true;
                                                 yy_push_state(IN_INTERP_HOLE, yyscanner);
                                                 SAVE_TOKEN_FROM_BUFFER(2);
                                                 return ISTR_CHUNK;
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
                                                 bool interp = yyextra->strInterp;
                                                 yy_pop_state(yyscanner);
                                                 SAVE_TOKEN_FROM_BUFFER(1);
                                                 // An interpolated string's closing `"` emits the final (tail) chunk;
                                                 // a plain string emits STRING_LITERAL as before.
                                                 return interp ? ISTR_CHUNK : STRING_LITERAL;
                                               }
<IN_REGULAR_STRING>.                           { error("unexpected character lexing string", yyscanner); }
<IN_REGULAR_STRING><<EOF>>                     {
                                                  error("unexpected end of file lexing string", yyscanner);
                                                  yyterminate();   /* stop — else flex re-matches <<EOF>> at EOF forever */
                                               }

  /* Interpolation hole `${ … }` — a RESTRICTED expression: an identifier optionally followed by `.field`
     or `[index]` accessors (no operators/calls — bind a `let` for anything more). These lex as the ordinary
     IDENTIFIER / DOT / bracket / index-literal tokens so the parser reuses real expression nodes; `}` closes
     the hole and resumes string lexing (the next ISTR_CHUNK follows). Whitespace inside the hole is ignored. */
<IN_INTERP_HOLE>[ \t]+                          { yyextra->next_column_number += yyleng; }
<IN_INTERP_HOLE>{new_line}                      { ++(yyextra->next_line_number); yyextra->next_column_number = KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE; }
<IN_INTERP_HOLE>{identifier}                    { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return getToken(yytext); }
<IN_INTERP_HOLE>{dec_literal_no_suffix}         { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return DEC_LITERAL_NO_SUFFIX; }
<IN_INTERP_HOLE>"."                             { RETURN_TOKEN(DOT, 1); }
<IN_INTERP_HOLE>"["                             { RETURN_TOKEN(LEFT_BRACKET, 1); }
<IN_INTERP_HOLE>"]"                             { RETURN_TOKEN(RIGHT_BRACKET, 1); }
<IN_INTERP_HOLE>":"                             { yyextra->next_column_number += 1; BEGIN(IN_INTERP_SPEC); }   // a format spec follows the hole expr
<IN_INTERP_HOLE>"}"                             {
                                                 yyextra->next_column_number += 1;
                                                 yy_pop_state(yyscanner);                       // back to IN_REGULAR_STRING
                                                 yyextra->workString = std::make_shared<std::string>();   // accumulate the next chunk
                                               }
<IN_INTERP_HOLE>.                               { error("unexpected character in a string interpolation hole - only an identifier with member or index accessors is allowed", yyscanner); }
<IN_INTERP_HOLE><<EOF>>                         {
                                                  error("unexpected end of file in a string interpolation hole", yyscanner);
                                                  yyterminate();
                                               }

  /* Interpolation format spec `${expr:SPEC}` — the raw spec text (e.g. `.2`, `0x`, `0X`, `x`, `0o`, `0b`) is
     returned as ONE ISTR_SPEC token and parsed at codegen (kept raw so the AST/hole model is stable as the
     specifier vocabulary grows). The `}` closes the whole hole and resumes string lexing, exactly like the
     hole-state closer above. Entered from IN_INTERP_HOLE on `:` via BEGIN (no stack push — the IN_REGULAR_STRING
     pushed at `${` stays on the stack, so yy_pop_state here returns to it). */
<IN_INTERP_SPEC>[-+.0-9A-Za-z]+                 { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return ISTR_SPEC; }
<IN_INTERP_SPEC>{new_line}                      { error("unexpected end of line in a string interpolation format specifier", yyscanner); }
<IN_INTERP_SPEC>"}"                             {
                                                 yyextra->next_column_number += 1;
                                                 yy_pop_state(yyscanner);                       // back to IN_REGULAR_STRING
                                                 yyextra->workString = std::make_shared<std::string>();   // accumulate the next chunk
                                               }
<IN_INTERP_SPEC>.                               { error("unexpected character in a string interpolation format specifier", yyscanner); }
<IN_INTERP_SPEC><<EOF>>                         {
                                                  error("unexpected end of file in a string interpolation format specifier", yyscanner);
                                                  yyterminate();
                                               }

  /* A tagged string `html"…"` / `sql"…"` / `stripIndent"…"` — an identifier IMMEDIATELY adjacent to a
     string's opening `"` (no whitespace). Trailing context `/\"` matches the identifier without consuming
     the quote, so the {regular_string_start} rule fires next and lexes the string/interpolation unchanged;
     the parser attaches this STRING_TAG to the resulting node. `foo "x"` (with a space) is NOT a tag. Placed
     before the plain {identifier} rule so it wins the same-length tie on quote-adjacency. */
{identifier}/\"                                 { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); return STRING_TAG; }
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
">>"    { /* Inside a generic `<…>` (depth>0), `>>` closes two levels: hand back one `>` and return a
             single GT (the parser's GT-close action owns the decrement). Else it's the shift operator. */
          if (yyextra->genericDepth > 0) { yyless(1); RETURN_TOKEN(GT, 1); }
          else                           { RETURN_TOKEN(GTGT, 2); } }
">>="   { RETURN_TOKEN(GTGTEQ, 3); }
"<<="   { RETURN_TOKEN(LTLTEQ, 3); }
"&="    { RETURN_TOKEN(ANDEQ, 2); }
"|="    { RETURN_TOKEN(OREQ, 2); }
"^="    { RETURN_TOKEN(XOREQ, 2); }

"{"     { RETURN_TOKEN(LEFT_BRACE, 1); }
"}"     { RETURN_TOKEN(RIGHT_BRACE, 1); }
"@"     { RETURN_TOKEN(AT, 1); }
"!"     { RETURN_TOKEN(EXCLAMATION, 1); }
"%"     { RETURN_TOKEN(PERCENT, 1); }
"&"     { RETURN_TOKEN(AMP, 1); }
"("     { RETURN_TOKEN(LPAREN, 1); }
")"     { RETURN_TOKEN(RPAREN, 1); }
"*"     { RETURN_TOKEN(STAR, 1); }
"+"     { RETURN_TOKEN(PLUS, 1); }
"-"     { RETURN_TOKEN(MINUS, 1); }
"..."   { RETURN_TOKEN(ELLIPSIS, 3); }
"."     { RETURN_TOKEN(DOT, 1); }
"/"     { RETURN_TOKEN(SLASH, 1); }
"::"    { RETURN_TOKEN(COLONCOLON, 2); }
":="    { RETURN_TOKEN(WALRUS, 2); }
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
              error("invalid token", yyscanner);
        }
%%

struct name_value {
  const char *name;
  int value;
};
static struct name_value keywords [] = { 
 /* This list must remain sorted!!! */
    {"abstract",    ABSTRACT},
    {"alignof",     ALIGNOF},
    {"as",          AS},
    {"asm",         ASM},
    {"base",        BASE},
    {"bool",        BOOL},
    {"break",       BREAK},
    {"case",        CASE},
    {"cast",        CAST},
    {"char",        CHAR},
    {"comptime",    COMPTIME},
    {"const",       CONST},
    {"continue",    CONTINUE},
    {"copy",        COPY},
    {"ctor",        CTOR},
    {"default",     DEFAULT},
    {"do",          DO},
    {"double",      DOUBLE},
    {"else",        ELSE},
    {"enum",        ENUM},
    {"export",      EXPORT},
    {"expose",      EXPOSE},
    {"extends",     EXTENDS},
    {"extern",      EXTERN},
    {"false",       FALSE},
    {"final",       FINAL},
    {"float32",     FLOAT32},
    {"float64",     FLOAT64},
    {"fn",          FN},
    {"fnptr",       FNPTR},
    {"for",         FOR},
    {"foreach",     FOREACH},
    {"friend",      FRIEND},
    {"give",        GIVE},
    {"hardware",    HARDWARE},
    {"if",          IF},
    {"immutable",   IMMUTABLE},
    {"implements",  IMPLEMENTS},
    {"import",      IMPORT},
    {"in",          IN},
    {"int",         INT},
    {"int16",       INT16},
    {"int32",       INT32},
    {"int64",       INT64},
    {"int8",        INT8},
    {"match",       MATCH},
    {"namespace",   NAMESPACE},
    {"new",         NEW},
    {"null",        NULL_LITERAL},
    {"operator",    OPERATOR},
    {"out",         OUT},
    {"override",    OVERRIDE},
    {"parallel_for", PARALLEL_FOR},
    {"private",     PRIVATE},
    {"protected",   PROTECTED},
    {"public",      PUBLIC},
    {"ref",         REF},
    {"return",      RETURN},
    {"scope",       SCOPE},
    {"sizeof",      SIZEOF},
    {"spawn",       SPAWN},
    {"static",      STATIC},
    {"string",      STRING},
    {"this",        THIS},
    {"true",        TRUE},
    {"try",         TRY},
    {"type",        TYPE},
    {"uint16",      UINT16},
    {"uint32",      UINT32},
    {"uint64",      UINT64},
    {"uint8",       UINT8},
    {"unsafe",      UNSAFE},
    {"virtual",     VIRTUAL},
    {"void",        VOID},
    {"when",        WHEN},
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
    case '$':          // `\$` -> a literal `$` (so `\${` is a literal `${`, not an interpolation hole)
      rtn = '$';
    break;
  }

  return rtn;
}

/* Decode a single UTF-8 codepoint from `len` bytes (1–4). Returns 0xFFFFFFFF on an overlong, surrogate,
   or out-of-range encoding (the caller rejects it). The flex pattern already constrains lead/continuation
   bytes; the min-value + surrogate + max-value checks reject the residual invalid forms. */
unsigned decodeUTF8Codepoint(const char* bytes, int len)
{
  const unsigned char* b = (const unsigned char*)bytes;
  unsigned cp;
  if (len == 1) { return b[0]; }
  if (len == 2) { cp = ((unsigned)(b[0] & 0x1F) << 6) | (unsigned)(b[1] & 0x3F);
                  return cp < 0x80u ? 0xFFFFFFFFu : cp; }
  if (len == 3) { cp = ((unsigned)(b[0] & 0x0F) << 12) | ((unsigned)(b[1] & 0x3F) << 6) | (unsigned)(b[2] & 0x3F);
                  if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0xFFFFFFFFu;
                  return cp; }
  if (len == 4) { cp = ((unsigned)(b[0] & 0x07) << 18) | ((unsigned)(b[1] & 0x3F) << 12)
                     | ((unsigned)(b[2] & 0x3F) << 6) | (unsigned)(b[3] & 0x3F);
                  return (cp < 0x10000u || cp > 0x10FFFFu) ? 0xFFFFFFFFu : cp; }
  return 0xFFFFFFFFu;
}

static int getToken(const char *lexeme)
{

  static int num_keywords = sizeof(keywords) / sizeof(name_value);
  int token = bin_search(lexeme,0,num_keywords);
  if (token == -1)
    token = IDENTIFIER;
  return token;
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
