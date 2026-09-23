#include "stream.h"

#include <assert.h>

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
    assert(milena_stream_csv_summary(input, output, metrics,
                                     sizeof(metrics) / sizeof(metrics[0]),
                                     2, &report, &error) == MILENA_OK);
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

    budget.max_elapsed_milliseconds = NAN;
    milena_error_clear(&budget_error);
    assert(milena_stream_csv_summary_with_options(
        budget_input, budget_output, metrics, 1, &budget, &budget_report,
        &budget_error) == MILENA_ERR_ARGUMENT);
    remove(budget_input);
    puts("stream tests passed");
    return 0;
}

