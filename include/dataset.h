#ifndef MILENA_DATASET_H
#define MILENA_DATASET_H

#include "common.h"

typedef struct {
    size_t max_rows;
    size_t max_columns;
    size_t max_field_bytes;
} DatasetLimits;

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
void dataset_init(Dataset *dataset);
void dataset_destroy(Dataset *dataset);
MilenaStatus dataset_load_csv_with_limits(Dataset *dataset, const char *filename,
                                        char delimiter, const DatasetLimits *limits,
                                        MilenaError *error);
/* The byte budget counts physical input bytes from the first header byte through
 * every record terminator. A zero budget accepts only an empty file (which still
 * returns the existing empty-CSV data error). Limit failures return LIMIT and
 * leave the destination Dataset unchanged. */
MilenaStatus dataset_load_csv_with_limits_and_byte_budget(
    Dataset *dataset, const char *filename, char delimiter,
    const DatasetLimits *limits, size_t max_file_bytes, MilenaError *error);
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
