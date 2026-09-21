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
    puts("stream tests passed");
    return 0;
}
