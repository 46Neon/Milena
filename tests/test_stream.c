#include "stream.h"
#include "grouped_aggregate.h"

#include <assert.h>

#define CHECK_OK(expr) do { \
    MilenaStatus check_status = (expr); \
    if (check_status != MILENA_OK) { \
        fprintf(stderr, "unexpected status %s at %s:%d: %s\n", \
                milena_status_name(check_status), __FILE__, __LINE__, grouped_error.message); \
        assert(check_status == MILENA_OK); \
    } \
} while (0)

static MilenaStatus capture_spill_target(const MilenaGroupedAggregateResult *result,
                                         void *context, MilenaError *error) {
    (void)error;
    double *sum = context;
    if (result->key_length == 6 && memcmp(result->key, "target", 6) == 0)
        *sum = result->aggregate.sum;
    return MILENA_OK;
}

static void write_fixture(const char *path) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fputs("id,importe\n1,10.0\n2,20.0\n3,\"30.0\"\n4,no-num\n", file);
    assert(fclose(file) == 0);
}

int main(void) {
    const char *input = "tests/.stream_fixture.csv";
    const char *output = "tests/.stream_report.json";
    write_fixture(input);
    MilenaStreamMetric metrics[] = {
        {"importe", "importe_suma", MILENA_STREAM_SUM},
        {"importe", "importe_media", MILENA_STREAM_MEAN},
        {"importe", "importe_varianza", MILENA_STREAM_VARIANCE},
        {"importe", "importe_conteo", MILENA_STREAM_COUNT}
    };
    MilenaStreamReport report = {0};
    MilenaError error;
    milena_error_clear(&error);
    MilenaStreamExecutionPlan execution_plan;
    assert(milena_stream_plan_build_csv(false, false, &execution_plan,
                                        &error) == MILENA_OK);
    assert(execution_plan.kind == MILENA_STREAM_PLAN_SUMMARY);
    assert(execution_plan.operator_count == 3 &&
           execution_plan.operators[0] == MILENA_STREAM_PLAN_SCAN_CSV_RECORDS &&
           execution_plan.operators[1] == MILENA_STREAM_PLAN_SUMMARY_AGGREGATE &&
           execution_plan.operators[2] == MILENA_STREAM_PLAN_JSON_SINK);
    assert(execution_plan.partition_count == 1 &&
           execution_plan.worker_count == 1 && execution_plan.csv_record_safe &&
           !execution_plan.parallel_enabled);
    assert(milena_stream_execute_csv_plan(&execution_plan, input, output, NULL,
        metrics, sizeof(metrics) / sizeof(metrics[0]), NULL, NULL, &report,
        &error) == MILENA_OK);
    assert(milena_stream_plan_build_csv(true, true, &execution_plan,
                                        &error) == MILENA_OK);
    assert(execution_plan.kind == MILENA_STREAM_PLAN_GROUPED_SPILL_MODE &&
           execution_plan.operator_count == 5 &&
           execution_plan.operators[0] == MILENA_STREAM_PLAN_SCAN_CSV_RECORDS &&
           execution_plan.operators[1] == MILENA_STREAM_PLAN_GROUPED_SPILL &&
           execution_plan.operators[2] ==
               MILENA_STREAM_PLAN_REDUCE_PARTIAL_STATES &&
           execution_plan.operators[3] == MILENA_STREAM_PLAN_ORDER_BY_KEY &&
           execution_plan.operators[4] == MILENA_STREAM_PLAN_JSON_SINK &&
           execution_plan.partition_count == 1 &&
           execution_plan.worker_count == 1 &&
           execution_plan.csv_record_safe &&
           !execution_plan.parallel_enabled);
    MilenaStreamExecutionPlan unsafe_plan = execution_plan;
    unsafe_plan.parallel_enabled = true;
    assert(milena_stream_plan_validate_csv(&unsafe_plan, &error) ==
           MILENA_ERR_UNSUPPORTED);
    unsafe_plan = execution_plan;
    unsafe_plan.csv_record_safe = false;
    assert(milena_stream_plan_validate_csv(&unsafe_plan, &error) ==
           MILENA_ERR_UNSUPPORTED);
    unsafe_plan = execution_plan;
    unsafe_plan.operators[2] = MILENA_STREAM_PLAN_ORDER_BY_KEY;
    assert(milena_stream_plan_validate_csv(&unsafe_plan, &error) ==
           MILENA_ERR_DATA);
    assert(milena_stream_plan_build_csv(false, true, &execution_plan,
                                        &error) == MILENA_ERR_ARGUMENT);
    assert(report.rows_read == 4);
    assert(report.rows_with_valid_values == 3);
    assert(report.malformed_rows == 1);
    assert(report.input_bytes > 0);
    assert(report.bytes_read > 0);
    assert(report.observed_record_bytes > 0);
    assert(report.peak_record_bytes >= report.observed_record_bytes);
    FILE *json = fopen(output, "rb");
    assert(json != NULL);
    char buffer[2048] = {0};
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    assert(strstr(buffer, "\"modo\":\"flujo\"") != NULL);
    assert(strstr(buffer, "\"importe_suma\"") != NULL);
    assert(strstr(buffer, "\"bytes_entrada\"") != NULL);
    assert(strstr(buffer, "\"filas_por_segundo\"") != NULL);
    assert(strstr(buffer, "\"megabytes_por_segundo\"") != NULL);
    assert(strstr(buffer, "\"valores_invalidos\":1") != NULL);
    assert(strstr(buffer, "\"bytes_leidos\"") != NULL);
    remove(input);
    remove(output);

    const char *large_input = "tests/.stream_large_record.csv";
    const char *large_output = "tests/.stream_large_record.json";
    FILE *large = fopen(large_input, "wb");
    assert(large != NULL);
    fputs("importe\n", large);
    for (size_t i = 0; i < 5000; i++) fputc('x', large);
    fputc('\n', large);
    assert(fclose(large) == 0);
    MilenaStreamOptions options = milena_stream_options_default();
    options.chunk_rows = 2;
    options.max_record_bytes = 4096;
    MilenaError limited_error;
    milena_error_clear(&limited_error);
    assert(milena_stream_csv_summary_with_options(
        large_input, large_output, metrics, 1, &options, NULL,
        &limited_error) == MILENA_ERR_OVERFLOW);
    assert(strstr(limited_error.message, "límite") != NULL);
    remove(large_input);
    remove(large_output);

    const char *budget_input = "tests/.stream_budget.csv";
    const char *budget_output = "tests/.stream_budget.json";
    write_fixture(budget_input);
    MilenaStreamOptions budget = milena_stream_options_default();
    budget.max_rows = 2;
    MilenaStreamReport budget_report = {0};
    MilenaError budget_error;
    milena_error_clear(&budget_error);
    assert(milena_stream_csv_summary_with_options(
        budget_input, budget_output, metrics, 1, &budget, &budget_report,
        &budget_error) == MILENA_ERR_OVERFLOW);
    assert(budget_report.resource_limit_reached);
    assert(budget_report.rows_read == 2);
    assert(budget_report.max_rows == 2);
    assert(budget_report.bytes_read > 0);
    remove(budget_input);
    remove(budget_output);

    write_fixture(budget_input);
    budget.max_rows = 0;
    budget.max_elapsed_milliseconds = 1e-9;
    milena_error_clear(&budget_error);
    assert(milena_stream_csv_summary_with_options(
        budget_input, budget_output, metrics, 1, &budget, &budget_report,
        &budget_error) == MILENA_ERR_OVERFLOW);
    assert(budget_report.resource_limit_reached);
    remove(budget_input);
    remove(budget_output);

    const char *empty_input = "tests/.stream_empty.csv";
    FILE *empty_file = fopen(empty_input, "wb");
    assert(empty_file != NULL);
    assert(fputs("importe\n", empty_file) >= 0);
    assert(fclose(empty_file) == 0);
    budget.max_elapsed_milliseconds = 1e-9;
    milena_error_clear(&budget_error);
    assert(milena_stream_csv_summary_with_options(
        empty_input, budget_output, metrics, 1, &budget, &budget_report,
        &budget_error) == MILENA_ERR_OVERFLOW);
    assert(budget_report.resource_limit_reached);
    assert(fopen(budget_output, "rb") == NULL);
    remove(empty_input);
    remove(budget_output);

    budget.max_elapsed_milliseconds = NAN;
    milena_error_clear(&budget_error);
    assert(milena_stream_csv_summary_with_options(
        budget_input, budget_output, metrics, 1, &budget, &budget_report,
        &budget_error) == MILENA_ERR_ARGUMENT);
    remove(budget_input);

    const char *group_input = "tests/.stream_group_fixture.csv";
    const char *group_output = "tests/.stream_group_report.json";
    FILE *group_file = fopen(group_input, "wb");
    assert(group_file != NULL);
    /* The final empty CSV cell is intentional: the stream splitter must keep
       a trailing delimiter as an empty third column. */
    fputs("zona,importe,referencia\nZ,4,r1\nA,7,r2\nZ,1,\nA,no-num,r3\n",
          group_file);
    assert(fclose(group_file) == 0);
    MilenaStreamMetric grouped_metrics[] = {
        {"importe", "importe_suma", MILENA_STREAM_SUM},
        {"referencia", "referencia_conteo", MILENA_STREAM_COUNT}
    };
    MilenaStreamOptions grouped_options = milena_stream_options_default();
    grouped_options.max_groups = 10;
    MilenaStreamReport grouped_report = {0};
    MilenaError grouped_error;
    milena_error_clear(&grouped_error);
    assert(milena_stream_plan_build_csv(true, false, &execution_plan,
                                        &grouped_error) == MILENA_OK);
    assert(milena_stream_execute_csv_plan(&execution_plan, group_input,
        group_output, "zona", grouped_metrics, 2, &grouped_options, NULL,
        &grouped_report, &grouped_error) == MILENA_OK);
    assert(grouped_report.rows_read == 4);
    assert(grouped_report.rows_with_valid_values == 4);
    assert(grouped_report.malformed_rows == 2);
    assert(grouped_report.input_bytes > 0);
    assert(grouped_report.groups == 2);
    assert(grouped_report.max_groups == 10);
    json = fopen(group_output, "rb");
    assert(json != NULL);
    memset(buffer, 0, sizeof(buffer));
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    const char *alpha = strstr(buffer, "\"clave\":\"A\"");
    const char *zeta = strstr(buffer, "\"clave\":\"Z\"");
    assert(alpha != NULL && zeta != NULL && alpha < zeta);
    assert(strstr(buffer,
        "\"nombre\":\"importe_suma\",\"valores_validos\":1,\"valores_nulos\":0,\"valores_invalidos\":1,\"valor\":7") != NULL);
    assert(strstr(buffer,
        "\"nombre\":\"referencia_conteo\",\"valores_validos\":2,\"valores_nulos\":0,\"valores_invalidos\":0,\"valor\":2") != NULL);
    /* Z's last row has an empty final field; it counts as one null value. */
    assert(strstr(buffer,
        "\"nombre\":\"referencia_conteo\",\"valores_validos\":1,\"valores_nulos\":1,\"valores_invalidos\":0,\"valor\":1") != NULL);
    assert(strstr(buffer, "\"limite_grupos\":10") != NULL);
    remove(group_output);

    const char *spill_group_output = "tests/.stream_group_spill_report.json";
    const char *spill_group_scratch = "tests/.stream_group_spill";
    (void)remove(spill_group_output);
    (void)remove(spill_group_scratch);
    MilenaStreamSpillPolicy plan_spill_policy = {
        spill_group_scratch, 4096u, 1024u * 1024u, 32u, 10u, 0u, 10u
    };
    assert(milena_stream_plan_build_csv(true, true, &execution_plan,
                                        &grouped_error) == MILENA_OK);
    memset(&grouped_report, 0, sizeof(grouped_report));
    assert(milena_stream_execute_csv_plan(&execution_plan, group_input,
        spill_group_output, "zona", &grouped_metrics[0], 1,
        &grouped_options, &plan_spill_policy, &grouped_report,
        &grouped_error) == MILENA_OK);
    assert(grouped_report.groups == 2);
    json = fopen(spill_group_output, "rb");
    assert(json != NULL);
    memset(buffer, 0, sizeof(buffer));
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    alpha = strstr(buffer, "\"clave\":\"A\"");
    zeta = strstr(buffer, "\"clave\":\"Z\"");
    assert(alpha != NULL && zeta != NULL && alpha < zeta);
    remove(spill_group_output);
    assert(fopen(spill_group_scratch, "rb") == NULL);
    grouped_options.max_groups = 1;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_with_options(group_input, group_output,
        "zona", grouped_metrics, 2, &grouped_options, &grouped_report,
        &grouped_error) == MILENA_ERR_OVERFLOW);
    assert(strstr(grouped_error.message, "límite") != NULL);
    assert(fopen(group_output, "rb") == NULL);
    remove(group_output);

    grouped_options = milena_stream_options_default();
    grouped_options.max_elapsed_milliseconds = 1e-9;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_with_options(group_input, group_output,
        "zona", grouped_metrics, 2, &grouped_options, &grouped_report,
        &grouped_error) == MILENA_ERR_OVERFLOW);
    assert(grouped_report.resource_limit_reached);
    assert(fopen(group_output, "rb") == NULL);
    remove(group_input);
    remove(group_output);

    /* The no-spill stream accumulator uses compensated summation. Compare it
     * with the mergeable grouped-spill path when cancellation values are
     * deliberately split across many map flushes and external merge passes. */
    const char *cancel_input = "tests/.stream_cancel_fixture.csv";
    const char *cancel_output = "tests/.stream_cancel_report.json";
    const double cancel_values[] = {1e16, 1.0, 1.0, -1e16,
                                    1e16, 1.0, 1.0, -1e16,
                                    1e16, 1.0, 1.0, -1e16};
    FILE *cancel_file = fopen(cancel_input, "wb");
    assert(cancel_file != NULL);
    fputs("zona,importe\n", cancel_file);
    for (size_t i = 0; i < sizeof(cancel_values) / sizeof(cancel_values[0]); ++i)
        fprintf(cancel_file, "target,%.17g\n", cancel_values[i]);
    assert(fclose(cancel_file) == 0);
    MilenaStreamMetric cancel_metric = {"importe", "cancel_suma", MILENA_STREAM_SUM};
    MilenaStreamOptions cancel_options = milena_stream_options_default();
    cancel_options.max_groups = 4;
    MilenaStreamReport cancel_report = {0};
    assert(milena_stream_csv_grouped_with_options(cancel_input, cancel_output,
        "zona", &cancel_metric, 1, &cancel_options, &cancel_report,
        &grouped_error) == MILENA_OK);
    json = fopen(cancel_output, "rb");
    assert(json != NULL);
    memset(buffer, 0, sizeof(buffer));
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    char *sum_field = strstr(buffer, "\"valor\":");
    assert(sum_field != NULL);
    double no_spill_sum = strtod(sum_field + strlen("\"valor\":"), NULL);
    assert(no_spill_sum == 6.0);

    const char *spill_path = "tests/.stream_cancel_spill";
    (void)remove(spill_path);
    MilenaGroupedAggregate spilled;
    CHECK_OK(milena_grouped_aggregate_open(spill_path, 2048, 32,
        4u * 1024u * 1024u, &spilled, &grouped_error));
    for (size_t i = 0; i < sizeof(cancel_values) / sizeof(cancel_values[0]); ++i) {
        CHECK_OK(milena_grouped_aggregate_add(&spilled, "target", 6,
                                               cancel_values[i], &grouped_error));
        for (size_t j = 0; j <= spilled.group_capacity; ++j) {
            char filler[16];
            (void)snprintf(filler, sizeof(filler), "f%03zu", i * (spilled.group_capacity + 1u) + j);
            CHECK_OK(milena_grouped_aggregate_add(&spilled, filler,
                strlen(filler), 0.0, &grouped_error));
        }
    }
    double spill_sum = NAN;
    size_t spill_groups = 0;
    CHECK_OK(milena_grouped_aggregate_finalize(&spilled, capture_spill_target,
        &spill_sum, &spill_groups, &grouped_error));
    assert(spill_groups == 1u + (sizeof(cancel_values) / sizeof(cancel_values[0])) *
                           (spilled.group_capacity + 1u));
    assert(isfinite(spill_sum));
    assert(fabs(spill_sum - no_spill_sum) <= 1e-12);
    CHECK_OK(milena_grouped_aggregate_close(&spilled, &grouped_error));
    assert(remove(spill_path) == 0);
    remove(cancel_input);
    remove(cancel_output);

    puts("stream tests passed");
    return 0;
}

