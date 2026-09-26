#include "dataset.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE "tests/test_dataset_byte_budget.csv"

static void write_fixture(const void *bytes, size_t length) {
    FILE *file = fopen(FIXTURE, "wb");
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void assert_empty_dataset(const Dataset *dataset) {
    assert(dataset->filename == NULL);
    assert(dataset->headers == NULL);
    assert(dataset->rows == NULL);
    assert(dataset->column_count == 0);
    assert(dataset->row_count == 0);
    assert(dataset->row_capacity == 0);
    assert(dataset->invalid_rows == 0);
}

static void test_exact_and_one_over(void) {
    static const char csv[] = "h\r\n1\r\n";
    Dataset dataset;
    MilenaError error;

    write_fixture(csv, sizeof(csv) - 1);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(csv) - 1, &error) == MILENA_OK);
    assert(dataset.column_count == 1 && dataset.row_count == 1);
    assert(strcmp(dataset.headers[0], "h") == 0);
    assert(strcmp(dataset.rows[0][0], "1") == 0);
    dataset_destroy(&dataset);

    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(csv) - 2, &error) == MILENA_ERR_LIMIT);
    assert(error.code == MILENA_ERR_LIMIT);
    assert(error.category == MILENA_ERROR_DATOS);
    assert(strcmp(milena_status_name(error.code), "LIMIT") == 0);
    assert(strstr(error.message, "bytes") != NULL);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(csv) - 1, &error) == MILENA_OK);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(csv) - 2, &error) == MILENA_ERR_LIMIT);
    assert(dataset.row_count == 1 && strcmp(dataset.rows[0][0], "1") == 0);
    dataset_destroy(&dataset);
}

static void test_total_budget_across_header_records_and_newlines(void) {
    static const char csv[] = "h\n1\n2\n";
    Dataset dataset;
    MilenaError error;

    write_fixture(csv, sizeof(csv) - 1);
    dataset_init(&dataset);
    /* Every individual record is shorter than five bytes; the cumulative input
     * (header, data records, and all line terminators) is six bytes. */
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, 5, &error) == MILENA_ERR_LIMIT);
    assert(error.code == MILENA_ERR_LIMIT);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    static const char quoted_csv[] = "a,b\n\"x\",1\n";
    write_fixture(quoted_csv, sizeof(quoted_csv) - 1);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(quoted_csv) - 1,
               &error) == MILENA_OK);
    assert(dataset.column_count == 2 && dataset.row_count == 1);
    dataset_destroy(&dataset);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(quoted_csv) - 2,
               &error) == MILENA_ERR_LIMIT);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);
}

static void test_field_and_record_limits_stop_growth(void) {
    DatasetLimits limits = dataset_default_limits();
    Dataset dataset;
    MilenaError error;
    const size_t long_field_bytes = 1024 * 1024;
    FILE *file = fopen(FIXTURE, "wb");
    assert(file);
    assert(fwrite("h\n", 1, 2, file) == 2);
    for (size_t i = 0; i < long_field_bytes; i++) assert(fputc('x', file) == 'x');
    assert(fputc('\n', file) == '\n');
    assert(fclose(file) == 0);

    limits.max_columns = 1;
    limits.max_field_bytes = 4;
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', &limits, long_field_bytes + 3, &error) ==
           MILENA_ERR_LIMIT);
    assert(error.code == MILENA_ERR_LIMIT);
    assert(strstr(error.message, "campo") != NULL);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    file = fopen(FIXTURE, "wb");
    assert(file);
    assert(fwrite("a,b\n\"", 1, 5, file) == 5);
    for (size_t i = 0; i < 4; i++) assert(fwrite("\"\"", 1, 2, file) == 2);
    assert(fwrite("\",x\n", 1, 4, file) == 4);
    assert(fclose(file) == 0);
    limits.max_columns = 2;
    limits.max_field_bytes = 4;
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', &limits, 100, &error) == MILENA_ERR_LIMIT);
    assert(strstr(error.message, "registro") != NULL);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    write_fixture("a,b\n", 4);
    limits.max_columns = 1;
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', &limits, 4, &error) == MILENA_ERR_LIMIT);
    assert(strstr(error.message, "columnas") != NULL);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    write_fixture("h\n1\n2\n", 6);
    limits.max_columns = 2;
    limits.max_field_bytes = 4;
    limits.max_rows = 1;
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', &limits, 6, &error) == MILENA_ERR_LIMIT);
    assert(strstr(error.message, "filas") != NULL);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);
}

static void test_empty_and_malformed_csv(void) {
    Dataset dataset;
    MilenaError error;

    write_fixture("", 0);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, 0, &error) == MILENA_ERR_DATA);
    assert(error.code == MILENA_ERR_DATA);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);

    static const char malformed_row[] = "h\n\"bad\"x\n1\n";
    write_fixture(malformed_row, sizeof(malformed_row) - 1);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(malformed_row) - 1,
               &error) == MILENA_OK);
    assert(dataset.row_count == 1);
    assert(dataset.invalid_rows == 1);
    assert(strcmp(dataset.rows[0][0], "1") == 0);
    dataset_destroy(&dataset);

    static const char unterminated[] = "h\n\"unfinished";
    write_fixture(unterminated, sizeof(unterminated) - 1);
    dataset_init(&dataset);
    assert(dataset_load_csv_with_limits_and_byte_budget(
               &dataset, FIXTURE, ',', NULL, sizeof(unterminated) - 1,
               &error) == MILENA_ERR_PARSE);
    assert(error.code == MILENA_ERR_PARSE);
    assert_empty_dataset(&dataset);
    dataset_destroy(&dataset);
}

static void test_legacy_loader_regression(void) {
    static const char csv[] = "name,value\nalpha,42\nbeta,7\n";
    Dataset dataset;
    MilenaError error;

    write_fixture(csv, sizeof(csv) - 1);
    dataset_init(&dataset);
    assert(dataset_load_csv(&dataset, FIXTURE, ',', &error) == MILENA_OK);
    assert(dataset.column_count == 2 && dataset.row_count == 2);
    assert(strcmp(dataset.headers[0], "name") == 0);
    assert(strcmp(dataset.rows[0][1], "42") == 0);
    assert(strcmp(dataset.rows[1][0], "beta") == 0);
    dataset_destroy(&dataset);
}

int main(void) {
    test_exact_and_one_over();
    test_total_budget_across_header_records_and_newlines();
    test_field_and_record_limits_stop_growth();
    test_empty_and_malformed_csv();
    test_legacy_loader_regression();
    assert(remove(FIXTURE) == 0);
    puts("dataset byte-budget tests passed");
    return 0;
}
