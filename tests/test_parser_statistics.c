#include "parser.h"
#include <assert.h>
#include <string.h>

static ASTNode *parse(const char *source, Parser *parser, Lexer *lexer) {
    lexer_init(lexer, source);
    parser_init(parser, lexer);
    return parser_parse(parser);
}

static void expect_error(const char *body) {
    char source[1024];
    int written = snprintf(source, sizeof(source),
                           ".analisis prueba { arreglo valores = [1, 2, 3]; %s }", body);
    assert(written > 0 && (size_t)written < sizeof(source));
    Lexer lexer;
    Parser parser;
    ASTNode *program = parse(source, &parser, &lexer);
    assert(program == NULL);
    assert(parser.has_error);
    assert(parser.error.code == MILENA_ERR_PARSE);
    assert(parser.error.message[0] != '\0');
    parser_release(&parser);
}

int main(void) {
    const char *source =
        ".analisis estadisticas {\n"
        "  arreglo valores = [1, 2, 3, 4];\n"
        "  suma(valores);\n"
        "  media(valores, eje 0);\n"
        "  minimo(valores);\n"
        "  maximo(valores);\n"
        "  varianza(valores);\n"
        "  desviacion_estandar(valores);\n"
        "  mediana(valores, eje 0, sin conservar dimensiones);\n"
        "  percentil(valores, 90, eje 0, conservar dimensiones);\n"
        "}\n";
    Lexer lexer;
    Parser parser;
    ASTNode *program = parse(source, &parser, &lexer);
    assert(program != NULL);
    assert(!parser.has_error);
    assert(program->child_count == 1);
    ASTNode *analysis = program->children[0];
    assert(analysis->type == AST_BLOQUE_ANALISIS);
    assert(analysis->child_count == 9);

    const ASTStatOperation expected[] = {
        AST_ESTADISTICA_SUMA, AST_ESTADISTICA_MEDIA, AST_ESTADISTICA_MINIMO,
        AST_ESTADISTICA_MAXIMO, AST_ESTADISTICA_VARIANZA,
        AST_ESTADISTICA_DESVIACION, AST_ESTADISTICA_MEDIANA,
        AST_ESTADISTICA_PERCENTIL
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        ASTNode *operation = analysis->children[i + 1];
        assert(operation->type == AST_OPERACION_ESTADISTICA);
        assert(operation->statistical_operation == expected[i]);
        assert(operation->child_count == 1);
        assert(operation->children[0]->type == AST_EXPRESION_IDENTIFICADOR);
        assert(strcmp(operation->children[0]->value, "valores") == 0);
        assert(strcmp(ast_type_name(operation->type), "OPERACION_ESTADISTICA") == 0);
        assert(strcmp(ast_stat_operation_name(operation->statistical_operation),
                      "DESCONOCIDA") != 0);
    }
    ASTNode *median = analysis->children[7];
    assert(median->axis == 0);
    assert(!median->keepdims);
    ASTNode *percentile = analysis->children[8];
    assert(percentile->axis == 0);
    assert(percentile->keepdims);
    assert(percentile->percentile == 90.0);
    ast_destroy(program);
    parser_release(&parser);

    for (int type = 0; type < AST_NODE_TYPE_COUNT; type++) {
        assert(strcmp(ast_type_name((ASTNodeType)type), "DESCONOCIDO") != 0);
    }
    for (int type = 0; type < TOKEN_TYPE_COUNT; type++) {
        assert(strcmp(token_type_name((TokenType)type), "DESCONOCIDO") != 0);
    }

    expect_error("media(no_declarado);");
    expect_error("percentil(valores, -1);");
    expect_error("percentil(valores, 101);");
    expect_error("media(valores, eje 1.5);");
    expect_error("media(valores, eje -1);");
    expect_error("media(valores, conservar dimensiones);");
    expect_error("media(valores, eje 0, eje 1);");
    expect_error("media(valores, eje 0, conservar dimensiones, sin conservar dimensiones);");
    expect_error("media(valores, eje 0, conservar eje);");
    expect_error("media(valores, porcentaje 10);");
    expect_error("media(valores, eje 0;");
    return 0;
}
