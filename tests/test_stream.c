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
    assert(report.observed_record_bytes > 0);
    assert(report.peak_record_bytes >= report.observed_record_bytes);
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
    const char *group_input = "tests/.stream_grouped.csv";
    const char *group_output = "tests/.stream_grouped.json";
    FILE *group = fopen(group_input, "wb");
    assert(group != NULL);
    fputs("grupo,a,b\nrojo,1,10\nazul,2,no\nrojo,no,30\nazul,4,40\n", group);
    assert(fclose(group) == 0);
    MilenaStreamMetric grouped_metrics[] = {
        {"a", "a_suma", MILENA_STREAM_SUM},
        {"b", "b_media", MILENA_STREAM_MEAN}
    };
    MilenaStreamOptions grouped_options = milena_stream_options_default();
    grouped_options.max_groups = 2;
    MilenaError grouped_error;
    milena_error_clear(&grouped_error);
    assert(milena_stream_csv_grouped_with_options(group_input, group_output,
        "grupo", grouped_metrics, 2, &grouped_options, NULL,
        &grouped_error) == MILENA_OK);
    FILE *group_json = fopen(group_output, "rb");
    assert(group_json != NULL);
    char grouped_buffer[4096] = {0};
    assert(fread(grouped_buffer, 1, sizeof(grouped_buffer) - 1, group_json) > 0);
    assert(fclose(group_json) == 0);
    assert(strstr(grouped_buffer, "\"clave\":\"rojo\"") != NULL);
    assert(strstr(grouped_buffer, "\"clave\":\"azul\"") != NULL);
    assert(strstr(grouped_buffer, "\"a_suma\"") != NULL);
    assert(strstr(grouped_buffer, "\"valores_invalidos\":1") != NULL);
    remove(group_input);
    remove(group_output);

    puts("stream tests passed");
    return 0;
}

