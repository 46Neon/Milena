#include "stream.h"
#include "spill.h"

#include <assert.h>

static void test_spill_contract(void);
static void test_grouped_spill(void);
static void test_grouped_limits(void);

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

    test_spill_contract();
    test_grouped_spill();
    test_grouped_limits();
    puts("stream tests passed");
    return 0;
}


/* The spill contract is deliberately exercised without relying on timing or
 * directory enumeration: this also catches checksum and truncation failures. */
static void test_spill_contract(void) {
    const char *keys[] = {"z", "a"};
    const double values[] = {3.5, 1.25};
    MilenaSpillPolicy policy = milena_spill_policy_default();
    policy.directory = ".";
    MilenaSpillMetadata meta = {0};
    MilenaError error;
    milena_error_clear(&error);
    assert(milena_spill_write(&policy, ".stream-contract", keys, values, 2,
                              &meta, &error) == MILENA_OK);
    FILE *file = fopen(meta.path, "r+b");
    assert(file != NULL);
    assert(fseek(file, 8, SEEK_SET) == 0);
    int byte = fgetc(file);
    assert(byte != EOF && fseek(file, 8, SEEK_SET) == 0);
    assert(fputc(byte ^ 1, file) != EOF);
    assert(fclose(file) == 0);
    char key[16]; char *key_ptr = key; double value = 0.0; size_t count = 0;
    assert(milena_spill_read(&policy, &meta, &key_ptr, &value, 1,
                             &count, &error) == MILENA_ERR_DATA);
    remove(meta.path);

    assert(milena_spill_write(&policy, ".stream-contract", keys, values, 2,
                              &meta, &error) == MILENA_OK);
    file = fopen(meta.path, "wb");
    assert(file != NULL);
    assert(fwrite("MLSP", 1, 4, file) == 4);
    assert(fclose(file) == 0);
    assert(milena_spill_read(&policy, &meta, &key_ptr, &value, 1,
                             &count, &error) == MILENA_ERR_DATA);
    assert(milena_spill_remove(&meta, &error) == MILENA_OK);
    assert(meta.path[0] == '\0');
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); assert(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = (char *)calloc((size_t)size + 1, 1); assert(text != NULL);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0); return text;
}

static void test_grouped_spill(void) {
    const char *input = "tests/.stream_spill.csv";
    const char *memory_output = "tests/.stream_memory.json";
    const char *spill_output = "tests/.stream_spill.json";
    FILE *file = fopen(input, "wb"); assert(file != NULL);
    fputs("grupo,a,b\ncharlie,1,10\nalfa,2,no\nbravo,3,30\nalfa,no,40\n"
          "charlie,5,50\nbravo,7,70\n", file);
    assert(fclose(file) == 0);
    MilenaStreamMetric metrics[] = {
        {"a", "a_suma", MILENA_STREAM_SUM},
        {"b", "b_media", MILENA_STREAM_MEAN}
    };
    MilenaStreamOptions memory = milena_stream_options_default(); memory.max_groups = 10;
    MilenaError error; milena_error_clear(&error);
    assert(milena_stream_csv_grouped_with_options(input, memory_output, "grupo",
        metrics, 2, &memory, NULL, &error) == MILENA_OK);
    MilenaStreamOptions spill = memory;
    spill.spill_directory = "."; spill.spill_partitions = 2;
    spill.spill_max_bytes = 4096; spill.spill_max_records = 16;
    assert(milena_stream_csv_grouped_with_options(input, spill_output, "grupo",
        metrics, 2, &spill, NULL, &error) == MILENA_OK);
    char *expected = read_all(memory_output); char *actual = read_all(spill_output);
    /* The only intentional difference is the execution mode marker. */
    char *p = strstr(expected, "\"derramado\":false");
    assert(p != NULL); memcpy(p + strlen("\"derramado\":"), "true", 5);
    assert(strcmp(expected, actual) == 0);
    free(expected); free(actual);
    /* Repeating the same run must produce byte-for-byte deterministic output. */
    assert(milena_stream_csv_grouped_with_options(input, spill_output, "grupo",
        metrics, 2, &spill, NULL, &error) == MILENA_OK);
    actual = read_all(spill_output); expected = read_all(memory_output);
    p = strstr(expected, "\"derramado\":false"); assert(p != NULL);
    memcpy(p + strlen("\"derramado\":"), "true", 5);
    assert(strcmp(expected, actual) == 0); free(expected); free(actual);
    remove(input); remove(memory_output); remove(spill_output);
}

static void test_grouped_limits(void) {
    const char *input = "tests/.stream_limits.csv"; const char *output = "tests/.stream_limits.json";
    FILE *file = fopen(input, "wb"); assert(file != NULL);
    fputs("grupo,valor\na,1\nb,2\nc,3\n", file); assert(fclose(file) == 0);
    MilenaStreamMetric metric = {"valor", "suma", MILENA_STREAM_SUM};
    MilenaStreamOptions options = milena_stream_options_default(); options.max_groups = 2;
    MilenaError error; milena_error_clear(&error);
    assert(milena_stream_csv_grouped_with_options(input, output, "grupo", &metric, 1,
        &options, NULL, &error) == MILENA_ERR_OVERFLOW);
    options.max_groups = 10; options.spill_directory = "."; options.spill_partitions = 1;
    options.spill_max_records = 1; options.spill_max_bytes = 4096;
    assert(milena_stream_csv_grouped_with_options(input, output, "grupo", &metric, 1,
        &options, NULL, &error) == MILENA_ERR_OVERFLOW);
    options.spill_max_records = 16; options.spill_max_bytes = 8;
    assert(milena_stream_csv_grouped_with_options(input, output, "grupo", &metric, 1,
        &options, NULL, &error) == MILENA_ERR_OVERFLOW);
    options.spill_max_bytes = 4096; options.spill_partitions = 65;
    assert(milena_stream_csv_grouped_with_options(input, output, "grupo", &metric, 1,
        &options, NULL, &error) == MILENA_ERR_ARGUMENT);
    remove(input); remove(output);
}
