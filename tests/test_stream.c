#include "stream.h"
#include "grouped_aggregate.h"
#include "source_reader.h"

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

static void test_local_source_reader(void) {
    const char *path = "tests/.source_reader_fixture.csv";
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fputs("id,nota\r\n1,\"linea uno\r\nlinea dos\"\r\n2,final", file);
    assert(fclose(file) == 0);

    MilenaSourceReader reader = {0};
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_source_reader_open_local_csv(&reader, path, &error) == MILENA_OK);
    char *record = NULL;
    size_t capacity = 0, length = 0, position = 0;
    assert(milena_source_reader_read_record(&reader, &record, &capacity,
        4096, &length, &error) == MILENA_OK);
    assert(length == strlen("id,nota") && strcmp(record, "id,nota") == 0);
    assert(milena_source_reader_position_bytes(&reader, &position) && position == 9);
    assert(milena_source_reader_read_record(&reader, &record, &capacity,
        4096, &length, &error) == MILENA_OK);
    assert(strcmp(record, "1,\"linea uno\nlinea dos\"") == 0);
    assert(milena_source_reader_read_record(&reader, &record, &capacity,
        4096, &length, &error) == MILENA_OK);
    assert(strcmp(record, "2,final") == 0);
    assert(milena_source_reader_read_record(&reader, &record, &capacity,
        4096, &length, &error) == MILENA_ERR_IO);
    assert(milena_source_reader_at_end(&reader));
    assert(milena_source_reader_close(&reader) == MILENA_OK);
    free(record);
    assert(remove(path) == 0);
}

int main(void) {
    test_local_source_reader();
    const char *input = "tests/.stream_fixture.csv";
    const char *output = "tests/.stream_report.json";
    write_fixture(input);
    MilenaStreamMetric metrics[] = {
        {"importe", "importe_suma", MILENA_STREAM_SUM, false},
        {"importe", "importe_media", MILENA_STREAM_MEAN, false},
        {"importe", "importe_varianza", MILENA_STREAM_VARIANCE, false},
        {"importe", "importe_conteo", MILENA_STREAM_COUNT, false}
    };
    MilenaStreamReport report = {0};
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_stream_csv_summary_with_options(input, output, metrics,
        sizeof(metrics) / sizeof(metrics[0]), NULL, &report, &error) == MILENA_OK);
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
        {"importe", "importe_suma", MILENA_STREAM_SUM, false},
        {"referencia", "referencia_conteo", MILENA_STREAM_COUNT, false},
        {"importe", "importe_conteo_numerico", MILENA_STREAM_COUNT, true}
    };
    MilenaStreamOptions grouped_options = milena_stream_options_default();
    grouped_options.max_groups = 10;
    MilenaStreamReport grouped_report = {0};
    MilenaError grouped_error;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_with_options(group_input, group_output,
        "zona", grouped_metrics, 3, &grouped_options, &grouped_report,
        &grouped_error) == MILENA_OK);
    assert(grouped_report.rows_read == 4);
    assert(grouped_report.rows_with_valid_values == 4);
    assert(grouped_report.malformed_rows == 2);
    assert(grouped_report.input_bytes > 0);
    assert(grouped_report.bytes_read == grouped_report.input_bytes);
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
        "\"nombre\":\"importe_conteo_numerico\",\"valores_validos\":1,\"valores_nulos\":0,\"valores_invalidos\":1,\"valor\":1") != NULL);
    assert(strstr(buffer,
        "\"nombre\":\"importe_conteo_numerico\",\"valores_validos\":2,\"valores_nulos\":0,\"valores_invalidos\":0,\"valor\":2") != NULL);
    assert(strstr(buffer,
        "\"nombre\":\"referencia_conteo\",\"valores_validos\":2,\"valores_nulos\":0,\"valores_invalidos\":0,\"valor\":2") != NULL);
    /* Z's last row has an empty final field; it counts as one null value. */
    assert(strstr(buffer,
        "\"nombre\":\"referencia_conteo\",\"valores_validos\":1,\"valores_nulos\":1,\"valores_invalidos\":0,\"valor\":1") != NULL);
    assert(strstr(buffer, "\"limite_grupos\":10") != NULL);
    assert(strstr(buffer, "\"bytes_leidos\":") != NULL);
    remove(group_output);

    const char *spill_group_output = "tests/.stream_group_spill_report.json";
    const char *spill_group_scratch = "tests/.stream_group_spill";
    (void)remove(spill_group_output);
    (void)remove(spill_group_scratch);
    MilenaStreamSpillPolicy plan_spill_policy = {
        spill_group_scratch, 4096u, 1024u * 1024u, 32u, 10u, 0u, 10u
    };
    memset(&grouped_report, 0, sizeof(grouped_report));
    assert(milena_stream_csv_grouped_spill_with_options(group_input,
        spill_group_output, "zona", &grouped_metrics[0], 1u, &grouped_options,
        &plan_spill_policy, &grouped_report, &grouped_error) == MILENA_OK);
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

    /* Public report counters must come from the canonical reducer/store, and
     * stay zero on a failed staged publication even after real spill occurred. */
    const char *spill_input = "tests/.stream_group_spill_fixture.csv";
    const char *spill_output = "tests/.stream_group_spill_report.json";
    const char *spill_scratch = "tests/.stream_group_spill_scratch";
    FILE *spill_fixture = fopen(spill_input, "wb");
    assert(spill_fixture != NULL);
    fputs("zona,importe\n", spill_fixture);
    for (size_t i = 0; i < 200; ++i)
        fprintf(spill_fixture, "g%03zu,1\n", i);
    assert(fclose(spill_fixture) == 0);
    MilenaStreamMetric spill_metric = {"importe", "importe_suma", MILENA_STREAM_SUM, false};
    MilenaStreamOptions spill_options = milena_stream_options_default();
    spill_options.max_groups = 256;
    MilenaStreamSpillPolicy spill_policy = {spill_scratch, 4096, 1048576,
        32, 256, 2097152, 4096};
    MilenaStreamReport spill_report = {0};
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_spill_with_options(spill_input, spill_output,
        "zona", &spill_metric, 1, &spill_options, &spill_policy, &spill_report,
        &grouped_error) == MILENA_OK);
    assert(spill_report.groups == 200);
    assert(spill_report.spill_bytes > 0);
    assert(spill_report.spill_records > 0);
    assert(spill_report.spill_runs > 0);
    FILE *spill_report_file = fopen(spill_output, "rb");
    assert(spill_report_file != NULL);
    assert(fclose(spill_report_file) == 0);
    spill_report_file = fopen(spill_scratch, "rb");
    assert(spill_report_file == NULL);
    remove(spill_output);

    /* Pair-key backend path: separators and CSV quotes stay unambiguous, the
     * same first key with a different second key is distinct, valid empty
     * strings remain distinct from invalid UTF-8 components, and each group
     * still carries the declared multi-metric aggregate state. */
    const char *pair_input = "tests/.stream_pair_spill_fixture.csv";
    const char *pair_output = "tests/.stream_pair_spill_report.json";
    const char *pair_scratch = "tests/.stream_pair_spill_scratch";
    FILE *pair_file = fopen(pair_input, "wb");
    assert(pair_file != NULL);
    fputs("zona,subzona,importe\n", pair_file);
    fputs("\"north|\"\"quoted\"\"\",\"a,b|c\",1\n", pair_file);
    fputs("\"north|\"\"quoted\"\"\",\"a,b|c\",2\n", pair_file);
    fputs("\"north|\"\"quoted\"\"\",third,3\n", pair_file);
    fputs(",,4\n", pair_file);
    fputc(0xff, pair_file);
    fputs(",invalid-component,5\n", pair_file);
    assert(fclose(pair_file) == 0);
    MilenaStreamGroupKeyDescriptor pair_keys[] = {
        {"zona", MILENA_STREAM_GROUP_KEY_TEXT},
        {"subzona", MILENA_STREAM_GROUP_KEY_TEXT}
    };
    MilenaStreamMetric pair_metrics[] = {
        {"importe", "importe_suma", MILENA_STREAM_SUM, false},
        {"importe", "filas", MILENA_STREAM_COUNT, false}
    };
    MilenaStreamSpillPolicy pair_policy = {pair_scratch, 4096, 1048576,
        128, 16, 2097152, 4096};
    spill_options = milena_stream_options_default();
    spill_options.max_groups = 16;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_spill_with_keys_and_options(
        pair_input, pair_output, pair_keys, 2, pair_metrics, 2,
        &spill_options, &pair_policy, &spill_report, &grouped_error) == MILENA_OK);
    assert(spill_report.groups == 4);
    assert(spill_report.spill_bytes > 0 && spill_report.spill_records > 0);
    json = fopen(pair_output, "rb");
    assert(json != NULL);
    memset(buffer, 0, sizeof(buffer));
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    assert(strstr(buffer, "\"columnas_grupo\":[{\"nombre\":\"zona\",\"tipo\":\"texto\"},{\"nombre\":\"subzona\",\"tipo\":\"texto\"}]") != NULL);
    assert(strstr(buffer, "\"valor\":\"north|\\\"quoted\\\"\",\"valido\":true") != NULL);
    assert(strstr(buffer, "\"valor\":\"a,b|c\",\"valido\":true") != NULL);
    assert(strstr(buffer, "\"valor\":\"third\",\"valido\":true") != NULL);
    assert(strstr(buffer, "\"valor\":\"\",\"valido\":true") != NULL);
    assert(strstr(buffer, "\"valor\":null,\"valido\":false") != NULL);
    assert(strstr(buffer, "\"valores_validos\":2,\"valores_nulos\":0,\"valores_invalidos\":0,\"valor\":3") != NULL);
    assert(fopen(pair_scratch, "rb") == NULL);
    remove(pair_output);

    /* A group-limit failure never publishes a partial JSON report and still
     * releases both reducer scratch and the sibling staging file. */
    pair_policy.max_output_groups = 1;
    spill_options.max_groups = 1;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_spill_with_keys_and_options(
        pair_input, pair_output, pair_keys, 2, pair_metrics, 2,
        &spill_options, &pair_policy, &spill_report, &grouped_error) == MILENA_ERR_OVERFLOW);
    assert(fopen(pair_output, "rb") == NULL);
    assert(fopen(pair_scratch, "rb") == NULL);
    remove(pair_input);

    spill_options.max_groups = 1;
    memset(&spill_report, 0xA5, sizeof(spill_report));
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_spill_with_options(spill_input, spill_output,
        "zona", &spill_metric, 1, &spill_options, &spill_policy, &spill_report,
        &grouped_error) != MILENA_OK);
    assert(spill_report.spill_bytes == 0);
    assert(spill_report.spill_records == 0);
    assert(spill_report.spill_runs == 0);
    spill_report_file = fopen(spill_output, "rb");
    assert(spill_report_file == NULL);
    spill_report_file = fopen(spill_scratch, "rb");
    assert(spill_report_file == NULL);
    remove(spill_input);

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
    MilenaStreamMetric cancel_metric = {"importe", "cancel_suma", MILENA_STREAM_SUM, false};
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

