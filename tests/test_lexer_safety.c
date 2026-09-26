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
    assert(token.line == 2 && token.column == 17);
    assert(token.end_line == token.line && token.end_column == token.column);

    lexer_init(&lexer, "x-2");
    assert(lexer_next_token(&lexer).type == TOKEN_IDENTIFICADOR);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_MENOS && token.start_offset == 1 && token.end_offset == 2);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_NUMERO && token.number_value == 2.0);

    /* An exponent sign belongs to the number; the following minus remains an
       independent operator with its own exact byte range. */
    lexer_init(&lexer, "1e-2-3");
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_NUMERO && token.number_value == 0.01);
    assert(token.start_offset == 0 && token.end_offset == 4);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_MENOS);
    assert(token.start_offset == 4 && token.end_offset == 5);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_NUMERO && token.number_value == 3.0);
    assert(token.start_offset == 5 && token.end_offset == 6);

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
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 0 && token.end_offset == 10);
    assert(token.line == 1 && token.column == 1);
    assert(lexer.error.line == 1 && lexer.error.column == 1);
    assert(strstr(lexer.error.message, "escape") != NULL);

    const char *unterminated = "\"line\nopen";
    lexer_init(&lexer, unterminated);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 0 && token.end_offset == strlen(unterminated));
    assert(token.line == 1 && token.column == 1);
    assert(token.end_line == 2 && token.end_column == 5);
    assert(lexer.error.code == MILENA_ERR_PARSE);
    assert(lexer.error.line == 1 && lexer.error.column == 1);

    /* CRLF occupies two source bytes but one logical line break. Spans stay
       byte-based, columns are one-based/exclusive, and EOF has a zero-width
       span at the final source position. */
    const char *crlf_source = "uno\r\n\"á\"\r\n1e+\r\n";
    lexer_init(&lexer, crlf_source);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_IDENTIFICADOR);
    assert(token.start_offset == 0 && token.end_offset == 3);
    assert(token.line == 1 && token.column == 1);
    assert(token.end_line == 1 && token.end_column == 4);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_CADENA);
    assert(token.start_offset == 5 && token.end_offset == 9);
    assert(token.line == 2 && token.column == 1);
    assert(token.end_line == 2 && token.end_column == 5);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 11 && token.end_offset == 14);
    assert(token.line == 3 && token.column == 1);
    assert(token.end_line == 3 && token.end_column == 4);
    assert(lexer.error.line == 3 && lexer.error.column == 1);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_EOF);
    assert(token.start_offset == sizeof("uno\r\n\"á\"\r\n1e+\r\n") - 1);
    assert(token.end_offset == token.start_offset);
    assert(token.line == 4 && token.column == 1);
    assert(token.end_line == 4 && token.end_column == 1);

    /* A terminal backslash is a truncated unsupported escape, not an out-of-
       bounds read; the diagnostic keeps the opening quote's source position. */
    const char *truncated_escape = "\"x\\";
    lexer_init(&lexer, truncated_escape);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 0 && token.end_offset == strlen(truncated_escape));
    assert(token.line == 1 && token.column == 1);
    assert(token.end_line == 1 && token.end_column == 4);
    assert(lexer.error.code == MILENA_ERR_PARSE);
    assert(lexer.error.line == 1 && lexer.error.column == 1);
    assert(strstr(lexer.error.message, "escape") != NULL);

    /* Token and diagnostic locations use size_t end-to-end: no narrowing to
       signed int occurs when a source position is beyond INT_MAX. */
    lexer_init(&lexer, "@");
    size_t wide_position = (size_t)INT_MAX + (size_t)1;
    lexer.line = wide_position;
    lexer.column = wide_position;
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.line == wide_position && token.column == wide_position);
    assert(lexer.error.line == wide_position);
    assert(lexer.error.column == wide_position);

    /* Peek must leave the previous/current tokens, coordinates, and diagnostic
       untouched even when the peeked token is malformed. */
    lexer_init(&lexer, "suma 1e+");
    Token current = lexer_next_token(&lexer);
    Token previous = lexer.previous_token;
    size_t saved_position = lexer.position;
    size_t saved_line = lexer.line;
    size_t saved_column = lexer.column;
    peeked = lexer_peek_token(&lexer);
    assert(peeked.type == TOKEN_ERROR);
    assert(lexer.position == saved_position);
    assert(lexer.line == saved_line && lexer.column == saved_column);
    assert(lexer.current_token.type == current.type);
    assert(strcmp(lexer.current_token.lexeme, current.lexeme) == 0);
    assert(lexer.previous_token.type == previous.type);
    assert(lexer.error.code == MILENA_OK);
    token = lexer_next_token(&lexer);
    assert(token.type == TOKEN_ERROR);
    assert(token.start_offset == 5 && token.end_offset == 8);
    assert(lexer.error.line == 1 && lexer.error.column == 6);

    puts("lexer safety: ok");
    return 0;
}
