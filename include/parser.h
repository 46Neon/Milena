#ifndef MILENA_PARSER_H
#define MILENA_PARSER_H

#include "common.h"
#include "lexer.h"
#include "ast.h"
#include "symbol_table.h"

typedef struct Parser {
    Lexer *lexer;
    Token current;
    Token previous;
    bool has_error;
    MilenaErrorInfo error;
    MilenaSymbolTable symbols;
} Parser;

void parser_init(Parser *parser, Lexer *lexer);
void parser_release(Parser *parser);
ASTNode* parser_parse(Parser *parser);
ASTNode* parser_parse_statistical_call(Parser *parser);
void parser_error(Parser *parser, const char *msg);
bool parser_match(Parser *parser, TokenType type);
bool parser_expect(Parser *parser, TokenType type, const char *msg);
void parser_advance(Parser *parser);

#endif
