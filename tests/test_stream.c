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
    assert(report.malformed_rows == 0);
    FILE *json = fopen(output, "rb");
    assert(json != NULL);
    char buffer[2048] = {0};
    assert(fread(buffer, 1, sizeof(buffer) - 1, json) > 0);
    assert(fclose(json) == 0);
    assert(strstr(buffer, "\"modo\":\"flujo\"") != NULL);
    assert(strstr(buffer, "\"importe_suma\"") != NULL);
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
    FILE *budget = fopen(budget_input, "wb");
    assert(budget != NULL);
    fputs("importe\n1\n2\n3\n", budget);
    assert(fclose(budget) == 0);
    MilenaStreamOptions budget_options = milena_stream_options_default();
    budget_options.max_rows = 2;
    MilenaError budget_error;
    milena_error_clear(&budget_error);
    MilenaStreamReport budget_report = {0};
    assert(milena_stream_csv_summary_with_options(
        budget_input, budget_output, metrics, 1, &budget_options,
        &budget_report, &budget_error) == MILENA_ERR_OVERFLOW);
    assert(budget_report.resource_limit_reached == true);
    assert(strstr(budget_error.message, "filas") != NULL);
    remove(budget_input);
    remove(budget_output);

    /* Duplicate names would make a metric reference ambiguous. */
    const char *duplicate_input = "tests/.stream_duplicate.csv";
    const char *duplicate_output = "tests/.stream_duplicate.json";
    FILE *duplicate = fopen(duplicate_input, "wb");
    assert(duplicate != NULL);
    fputs("importe,importe\n1,2\n", duplicate);
    assert(fclose(duplicate) == 0);
    MilenaError duplicate_error;
    milena_error_clear(&duplicate_error);
    assert(milena_stream_csv_summary(duplicate_input, duplicate_output,
                                     metrics, 1, 2, NULL,
                                     &duplicate_error) == MILENA_ERR_DATA);
    assert(strstr(duplicate_error.message, "duplicadas") != NULL);
    remove(duplicate_input);
    remove(duplicate_output);

    /* A blank record is a valid one-field CSV record, not an out-of-bounds read. */
    const char *blank_input = "tests/.stream_blank.csv";
    const char *blank_output = "tests/.stream_blank.json";
    FILE *blank = fopen(blank_input, "wb");
    assert(blank != NULL);
    fputs("importe\n\n", blank);
    assert(fclose(blank) == 0);
    MilenaError blank_error;
    milena_error_clear(&blank_error);
    assert(milena_stream_csv_summary(blank_input, blank_output,
                                     metrics, 1, 2, NULL,
                                     &blank_error) == MILENA_OK);
    remove(blank_input);
    remove(blank_output);
    puts("stream tests passed");
    return 0;
}
