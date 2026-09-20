#ifndef MILENA_PARSER_H
#define MILENA_PARSER_H

#include "common.h"
#include "lexer.h"
#include "ast.h"

typedef struct Parser {
    Lexer *lexer;
    Token current;
    Token previous;
    bool has_error;
    MilenaErrorInfo error;
} Parser;

void parser_init(Parser *parser, Lexer *lexer);
ASTNode* parser_parse(Parser *parser);
void parser_error(Parser *parser, const char *msg);
bool parser_match(Parser *parser, TokenType type);
bool parser_expect(Parser *parser, TokenType type, const char *msg);
void parser_advance(Parser *parser);

#endif
