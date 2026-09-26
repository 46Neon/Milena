#include "query_plan.h"
#include "canonical_compiler.h"

#include <assert.h>
#include <math.h>
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

static void test_common_data_operator_overlap(void) {
    MilenaHIRDataOperation hir_operations[2] = {{0}};
    MilenaHIRAggregate hir_aggregates[3] = {{0}};
    MilenaDataHIR hir = {0};
    hir.source.path = "rows.csv";
    hir.export_path = "memory.json";
    hir.schema_bound = true;
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
    assert(in_memory.operator_count == 4u && in_memory.has_numeric_greater_filter);

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
    puts("Canonical logical/physical stream plans and shared data-operator overlap validated.");
    return 0;
}
