#ifndef MILENA_TABLE_H
#define MILENA_TABLE_H

#include "array.h"
#include "dataset.h"
#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MILENA_COLUMN_ARRAY = 0,
    MILENA_COLUMN_STRING,
    MILENA_COLUMN_CATEGORICAL
} MilenaColumnType;

typedef struct {
    char *key;
    char *value;
} MilenaTableMetadata;

typedef struct {
    char *name;
    MilenaArray values;
    bool *validity;
    /* Appended fields preserve the legacy prefix. */
    MilenaColumnType type;
    bool nullable;
    char **strings;
    char **dictionary;
    size_t dictionary_size;
    MilenaTableMetadata *metadata;
    size_t metadata_count;
} MilenaTableColumn;

typedef struct {
    MilenaTableColumn *columns;
    size_t column_count;
    size_t row_count;
    size_t capacity;
    /* Appended fields preserve the legacy prefix. */
    MilenaTableMetadata *metadata;
    size_t metadata_count;
    uint64_t _table_magic;
} MilenaTable;

typedef enum {
    MILENA_AGG_COUNT = 0,
    MILENA_AGG_SUM,
    MILENA_AGG_MEAN,
    MILENA_AGG_MIN,
    MILENA_AGG_MAX
} MilenaAggregateOp;

typedef enum {
    MILENA_JOIN_INNER = 0,
    MILENA_JOIN_LEFT,
    MILENA_JOIN_RIGHT,
    MILENA_JOIN_FULL
} MilenaJoinType;

typedef struct {
    const char *column;
    bool ascending;
} MilenaSortKey;

typedef struct {
    const char *value_column;
    MilenaAggregateOp operation;
    const char *output_name; /* NULL selects "<value>_<operation>". */
} MilenaAggregateSpec;

typedef struct {
    MilenaColumnType type;
    MilenaDType dtype; /* Used when type == MILENA_COLUMN_ARRAY. */
    union {
        bool boolean;
        int64_t i64;
        uint64_t u64;
        float f32;
        double f64;
        const char *string;
        uint32_t category_code;
    } as;
} MilenaTableValue;

void milena_table_init(MilenaTable *table);
void milena_table_destroy(MilenaTable *table);
void milena_table_swap(MilenaTable *left, MilenaTable *right);
MilenaStatus milena_table_validate(const MilenaTable *table,
                                   MilenaError *error);
MilenaStatus milena_table_clone(MilenaTable *out, const MilenaTable *source,
                                MilenaError *error);

/* Materializa el Dataset heredado en la tabla tipada canónica. */
MilenaStatus milena_table_from_dataset(MilenaTable *out,
                                        const Dataset *dataset,
                                        const MilenaSchema *schema,
                                        MilenaError *error);

/* Legacy numeric/bool/complex column copy. Strided arrays are materialized. */
MilenaStatus milena_table_add_column_copy(MilenaTable *table,
                                          const char *name,
                                          const MilenaArray *values,
                                          const bool *validity,
                                          MilenaError *error);
MilenaStatus milena_table_add_string_column_copy(MilenaTable *table,
                                                 const char *name,
                                                 const char *const *values,
                                                 size_t row_count,
                                                 const bool *validity,
                                                 MilenaError *error);
MilenaStatus milena_table_add_categorical_column_copy(
    MilenaTable *table, const char *name, const MilenaArray *codes,
    const char *const *dictionary, size_t dictionary_size,
    const bool *validity, MilenaError *error);

int milena_table_column_index(const MilenaTable *table, const char *name);
const MilenaTableColumn *milena_table_column(const MilenaTable *table,
                                             size_t index);
bool milena_table_is_null(const MilenaTable *table, size_t column,
                          size_t row);
MilenaStatus milena_table_get_array_value(const MilenaTable *table,
                                          size_t column, size_t row,
                                          const void **value,
                                          MilenaError *error);
MilenaStatus milena_table_get_string(const MilenaTable *table,
                                     size_t column, size_t row,
                                     const char **value,
                                     MilenaError *error);
MilenaStatus milena_table_get_category(const MilenaTable *table,
                                       size_t column, size_t row,
                                       uint32_t *code, const char **label,
                                       MilenaError *error);

MilenaStatus milena_table_set_metadata(MilenaTable *table, const char *key,
                                       const char *value,
                                       MilenaError *error);
const char *milena_table_get_metadata(const MilenaTable *table,
                                      const char *key);
MilenaStatus milena_table_column_set_metadata(MilenaTable *table,
                                              const char *column_name,
                                              const char *key,
                                              const char *value,
                                              MilenaError *error);
const char *milena_table_column_get_metadata(const MilenaTable *table,
                                             const char *column_name,
                                             const char *key);

MilenaStatus milena_table_filter(MilenaTable *out,
                                 const MilenaTable *source,
                                 const MilenaArray *mask,
                                 MilenaError *error);
MilenaStatus milena_table_select_columns(MilenaTable *out,
                                         const MilenaTable *source,
                                         const char *const *names,
                                         size_t name_count,
                                         MilenaError *error);
MilenaStatus milena_table_fill_null(MilenaTable *table,
                                    const char *column_name,
                                    const MilenaTableValue *value,
                                    MilenaError *error);
MilenaStatus milena_table_fill_null_f64(MilenaTable *table,
                                        const char *column_name,
                                        double value,
                                        MilenaError *error);
MilenaStatus milena_table_drop_null(MilenaTable *out,
                                    const MilenaTable *source,
                                    MilenaError *error);
MilenaStatus milena_table_drop_null_columns(MilenaTable *out,
                                            const MilenaTable *source,
                                            const char *const *column_names,
                                            size_t column_count,
                                            MilenaError *error);
MilenaStatus milena_table_sort(MilenaTable *out,
                               const MilenaTable *source,
                               const char *column_name,
                               bool ascending,
                               MilenaError *error);
MilenaStatus milena_table_sort_keys(MilenaTable *out,
                                    const MilenaTable *source,
                                    const MilenaSortKey *keys,
                                    size_t key_count,
                                    MilenaError *error);
MilenaStatus milena_table_group_by_aggregate(MilenaTable *out,
                                             const MilenaTable *source,
                                             const char *key_column,
                                             const char *value_column,
                                             MilenaAggregateOp operation,
                                             MilenaError *error);
MilenaStatus milena_table_group_by(MilenaTable *out,
                                   const MilenaTable *source,
                                   const char *const *key_columns,
                                   size_t key_count,
                                   const MilenaAggregateSpec *aggregates,
                                   size_t aggregate_count,
                                   MilenaError *error);
MilenaStatus milena_table_join(MilenaTable *out, const MilenaTable *left,
                               const MilenaTable *right,
                               const char *const *left_keys,
                               const char *const *right_keys,
                               size_t key_count, MilenaJoinType join_type,
                               MilenaError *error);

/* Unpivot requires all value columns to have the same logical type/storage. */
MilenaStatus milena_table_unpivot(MilenaTable *out,
                                  const MilenaTable *source,
                                  const char *const *id_columns,
                                  size_t id_count,
                                  const char *const *value_columns,
                                  size_t value_count,
                                  const char *variable_name,
                                  const char *value_name,
                                  MilenaError *error);

#ifdef __cplusplus
}
#endif

#endif
