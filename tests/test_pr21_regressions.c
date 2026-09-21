#include "table.h"

#include <assert.h>
#include <complex.h>
#include <stdio.h>
#include <string.h>

static void require_ok(MilenaStatus status, MilenaError *error) {
    if (status != MILENA_OK) {
        fprintf(stderr, "PR21 regression failed: %s (%s)\n",
                milena_status_name(status), error->message);
        assert(status == MILENA_OK);
    }
    milena_error_clear(error);
}

static MilenaArray one_value(MilenaDType dtype, MilenaError *error) {
    size_t shape[] = {1};
    MilenaArray array = {0};
    require_ok(milena_array_zeros(&array, dtype, 1, shape, error), error);
    void *raw = NULL;
    require_ok(milena_array_mut_data(&array, &raw, error), error);
    switch (dtype) {
        case MILENA_DTYPE_BOOL: *(bool *)raw = true; break;
        case MILENA_DTYPE_INT8: *(int8_t *)raw = -8; break;
        case MILENA_DTYPE_INT16: *(int16_t *)raw = -16; break;
        case MILENA_DTYPE_INT32: *(int32_t *)raw = -32; break;
        case MILENA_DTYPE_INT64: *(int64_t *)raw = -64; break;
        case MILENA_DTYPE_UINT8: *(uint8_t *)raw = 8u; break;
        case MILENA_DTYPE_UINT16: *(uint16_t *)raw = 16u; break;
        case MILENA_DTYPE_UINT32: *(uint32_t *)raw = 32u; break;
        case MILENA_DTYPE_UINT64: *(uint64_t *)raw = 64u; break;
        case MILENA_DTYPE_FLOAT32: *(float *)raw = 32.5f; break;
        case MILENA_DTYPE_FLOAT64: *(double *)raw = 64.5; break;
        default: assert(!"unexpected dtype"); break;
    }
    return array;
}

static void dataset_one_row(Dataset *dataset, const char *filename,
                            const char *header, const char *value) {
    dataset_init(dataset);
    dataset->filename = milena_strdup(filename);
    dataset->column_count = 1;
    dataset->row_count = 1;
    dataset->row_capacity = 1;
    dataset->headers = (char **)calloc(1, sizeof(*dataset->headers));
    dataset->rows = (char ***)calloc(1, sizeof(*dataset->rows));
    dataset->headers[0] = milena_strdup(header);
    dataset->rows[0] = (char **)calloc(1, sizeof(*dataset->rows[0]));
    dataset->rows[0][0] = milena_strdup(value);
    assert(dataset->filename && dataset->headers[0] && dataset->rows[0] &&
           dataset->rows[0][0]);
}

static void test_every_supported_numeric_dtype(MilenaError *error) {
    const MilenaDType dtypes[] = {
        MILENA_DTYPE_BOOL, MILENA_DTYPE_INT8, MILENA_DTYPE_INT16,
        MILENA_DTYPE_INT32, MILENA_DTYPE_INT64, MILENA_DTYPE_UINT8,
        MILENA_DTYPE_UINT16, MILENA_DTYPE_UINT32, MILENA_DTYPE_UINT64,
        MILENA_DTYPE_FLOAT32, MILENA_DTYPE_FLOAT64
    };
    MilenaTable table = {0};
    milena_table_init(&table);
    for (size_t i = 0; i < sizeof(dtypes) / sizeof(dtypes[0]); ++i) {
        MilenaArray value = one_value(dtypes[i], error);
        char name[32];
        int written = snprintf(name, sizeof(name), "dtype_%zu", i);
        assert(written > 0 && (size_t)written < sizeof(name));
        require_ok(milena_table_add_column_copy(&table, name, &value, NULL,
                                                error), error);
        milena_array_release(&value);
    }
    require_ok(milena_table_validate(&table, error), error);
    Dataset dataset;
    dataset_init(&dataset);
    require_ok(milena_dataset_from_table(&dataset, &table, error), error);
    assert(dataset.column_count == sizeof(dtypes) / sizeof(dtypes[0]) &&
           dataset.row_count == 1);
    dataset_destroy(&dataset);
    milena_table_destroy(&table);
}

static void test_json_escapes(MilenaError *error) {
    Dataset dataset;
    dataset_one_row(&dataset, "file\"\\\n", "col\"\\\n\t\r\x01",
                    "value\"\\\n\t\r\x02");
    const char *path = "pr21_regression.json";
    require_ok(dataset_save_json(&dataset, path, error), error);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    char output[4096];
    size_t length = fread(output, 1, sizeof(output) - 1, file);
    assert(!ferror(file));
    fclose(file);
    output[length] = '\0';
    assert(strstr(output, "\\\"") != NULL);
    assert(strstr(output, "\\\\") != NULL);
    assert(strstr(output, "\\n") != NULL);
    assert(strstr(output, "\\t") != NULL);
    assert(strstr(output, "\\r") != NULL);
    assert(strstr(output, "\\u0001") != NULL);
    assert(strstr(output, "\\u0002") != NULL);
    remove(path);
    dataset_destroy(&dataset);
}

static void test_conversion_failure_preserves_destination(MilenaError *error) {
    MilenaTable bad = {0};
    milena_table_init(&bad);
    size_t shape[] = {1};
    MilenaArray complex_value = {0};
    require_ok(milena_array_zeros(&complex_value, MILENA_DTYPE_COMPLEX64, 1,
                                  shape, error), error);
    require_ok(milena_table_add_column_copy(&bad, "unsupported", &complex_value,
                                            NULL, error), error);
    milena_array_release(&complex_value);

    Dataset destination;
    dataset_one_row(&destination, "original.csv", "keep", "untouched");
    assert(milena_dataset_from_table(&destination, &bad, error) ==
           MILENA_ERR_UNSUPPORTED);
    assert(strcmp(destination.filename, "original.csv") == 0 &&
           strcmp(destination.headers[0], "keep") == 0 &&
           strcmp(destination.rows[0][0], "untouched") == 0);
    milena_error_clear(error);

    assert(milena_table_add_product(&bad, "unsupported", "unsupported", "out",
                                    error) == MILENA_ERR_TYPE);
    assert(bad.column_count == 1);
    milena_error_clear(error);
    dataset_destroy(&destination);
    milena_table_destroy(&bad);
}

int main(void) {
    MilenaError error;
    milena_error_clear(&error);
    test_every_supported_numeric_dtype(&error);
    test_json_escapes(&error);
    test_conversion_failure_preserves_destination(&error);
    puts("PR21 regressions: OK");
    return 0;
}
