#include "language_grouped_spill.h"
#include "grouped_aggregate.h"

#include <string.h>

#define GROUP_SPILL_ERR(error, code, message) \
    do { if (error) milena_error_set((error), (code), 0, 0, 0, (message)); } while (0)

typedef struct {
    char **keys;
    bool *key_validity;
    double *values;
    uint64_t *counts;
    bool *value_validity;
    size_t count;
    size_t capacity;
    size_t max_groups;
    MilenaAggregateOp operation;
} GroupSpillRows;

static void rows_release(GroupSpillRows *rows) {
    if (!rows) return;
    for (size_t i = 0; i < rows->count; ++i) free(rows->keys[i]);
    free(rows->keys); free(rows->key_validity); free(rows->values);
    free(rows->counts); free(rows->value_validity);
    memset(rows, 0, sizeof(*rows));
}

static MilenaStatus rows_reserve(GroupSpillRows *rows, size_t needed,
                                 MilenaError *error) {
    if (needed <= rows->capacity) return MILENA_OK;
    size_t capacity = rows->capacity ? rows->capacity * 2u : 16u;
    if (capacity < rows->capacity || capacity < needed) capacity = needed;
    if (capacity > SIZE_MAX / sizeof(char *) ||
        capacity > SIZE_MAX / sizeof(double) ||
        capacity > SIZE_MAX / sizeof(uint64_t) ||
        capacity > SIZE_MAX / sizeof(bool)) {
        GROUP_SPILL_ERR(error, MILENA_ERR_OVERFLOW,
                        "La tabla de salida spill excede el espacio direccionable");
        return MILENA_ERR_OVERFLOW;
    }
#define RESERVE(field, type) do { \
    type *next = realloc(rows->field, capacity * sizeof(type)); \
    if (!next) { GROUP_SPILL_ERR(error, MILENA_ERR_MEMORY, \
        "Sin memoria para materializar resultado agrupado"); return MILENA_ERR_MEMORY; } \
    rows->field = next; \
} while (0)
    RESERVE(keys, char *);
    RESERVE(key_validity, bool);
    RESERVE(values, double);
    RESERVE(counts, uint64_t);
    RESERVE(value_validity, bool);
#undef RESERVE
    rows->capacity = capacity;
    return MILENA_OK;
}

static MilenaStatus capture_group(const MilenaGroupedAggregateResult *result,
                                  void *context, MilenaError *error) {
    GroupSpillRows *rows = context;
    if (!rows || !result || !result->key || result->key_length == 0 ||
        (result->key[0] == 0 && result->key_length != 1) ||
        (result->key[0] != 0 && result->key[0] != 1)) {
        GROUP_SPILL_ERR(error, MILENA_ERR_DATA,
                        "Clave tipada inválida al finalizar #agrupar spill");
        return MILENA_ERR_DATA;
    }
    if (rows->count >= rows->max_groups) {
        GROUP_SPILL_ERR(error, MILENA_ERR_OVERFLOW,
                        "Se agotó el límite AST de grupos de salida de #agrupar");
        return MILENA_ERR_OVERFLOW;
    }
    MilenaStatus status = rows_reserve(rows, rows->count + 1u, error);
    if (status != MILENA_OK) return status;
    size_t at = rows->count;
    bool key_valid = result->key[0] == 1;
    size_t text_length = result->key_length - 1u;
    rows->keys[at] = NULL;
    if (key_valid) {
        rows->keys[at] = malloc(text_length + 1u);
        if (!rows->keys[at]) {
            GROUP_SPILL_ERR(error, MILENA_ERR_MEMORY,
                            "Sin memoria para copiar clave agrupada");
            return MILENA_ERR_MEMORY;
        }
        if (text_length) memcpy(rows->keys[at], result->key + 1u, text_length);
        rows->keys[at][text_length] = '\0';
    }
    rows->key_validity[at] = key_valid;
    rows->value_validity[at] = rows->operation == MILENA_AGG_COUNT ||
                                result->aggregate.has_values;
    switch (rows->operation) {
        case MILENA_AGG_COUNT:
            if (result->aggregate.count > (uint64_t)INT64_MAX) {
                free(rows->keys[at]); rows->keys[at] = NULL;
                GROUP_SPILL_ERR(error, MILENA_ERR_OVERFLOW,
                                "Conteo agrupado excede INT64_MAX");
                return MILENA_ERR_OVERFLOW;
            }
            rows->counts[at] = result->aggregate.count;
            rows->values[at] = 0.0;
            break;
        case MILENA_AGG_SUM: rows->values[at] = result->aggregate.sum; break;
        case MILENA_AGG_MEAN: rows->values[at] = result->aggregate.mean; break;
        case MILENA_AGG_MIN: rows->values[at] = result->aggregate.min; break;
        case MILENA_AGG_MAX: rows->values[at] = result->aggregate.max; break;
        default:
            free(rows->keys[at]); rows->keys[at] = NULL;
            GROUP_SPILL_ERR(error, MILENA_ERR_UNSUPPORTED,
                            "Métrica no admitida por el adaptador spill");
            return MILENA_ERR_UNSUPPORTED;
    }
    rows->count++;
    return MILENA_OK;
}

static bool numeric_f64(const MilenaTable *table, size_t column, size_t row,
                        double *value, MilenaError *error) {
    const void *raw = NULL;
    if (milena_table_get_array_value(table, column, row, &raw, error) != MILENA_OK ||
        !raw) return false;
    *value = *(const double *)raw;
    return isfinite(*value);
}

MilenaStatus milena_language_group_by_spill(
    MilenaTable *out, const MilenaTable *source, const char *key_name,
    const MilenaAggregateSpec *specification, const ASTNode *policy,
    MilenaError *error) {
    if (!out || !source || !key_name || !specification || !policy ||
        policy->type != AST_AGRUPACION_SPILL || !policy->value ||
        policy->group_memory_budget_bytes == 0 ||
        policy->group_spill_quota_bytes == 0 ||
        policy->group_max_key_bytes < 2 ||
        policy->group_max_output_groups == 0 ||
        policy->group_max_runs == 0 ||
        policy->group_max_runs > MILENA_GROUPED_HARD_MAX_RUNS) {
        GROUP_SPILL_ERR(error, MILENA_ERR_ARGUMENT,
                        "Configuración AST inválida para spill #agrupar");
        return MILENA_ERR_ARGUMENT;
    }
    size_t path_length = strlen(policy->value);
    if (path_length == 0 || path_length > 220u) {
        GROUP_SPILL_ERR(error, MILENA_ERR_ARGUMENT,
                        "La ruta scratch de #spill excede el límite de 220 bytes");
        return MILENA_ERR_ARGUMENT;
    }
    int key_index = milena_table_column_index(source, key_name);
    int value_index = milena_table_column_index(source, specification->value_column);
    if (key_index < 0 || value_index < 0) {
        GROUP_SPILL_ERR(error, MILENA_ERR_DATA,
                        "La clave o métrica de #agrupar no existe en MilenaTable");
        return MILENA_ERR_DATA;
    }
    const MilenaTableColumn *key_column = &source->columns[key_index];
    const MilenaTableColumn *value_column = &source->columns[value_index];
    if (key_column->type != MILENA_COLUMN_STRING ||
        (specification->operation != MILENA_AGG_COUNT &&
         (value_column->type != MILENA_COLUMN_ARRAY ||
          value_column->values.dtype != MILENA_DTYPE_FLOAT64))) {
        GROUP_SPILL_ERR(error, MILENA_ERR_UNSUPPORTED,
            "#spill admite clave texto y una métrica FLOAT64; conteo admite cualquier columna");
        return MILENA_ERR_UNSUPPORTED;
    }
    MilenaGroupedAggregate reducer = {0};
    MilenaStatus status = milena_grouped_aggregate_open_with_max_runs(policy->value,
        policy->group_memory_budget_bytes, policy->group_max_key_bytes,
        policy->group_spill_quota_bytes, policy->group_max_runs,
        &reducer, error);
    if (status != MILENA_OK) return status;
    for (size_t row = 0; status == MILENA_OK && row < source->row_count; ++row) {
        bool key_valid = !milena_table_is_null(source, (size_t)key_index, row);
        const char *text = NULL;
        if (key_valid) status = milena_table_get_string(source,
            (size_t)key_index, row, &text, error);
        if (status != MILENA_OK) break;
        size_t text_length = key_valid ? strlen(text) : 0u;
        size_t key_length = text_length + 1u;
        if (key_length > policy->group_max_key_bytes) {
            GROUP_SPILL_ERR(error, MILENA_ERR_OVERFLOW,
                           "Clave de #agrupar supera el límite AST de bytes");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        unsigned char *key = malloc(key_length);
        if (!key) {
            GROUP_SPILL_ERR(error, MILENA_ERR_MEMORY,
                           "Sin memoria para codificar clave de agrupación");
            status = MILENA_ERR_MEMORY;
            break;
        }
        key[0] = key_valid ? 1u : 0u;
        if (text_length) memcpy(key + 1u, text, text_length);
        if (milena_table_is_null(source, (size_t)value_index, row)) {
            status = milena_grouped_aggregate_add_null(&reducer, key,
                                                        key_length, error);
        } else {
            double value = 1.0;
            if (specification->operation != MILENA_AGG_COUNT &&
                !numeric_f64(source, (size_t)value_index, row, &value, error)) {
                if (!error || error->code == MILENA_OK)
                    GROUP_SPILL_ERR(error, MILENA_ERR_TYPE,
                                   "#spill requiere métrica finita FLOAT64");
                status = error ? error->code : MILENA_ERR_TYPE;
            }
            if (status == MILENA_OK)
                status = milena_grouped_aggregate_add(&reducer, key, key_length,
                                                       value, error);
        }
        free(key);
    }
    GroupSpillRows rows = {0};
    rows.operation = specification->operation;
    rows.max_groups = policy->group_max_output_groups;
    if (status == MILENA_OK)
        status = milena_grouped_aggregate_finalize(&reducer, capture_group,
                                                   &rows, NULL, error);
    MilenaStatus close_status = milena_grouped_aggregate_close(&reducer,
        status == MILENA_OK ? error : NULL);
    if (status == MILENA_OK) status = close_status;
    (void)remove(policy->value);

    MilenaArray values = {0};
    if (status == MILENA_OK) {
        size_t shape[] = {rows.count};
        MilenaDType dtype = specification->operation == MILENA_AGG_COUNT ?
                            MILENA_DTYPE_INT64 : MILENA_DTYPE_FLOAT64;
        status = milena_array_zeros(&values, dtype, 1, shape, error);
        bool *validity = rows.count ? calloc(rows.count, sizeof(bool)) : NULL;
        if (status == MILENA_OK && rows.count && !validity) {
            GROUP_SPILL_ERR(error, MILENA_ERR_MEMORY,
                           "Sin memoria para validez del resultado spill");
            status = MILENA_ERR_MEMORY;
        }
        for (size_t i = 0; status == MILENA_OK && i < rows.count; ++i) {
            validity[i] = rows.value_validity[i];
            if (!validity[i]) continue;
            void *cell = (unsigned char *)milena_array_data(&values) +
                         i * values.itemsize;
            if (dtype == MILENA_DTYPE_INT64) {
                int64_t count = (int64_t)rows.counts[i];
                memcpy(cell, &count, sizeof(count));
            } else memcpy(cell, &rows.values[i], sizeof(double));
        }
        if (status == MILENA_OK)
            status = milena_table_add_string_column_copy(out, key_name,
                (const char *const *)rows.keys, rows.count, rows.key_validity,
                error);
        const char *suffix = specification->operation == MILENA_AGG_COUNT ? "conteo" :
            specification->operation == MILENA_AGG_SUM ? "suma" :
            specification->operation == MILENA_AGG_MEAN ? "media" :
            specification->operation == MILENA_AGG_MIN ? "minimo" : "maximo";
        char aggregate_name[256];
        int n = snprintf(aggregate_name, sizeof(aggregate_name), "%s_%s",
                         specification->value_column, suffix);
        if (status == MILENA_OK && (n < 0 || (size_t)n >= sizeof(aggregate_name))) {
            GROUP_SPILL_ERR(error, MILENA_ERR_OVERFLOW,
                            "Nombre de salida #agrupar demasiado largo");
            status = MILENA_ERR_OVERFLOW;
        }
        if (status == MILENA_OK)
            status = milena_table_add_column_copy(out, aggregate_name, &values,
                                                   validity, error);
        free(validity);
    }
    milena_array_release(&values);
    rows_release(&rows);
    if (status != MILENA_OK) milena_table_destroy(out);
    return status;
}
