#include "parser.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const char *source =
        ". analisis ventas {\n"
        "  variable base = 10;\n"
        "  variable incremento = base + 5;\n"
        "  variable total = incremento - base;\n"
        "  total = total + 2;\n"
        "}\n";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    assert(program != NULL);
    assert(!parser.has_error);
    assert(program->child_count == 1);
    ASTNode *analysis = program->children[0];
    assert(analysis->type == AST_BLOQUE_ANALISIS);
    assert(strcmp(analysis->value, "ventas") == 0);
    assert(analysis->child_count == 4);

    assert(analysis->children[0]->type == AST_DECLARACION_VARIABLE);
    assert(strcmp(analysis->children[0]->value, "base") == 0);
    assert(analysis->children[0]->children[0]->type == AST_EXPRESION_LITERAL);
    assert(analysis->children[0]->children[0]->number_value == 10.0);

    ASTNode *incremento = analysis->children[1];
    assert(incremento->type == AST_DECLARACION_VARIABLE);
    assert(incremento->children[0]->type == AST_EXPRESION_OPERACION);
    assert(strcmp(incremento->children[0]->value, "+") == 0);
    assert(incremento->children[0]->children[0]->type == AST_EXPRESION_IDENTIFICADOR);
    assert(strcmp(incremento->children[0]->children[0]->value, "base") == 0);

    ASTNode *total = analysis->children[2];
    assert(total->children[0]->type == AST_EXPRESION_OPERACION);
    assert(strcmp(total->children[0]->value, "-") == 0);
    assert(strcmp(total->children[0]->children[0]->value, "incremento") == 0);

    ASTNode *assignment = analysis->children[3];
    assert(assignment->type == AST_ASIGNACION_VARIABLE);
    assert(strcmp(assignment->value, "total") == 0);
    assert(assignment->children[0]->type == AST_EXPRESION_OPERACION);
    assert(strcmp(assignment->children[0]->value, "+") == 0);
    assert(strcmp(assignment->children[0]->children[0]->value, "total") == 0);
    assert(assignment->children[0]->children[1]->number_value == 2.0);

    ast_destroy(program);
    parser_release(&parser);
    return 0;
}
