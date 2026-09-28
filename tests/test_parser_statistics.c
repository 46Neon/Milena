#include "parser.h"
#include <assert.h>
#include <string.h>

static ASTNode *parse(const char *source, Parser *parser, Lexer *lexer) {
    lexer_init(lexer, source);
    parser_init(parser, lexer);
    return parser_parse(parser);
}

static void expect_source_span(const char *source, const ASTNode *node,
                               const char *spelling) {
    assert(source != NULL && node != NULL && spelling != NULL);
    const char *start = strstr(source, spelling);
    assert(start != NULL && node->has_source_span);
    size_t start_offset = (size_t)(start - source);
    size_t end_offset = start_offset + strlen(spelling);
    size_t line = 1, column = 1;
    for (size_t i = 0; i < start_offset; ++i) {
        if (source[i] == '\n') { ++line; column = 1; }
        else ++column;
    }
    assert(node->start_offset == start_offset && node->end_offset == end_offset);
    assert(node->line == line && node->column == column);
    for (size_t i = start_offset; i < end_offset; ++i) {
        if (source[i] == '\n') { ++line; column = 1; }
        else ++column;
    }
    assert(node->end_line == line && node->end_column == column);
}

static void expect_span_coordinates(const char *source, const ASTNode *node) {
    assert(source != NULL && node != NULL && node->has_source_span);
    size_t length = strlen(source);
    assert(node->start_offset <= node->end_offset && node->end_offset <= length);
    size_t line = 1, column = 1;
    for (size_t i = 0; i < node->start_offset; ++i) {
        if (source[i] == '\n') { ++line; column = 1; }
        else ++column;
    }
    assert(node->line == line && node->column == column);
    for (size_t i = node->start_offset; i < node->end_offset; ++i) {
        if (source[i] == '\n') { ++line; column = 1; }
        else ++column;
    }
    assert(node->end_line == line && node->end_column == column);
}

static void expect_error_contains(const char *body, const char *message) {
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
    if (message) assert(strstr(parser.error.message, message) != NULL);
    parser_release(&parser);
}

static void expect_error(const char *body) {
    expect_error_contains(body, NULL);
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
    assert(analysis->has_source_span);
    assert(analysis->start_offset == 0);
    assert(analysis->end_offset == strlen(source) - 1);
    assert(analysis->child_count == 9);

    const ASTStatOperation expected[] = {
        AST_ESTADISTICA_SUMA, AST_ESTADISTICA_MEDIA, AST_ESTADISTICA_MINIMO,
        AST_ESTADISTICA_MAXIMO, AST_ESTADISTICA_VARIANZA,
        AST_ESTADISTICA_DESVIACION, AST_ESTADISTICA_MEDIANA,
        AST_ESTADISTICA_PERCENTIL
    };
    const char *expected_call_spans[] = {
        "suma(valores)", "media(valores, eje 0)", "minimo(valores)",
        "maximo(valores)", "varianza(valores)",
        "desviacion_estandar(valores)",
        "mediana(valores, eje 0, sin conservar dimensiones)",
        "percentil(valores, 90, eje 0, conservar dimensiones)"
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        ASTNode *operation = analysis->children[i + 1];
        assert(operation->type == AST_OPERACION_ESTADISTICA);
        const char *call_start = strstr(source, expected_call_spans[i]);
        assert(call_start != NULL && operation->has_source_span);
        expect_source_span(source, operation, expected_call_spans[i]);
        assert(operation->start_offset == (size_t)(call_start - source));
        assert(operation->end_offset == operation->start_offset +
               strlen(expected_call_spans[i]));
        expect_span_coordinates(source, operation);
        assert(operation->statistical_operation == expected[i]);
        assert(operation->child_count == 1);
        assert(operation->children[0]->type == AST_EXPRESION_IDENTIFICADOR);
        const char *input_start = strstr(call_start, "valores");
        assert(input_start != NULL && operation->children[0]->has_source_span);
        assert(operation->children[0]->start_offset ==
               (size_t)(input_start - source));
        assert(operation->children[0]->end_offset ==
               (size_t)(input_start - source) + strlen("valores"));
        expect_span_coordinates(source, operation->children[0]);
        assert(strcmp(operation->children[0]->value, "valores") == 0);
        assert(strcmp(ast_type_name(operation->type), "OPERACION_ESTADISTICA") == 0);
        assert(strcmp(ast_stat_operation_name(operation->statistical_operation),
                      "DESCONOCIDA") != 0);
    }
    ASTNode *sum_call = analysis->children[1];
    const char *sum_source = strstr(source, "suma(valores)");
    assert(sum_source != NULL);
    assert(sum_call->has_source_span);
    assert(sum_call->start_offset == (size_t)(sum_source - source));
    assert(sum_call->end_offset ==
           (size_t)(sum_source - source) + strlen("suma(valores)"));
    assert(sum_call->children[0]->has_source_span);
    assert(sum_call->children[0]->start_offset ==
           (size_t)(sum_source - source) + strlen("suma("));
    assert(sum_call->children[0]->end_offset ==
           (size_t)(sum_source - source) + strlen("suma(valores"));

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
        assert(strcmp(token_type_name((MilenaTokenType)type), "DESCONOCIDO") != 0);
    }

    expect_error_contains("media(no_declarado);", "no ha sido declarado");
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

    /* A semantic diagnostic must point at the undeclared operand itself, not
       at the closing parenthesis consumed during lookahead. */
    const char *location_source =
        ".analisis ubicacion {\n"
        "  media(no_declarado);\n"
        "}\n";
    program = parse(location_source, &parser, &lexer);
    assert(program == NULL);
    assert(parser.has_error);
    assert(parser.error.line == 2 && parser.error.column == 9);
    assert(strstr(parser.error.message, "no ha sido declarado") != NULL);
    parser_release(&parser);
    return 0;
}
