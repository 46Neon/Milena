#include "query_plan.h"

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

int main(void) {
    test_global_stream_plan();
    test_grouped_stream_plan();
    test_grouped_spill_stream_plan();
    test_grouped_spill_rejects_unsupported_metric();
    test_filtered_grouped_stream_plan();
    test_numeric_filter_plan();
    test_legacy_global_summary_plan();
    test_ambiguous_and_unsupported_plans();
    puts("Canonical logical/physical stream plans validated.");
    return 0;
}
