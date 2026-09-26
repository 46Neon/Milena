#include "parser.h"

#include <assert.h>
#include <string.h>

static ASTNode *find_metric(ASTNode *node, ASTAggregateOperation operation) {
    if (!node) return NULL;
    if (node->type == AST_RESUMEN_METRICA && node->has_aggregate_metric &&
        node->aggregate_operation == operation) return node;
    for (size_t i = 0; i < node->child_count; ++i) {
        ASTNode *found = find_metric(node->children[i], operation);
        if (found) return found;
    }
    return NULL;
}

int main(void) {
    const char *source =
        ". analisis demo {\n"
        "  variable base = 10;\n"
        "  variable total = base + 5;\n"
        "}\n";
    Lexer lexer;
    Parser parser;
    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    ASTNode *program = parser_parse(&parser);
    assert(program != NULL);
    assert(!parser.has_error);

    MilenaError error;
    milena_error_init(&error);
    assert(ast_validate(program, &error));
    assert(error.code == MILENA_OK);

    ASTNode *analysis = program->children[0];
    ASTNode *declaration = analysis->children[1];
    ASTNode *binary = declaration->children[0];
    assert(binary->type == AST_EXPRESION_OPERACION);
    assert(binary->child_count == 2);

    ASTNode *saved_parent = binary->parent;
    binary->parent = NULL;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    assert(error.line == (size_t)binary->line);
    binary->parent = saved_parent;
    assert(ast_validate(program, &error));

    ASTNodeType saved_type = binary->type;
    binary->type = (ASTNodeType)AST_NODE_TYPE_COUNT;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->type = saved_type;
    assert(ast_validate(program, &error));

    ASTOperatorKind saved_operator = binary->operator_kind;
    binary->operator_kind = AST_OPERATOR_NONE;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->operator_kind = saved_operator;
    assert(binary->left_operand == binary->children[0]);
    assert(binary->right_operand == binary->children[1]);
    assert(ast_validate(program, &error));

    size_t saved_end = binary->end_offset;
    binary->end_offset = binary->start_offset - 1;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->end_offset = saved_end;
    assert(ast_validate(program, &error));

    ASTNode *saved_second = binary->children[1];
    binary->children[1] = binary->children[0];
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->children[1] = saved_second;
    assert(ast_validate(program, &error));

    size_t saved_capacity = binary->child_capacity;
    binary->child_capacity = binary->child_count - 1;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    binary->child_capacity = saved_capacity;
    assert(ast_validate(program, &error));

    ASTNode *saved_root_parent = program->parent;
    program->parent = analysis;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    program->parent = saved_root_parent;

    /* AST spans preserve lexer coordinates above INT_MAX without narrowing. */
    Token wide_start = {0};
    Token wide_end = {0};
    size_t wide_position = (size_t)INT_MAX + (size_t)1;
    wide_start.line = wide_position;
    wide_start.column = wide_position;
    wide_start.start_offset = 9;
    wide_end.end_line = wide_position;
    wide_end.end_column = wide_position + (size_t)1;
    wide_end.end_offset = 10;
    ASTNode *wide_span = ast_create(AST_EXPRESION_LITERAL);
    assert(wide_span != NULL);
    assert(ast_set_source_span(wide_span, &wide_start, &wide_end));
    assert(wide_span->line == wide_position);
    assert(wide_span->column == wide_position);
    assert(wide_span->end_line == wide_position);
    assert(wide_span->end_column == wide_position + (size_t)1);
    ast_destroy(wide_span);

    ast_destroy(program);
    parser_release(&parser);

    const char *filter_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".filtrar { #condicion(\"total >= 10\") } }";
    lexer_init(&lexer, filter_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program != NULL && !parser.has_error);
    ASTNode *filter = program->children[0]->children[1];
    ASTNode *condition = filter->children[0];
    assert(condition->type == AST_COMANDO_CONDICION);
    assert(condition->has_filter_predicate);
    assert(condition->filter_column != condition->value);
    assert(strcmp(condition->filter_column, "total") == 0);
    assert(condition->filter_operator == AST_OPERATOR_GREATER_EQUAL);
    assert(condition->filter_threshold == 10.0);
    assert(condition->has_source_span &&
           condition->end_offset > condition->start_offset);
    assert(ast_validate(program, &error));

    const char *predicates[] = {
        "a == 2", "a != 2", "a > 2", "a >= 2", "a < 2", "a <= 2"
    };
    const ASTOperatorKind operators[] = {
        AST_OPERATOR_EQUAL, AST_OPERATOR_NOT_EQUAL, AST_OPERATOR_GREATER,
        AST_OPERATOR_GREATER_EQUAL, AST_OPERATOR_LESS,
        AST_OPERATOR_LESS_EQUAL
    };
    for (size_t i = 0; i < sizeof(predicates) / sizeof(predicates[0]); ++i) {
        ASTNode *predicate = ast_create_leaf(AST_COMANDO_CONDICION, predicates[i]);
        assert(predicate != NULL);
        assert(ast_set_filter_predicate(predicate, predicate->value) ==
               AST_FILTER_PREDICATE_OK);
        assert(predicate->filter_operator == operators[i] &&
               predicate->filter_threshold == 2.0 &&
               strcmp(predicate->filter_column, "a") == 0);
        assert(ast_validate(predicate, &error));
        ast_destroy(predicate);
    }

    ASTNode *malformed = ast_create_leaf(AST_COMANDO_CONDICION, "total =~ 10");
    assert(malformed != NULL);
    assert(ast_set_filter_predicate(malformed, malformed->value) ==
           AST_FILTER_PREDICATE_INVALID);
    assert(!malformed->has_filter_predicate && malformed->filter_column == NULL);
    assert(ast_validate(malformed, &error));
    ast_destroy(malformed);

    /* Invalid reparsing leaves the previous owned, structured value untouched. */
    assert(ast_set_filter_predicate(condition, "total =~ 10") ==
           AST_FILTER_PREDICATE_INVALID);
    assert(condition->has_filter_predicate &&
           strcmp(condition->filter_column, "total") == 0 &&
           condition->filter_operator == AST_OPERATOR_GREATER_EQUAL &&
           condition->filter_threshold == 10.0);
    ASTOperatorKind saved_filter_operator = condition->filter_operator;
    condition->filter_operator = AST_OPERATOR_NONE;
    assert(!ast_validate(program, &error));
    assert(error.code == MILENA_ERR_ARGUMENT && error.line > 0);
    condition->filter_operator = saved_filter_operator;
    assert(ast_validate(program, &error));

    ast_destroy(program);
    parser_release(&parser);

    /* Canonical group/summary metrics carry an owned typed payload and call span. */
    const char *aggregate_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".agrupar dataset { #por(\"ciudad\") #suma(\"precio\") } "
        ".resumir dataset { #media(\"precio\") } }";
    lexer_init(&lexer, aggregate_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program != NULL && !parser.has_error);
    ASTNode *sum_metric = find_metric(program, AST_AGGREGATE_OPERATION_SUM);
    ASTNode *mean_metric = find_metric(program, AST_AGGREGATE_OPERATION_MEAN);
    assert(sum_metric && mean_metric);
    assert(sum_metric->has_aggregate_metric && sum_metric->aggregate_column &&
           strcmp(sum_metric->aggregate_column, "precio") == 0 &&
           strcmp(sum_metric->value, "suma:precio") == 0);
    assert(sum_metric->aggregate_column != sum_metric->value);
    assert(sum_metric->has_source_span &&
           strncmp(aggregate_source + sum_metric->start_offset,
                   "suma(\"precio\")", sum_metric->end_offset -
                       sum_metric->start_offset) == 0 &&
           sum_metric->end_offset - sum_metric->start_offset ==
               strlen("suma(\"precio\")"));
    assert(mean_metric->has_source_span &&
           mean_metric->aggregate_operation == AST_AGGREGATE_OPERATION_MEAN);
    assert(ast_validate(program, &error));
    ASTAggregateOperation saved_aggregate_operation = sum_metric->aggregate_operation;
    sum_metric->aggregate_operation = AST_AGGREGATE_OPERATION_MAX;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT);
    sum_metric->aggregate_operation = saved_aggregate_operation;
    assert(ast_validate(program, &error));
    ast_destroy(program);
    parser_release(&parser);

    ASTNode *typed_metric = ast_create_leaf(AST_RESUMEN_METRICA, "conteo:ciudad");
    assert(typed_metric && ast_set_aggregate_metric(typed_metric,
           AST_AGGREGATE_OPERATION_COUNT, "ciudad"));
    assert(ast_validate(typed_metric, &error));
    ast_destroy(typed_metric); /* frees the independently owned aggregate column */

    /* Natural-language stream summaries retain their distinct legacy payload. */
    ASTNode *stream_metric = ast_create_leaf(AST_RESUMEN_METRICA, "importe");
    assert(stream_metric);
    stream_metric->stream_operation = AST_STREAM_OPERATION_SUM;
    assert(!stream_metric->has_aggregate_metric && !stream_metric->aggregate_column &&
           ast_validate(stream_metric, &error));
    ast_destroy(stream_metric);

    assert(!ast_validate(NULL, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    return 0;
}
