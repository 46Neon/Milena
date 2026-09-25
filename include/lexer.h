#ifndef MILENA_LEXER_H
#define MILENA_LEXER_H

#include "common.h"
#include "token.h"

typedef struct Lexer {
    const char *source;
    size_t position;
    size_t length;
    size_t line;
    size_t column;
    size_t token_start_offset;
    size_t token_start_line;
    size_t token_start_column;
    Token current_token;
    Token previous_token;
    MilenaError error;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);
Token lexer_peek_token(Lexer *lexer);
void lexer_advance_token(Lexer *lexer);
bool lexer_match(Lexer *lexer, MilenaTokenType type);
bool lexer_expect(Lexer *lexer, MilenaTokenType type, const char *error_msg);

#endif
