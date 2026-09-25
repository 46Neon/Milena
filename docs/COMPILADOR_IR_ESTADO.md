#include "lexer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    Lexer lexer;
    lexer_init(&lexer, NULL);
    assert(lexer.length == 0);
    assert(lexer_next_token(&lexer).type == TOKEN_EOF);

    lexer_init(&lexer, "suma media");
    Token first = lexer_next_token(&lexer);
    assert(first.type == TOKEN_FUNCION_SUMA);
    lexer_advance_token(&lexer);
    assert(lexer.current_token.type == TOKEN_FUNCION_MEDIA);

    char long_identifier[300];
    memset(long_identifier, 'a', sizeof(long_identifier) - 1);
    long_identifier[sizeof(long_identifier) - 1] = '\0';
    lexer_init(&lexer, long_identifier);
    Token token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(strstr(lexer.error.message, "255") != NULL);

    char long_string[300];
    long_string[0] = '"';
    memset(long_string + 1, 'x', sizeof(long_string) - 3);
    long_string[sizeof(long_string) - 2] = '"';
    long_string[sizeof(long_string) - 1] = '\0';
    lexer_init(&lexer, long_string);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(strstr(lexer.error.message, "255") != NULL);

    /* Spans are half-open byte ranges; line/column positions are one-based
       byte columns and refer to the token start and exclusive end. */
    const char *span_source = " \n  12.5e-2 + \"á\"";
    lexer_init(&lexer, span_source);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_NUMERO);
    assert(strcmp(token.lexeme, "12.5e-2") == 0);
    assert(token.number_value == 0.125);
    assert(token.start_offset == 4 && token.end_offset == 11);
    assert(token.line == 2 && token.column == 3);
    assert(token.end_line == 2 && token.end_column == 10);
    Token peeked = lexer_peek_token(&lexer);
    assert(peeked.type == TOKEN_MAS);
    assert(peeked.start_offset == 12 && peeked.end_offset == 13);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_MAS && token.start_offset == 12);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_CADENA);
    assert(strcmp(token.lexeme, "á") == 0);
    assert(token.start_offset == 14 && token.end_offset == 18);
    assert(token.line == 2 && token.column == 13);
    assert(token.end_line == 2 && token.end_column == 17);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_EOF);
    assert(token.start_offset == strlen(span_source));
    assert(token.end_offset == token.start_offset);

    lexer_init(&lexer, "x-2");
    assert(lexer_next_token(&lexer).type == TOKEN_IDENTIFICADOR);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_MENOS && token.start_offset == 1 && token.end_offset == 2);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_NUMERO && token.number_value == 2.0);

    lexer_init(&lexer, "1e+");
    token = lexer_peek_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 0 && token.end_offset == 3);
    assert(lexer.position == 0 && lexer.error.code == MILENA_OK);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 0 && token.end_offset == 3);
    assert(lexer.error.line == 1 && lexer.error.column == 1);

    lexer_init(&lexer, "1e9999");
    assert(lexer_next_token(&lexer).type == TOKEN_ERROR);
    assert(lexer.error.code == MILENA_ERR_PARSE);

    lexer_init(&lexer, "\"escape\\q\"");
    assert(lexer_next_token(&lexer).type == TOKEN_ERROR);
    assert(strstr(lexer.error.message, "escape") != NULL);

    puts("lexer safety: ok");
    return 0;
}
