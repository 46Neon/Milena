#include "parser.h"

#include <assert.h>
#include <stdio.h>
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
    ASTNode *group_key_node = program->children[0]->children[1]->children[0];
    const char *group_key_text = "#por(\"ciudad\")";
    assert(group_key_node->group_key.present && group_key_node->group_key.name &&
           group_key_node->group_key.name != group_key_node->value &&
           strcmp(group_key_node->group_key.name, "ciudad") == 0 &&
           group_key_node->has_source_span &&
           group_key_node->end_offset - group_key_node->start_offset ==
               strlen(group_key_text) &&
           strncmp(aggregate_source + group_key_node->start_offset,
                   group_key_text, strlen(group_key_text)) == 0);
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
    char saved_group_key_char = group_key_node->value[0];
    group_key_node->value[0] = saved_group_key_char == 'c' ? 'x' : 'c';
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)group_key_node->line);
    group_key_node->value[0] = saved_group_key_char;
    bool saved_group_key_presence = group_key_node->group_key.present;
    group_key_node->group_key.present = false;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT);
    group_key_node->group_key.present = saved_group_key_presence;
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

    /* Canonical source, schema column and export nodes carry owned typed data
     * and exact token-derived spans, while preserving their legacy mirrors. */
    const char *structured_data_source =
        ".analisis data {\n"
        " dataset cargar datos(\"entrada.csv\")\n"
        " variable importe numerica;\n"
        " .exportar { (\"salida.csv\") }\n"
        "}\n";
    lexer_init(&lexer, structured_data_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program && !parser.has_error);
    ASTNode *typed_analysis = program->children[0];
    ASTNode *typed_source_node = typed_analysis->children[0];
    ASTNode *typed_column_node = typed_analysis->children[1];
    ASTNode *typed_export_node = typed_analysis->children[2];
    const char *source_span_text = "dataset cargar datos(\"entrada.csv\")";
    const char *column_span_text = "variable importe numerica;";
    const char *export_span_text = "exportar { (\"salida.csv\") }";
    assert(typed_source_node->data_source.present && typed_source_node->data_source.path &&
           typed_source_node->data_source.path != typed_source_node->value &&
           strcmp(typed_source_node->data_source.path, "entrada.csv") == 0 &&
           typed_source_node->has_source_span &&
           typed_source_node->end_offset - typed_source_node->start_offset == strlen(source_span_text) &&
           strncmp(structured_data_source + typed_source_node->start_offset,
                   source_span_text, strlen(source_span_text)) == 0);
    assert(typed_column_node->data_column.present && typed_column_node->data_column.name &&
           typed_column_node->data_column.name != typed_column_node->value &&
           strcmp(typed_column_node->data_column.name, "importe") == 0 &&
           typed_column_node->data_column.type == AST_DATA_COLUMN_TYPE_NUMERIC &&
           typed_column_node->has_source_span &&
           typed_column_node->end_offset - typed_column_node->start_offset == strlen(column_span_text) &&
           strncmp(structured_data_source + typed_column_node->start_offset,
                   column_span_text, strlen(column_span_text)) == 0);
    assert(typed_export_node->export_result.present &&
           typed_export_node->export_result.destination &&
           typed_export_node->export_result.destination != typed_export_node->value &&
           strcmp(typed_export_node->export_result.destination, "salida.csv") == 0 &&
           typed_export_node->has_source_span &&
           typed_export_node->end_offset - typed_export_node->start_offset ==
               strlen(export_span_text) &&
           strncmp(structured_data_source + typed_export_node->start_offset,
                   export_span_text, strlen(export_span_text)) == 0);
    assert(ast_validate(program, &error));

    char saved_path_char = typed_source_node->value[0];
    typed_source_node->value[0] = saved_path_char == 'e' ? 'X' : 'e';
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)typed_source_node->line);
    typed_source_node->value[0] = saved_path_char;
    ASTDataColumnType saved_column_type = typed_column_node->data_column.type;
    typed_column_node->data_column.type = AST_DATA_COLUMN_TYPE_UNSPECIFIED;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)typed_column_node->line);
    typed_column_node->data_column.type = saved_column_type;
    typed_export_node->export_result.present = false;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)typed_export_node->line);
    typed_export_node->export_result.present = true;
    assert(ast_validate(program, &error));
    ast_destroy(program); /* releases both typed payloads and legacy strings */
    parser_release(&parser);

    /* Old AST clients may still build an untyped node, but it is not an HIR payload. */
    ASTNode *legacy_source = ast_create_leaf(AST_LLAMADA_CARGAR, "legacy.csv");
    assert(legacy_source && ast_validate(legacy_source, &error));
    ast_destroy(legacy_source);

    const char *join_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".unir { #derecha(\"catalogo.csv\") #clave(\"id\") "
        "#limites(4096, 20) } }";
    lexer_init(&lexer, join_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program && !parser.has_error);
    ASTNode *join = program->children[0]->children[1];
    assert(join->type == AST_BLOQUE_UNIR && join->join_limits_explicit &&
           join->join_memory_budget_bytes == 4096 &&
           join->join_max_output_rows == 20 && join->child_count == 2);
    ASTNode *right_source = join->children[0];
    ASTNode *join_key = join->children[1];
    const char *right_text = "#derecha(\"catalogo.csv\")";
    const char *key_text = "#clave(\"id\")";
    assert(right_source->join_right.present && right_source->join_right.path &&
           right_source->join_right.path != right_source->value &&
           strcmp(right_source->join_right.path, "catalogo.csv") == 0 &&
           right_source->has_source_span &&
           right_source->end_offset - right_source->start_offset ==
               strlen(right_text) &&
           strncmp(join_source + right_source->start_offset, right_text,
                   strlen(right_text)) == 0);
    assert(join_key->join_key.present && join_key->join_key.name &&
           join_key->join_key.name != join_key->value &&
           strcmp(join_key->join_key.name, "id") == 0 &&
           join_key->has_source_span &&
           join_key->end_offset - join_key->start_offset == strlen(key_text) &&
           strncmp(join_source + join_key->start_offset, key_text,
                   strlen(key_text)) == 0);
    assert(ast_validate(program, &error));
    char saved_right_char = right_source->value[0];
    right_source->value[0] = saved_right_char == 'c' ? 'X' : 'c';
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)right_source->line);
    right_source->value[0] = saved_right_char;
    bool saved_key_presence = join_key->join_key.present;
    join_key->join_key.present = false;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)join_key->line);
    join_key->join_key.present = saved_key_presence;
    assert(ast_validate(program, &error));
    ast_destroy(program);
    parser_release(&parser);

    /* #total owns typed product operands and spans the whole command. */
    const char *product_source =
        ".analisis ventas { .transformar dataset { #total(\"precio * cantidad\") } }";
    lexer_init(&lexer, product_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program && !parser.has_error);
    ASTNode *transform = program->children[0]->children[0];
    ASTNode *product = transform->children[0];
    const char *product_command = "#total(\"precio * cantidad\")";
    assert(product->type == AST_COMANDO_TOTAL && product->data_product.present &&
           product->data_product.left_column && product->data_product.right_column &&
           product->data_product.left_column != product->value &&
           product->data_product.right_column != product->value &&
           strcmp(product->data_product.left_column, "precio") == 0 &&
           strcmp(product->data_product.right_column, "cantidad") == 0 &&
           product->has_source_span && product->end_offset - product->start_offset ==
               strlen(product_command) &&
           strncmp(product_source + product->start_offset, product_command,
                   strlen(product_command)) == 0);
    assert(ast_validate(program, &error));
    char saved_product_char = product->value[0];
    product->value[0] = saved_product_char == '#' ? 'X' : '#';
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)product->line);
    product->value[0] = saved_product_char;
    bool saved_product_presence = product->data_product.present;
    product->data_product.present = false;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)product->line);
    product->data_product.present = saved_product_presence;
    assert(ast_validate(program, &error));
    ast_destroy(program); /* releases the two independently owned product operands */
    parser_release(&parser);

    ASTNode *owned_product = ast_create_leaf(AST_COMANDO_TOTAL, "left * right");
    assert(owned_product && ast_set_data_product(owned_product, owned_product->value));
    assert(owned_product->data_product.left_column != owned_product->value &&
           owned_product->data_product.right_column != owned_product->value);
    ast_destroy(owned_product); /* cleanup for both owned operand strings */

    const char *selection_source =
        ".analisis ventas { dataset cargar datos(\"entrada.csv\") "
        ".seleccionar { #columnas(\" id, total, ciudad \") } }";
    lexer_init(&lexer, selection_source);
    parser_init(&parser, &lexer);
    program = parser_parse(&parser);
    assert(program && !parser.has_error);
    ASTNode *selection = program->children[0]->children[1];
    ASTNode *columns = selection->children[0];
    const char *selection_text = "#columnas(\" id, total, ciudad \")";
    assert(columns->column_selection.present &&
           columns->column_selection.count == 3u &&
           strcmp(columns->column_selection.names[0], "id") == 0 &&
           strcmp(columns->column_selection.names[1], "total") == 0 &&
           strcmp(columns->column_selection.names[2], "ciudad") == 0 &&
           columns->has_source_span &&
           columns->end_offset - columns->start_offset == strlen(selection_text) &&
           strncmp(selection_source + columns->start_offset, selection_text,
                   strlen(selection_text)) == 0 && ast_validate(program, &error));
    char saved_selection_char = columns->value[1];
    columns->value[1] = 'x';
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT &&
           error.line == (size_t)columns->line);
    columns->value[1] = saved_selection_char;
    bool saved_selection_presence = columns->column_selection.present;
    columns->column_selection.present = false;
    assert(!ast_validate(program, &error) && error.code == MILENA_ERR_ARGUMENT);
    columns->column_selection.present = saved_selection_presence;
    assert(ast_validate(program, &error));
    ast_destroy(program);
    parser_release(&parser);

    ASTNode *bad_columns = ast_create_leaf(AST_COMANDO_COLUMNAS, "a, a");
    assert(bad_columns && !ast_set_column_selection(bad_columns, bad_columns->value));
    ast_destroy(bad_columns);
    bad_columns = ast_create_leaf(AST_COMANDO_COLUMNAS, "a,");
    assert(bad_columns && !ast_set_column_selection(bad_columns, bad_columns->value));
    ast_destroy(bad_columns);
    char long_column[129];
    memset(long_column, 'x', sizeof(long_column) - 1u);
    long_column[sizeof(long_column) - 1u] = '\0';
    bad_columns = ast_create_leaf(AST_COMANDO_COLUMNAS, long_column);
    assert(bad_columns && !ast_set_column_selection(bad_columns, bad_columns->value));
    ast_destroy(bad_columns);
    char columns_32[512] = {0};
    size_t used = 0u;
    for (size_t i = 0u; i < 32u; ++i) {
        int count_written = snprintf(columns_32 + used, sizeof(columns_32) - used,
                                     "%scol%zu", i ? "," : "", i);
        assert(count_written > 0 &&
               (size_t)count_written < sizeof(columns_32) - used);
        used += (size_t)count_written;
    }
    bad_columns = ast_create_leaf(AST_COMANDO_COLUMNAS, columns_32);
    assert(bad_columns && ast_set_column_selection(bad_columns, bad_columns->value) &&
           bad_columns->column_selection.count == 32u);
    ast_destroy(bad_columns);
    int count_written = snprintf(columns_32 + used, sizeof(columns_32) - used,
                                 ",col32");
    assert(count_written > 0 && (size_t)count_written < sizeof(columns_32) - used);
    bad_columns = ast_create_leaf(AST_COMANDO_COLUMNAS, columns_32);
    assert(bad_columns && !ast_set_column_selection(bad_columns, bad_columns->value));
    ast_destroy(bad_columns);

    const char *cleanup_source =
        ".analisis demo { .limpiar dataset { #nulos(\"eliminar\") "
        "#duplicados(\"eliminar\") #nulos(\"rellenar\") } }";
    lexer_init(&lexer, cleanup_source);
    parser_init(&parser, &lexer);
    ASTNode *cleanup_program = parser_parse(&parser);
    assert(cleanup_program && !parser.has_error &&
           cleanup_program->child_count == 1u &&
           cleanup_program->children[0]->child_count == 1u);
    ASTNode *cleanup_block = cleanup_program->children[0]->children[0];
    assert(cleanup_block->type == AST_BLOQUE_LIMPIAR &&
           cleanup_block->child_count == 3u);
    ASTNode *cleanup_nulls = cleanup_block->children[0];
    ASTNode *cleanup_duplicates = cleanup_block->children[1];
    ASTNode *cleanup_unsupported = cleanup_block->children[2];
    const char *nulls_command_text = "#nulos(\"eliminar\")";
    const char *duplicates_command_text = "#duplicados(\"eliminar\")";
    const char *unsupported_command_text = "#nulos(\"rellenar\")";
    assert(cleanup_nulls->data_cleanup.present &&
           cleanup_nulls->data_cleanup.action == AST_DATA_CLEANUP_ACTION_REMOVE &&
           cleanup_nulls->has_source_span &&
           cleanup_nulls->end_offset - cleanup_nulls->start_offset ==
               strlen(nulls_command_text) &&
           strncmp(cleanup_source + cleanup_nulls->start_offset,
                   nulls_command_text, strlen(nulls_command_text)) == 0);
    assert(cleanup_duplicates->data_cleanup.present &&
           cleanup_duplicates->data_cleanup.action ==
               AST_DATA_CLEANUP_ACTION_REMOVE &&
           cleanup_duplicates->has_source_span &&
           cleanup_duplicates->end_offset - cleanup_duplicates->start_offset ==
               strlen(duplicates_command_text) &&
           strncmp(cleanup_source + cleanup_duplicates->start_offset,
                   duplicates_command_text, strlen(duplicates_command_text)) == 0);
    assert(!cleanup_unsupported->data_cleanup.present &&
           cleanup_unsupported->data_cleanup.action ==
               AST_DATA_CLEANUP_ACTION_NONE &&
           strcmp(cleanup_unsupported->value, "rellenar") == 0 &&
           cleanup_unsupported->has_source_span &&
           cleanup_unsupported->end_offset - cleanup_unsupported->start_offset ==
               strlen(unsupported_command_text) &&
           strncmp(cleanup_source + cleanup_unsupported->start_offset,
                   unsupported_command_text, strlen(unsupported_command_text)) == 0 &&
           ast_validate(cleanup_program, &error));
    char cleanup_mirror = cleanup_nulls->value[0];
    cleanup_nulls->value[0] = cleanup_mirror == '#' ? 'X' : '#';
    assert(!ast_validate(cleanup_program, &error) &&
           error.code == MILENA_ERR_ARGUMENT);
    cleanup_nulls->value[0] = cleanup_mirror;
    cleanup_nulls->data_cleanup.present = false;
    assert(!ast_validate(cleanup_program, &error) &&
           error.code == MILENA_ERR_ARGUMENT);
    cleanup_nulls->data_cleanup.action = AST_DATA_CLEANUP_ACTION_NONE;
    assert(ast_validate(cleanup_program, &error));
    cleanup_nulls->data_cleanup.present = true;
    cleanup_nulls->data_cleanup.action = AST_DATA_CLEANUP_ACTION_REMOVE;
    assert(ast_validate(cleanup_program, &error));
    ast_destroy(cleanup_program); /* no cleanup payload owns memory */
    parser_release(&parser);

    ASTNode *unsupported_cleanup = ast_create_leaf(AST_COMANDO_NULOS, "rellenar");
    assert(unsupported_cleanup &&
           !ast_set_data_cleanup_action(unsupported_cleanup,
                                        AST_DATA_CLEANUP_ACTION_REMOVE));
    ast_destroy(unsupported_cleanup);
    ASTNode *wrong_kind_cleanup = ast_create_leaf(AST_COMANDO_TOTAL, "eliminar");
    assert(wrong_kind_cleanup &&
           !ast_set_data_cleanup_action(wrong_kind_cleanup,
                                        AST_DATA_CLEANUP_ACTION_REMOVE));
    ast_destroy(wrong_kind_cleanup);

    assert(!ast_validate(NULL, &error));
    assert(error.code == MILENA_ERR_ARGUMENT);
    return 0;
}

