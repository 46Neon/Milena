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

    puts("lexer safety: ok");
    return 0;
}
