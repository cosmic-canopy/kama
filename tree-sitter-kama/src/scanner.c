// The one decision a grammar cannot make: whether `give`, `copy` or `truncate` is the KEYWORD or a NAME.
//
// kama's lexer makes it from the text AFTER the word (contextualWord in src/kama.l), and this is the same rule, so the
// editor and the compiler read one file one way:
//
//   `give` / `copy`  the hand-off, directly before a name — other than `in`, which a `foreach` binding so named is
//                    followed by — or before a string or number literal. `give x`, `copy this.items`.
//   `truncate`       the conversion, before `<`, a name and `>`. `truncate<uint8>(n)`.
//
// Anywhere else the word is a name (`give + 1`, `copy(x: 1)`, `truncate < n`), and the scanner declines: the word then
// lexes as an ordinary identifier, since the grammar holds no string literal for it. Blanks and comments between the
// word and what decides it are skipped, as the compiler's parser skips them.
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <string.h>

enum TokenType { GIVE_KEYWORD, COPY_KEYWORD, TRUNCATE_KEYWORD };

void *tree_sitter_kama_external_scanner_create(void) { return NULL; }
void tree_sitter_kama_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_kama_external_scanner_serialize(void *payload, char *buffer) { (void)payload; (void)buffer; return 0; }
void tree_sitter_kama_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  (void)payload; (void)buffer; (void)length;
}

static bool ident_start(int32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool ident_char(int32_t c) { return ident_start(c) || (c >= '0' && c <= '9'); }

// Past the token's end (already marked), so nothing skipped here becomes part of it.
static void skip_blank(TSLexer *lexer) {
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { lexer->advance(lexer, false); continue; }
    if (c != '/') return;
    lexer->advance(lexer, false);
    if (lexer->lookahead == '/') {
      while (!lexer->eof(lexer) && lexer->lookahead != '\n') lexer->advance(lexer, false);
    } else if (lexer->lookahead == '*') {
      lexer->advance(lexer, false);
      for (;;) {
        if (lexer->eof(lexer)) return;
        int32_t d = lexer->lookahead;
        lexer->advance(lexer, false);
        if (d == '*' && lexer->lookahead == '/') { lexer->advance(lexer, false); break; }
      }
    } else {
      return;   // a lone `/` — a division; it decides as itself
    }
  }
}

bool tree_sitter_kama_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  (void)payload;
  if (!valid_symbols[GIVE_KEYWORD] && !valid_symbols[COPY_KEYWORD] && !valid_symbols[TRUNCATE_KEYWORD]) return false;

  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' || lexer->lookahead == '\n')
    lexer->advance(lexer, true);
  if (!ident_start(lexer->lookahead)) return false;

  char word[10];
  unsigned n = 0;
  while (ident_char(lexer->lookahead)) {
    if (n < sizeof word - 1) word[n] = (char)lexer->lookahead;
    ++n;
    lexer->advance(lexer, false);
  }
  if (n >= sizeof word) return false;
  word[n] = '\0';
  lexer->mark_end(lexer);

  enum TokenType kind;
  if (strcmp(word, "give") == 0) kind = GIVE_KEYWORD;
  else if (strcmp(word, "copy") == 0) kind = COPY_KEYWORD;
  else if (strcmp(word, "truncate") == 0) kind = TRUNCATE_KEYWORD;
  else return false;
  if (!valid_symbols[kind]) return false;

  skip_blank(lexer);
  int32_t c = lexer->lookahead;
  if (kind == TRUNCATE_KEYWORD) {
    if (c != '<') return false;
    lexer->advance(lexer, false);
    skip_blank(lexer);
    if (!ident_start(lexer->lookahead)) return false;
    while (ident_char(lexer->lookahead)) lexer->advance(lexer, false);
    skip_blank(lexer);
    if (lexer->lookahead != '>') return false;
  } else if (c == '"' || (c >= '0' && c <= '9')) {
    // a literal operand — the checker refuses it as no named value, as the compiler's does
  } else {
    if (!ident_start(c)) return false;
    if (c == 'i') {
      lexer->advance(lexer, false);
      if (lexer->lookahead == 'n') {
        lexer->advance(lexer, false);
        if (!ident_char(lexer->lookahead)) return false;   // `give in` — a `foreach` binding named `give`
      }
    }
  }
  lexer->result_symbol = kind;
  return true;
}
