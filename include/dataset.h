#ifndef MILENA_DATASET_H
#define MILENA_DATASET_H

#include "common.h"

typedef struct {
    size_t max_rows;
    size_t max_columns;
    size_t max_field_bytes;
} DatasetLimits;

/* Resource limits for the materialized CSV loader. Record bytes are logical
 * CSV-record bytes (excluding the terminating NUL and record separator).
 * max_record_bytes > 0 applies a fixed cap; max_field_bytes > 0 also applies
 * the established cap (max_field_bytes * actual_columns + actual_columns).
 * At least one record cap must be nonzero. max_memory_bytes > 0 caps the sum
 * of current requested capacities for loader-retained Dataset allocations:
 * filename/header/cell strings, header/row field vectors, and the row vector.
 * It is not RSS: allocator overhead, the raw CSV-record input scratch buffer,
 * realloc transient peaks, and later MilenaTable/operator copies are excluded.
 * max_elapsed_milliseconds == 0 disables the elapsed-time limit. */
typedef struct {
    size_t max_rows;
    size_t max_columns;
    /* Zero selects the legacy per-field-derived record cap below. */
    size_t max_record_bytes;
    size_t max_memory_bytes;
    size_t max_field_bytes;
    double max_elapsed_milliseconds;
} DatasetLoadLimits;

typedef struct {
    char *filename;
    char **headers;
    char ***rows;
    size_t column_count;
    size_t row_count;
    size_t row_capacity;
    size_t invalid_rows;
} Dataset;

DatasetLimits dataset_default_limits(void);
DatasetLoadLimits dataset_default_load_limits(void);
void dataset_init(Dataset *dataset);
void dataset_destroy(Dataset *dataset);
MilenaStatus dataset_load_csv_with_limits(Dataset *dataset, const char *filename,
                                        char delimiter, const DatasetLimits *limits,
                                        MilenaError *error);
MilenaStatus dataset_load_csv_with_resource_limits(
    Dataset *dataset, const char *filename, char delimiter,
    const DatasetLoadLimits *limits, MilenaError *error);
MilenaStatus dataset_load_csv(Dataset *dataset, const char *filename,
                            char delimiter, MilenaError *error);
MilenaStatus dataset_save_json(const Dataset *dataset, const char *filename,
                             MilenaError *error);
int dataset_column_index(const Dataset *dataset, const char *name);
MilenaStatus dataset_remove_null_rows(Dataset *dataset, MilenaError *error);
MilenaStatus dataset_remove_duplicates(Dataset *dataset, MilenaError *error);
MilenaStatus dataset_add_product(Dataset *dataset, const char *left,
                               const char *right, const char *output,
                               MilenaError *error);
MilenaStatus dataset_add_month(Dataset *dataset, const char *date_column,
                             const char *output, MilenaError *error);
MilenaStatus dataset_filter_positive_product(Dataset *dataset,
                                           const char *left,
                                           const char *right,
                                           MilenaError *error);
void dataset_print(const Dataset *dataset, size_t max_rows, FILE *stream);

/* Compatibility names retained from the original project. */
bool dataset_cargar_csv(Dataset *dataset, const char *filename);
bool dataset_guardar_json(const Dataset *dataset, const char *filename);
void dataset_destruir(Dataset *dataset);
int dataset_indice_columna(const Dataset *dataset, const char *name);

#endif
