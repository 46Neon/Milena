#include "parser.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const char *source = ".analisis demo { array valores = [1, 2.5, 3]; }";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    assert(program);
    assert(!parser.has_error);
    assert(program->child_count == 1);
    ASTNode *analysis = program->children[0];
    assert(analysis->type == AST_BLOQUE_ANALISIS);
    assert(analysis->child_count == 1);
    ASTNode *declaration = analysis->children[0];
    assert(declaration->type == AST_DECLARACION_ARRAY);
    assert(strcmp(declaration->value, "valores") == 0);
    assert(declaration->child_count == 1);
    ASTNode *array = declaration->children[0];
    assert(array->type == AST_EXPRESION_ARRAY);
    assert(array->child_count == 3);
    assert(array->children[0]->number_value == 1.0);
    assert(array->children[1]->number_value == 2.5);
    assert(array->children[2]->number_value == 3.0);
    ast_destroy(program);
    return 0;
}
