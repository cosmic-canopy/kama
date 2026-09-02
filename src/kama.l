%{
#include <string>
#include <memory>
#include "kama.parser.hpp"

/* Each macro records the token's END position (one past its last column) after advancing, alongside the
 * START it already tracked — so an AST node built at reduction time can carry a [start,end) range, not
 * just a point (LSP source spans, T3). End line == start line for these single-line tokens.
 * STAMP_YYLLOC also publishes that [start,end) into Bison's per-token yylloc (%locations, M0), so the
 * parser reconstructs an accurate span for every rule (@$) instead of reading the lexer counter at reduce
 * time (which lags by one lookahead token). */
#define STAMP_YYLLOC  yylloc->first_line   = yyextra->codeGenContext->line;    \
                      yylloc->first_column = yyextra->codeGenContext->col;     \
                      yylloc->last_line    = yyextra->codeGenContext->endLine; \
                      yylloc->last_column  = yyextra->codeGenContext->endCol;

#define SAVE_TOKEN_FROM_BUFFER(l)  yyextra->next_column_number += l; \
                                   yyextra->codeGenContext->endLine = yyextra->next_line_number; \
                                   yyextra->codeGenContext->endCol  = yyextra->next_column_number; \
                                   STAMP_YYLLOC \
                                   yylval->string = yyextra->workString; \
                                   yyextra->workString = NULL;

#define SAVE_TOKEN(src, leng, l)  yyextra->codeGenContext->line = yyextra->next_line_number; \
                                  yyextra->codeGenContext->col = yyextra->next_column_number; \
                                  yyextra->next_column_number += l; \
                                  yyextra->codeGenContext->endLine = yyextra->next_line_number; \
                                  yyextra->codeGenContext->endCol  = yyextra->next_column_number; \
                                  STAMP_YYLLOC \
                                  yylval->string = std::make_shared<std::string>(src, leng);

#define RETURN_TOKEN(t, l)   yyextra->codeGenContext->line = yyextra->next_line_number; \
                             yyextra->codeGenContext->col = yyextra->next_column_number; \
                             yyextra->next_column_number += l; \
                             yyextra->codeGenContext->endLine = yyextra->next_line_number; \
                             yyextra->codeGenContext->endCol  = yyextra->next_column_number; \
                             STAMP_YYLLOC \
                             return yylval->token = t;

/* Record an identifier spelling for closure pruning (kama.ast.h CompilationUnit::identTokens). The set
 * is a deliberate OVER-approximation of what the file references — a local named `sort` counts, and the
 * only cost is one extra translation unit kept. Under-approximating is what would be fatal, so this must
 * fire on EVERY rule that can carry a reference.
 *
 * There are THREE such rules, not two: `grep -n '{identifier}' kama.l`. The easy one to miss is the
 * STRING_TAG rule (`{identifier}/\"`), which returns its token directly instead of going through
 * getToken — and a string tag IS an unqualified reference to a sibling top-level function
 * (`resolveFunc(*is->tag, nullptr)` in kama.cemit.cpp; `html`/`sql`/`stripIndent` live in
 * lib/std/fmt/tags.kama). Hooking getToken alone would silently drop it.
 *
 * Keywords are skipped — not for correctness (no keyword can be a top-level declared name, so recording
 * them would only waste memory) but because `getToken` has already classified the spelling, so the test
 * is free. The STRING_TAG rule has no such classification and records unconditionally; a tag is an
 * identifier by construction.
 *
 * A construct that requires a library seam WITHOUT naming anything — `spawn`, `parallel_for` — cannot be
 * handled here, and trying was the first thing that failed. The `spawn` sits in the importing program, not
 * in the module being pruned, and the requirement is program-wide, so no per-import closure can see it.
 * The provider is marked unprunable instead; see harvestUnitFacts in kama.ast.h. */
#define RECORD_IDENT(src, leng)  yyextra->identTokens.emplace(src, leng)

/* kama reserves every C keyword (SPEC.md § Modules), so a spelling that would lex as an
 * identifier but IS one is rejected here, at the single point where a spelling becomes a name.
 *
 * ⚠️ It rides on the identifier RULES, and there are THREE of them — see the note above; the easy miss is
 * the STRING_TAG rule, which returns its token directly instead of going through `getToken`. A reserved
 * word is reserved everywhere a name can appear, so all three carry this.
 *
 * The rules fire on every OCCURRENCE, not just the declaration, so `reservedReported` keeps it to one
 * error per spelling per file. Lexing continues (handleError accumulates) so a file naming two reserved
 * words reports both rather than one at a time. */
#define RESERVED_CHECK(src)                                                       \
    do {                                                                          \
        if (cIsReservedWord(src) && yyextra->reservedReported.insert(src).second)  \
            error(cReservedWhy(src).c_str(), yyscanner);                          \
    } while (0)

#define YY_EXTRA_TYPE struct LexerInstanceData *

static int getToken(const char *);
bool cIsReservedWord(const char *);
static std::string cReservedWhy(const std::string&);
char getSimpleEscape(const char* input, int index);
std::string hexToUTF8(int cp);
unsigned decodeUTF8Codepoint(const char* bytes, int len);

void error(const char *, yyscan_t scanner);
%}

%option extra-type="struct LexerInstanceData *"
%option 8bit reentrant bison-bridge bison-locations
%option never-interactive
%option warn nodefault
%option stack
%option noyywrap nounistd nounput noinput noyy_top_state


new_line                    \n
white_space_1               " "
white_space_2               [\t\r]

reserved_preprocessor       ^[ \t]*#(region|endregion).*

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
  /* Two forms, matching Swift and Zig exactly: `digits.digits` with an optional exponent, or `digits`
     with a REQUIRED exponent (`1e10` — the exponent marker already makes it unmistakably a float).
     A dot still needs digits on BOTH sides, so `1.` and `.5` stay rejected: those are the error-prone
     spellings (`.5` reads as a stray member access, `1.` as an unfinished expression), and rejecting
     them is ordinary — Swift, Zig and Rust do. Rejecting `1e10`, which kama used to do, is not: no
     mainstream language does, and it blocked pasting constants from C headers and datasheets.
     ⚠️ These used to be two macros, and the exponent one accepted only a SINGLE leading digit — so
     `12.5` lexed but `12.5e10` did not, while non-normalized `0.0001e10` was fine. One macro can't
     drift that way. See docs/SPEC.md "Numeric literals". */
float_literal_no_suffix     ({dec_digit}+\.{dec_digit}+{exponent}?|{dec_digit}+{exponent})
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

  /* #region / #endregion, as in C#, so an IDE can fold. The pattern names those two words on purpose
     and is NOT the wider `^[ \t]*#.*` it used to be: `#` now opens a compile-time argument list
     (`buf#(4)`), and a wrapped list can start a line. Flex takes the LONGEST match, so the wide
     pattern would swallow such a line whole and always beat the `"#"` rule below. */
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
  /* A raw newline inside a plain string. It MUST come before {single_string_char}, which also matches
     `\n` — both are one character, so flex breaks the tie by rule order, and without this rule the
     newline was consumed as ordinary content and the line counter never advanced.
     That made every diagnostic AFTER a multi-line literal fire that many lines EARLY, and the deficit
     accumulated over every literal in the file: a parse error 27 lines below a WGSL shader was reported
     27 lines above the mistake, on a line holding valid code. The column stayed right, which is what
     made it read as a compiler fault rather than an offset.
     Multi-line plain strings are a FEATURE, not an accident — `stripIndent"…"` exists to dedent one
     (SPEC, *Interpolation*), and kama's own WebGPU example holds a ten-line shader — so counting the
     newline is the fix and rejecting it is not. Every other state that can span lines already had this
     rule (IN_COMMENT, IN_VERBATIM_STRING, IN_INTERP_HOLE); the plain-string state was the only one
     without it. Appends `yytext` so the string's VALUE is byte-identical to before. */
<IN_REGULAR_STRING>{new_line}                  {
                                                 ++(yyextra->next_line_number);
                                                 yyextra->next_column_number = KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE;
                                                 yyextra->workString->append(yytext);
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
<IN_INTERP_HOLE>{identifier}                    { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); int t = getToken(yytext); if (t == IDENTIFIER) { RESERVED_CHECK(yytext); RECORD_IDENT(yytext, leng); } return t; }
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
{identifier}/\"                                 { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); RESERVED_CHECK(yytext); RECORD_IDENT(yytext, leng); return STRING_TAG; }
{identifier}                                   { int leng = yyleng; SAVE_TOKEN(yytext, leng, leng); int t = getToken(yytext); if (t == IDENTIFIER) { RESERVED_CHECK(yytext); RECORD_IDENT(yytext, leng); } return t; }

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
"#"     { RETURN_TOKEN(HASH, 1); }
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
    {"bitcast",     BITCAST},
    {"bool",        BOOL},
    {"borrow",      BORROW},
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
    {"else",        ELSE},
    {"enum",        ENUM},
    {"export",      EXPORT},
    {"expose",      EXPOSE},
    {"extends",     EXTENDS},
    {"extern",      EXTERN},
    {"false",       FALSE},
    /* CONTEXTUAL, like `slot`/`type`: it leads the file gate (`file @compileFor(FLAG);`) as a unit's
       first line, and names an ordinary binding everywhere else. The token is FILE_KW, not FILE,
       because <cstdio> defines FILE and bison writes the token name into an enum. */
    {"file",        FILE_KW},
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
    {"int16",       INT16},
    {"int32",       INT32},
    {"int64",       INT64},
    {"int8",        INT8},
    {"isize",       ISIZE},
    {"match",       MATCH},
    {"new",         NEW},
    {"null",        NULL_LITERAL},
    {"operator",    OPERATOR},
    {"out",         OUT},
    {"override",    OVERRIDE},
    {"parallel_for", PARALLEL_FOR},
    {"parallel_spawn", PARALLEL_SPAWN},
    {"private",     PRIVATE},
    {"protected",   PROTECTED},
    {"public",      PUBLIC},
    {"ref",         REF},
    {"return",      RETURN},
    {"scope",       SCOPE},
    {"sizeof",      SIZEOF},
    {"slot",        SLOT},
    {"spawn",       SPAWN},
    {"static",      STATIC},
    {"string",      STRING},
    {"this",        THIS},
    {"true",        TRUE},
    {"truncate",    TRUNCATE},
    {"try",         TRY},
    {"type",        TYPE},
    {"uint16",      UINT16},
    {"uint32",      UINT32},
    {"uint64",      UINT64},
    {"uint8",       UINT8},
    {"unsafe",      UNSAFE},
    {"usize",       USIZE},
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

/* ---- C's reserved words, which kama reserves too (SPEC.md § Modules) ------------------
 *
 * kama lowers to C, and a kama name that is a C keyword emits C that does not compile: `int32 switch;`
 * becomes `int32_t switch;`. Measured at `0.9.80`, the exposure was not one position but seven — struct
 * field, variant payload field, variant case union member, vtable slot, parameter (in the prototype, the
 * definition AND the thunk cast), local, and an `expose`d function's bare C name. Type names, function
 * names and enum case constants are scope-prefixed and were never at risk.
 *
 * The alternative was to RENAME on collision (`switch` -> `k_switch`) and keep the spelling legal. That is
 * what an earlier draft of the design specified, and reserving was chosen over it deliberately: reserving
 * now and relaxing later is source-compatible, the reverse is not, and pre-1.0 is the only moment. It also
 * dissolves three sub-problems a rename carries — a `k_switch` clash with a user's own `k_switch`, a
 * separate rule for `expose`/`extern` names (which are a declared C ABI and could never be renamed), and
 * the risk of the C name leaking into a diagnostic, a hover or a `kama query` answer.
 *
 * Corpus cost, measured before deciding by PRINTING every match rather than counting it: zero. Every hit
 * across lib/ tests/ prelude/ examples/ bench/ is a comment.
 *
 * The table is the FULL C11 + C23 set, not just the 38 spellings kama leaves free. The other 21 are
 * already kama keywords and so never reach the identifier rule — but kama's own reserved list may shrink
 * later, and a wider table costs nothing since an entry only ever fires on a real collision.
 *
 * This list must remain sorted (bin_search), and `tools/check-c-keywords.sh` holds it to both standards.
 */
static const char* c_reserved[] = {
    "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal128", "_Decimal32",
    "_Decimal64", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    "alignas", "alignof", "auto", "bool", "break", "case", "char", "const", "constexpr", "continue",
    "default", "do", "double", "else", "enum", "extern", "false", "float", "for", "goto", "if",
    "inline", "int", "long", "nullptr", "register", "restrict", "return", "short", "signed", "sizeof",
    "static", "static_assert", "struct", "switch", "thread_local", "true", "typedef", "typeof",
    "typeof_unqual", "union", "unsigned", "void", "volatile", "while",
};

/* Is this spelling reserved by C, and therefore by kama? Declared in kama.forward.h. Lives here beside
   `kamaIsKeyword` for the same single-source-of-truth reason: one table per language, in the lexer. */
bool cIsReservedWord(const char *word)
{
  int lo = 0, hi = (int)(sizeof(c_reserved) / sizeof(c_reserved[0]));
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int cmp = strcmp(word, c_reserved[mid]);
    if (cmp == 0) return true;
    if (cmp < 0) hi = mid; else lo = mid + 1;
  }
  return false;
}


/* The diagnostic for a reserved spelling, reported once per file per spelling (see RESERVED_CHECK).
 *
 * Four of these already had a BETTER message than "reserved", and losing it would be a regression the
 * corpus would not have caught: `int`, `double` and `float` are retired TYPE spellings that a C or Java
 * reader will try, and telling them the width to write is the whole point. Those arms used to live in
 * `checkTypeResolves`, which the lexer now precedes — so the guidance moves here rather than dying.
 * (`uint` is NOT a C keyword and so is not reserved; it stays on the type-position path, which is the one
 * asymmetry here and is asserted in tools/check-c-keywords.sh so nobody "fixes" it by accident.) */
static std::string cReservedWhy(const std::string& w)
{
  if (w == "int")
    return "`int` is a reserved word — write `int32` for a fixed 32-bit integer, or `isize` for a "
           "platform-width size (the type of a length or index)";
  if (w == "double" || w == "float")
    return "`" + w + "` is a reserved word — every kama float states its width: write `float64`"
         + (w == "float" ? " or `float32`" : " (`double` was an alias for it)");
  return "`" + w + "` is a reserved word — kama reserves every C keyword, because it lowers to C and a "
         "name that collides with one emits C that does not compile. Pick another name";
}

// Does this spelling lex as a reserved word rather than an identifier? The LSP's rename validation asks
// here rather than carrying its own keyword list, so the two can never drift (the table above is the
// single source of truth — tools/check-syntax-drift.sh already holds the VS Code grammar to it).
// NB: bin_search returns IDENTIFIER (not -1) on a miss, which is what `getToken` above actually relies on.
bool kamaIsKeyword(const char *word)
{
  return bin_search(word, 0, (int)kamaKeywordCount()) != IDENTIFIER;
}

/* Would this spelling lex as an identifier? A LEXICAL question, so it lives here beside kamaIsKeyword
   rather than in whichever caller happened to need it first. Two do: the LSP's rename validation (an
   editor will happily send `2x`, `my var` or `return` as a new name, and writing any of those produces a
   file that no longer parses) and the manifest reader, where a `modules` key or a `name` override becomes
   a segment of a module's name and so has to be spellable in an `import`. */
bool kamaIsIdentifier(const std::string &s)
{
  if (s.empty()) return false;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
  for (char ch : s)
    if (!(isalnum((unsigned char)ch) || ch == '_')) return false;
  return !kamaIsKeyword(s.c_str());
}

/* The keyword table, exposed for keyword COMPLETION (LSP M4.3). Same single-source-of-truth reasoning as
   kamaIsKeyword: the completion list and the lexer must never disagree about what a keyword is. */
size_t kamaKeywordCount(void)
{
  return sizeof(keywords) / sizeof(name_value);
}

const char *kamaKeywordAt(size_t i)
{
  return i < kamaKeywordCount() ? keywords[i].name : "";
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
