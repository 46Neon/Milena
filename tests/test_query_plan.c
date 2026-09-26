#include "query_plan.h"
#include "canonical_compiler.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static ASTNode *leaf_with_value(ASTNodeType type, const char *value) {
    ASTNode *node = ast_create_leaf(type, value);
    assert(node);
    return node;
}

static ASTNode *stream_source(void) {
    ASTNode *source = leaf_with_value(AST_LLAMADA_CARGAR, "datos.csv");
    source->type_name = milena_strdup("flujo");
    assert(source->type_name);
    source->stream_chunk_rows = 4096;
    source->stream_row_limit = 1000000;
    return source;
}

static ASTNode *summary_block(void) {
    ASTNode *summary = ast_create(AST_BLOQUE_RESUMIR);
    ASTNode *metric = leaf_with_value(AST_RESUMEN_METRICA, "importe");
    assert(summary);
    metric->stream_operation = AST_STREAM_OPERATION_SUM;
    assert(ast_add_child(summary, metric));
    return summary;
}

static ASTNode *new_analysis(void) {
    ASTNode *analysis = ast_create(AST_BLOQUE_ANALISIS);
    assert(analysis);
    assert(ast_add_child(analysis, stream_source()));
    assert(ast_add_child(analysis, leaf_with_value(AST_BLOQUE_EXPORTAR,
                                                    "reporte.json")));
    return analysis;
}

static void assert_record_safe_physical_plan(const MilenaStreamExecutionPlan *plan,
                                            MilenaError *error) {
    assert(plan->partition_count == 1u);
    assert(plan->worker_count == 1u);
    assert(plan->csv_record_safe);
    assert(!plan->parallel_enabled);
    assert(plan->physical_plan_reason != NULL);
    assert(plan->physical_operator_count >= 3u);
    assert(plan->physical_operator_count <= MILENA_STREAM_PLAN_MAX_OPERATORS);
    assert(plan->physical_operators[0] == MILENA_STREAM_PLAN_SCAN_CSV_RECORDS);
    assert(plan->physical_operators[plan->physical_operator_count - 1u] ==
           MILENA_STREAM_PLAN_JSON_SINK);
    assert(milena_stream_execution_plan_validate(plan, error) == MILENA_OK);
}

static void test_global_stream_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *summary = summary_block();
    assert(ast_add_child(analysis, summary));

    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.source == analysis->children[0]);
    assert(plan.sink == analysis->children[1]);
    assert(plan.summary == summary);
    assert(plan.group == NULL && plan.group_key == NULL);
    assert(plan.logical_operator_count == 3);
    assert(plan.logical_operators[0] == MILENA_LOGICAL_CSV_SCAN);
    assert(plan.logical_operators[1] == MILENA_LOGICAL_GLOBAL_AGGREGATE);
    assert(plan.logical_operators[2] == MILENA_LOGICAL_JSON_REPORT);
    assert(plan.physical_operator == MILENA_PHYSICAL_CSV_STREAM_SUMMARY);
    assert(plan.physical_operator_count == 3u);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_SUMMARY_AGGREGATE);
    assert_record_safe_physical_plan(&plan, &error);
    MilenaStreamExecutionPlan unsafe = plan;
    unsafe.parallel_enabled = true;
    assert(milena_stream_execution_plan_validate(&unsafe, &error) ==
           MILENA_ERR_UNSUPPORTED);
    unsafe = plan;
    unsafe.csv_record_safe = false;
    assert(milena_stream_execution_plan_validate(&unsafe, &error) ==
           MILENA_ERR_UNSUPPORTED);
    ast_destroy(analysis);
}

static void test_grouped_stream_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    assert(ast_add_child(group, summary_block()));
    assert(ast_add_child(analysis, group));

    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.group == group);
    assert(plan.group_key == group->children[0]);
    assert(plan.group_summary == group->children[1]);
    assert(plan.logical_operators[1] == MILENA_LOGICAL_GROUP_AGGREGATE);
    assert(plan.physical_operator == MILENA_PHYSICAL_CSV_STREAM_GROUPED);
    assert(plan.physical_operator_count == 4u);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_GROUPED_AGGREGATE);
    assert(plan.physical_operators[2] == MILENA_STREAM_PLAN_ORDER_BY_KEY);
    assert_record_safe_physical_plan(&plan, &error);
    ast_destroy(analysis);
}


static void test_grouped_spill_stream_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    ASTNode *summary = summary_block();
    ASTNode *mean = leaf_with_value(AST_RESUMEN_METRICA, "otra");
    mean->stream_operation = AST_STREAM_OPERATION_MEAN;
    assert(ast_add_child(summary, mean));
    assert(ast_add_child(group, summary));
    ASTNode *policy = leaf_with_value(AST_AGRUPACION_SPILL, "scratch.bin");
    policy->group_memory_budget_bytes = 4096;
    policy->group_spill_quota_bytes = 1024 * 1024;
    policy->group_max_key_bytes = 128;
    policy->group_max_output_groups = 100;
    policy->group_max_output_bytes = 1073741824u;
    policy->group_max_runs = 4096u;
    policy->group_output_limit_explicit = true;
    assert(ast_add_child(group, policy));
    assert(ast_add_child(analysis, group));
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.group == group && plan.spill_policy == policy);
    assert(plan.group_key == group->children[0]);
    assert(plan.physical_operator == MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL);
    assert(plan.physical_operator_count == 5u);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_GROUPED_SPILL);
    assert(plan.physical_operators[2] == MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES);
    assert(plan.physical_operators[3] == MILENA_STREAM_PLAN_ORDER_BY_KEY);
    assert_record_safe_physical_plan(&plan, &error);
    MilenaStreamExecutionPlan unsafe = plan;
    unsafe.physical_operators[2] = MILENA_STREAM_PLAN_ORDER_BY_KEY;
    assert(milena_stream_execution_plan_validate(&unsafe, &error) == MILENA_ERR_DATA);
    ast_destroy(analysis);
}

static void test_composite_grouped_spill_stream_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "segmento")));
    ASTNode *summary = summary_block();
    ASTNode *mean = leaf_with_value(AST_RESUMEN_METRICA, "valor");
    mean->stream_operation = AST_STREAM_OPERATION_MEAN;
    assert(ast_add_child(summary, mean));
    assert(ast_add_child(group, summary));
    ASTNode *policy = leaf_with_value(AST_AGRUPACION_SPILL, "scratch.bin");
    policy->group_memory_budget_bytes = 4096;
    policy->group_spill_quota_bytes = 1024 * 1024;
    policy->group_max_key_bytes = 128;
    policy->group_max_output_groups = 100;
    policy->group_max_output_bytes = 1073741824u;
    policy->group_max_runs = 4096u;
    assert(ast_add_child(group, policy));
    assert(ast_add_child(analysis, group));
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.group == group && plan.spill_policy == policy);
    assert(plan.group_key_count == 2);
    assert(plan.group_keys[0] == group->children[0]);
    assert(plan.group_keys[1] == group->children[1]);
    assert(plan.group_key == plan.group_keys[0]);
    assert(plan.physical_operator == MILENA_PHYSICAL_CSV_STREAM_GROUPED_SPILL);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_GROUPED_SPILL);
    assert(plan.physical_operators[2] == MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES);
    assert_record_safe_physical_plan(&plan, &error);
    ast_destroy(analysis);

    analysis = new_analysis();
    group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "segmento")));
    assert(ast_add_child(group, summary_block()));
    assert(ast_add_child(analysis, group));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) ==
           MILENA_ERR_UNSUPPORTED);
    ast_destroy(analysis);
}

static void test_grouped_spill_rejects_unsupported_metric(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    ASTNode *summary = summary_block();
    summary->children[0]->stream_operation = AST_STREAM_OPERATION_VARIANCE;
    assert(ast_add_child(group, summary));
    ASTNode *policy = leaf_with_value(AST_AGRUPACION_SPILL, "scratch.bin");
    policy->group_memory_budget_bytes = 4096;
    policy->group_spill_quota_bytes = 1024 * 1024;
    policy->group_max_key_bytes = 128;
    policy->group_max_output_groups = 100;
    policy->group_max_output_bytes = 1073741824u;
    policy->group_max_runs = 4096u;
    assert(ast_add_child(group, policy));
    assert(ast_add_child(analysis, group));
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) ==
           MILENA_ERR_UNSUPPORTED);
    ast_destroy(analysis);
}

static void test_filtered_grouped_stream_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *filter = leaf_with_value(AST_STREAM_FILTER, "region");
    filter->type_name = milena_strdup("North");
    assert(filter->type_name);
    assert(ast_add_child(analysis, filter));
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    assert(ast_add_child(group, summary_block()));
    assert(ast_add_child(analysis, group));

    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.filter == filter && plan.group == group);
    assert(plan.logical_operator_count == 4);
    assert(plan.logical_operators[0] == MILENA_LOGICAL_CSV_SCAN);
    assert(plan.logical_operators[1] == MILENA_LOGICAL_FILTER);
    assert(plan.logical_operators[2] == MILENA_LOGICAL_GROUP_AGGREGATE);
    assert(plan.logical_operators[3] == MILENA_LOGICAL_JSON_REPORT);
    assert(plan.physical_operator_count == 5u);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_FILTER);
    assert(plan.physical_operators[2] == MILENA_STREAM_PLAN_GROUPED_AGGREGATE);
    assert_record_safe_physical_plan(&plan, &error);
    ast_destroy(analysis);

    analysis = new_analysis();
    filter = leaf_with_value(AST_STREAM_FILTER, "region");
    filter->type_name = milena_strdup("North");
    assert(filter->type_name && ast_add_child(analysis, filter));
    ASTNode *duplicate = leaf_with_value(AST_STREAM_FILTER, "region");
    duplicate->type_name = milena_strdup("South");
    assert(duplicate->type_name && ast_add_child(analysis, duplicate));
    assert(ast_add_child(analysis, summary_block()));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_ERR_PARSE);
    ast_destroy(analysis);
}

static void test_numeric_filter_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *filter = leaf_with_value(AST_STREAM_FILTER, "importe");
    filter->stream_filter_kind = AST_STREAM_FILTER_NUMERIC_GREATER;
    filter->number_value = 10.0;
    assert(ast_add_child(analysis, filter));
    assert(ast_add_child(analysis, summary_block()));
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.filter == filter && plan.filter->number_value == 10.0 &&
           plan.filter->stream_filter_kind == AST_STREAM_FILTER_NUMERIC_GREATER &&
           plan.filter->type_name == NULL);
    assert(plan.logical_operators[0] == MILENA_LOGICAL_CSV_SCAN);
    assert(plan.logical_operators[1] == MILENA_LOGICAL_FILTER);
    assert(plan.logical_operators[2] == MILENA_LOGICAL_GLOBAL_AGGREGATE);
    assert(plan.physical_operator_count == 4u);
    assert(plan.physical_operators[1] == MILENA_STREAM_PLAN_FILTER);
    assert(plan.physical_operators[2] == MILENA_STREAM_PLAN_SUMMARY_AGGREGATE);
    assert_record_safe_physical_plan(&plan, &error);
    ast_destroy(analysis);

    analysis = new_analysis();
    filter = leaf_with_value(AST_STREAM_FILTER, "importe");
    filter->stream_filter_kind = AST_STREAM_FILTER_NUMERIC_GREATER;
    filter->number_value = NAN;
    assert(ast_add_child(analysis, filter));
    assert(ast_add_child(analysis, summary_block()));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_ERR_PARSE);
    ast_destroy(analysis);
}

static void test_legacy_global_summary_plan(void) {
    ASTNode *analysis = new_analysis();
    ASTNode *summary = ast_create(AST_BLOQUE_RESUMIR);
    assert(summary);
    assert(ast_add_child(summary, leaf_with_value(AST_RESUMEN_METRICA,
                                                  "suma:importe")));
    assert(ast_add_child(analysis, summary));
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_OK);
    assert(plan.physical_operator == MILENA_PHYSICAL_CSV_STREAM_SUMMARY);
    ast_destroy(analysis);
}

static void test_ambiguous_and_unsupported_plans(void) {
    MilenaStreamExecutionPlan plan;
    MilenaError error = {0};
    ASTNode *analysis = new_analysis();
    assert(ast_add_child(analysis, summary_block()));
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "region")));
    assert(ast_add_child(group, summary_block()));
    assert(ast_add_child(analysis, group));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_ERR_PARSE);
    ast_destroy(analysis);

    analysis = new_analysis();
    assert(ast_add_child(analysis, summary_block()));
    assert(ast_add_child(analysis, ast_create(AST_BLOQUE_LIMPIAR)));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) ==
           MILENA_ERR_UNSUPPORTED);
    ast_destroy(analysis);

    analysis = ast_create(AST_BLOQUE_ANALISIS);
    assert(analysis);
    assert(ast_add_child(analysis, summary_block()));
    assert(milena_stream_execution_plan_build(analysis, &plan, &error) == MILENA_ERR_PARSE);
    ast_destroy(analysis);
}

static ASTNode *typed_declaration(const char *name, const char *type_name) {
    ASTNode *node = leaf_with_value(AST_DECLARACION_VARIABLE, name);
    node->type_name = milena_strdup(type_name);
    assert(node->type_name);
    return node;
}

static double test_table_numeric_value(const MilenaTable *table,
                                       const char *column, size_t row) {
    int index = milena_table_column_index(table, column);
    assert(index >= 0);
    const void *raw = NULL;
    assert(milena_table_get_array_value(table, (size_t)index, row, &raw, NULL) ==
           MILENA_OK);
    const MilenaTableColumn *data =
        milena_table_column(table, (size_t)index);
    assert(data && raw);
    if (data->values.dtype == MILENA_DTYPE_FLOAT64)
        return *(const double *)raw;
    if (data->values.dtype == MILENA_DTYPE_INT64)
        return (double)*(const int64_t *)raw;
    assert(data->values.dtype == MILENA_DTYPE_FLOAT64 ||
           data->values.dtype == MILENA_DTYPE_INT64);
    return 0.0;
}

static void test_materialized_executor_uses_common_plan(
    const MilenaDataOperatorPlan *source_plan, MilenaError *error) {
    const char *groups[] = {"same", "same", "same", "same"};
    const char *buckets[] = {"A", "B", "A", "B"};
    const double values[] = {3.0, 5.0, 6.0, 8.0};
    const size_t shape[] = {4u};
    MilenaArray numeric = {0};
    MilenaTable input = {0};
    MilenaTable output = {0};
    milena_array_init(&numeric);
    milena_table_init(&input);
    milena_table_init(&output);
    assert(milena_table_add_string_column_copy(&input, "grupo", groups, 4u,
                                               NULL, error) == MILENA_OK);
    assert(milena_table_add_string_column_copy(&input, "bucket", buckets, 4u,
                                               NULL, error) == MILENA_OK);
    assert(milena_array_from_f64(&numeric, 1u, shape, values, error) ==
           MILENA_OK);
    assert(milena_table_add_column_copy(&input, "other", &numeric, NULL,
                                        error) == MILENA_OK);
    milena_array_release(&numeric);

    /* Deliberately vary every materialized execution operand after building the
     * plan from HIR. The executor receives no HIR and must follow these plan
     * parameters, rather than re-discovering the original filter/key/metrics. */
    MilenaDataOperatorPlan execution_plan = *source_plan;
    execution_plan.filter_column = "other";
    execution_plan.filter_threshold = 4.0;
    execution_plan.group_key = "bucket";
    for (size_t i = 0; i < execution_plan.metric_count; ++i)
        execution_plan.metrics[i].input_column = "other";
    assert(milena_data_operator_plan_execute_materialized(
        &execution_plan, &input, 100u, 100u, 8u, &output, error) == MILENA_OK);
    assert(output.row_count == 2u && output.column_count == 4u);
    const char *key = NULL;
    assert(milena_table_get_string(&output, 0u, 0u, &key, error) == MILENA_OK);
    assert(strcmp(key, "B") == 0);
    assert(milena_table_get_string(&output, 0u, 1u, &key, error) == MILENA_OK);
    assert(strcmp(key, "A") == 0);
    assert(test_table_numeric_value(&output, "other_suma", 0u) == 13.0);
    assert(test_table_numeric_value(&output, "other_media", 0u) == 6.5);
    assert(test_table_numeric_value(&output, "other_conteo", 0u) == 2.0);
    assert(test_table_numeric_value(&output, "other_suma", 1u) == 6.0);
    assert(test_table_numeric_value(&output, "other_media", 1u) == 6.0);
    assert(test_table_numeric_value(&output, "other_conteo", 1u) == 1.0);

    /* A malformed operator order fails without replacing the previous result. */
    MilenaDataOperatorPlan malformed = execution_plan;
    malformed.operators[1] = MILENA_DATA_OPERATOR_GROUP_AGGREGATE;
    assert(milena_data_operator_plan_execute_materialized(
        &malformed, &input, 100u, 100u, 8u, &output, error) == MILENA_ERR_DATA);
    assert(output.row_count == 2u && output.column_count == 4u);
    milena_table_destroy(&output);
    milena_table_destroy(&input);
}

static void test_common_data_operator_overlap(void) {
    MilenaHIRDataOperation hir_operations[2] = {{0}};
    MilenaHIRAggregate hir_aggregates[3] = {{0}};
    MilenaDataHIR hir = {0};
    MilenaHIRColumnRef declared_columns[2] = {{0}};
    hir.source.path = "rows.csv";
    hir.source.max_memory_bytes = 4096u;
    hir.source.max_input_bytes = 8192u;
    hir.export_path = "memory.json";
    hir.schema_bound = true;
    declared_columns[0].name = "valor";
    declared_columns[0].type = MILENA_HIR_COLUMN_NUMERIC;
    declared_columns[0].declared_type = MILENA_HIR_COLUMN_NUMERIC;
    declared_columns[0].rank = 1u;
    declared_columns[1].name = "grupo";
    declared_columns[1].type = MILENA_HIR_COLUMN_TEXT;
    declared_columns[1].declared_type = MILENA_HIR_COLUMN_TEXT;
    declared_columns[1].rank = 1u;
    hir.declared_schema = declared_columns;
    hir.declared_column_count = 2u;
    hir.operation_count = 2u;
    hir.operations = hir_operations;
    hir_operations[0].kind = MILENA_HIR_DATA_FILTER_NUMERIC;
    hir_operations[0].as.filter.operation = AST_OPERATOR_GREATER;
    hir_operations[0].as.filter.threshold = 1.0;
    hir_operations[0].as.filter.column.name = "valor";
    hir_operations[0].as.filter.column.type = MILENA_HIR_COLUMN_NUMERIC;
    hir_operations[1].kind = MILENA_HIR_DATA_GROUP;
    hir_operations[1].as.group.key.name = "grupo";
    hir_operations[1].as.group.key.type = MILENA_HIR_COLUMN_TEXT;
    hir_operations[1].as.group.aggregate_count = 3u;
    hir_operations[1].as.group.aggregates = hir_aggregates;
    const MilenaAggregateOp table_operations[] = {
        MILENA_AGG_SUM, MILENA_AGG_MEAN, MILENA_AGG_COUNT
    };
    const char *metric_columns[] = {"valor", "valor", "valor"};
    for (size_t i = 0; i < 3u; ++i) {
        hir_aggregates[i].operation = table_operations[i];
        hir_aggregates[i].input.name = (char *)metric_columns[i];
        hir_aggregates[i].input.type = MILENA_HIR_COLUMN_NUMERIC;
    }
    MilenaDataOperatorPlan in_memory = {0};
    MilenaError error = {0};
    assert(milena_data_operator_plan_from_hir(&hir, &in_memory, &error) ==
           MILENA_OK);
    assert(in_memory.execution_mode ==
           MILENA_DATA_EXECUTION_MATERIALIZED_TABLE);
    assert(in_memory.source_max_memory_bytes == hir.source.max_memory_bytes);
    assert(in_memory.source_max_input_bytes == hir.source.max_input_bytes);
    assert(in_memory.operator_count == 4u && in_memory.has_numeric_greater_filter);
    test_materialized_executor_uses_common_plan(&in_memory, &error);

    MilenaDataOperatorPlan preflight = {0};
    assert(milena_data_operator_plan_preflight_from_hir(
        &hir, &preflight, &error) == MILENA_OK);
    assert(preflight.source_max_memory_bytes == hir.source.max_memory_bytes);
    assert(preflight.source_max_input_bytes == hir.source.max_input_bytes);
    assert(milena_data_operator_plan_check_bound_hir(
        &preflight, &hir, &error) == MILENA_OK);
    MilenaDataHIR changed_source = hir;
    changed_source.source.max_memory_bytes++;
    assert(milena_data_operator_plan_check_bound_hir(
        &preflight, &changed_source, &error) == MILENA_ERR_DATA);
    changed_source = hir;
    changed_source.source.max_input_bytes++;
    assert(milena_data_operator_plan_check_bound_hir(
        &preflight, &changed_source, &error) == MILENA_ERR_DATA);

    ASTNode *analysis = ast_create(AST_BLOQUE_ANALISIS);
    assert(analysis);
    assert(ast_add_child(analysis, typed_declaration("grupo", "texto")));
    assert(ast_add_child(analysis, typed_declaration("valor", "numerica")));
    ASTNode *source = leaf_with_value(AST_LLAMADA_CARGAR, "rows.csv");
    source->type_name = milena_strdup("flujo");
    assert(source->type_name);
    source->stream_chunk_rows = 4096;
    source->stream_row_limit = 1000000;
    assert(ast_add_child(analysis, source));
    ASTNode *filter = leaf_with_value(AST_STREAM_FILTER, "valor");
    filter->stream_filter_kind = AST_STREAM_FILTER_NUMERIC_GREATER;
    filter->number_value = 1.0;
    assert(ast_add_child(analysis, filter));
    ASTNode *group = ast_create(AST_BLOQUE_AGRUPAR);
    assert(group);
    assert(ast_add_child(group, leaf_with_value(AST_AGRUPACION_POR, "grupo")));
    ASTNode *summary = ast_create(AST_BLOQUE_RESUMIR);
    assert(summary);
    const ASTStreamOperation stream_operations[] = {
        AST_STREAM_OPERATION_SUM, AST_STREAM_OPERATION_MEAN,
        AST_STREAM_OPERATION_COUNT
    };
    for (size_t i = 0; i < 3u; ++i) {
        ASTNode *metric = leaf_with_value(AST_RESUMEN_METRICA, "valor");
        metric->stream_operation = stream_operations[i];
        assert(ast_add_child(summary, metric));
    }
    assert(ast_add_child(group, summary));
    assert(ast_add_child(analysis, group));
    assert(ast_add_child(analysis, leaf_with_value(AST_BLOQUE_EXPORTAR,
                                                    "stream.json")));
    MilenaStreamExecutionPlan stream_plan = {0};
    assert(milena_stream_execution_plan_build(analysis, &stream_plan, &error) ==
           MILENA_OK);
    MilenaDataOperatorPlan streaming = {0};
    assert(milena_data_operator_plan_from_stream(analysis, &stream_plan,
                                                  &streaming, &error) ==
           MILENA_OK);
    assert(streaming.execution_mode == MILENA_DATA_EXECUTION_CSV_RECORD_STREAM);
    /* Resource caps are explicit preflight identity, not operator semantics. */
    streaming.source_max_memory_bytes = 7u;
    streaming.source_max_input_bytes = 9u;
    assert(milena_data_operator_plans_same_logic(&in_memory, &streaming));
    assert(strcmp(in_memory.source_path, streaming.source_path) == 0);
    assert(strcmp(in_memory.sink_path, "memory.json") == 0 &&
           strcmp(streaming.sink_path, "stream.json") == 0);

    summary->children[0]->stream_operation = AST_STREAM_OPERATION_MIN;
    assert(milena_stream_execution_plan_build(analysis, &stream_plan, &error) ==
           MILENA_OK);
    assert(milena_data_operator_plan_from_stream(analysis, &stream_plan,
        &streaming, &error) == MILENA_ERR_UNSUPPORTED);
    summary->children[0]->stream_operation = AST_STREAM_OPERATION_SUM;
    assert(milena_stream_execution_plan_build(analysis, &stream_plan, &error) ==
           MILENA_OK);
    assert(milena_data_operator_plan_from_stream(analysis, &stream_plan,
        &streaming, &error) == MILENA_OK);

    MilenaDataOperatorPlan malformed = streaming;
    malformed.operators[1] = MILENA_DATA_OPERATOR_GROUP_AGGREGATE;
    assert(milena_data_operator_plan_validate(&malformed, &error) ==
           MILENA_ERR_DATA);
    ast_destroy(analysis);

    hir_aggregates[0].operation = MILENA_AGG_MIN;
    assert(milena_data_operator_plan_from_hir(&hir, &in_memory, &error) ==
           MILENA_ERR_UNSUPPORTED);
}

static void test_shared_text_scan_filter_project_plan(void) {
    static const MilenaSharedQueryProjection projections[] = {
        {"label", MILENA_SHARED_QUERY_VALUE_TEXT},
        {"amount", MILENA_SHARED_QUERY_VALUE_NUMERIC}
    };
    MilenaSharedQueryPlan arrow_plan = {0};
    MilenaSharedQueryPlan sqlite_plan = {0};
    MilenaError error;
    assert(milena_shared_query_plan_build(
        "local.arrow", true, "tag", "match", projections, 2u,
        MILENA_SHARED_QUERY_SINK_ARROW_IPC_STREAM, &arrow_plan, &error) ==
        MILENA_OK);
    assert(milena_shared_query_plan_build(
        "records", true, "tag", "match", projections, 2u,
        MILENA_SHARED_QUERY_SINK_SQLITE_RESULT, &sqlite_plan, &error) ==
        MILENA_OK);
    assert(arrow_plan.operator_count == 4u &&
           arrow_plan.operators[0] == MILENA_SHARED_QUERY_SCAN &&
           arrow_plan.operators[1] == MILENA_SHARED_QUERY_TEXT_EQUAL_FILTER &&
           arrow_plan.operators[2] == MILENA_SHARED_QUERY_PROJECT &&
           arrow_plan.operators[3] == MILENA_SHARED_QUERY_RESULT &&
           arrow_plan.preserve_source_order &&
           arrow_plan.filter_text_length == strlen("match") &&
           arrow_plan.projections[0].name == projections[0].name &&
           arrow_plan.projections[1].name == projections[1].name &&
           milena_shared_query_plans_same_logic(&arrow_plan, &sqlite_plan));

    MilenaSharedQueryPlan unfiltered = {0};
    assert(milena_shared_query_plan_build(
        "local.arrow", false, NULL, NULL, projections, 2u,
        MILENA_SHARED_QUERY_SINK_ARROW_IPC_STREAM, &unfiltered, &error) ==
        MILENA_OK);
    assert(unfiltered.operator_count == 3u &&
           unfiltered.operators[0] == MILENA_SHARED_QUERY_SCAN &&
           unfiltered.operators[1] == MILENA_SHARED_QUERY_PROJECT &&
           unfiltered.operators[2] == MILENA_SHARED_QUERY_RESULT);

    MilenaSharedQueryPlan invalid = {0};
    assert(milena_shared_query_plan_build(
        "records", true, "tag", NULL, projections, 2u,
        MILENA_SHARED_QUERY_SINK_SQLITE_RESULT, &invalid, &error) ==
        MILENA_ERR_TYPE);
    static const char bad_utf8[] = {(char)0xc0, (char)0xaf, '\0'};
    assert(milena_shared_query_plan_build(
        "records", true, "tag", bad_utf8, projections, 2u,
        MILENA_SHARED_QUERY_SINK_SQLITE_RESULT, &invalid, &error) ==
        MILENA_ERR_TYPE);
    MilenaSharedQueryPlan reordered = arrow_plan;
    reordered.operators[1] = MILENA_SHARED_QUERY_PROJECT;
    assert(milena_shared_query_plan_validate(&reordered, &error) ==
           MILENA_ERR_DATA);
}

int main(void) {
    test_global_stream_plan();
    test_grouped_stream_plan();
    test_grouped_spill_stream_plan();
    test_composite_grouped_spill_stream_plan();
    test_grouped_spill_rejects_unsupported_metric();
    test_filtered_grouped_stream_plan();
    test_numeric_filter_plan();
    test_legacy_global_summary_plan();
    test_ambiguous_and_unsupported_plans();
    test_common_data_operator_overlap();
    test_shared_text_scan_filter_project_plan();
    puts("Canonical logical/physical stream plans and shared query overlap validated.");
    return 0;
}
