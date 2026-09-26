#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "dataset.h"
#include <time.h>

DatasetLimits dataset_default_limits(void) {
    DatasetLimits limits = {5000, 70, 1024 * 1024};
    return limits;
}

static bool dataset_record_limit_from_field(size_t max_field_bytes,
                                              size_t max_columns,
                                              size_t *record_bytes) {
    size_t fields = 0;
    return record_bytes && max_field_bytes > 0 && max_columns > 0 &&
        milena_size_mul(max_field_bytes, max_columns, &fields) &&
        milena_size_add(fields, max_columns, record_bytes);
}

static bool dataset_record_limit_from_legacy(const DatasetLimits *limits,
                                              size_t *record_bytes) {
    return limits && dataset_record_limit_from_field(limits->max_field_bytes,
        limits->max_columns, record_bytes);
}

DatasetLoadLimits dataset_default_load_limits(void) {
    DatasetLimits legacy = dataset_default_limits();
    DatasetLoadLimits limits = {
        legacy.max_rows, legacy.max_columns, 0, 0, 0, legacy.max_field_bytes, 0.0
    };
    return limits;
}

static void free_fields(char **fields, size_t count) {
    if (!fields) return;
    for (size_t i = 0; i < count; i++) free(fields[i]);
    free(fields);
}

void dataset_init(Dataset *dataset) {
    if (!dataset) return;
    memset(dataset, 0, sizeof(*dataset));
}

void dataset_destroy(Dataset *dataset) {
    if (!dataset) return;
    free(dataset->filename);
    free_fields(dataset->headers, dataset->column_count);
    if (dataset->rows) {
        for (size_t r = 0; r < dataset->row_count; r++) {
            free_fields(dataset->rows[r], dataset->column_count);
        }
    }
    free(dataset->rows);
    dataset_init(dataset);
}

static MilenaStatus grow_record(char **buffer, size_t *capacity, size_t need,
                                size_t maximum_capacity) {
    if (need <= *capacity) return MILENA_OK;
    if (need > maximum_capacity || maximum_capacity == 0) return MILENA_ERR_OVERFLOW;
    size_t next = *capacity ? *capacity : (maximum_capacity < 256 ? maximum_capacity : 256);
    while (next < need) {
        if (next > maximum_capacity / 2) {
            next = maximum_capacity;
            break;
        }
        next *= 2;
    }
    if (next < need) return MILENA_ERR_OVERFLOW;
    char *tmp = (char *)realloc(*buffer, next);
    if (!tmp) return MILENA_ERR_MEMORY;
    *buffer = tmp;
    *capacity = next;
    return MILENA_OK;
}

static double dataset_now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0)
        return (double)monotonic.tv_sec * 1000.0 +
               (double)monotonic.tv_nsec / 1000000.0;
#endif
    struct timespec realtime;
    if (timespec_get(&realtime, TIME_UTC) == TIME_UTC)
        return (double)realtime.tv_sec * 1000.0 +
               (double)realtime.tv_nsec / 1000000.0;
    return 0.0;
}

static bool dataset_time_exceeded(double started_ms, double maximum_ms) {
    return maximum_ms > 0.0 && dataset_now_ms() - started_ms >= maximum_ms;
}

static MilenaStatus dataset_limit_error(MilenaError *error, size_t line,
                                        const char *message) {
    milena_error_set(error, MILENA_ERR_DATA, line, 1, 0, message);
    return MILENA_ERR_DATA;
}

/* Tracks requested sizes of loader-retained Dataset allocations, not allocator
 * metadata, raw-record scratch, or temporary realloc peaks. */
typedef struct {
    size_t retained_bytes;
    size_t maximum_bytes;
} DatasetMemoryBudget;

static MilenaStatus dataset_memory_resize(DatasetMemoryBudget *budget,
                                           size_t old_bytes, size_t new_bytes,
                                           size_t line, MilenaError *error) {
    if (!budget) return MILENA_OK;
    if (old_bytes > budget->retained_bytes) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, line, 1, 0,
                         "Desbordamiento en la contabilidad de memoria CSV");
        return MILENA_ERR_OVERFLOW;
    }
    size_t next_bytes = 0;
    if (!milena_size_add(budget->retained_bytes - old_bytes, new_bytes,
                         &next_bytes)) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, line, 1, 0,
                         "Desbordamiento calculando las asignaciones retenidas del CSV");
        return MILENA_ERR_OVERFLOW;
    }
    if (budget->maximum_bytes != 0 && next_bytes > budget->maximum_bytes)
        return dataset_limit_error(error, line,
            "El CSV supera el presupuesto de memoria retenida configurado");
    budget->retained_bytes = next_bytes;
    return MILENA_OK;
}

typedef struct {
    size_t max_bytes;
    size_t bytes_read;
    int pending_byte;
    bool has_pending_byte;
} DatasetInputBudget;

/* Physical bytes are charged once when first read. A non-LF byte looked ahead
 * after CR remains pending for the next record, without being charged again. */
static MilenaStatus read_source_byte(FILE *file, DatasetInputBudget *budget,
                                     int *byte, size_t line,
                                     MilenaError *error) {
    if (!file || !byte) return MILENA_ERR_ARGUMENT;
    if (budget && budget->has_pending_byte) {
        *byte = budget->pending_byte;
        budget->has_pending_byte = false;
        return MILENA_OK;
    }
    int ch = fgetc(file);
    if (ch == EOF) {
        *byte = EOF;
        return MILENA_ERR_IO;
    }
    if (budget && budget->max_bytes > 0) {
        if (budget->bytes_read >= budget->max_bytes)
            return dataset_limit_error(error, line,
                "El CSV supera el límite total de bytes de entrada configurado");
        budget->bytes_read++;
    }
    *byte = ch;
    return MILENA_OK;
}

/* Reads one CSV record with a strict logical-byte cap before growing the buffer. */
static MilenaStatus read_record(FILE *file, char **record, size_t *line,
                              size_t max_record_bytes,
                              DatasetInputBudget *input_budget,
                              double started_ms,
                              double max_elapsed_milliseconds,
                              MilenaError *error) {
    if (!file || !record || !line || !input_budget || max_record_bytes == 0 ||
        max_record_bytes == SIZE_MAX) return MILENA_ERR_ARGUMENT;
    *record = NULL;
    size_t capacity = 0, length = 0, start_line = *line;
    size_t maximum_capacity = max_record_bytes + 1u;
    bool in_quotes = false;
    int ch;

    while (true) {
        MilenaStatus byte_status = read_source_byte(file, input_budget, &ch,
                                                   start_line, error);
        if (byte_status == MILENA_ERR_IO) {
            ch = EOF;
            break;
        }
        if (byte_status != MILENA_OK) {
            free(*record);
            *record = NULL;
            return byte_status;
        }
        if (max_elapsed_milliseconds > 0.0 && (length & 4095u) == 0u &&
            dataset_time_exceeded(started_ms, max_elapsed_milliseconds)) {
            free(*record);
            *record = NULL;
            return dataset_limit_error(error, start_line,
                                       "El CSV superó el límite de tiempo de carga");
        }
        if (ch == '\r') {
            int next = EOF;
            byte_status = read_source_byte(file, input_budget, &next,
                                           start_line, error);
            if (byte_status == MILENA_OK && next != '\n') {
                input_budget->pending_byte = next;
                input_budget->has_pending_byte = true;
            } else if (byte_status != MILENA_OK &&
                       byte_status != MILENA_ERR_IO) {
                free(*record);
                *record = NULL;
                return byte_status;
            }
            if (in_quotes) {
                if (length >= max_record_bytes) goto too_large;
                MilenaStatus st = grow_record(record, &capacity, length + 2u,
                                              maximum_capacity);
                if (st != MILENA_OK) goto allocation_fail;
                (*record)[length++] = '\n';
                (*line)++;
            } else {
                break;
            }
        } else if (ch == '\n') {
            if (in_quotes) {
                if (length >= max_record_bytes) goto too_large;
                MilenaStatus st = grow_record(record, &capacity, length + 2u,
                                              maximum_capacity);
                if (st != MILENA_OK) goto allocation_fail;
                (*record)[length++] = '\n';
                (*line)++;
            } else {
                break;
            }
        } else {
            if (length >= max_record_bytes) goto too_large;
            MilenaStatus st = grow_record(record, &capacity, length + 2u,
                                          maximum_capacity);
            if (st != MILENA_OK) goto allocation_fail;
            (*record)[length++] = (char)ch;
            if (ch == '"') in_quotes = !in_quotes;
        }
    }

    if (ch == EOF && length == 0) {
        free(*record);
        *record = NULL;
        return MILENA_ERR_IO;
    }
    if (in_quotes) {
        milena_error_set(error, MILENA_ERR_PARSE, start_line, 1, 0,
                       "CSV con comillas sin cerrar");
        free(*record);
        *record = NULL;
        return MILENA_ERR_PARSE;
    }
    {
        MilenaStatus st = grow_record(record, &capacity, length + 1u,
                                      maximum_capacity);
        if (st != MILENA_OK) goto allocation_fail;
    }
    (*record)[length] = '\0';
    return MILENA_OK;

too_large:
    free(*record);
    *record = NULL;
    return dataset_limit_error(error, start_line,
                               "El registro CSV supera el límite de bytes configurado");

allocation_fail:
    free(*record);
    *record = NULL;
    milena_error_set(error, MILENA_ERR_MEMORY, start_line, 1, 0,
                   "Memoria insuficiente leyendo CSV o tamaño de registro desbordado");
    return MILENA_ERR_MEMORY;
}

/* Counts logical CSV fields without allocating them. Doubled quotes inside a
 * quoted field do not end the quoted region. */
static bool csv_field_count(const char *record, char delimiter, size_t *count) {
    if (!record || !count) return false;
    size_t fields = 1;
    bool in_quotes = false;
    for (size_t i = 0; record[i] != '\0'; ++i) {
        if (record[i] == '"') {
            if (in_quotes && record[i + 1u] == '"') {
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (record[i] == delimiter && !in_quotes) {
            if (fields == SIZE_MAX) return false;
            ++fields;
        }
    }
    *count = fields;
    return true;
}

static MilenaStatus append_field(char ***fields, size_t *count, size_t *capacity,
                                 char *field, DatasetMemoryBudget *budget,
                                 size_t line, MilenaError *error) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 8u;
        size_t old_bytes = 0, new_bytes = 0;
        if (next < *capacity ||
            !milena_size_mul(*capacity, sizeof(**fields), &old_bytes) ||
            !milena_size_mul(next, sizeof(**fields), &new_bytes)) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, line, 1, 0,
                             "Desbordamiento del vector de campos CSV");
            return MILENA_ERR_OVERFLOW;
        }
        size_t old_accounted = budget ? budget->retained_bytes : 0;
        MilenaStatus status = dataset_memory_resize(budget, old_bytes,
                                                     new_bytes, line, error);
        if (status != MILENA_OK) return status;
        char **tmp = (char **)realloc(*fields, new_bytes);
        if (!tmp) {
            if (budget) budget->retained_bytes = old_accounted;
            return MILENA_ERR_MEMORY;
        }
        *fields = tmp;
        *capacity = next;
    }
    (*fields)[(*count)++] = field;
    return MILENA_OK;
}

static MilenaStatus append_char(char **text, size_t *length, size_t *capacity,
                                char ch, DatasetMemoryBudget *budget,
                                size_t line, MilenaError *error) {
    if (*length + 1u >= *capacity) {
        size_t next = *capacity ? *capacity * 2u : 32u;
        if (next < *capacity) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, line, 1, 0,
                             "Desbordamiento de capacidad de campo CSV");
            return MILENA_ERR_OVERFLOW;
        }
        size_t old_accounted = budget ? budget->retained_bytes : 0;
        MilenaStatus status = dataset_memory_resize(budget, *capacity, next,
                                                     line, error);
        if (status != MILENA_OK) return status;
        char *tmp = (char *)realloc(*text, next);
        if (!tmp) {
            if (budget) budget->retained_bytes = old_accounted;
            return MILENA_ERR_MEMORY;
        }
        *text = tmp;
        *capacity = next;
    }
    (*text)[(*length)++] = ch;
    return MILENA_OK;
}

static MilenaStatus parse_record(const char *record, char delimiter,
                                 char ***out_fields, size_t *out_count,
                                 DatasetMemoryBudget *budget, size_t line,
                                 MilenaError *error) {
    if (!record || !out_fields || !out_count) return MILENA_ERR_ARGUMENT;
    size_t budget_checkpoint = budget ? budget->retained_bytes : 0;
    char **fields = NULL;
    size_t count = 0, field_capacity = 0;
    const char *p = record;

    char *field = NULL;
    while (true) {
        size_t length = 0, capacity = 0;
        bool quoted = false, closed_quote = false;
        field = NULL;

        if (*p == '"') {
            quoted = true;
            p++;
        }

        while (*p) {
            char ch = *p;
            if (quoted) {
                if (ch == '"') {
                    if (p[1] == '"') {
                        MilenaStatus st = append_char(&field, &length, &capacity, '"', budget, line, error);
                        if (st != MILENA_OK) goto fail;
                        p += 2;
                        continue;
                    }
                    p++;
                    closed_quote = true;
                    quoted = false;
                    continue;
                }
                MilenaStatus st = append_char(&field, &length, &capacity, ch, budget, line, error);
                if (st != MILENA_OK) goto fail;
                p++;
            } else {
                if (ch == delimiter) break;
                if (ch == '"' && !closed_quote) {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                   "Comilla inesperada dentro de campo CSV");
                    goto fail;
                }
                if (closed_quote && ch != ' ' && ch != '\t') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                   "Caracteres después de comilla CSV");
                    goto fail;
                }
                if (!closed_quote) {
                    MilenaStatus st = append_char(&field, &length, &capacity, ch, budget, line, error);
                    if (st != MILENA_OK) goto fail;
                }
                p++;
            }
        }

        MilenaStatus st = append_char(&field, &length, &capacity, '\0', budget, line, error);
        if (st != MILENA_OK) goto fail;
        st = append_field(&fields, &count, &field_capacity, field, budget, line, error);
        if (st != MILENA_OK) goto fail;
        field = NULL;

        if (*p == delimiter) {
            p++;
            if (*p == '\0') {
                MilenaStatus reserve_status = dataset_memory_resize(
                    budget, 0, 1u, line, error);
                if (reserve_status != MILENA_OK) goto fail;
                size_t empty_accounted = budget ? budget->retained_bytes : 0;
                char *empty = milena_strdup("");
                if (!empty) {
                    if (budget) budget->retained_bytes = empty_accounted - 1u;
                    goto fail;
                }
                if (append_field(&fields, &count, &field_capacity, empty,
                                 budget, line, error) != MILENA_OK) {
                    free(empty);
                    goto fail;
                }
                break;
            }
            continue;
        }
        break;
    }

    *out_fields = fields;
    *out_count = count;
    return MILENA_OK;

fail:
    free(field);
    free_fields(fields, count);
    if (budget) budget->retained_bytes = budget_checkpoint;
    return error && error->code != MILENA_OK ? error->code : MILENA_ERR_MEMORY;
}

static MilenaStatus add_row(Dataset *dataset, char **row, size_t max_rows,
                            DatasetMemoryBudget *budget, size_t line,
                            MilenaError *error) {
    if (dataset->row_count == dataset->row_capacity) {
        size_t next = dataset->row_capacity ? dataset->row_capacity * 2u : 64u;
        size_t allocation_bytes = 0;
        if (next < dataset->row_capacity) return MILENA_ERR_OVERFLOW;
        if (next > max_rows) next = max_rows;
        if (next <= dataset->row_capacity ||
            !milena_size_mul(next, sizeof(*dataset->rows), &allocation_bytes))
            return MILENA_ERR_OVERFLOW;
        size_t old_bytes = 0;
        if (!milena_size_mul(dataset->row_capacity, sizeof(*dataset->rows),
                             &old_bytes)) return MILENA_ERR_OVERFLOW;
        size_t old_accounted = budget ? budget->retained_bytes : 0;
        MilenaStatus budget_status = dataset_memory_resize(budget, old_bytes,
            allocation_bytes, line, error);
        if (budget_status != MILENA_OK) return budget_status;
        char ***tmp = (char ***)realloc(dataset->rows, allocation_bytes);
        if (!tmp) {
            if (budget) budget->retained_bytes = old_accounted;
            milena_error_set(error, MILENA_ERR_MEMORY, line, 1, dataset->row_count,
                           "Memoria insuficiente agregando fila");
            return MILENA_ERR_MEMORY;
        }
        dataset->rows = tmp;
        dataset->row_capacity = next;
    }
    dataset->rows[dataset->row_count++] = row;
    return MILENA_OK;
}

static bool headers_valid(char **headers, size_t count, MilenaError *error) {
    for (size_t i = 0; i < count; i++) {
        if (!headers[i] || headers[i][0] == '\0') {
            milena_error_set(error, MILENA_ERR_DATA, 1, i + 1, 0,
                           "Nombre de columna vacío");
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(headers[i], headers[j]) == 0) {
                milena_error_set(error, MILENA_ERR_DATA, 1, i + 1, 0,
                               "Columnas duplicadas");
                return false;
            }
        }
    }
    return true;
}

MilenaStatus dataset_load_csv_with_resource_limits(
    Dataset *dataset, const char *filename, char delimiter,
    const DatasetLoadLimits *limits, MilenaError *error) {
    if (!dataset || !filename || delimiter == '\0') return MILENA_ERR_ARGUMENT;
    DatasetLoadLimits defaults = dataset_default_load_limits();
    if (!limits) limits = &defaults;
    MilenaError local;
    if (!error) error = &local;
    milena_error_clear(error);
    if (limits->max_rows == 0 || limits->max_columns == 0 ||
        (limits->max_record_bytes == 0 && limits->max_field_bytes == 0) ||
        limits->max_record_bytes == SIZE_MAX ||
        !isfinite(limits->max_elapsed_milliseconds) ||
        limits->max_elapsed_milliseconds < 0.0) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "Límites CSV inválidos: filas y columnas deben ser positivos, y debe existir un límite de registro válido");
        return MILENA_ERR_ARGUMENT;
    }
    size_t header_record_limit = limits->max_record_bytes;
    if (limits->max_field_bytes > 0) {
        size_t field_based_limit = 0;
        if (!dataset_record_limit_from_field(limits->max_field_bytes,
                limits->max_columns, &field_based_limit) ||
            field_based_limit == SIZE_MAX) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "Desbordamiento calculando el límite CSV derivado de campos");
            return MILENA_ERR_OVERFLOW;
        }
        if (header_record_limit == 0 || field_based_limit < header_record_limit)
            header_record_limit = field_based_limit;
    }
    if (header_record_limit == 0 || header_record_limit == SIZE_MAX) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "El límite de registro CSV debe ser positivo y finito");
        return MILENA_ERR_ARGUMENT;
    }

    double started_ms = dataset_now_ms();
    FILE *file = fopen(filename, "rb");
    if (!file) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo abrir el CSV");
        return MILENA_ERR_IO;
    }

    Dataset tmp;
    dataset_init(&tmp);
    MilenaStatus status = MILENA_OK;
    DatasetMemoryBudget memory_budget = {0, limits->max_memory_bytes};
    size_t filename_bytes = 0;
    if (!milena_size_add(strlen(filename), 1u, &filename_bytes)) {
        fclose(file);
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "Desbordamiento calculando el nombre de archivo CSV");
        return MILENA_ERR_OVERFLOW;
    }
    status = dataset_memory_resize(&memory_budget, 0, filename_bytes, 0, error);
    if (status != MILENA_OK) {
        fclose(file);
        dataset_destroy(&tmp);
        return status;
    }
    tmp.filename = milena_strdup(filename);
    if (!tmp.filename) {
        fclose(file);
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0, "Sin memoria para nombre de archivo");
        return MILENA_ERR_MEMORY;
    }

    size_t line = 1;
    char *record = NULL;
    DatasetInputBudget input_budget = {limits->max_input_bytes, 0u, 0, false};
    status = read_record(file, &record, &line,
        header_record_limit, &input_budget, started_ms,
        limits->max_elapsed_milliseconds, error);
    if (status != MILENA_OK) {
        if (status == MILENA_ERR_IO && feof(file)) {
            milena_error_set(error, MILENA_ERR_DATA, 1, 1, 0, "CSV vacío");
            status = MILENA_ERR_DATA;
        }
        goto fail;
    }
    size_t header_columns = 0;
    if (!csv_field_count(record, delimiter, &header_columns)) {
        status = MILENA_ERR_OVERFLOW;
        milena_error_set(error, status, 1, 1, 0,
                         "Desbordamiento contando columnas del encabezado CSV");
        goto fail;
    }
    if (header_columns > limits->max_columns) {
        status = dataset_limit_error(error, 1,
                                     "El encabezado CSV supera el máximo de columnas permitido");
        goto fail;
    }
    status = parse_record(record, delimiter, &tmp.headers, &tmp.column_count,
                          &memory_budget, 1u, error);
    free(record);
    record = NULL;
    if (status != MILENA_OK || tmp.column_count == 0 ||
        !headers_valid(tmp.headers, tmp.column_count, error)) {
        status = status == MILENA_OK ? MILENA_ERR_DATA : status;
        goto fail;
    }
    size_t data_record_limit = limits->max_record_bytes;
    if (limits->max_field_bytes > 0) {
        size_t field_based_limit = 0;
        if (!dataset_record_limit_from_field(limits->max_field_bytes,
                tmp.column_count, &field_based_limit) ||
            field_based_limit == SIZE_MAX) {
            status = MILENA_ERR_OVERFLOW;
            milena_error_set(error, status, 1, 1, 0,
                             "Desbordamiento calculando el límite CSV por columnas físicas");
            goto fail;
        }
        if (data_record_limit == 0 || field_based_limit < data_record_limit)
            data_record_limit = field_based_limit;
    }
    if (data_record_limit == 0 || data_record_limit == SIZE_MAX) {
        status = MILENA_ERR_ARGUMENT;
        milena_error_set(error, status, 1, 1, 0,
                         "El límite de registro CSV debe ser positivo y finito");
        goto fail;
    }
    if (dataset_time_exceeded(started_ms, limits->max_elapsed_milliseconds)) {
        status = dataset_limit_error(error, line,
                                     "El CSV superó el límite de tiempo de carga");
        goto fail;
    }

    /* Count source records, not only accepted table rows, so malformed input
     * cannot bypass the ingestion row budget. */
    size_t rows_seen = 0;
    while (true) {
        status = read_record(file, &record, &line,
            data_record_limit, &input_budget, started_ms,
            limits->max_elapsed_milliseconds, error);
        if (status == MILENA_ERR_IO && feof(file)) {
            status = MILENA_OK;
            break;
        }
        if (status != MILENA_OK) goto fail;
        if (rows_seen >= limits->max_rows) {
            status = dataset_limit_error(error, line,
                                         "El CSV supera el máximo de filas de entrada permitido");
            goto fail;
        }
        rows_seen++;
        if (record[0] == '\0') {
            free(record);
            record = NULL;
            continue;
        }
        size_t record_columns = 0;
        if (!csv_field_count(record, delimiter, &record_columns)) {
            status = MILENA_ERR_OVERFLOW;
            milena_error_set(error, status, line, 1, 0,
                             "Desbordamiento contando columnas del registro CSV");
            goto fail;
        }
        if (record_columns > limits->max_columns) {
            status = dataset_limit_error(error, line,
                                         "Un registro CSV supera el máximo de columnas permitido");
            goto fail;
        }
        if (record_columns != tmp.column_count) {
            tmp.invalid_rows++;
            free(record);
            record = NULL;
            continue;
        }

        char **row = NULL;
        size_t fields = 0;
        size_t row_budget_checkpoint = memory_budget.retained_bytes;
        status = parse_record(record, delimiter, &row, &fields,
                              &memory_budget, line, error);
        free(record);
        record = NULL;
        if (status == MILENA_ERR_MEMORY || status == MILENA_ERR_OVERFLOW ||
            status == MILENA_ERR_DATA) {
            free_fields(row, fields);
            goto fail;
        }
        if (status != MILENA_OK || fields != tmp.column_count) {
            tmp.invalid_rows++;
            free_fields(row, fields);
            memory_budget.retained_bytes = row_budget_checkpoint;
            milena_error_clear(error);
            status = MILENA_OK;
            continue;
        }
        if (dataset_time_exceeded(started_ms, limits->max_elapsed_milliseconds)) {
            free_fields(row, fields);
            memory_budget.retained_bytes = row_budget_checkpoint;
            status = dataset_limit_error(error, line,
                                         "El CSV superó el límite de tiempo de carga");
            goto fail;
        }
        status = add_row(&tmp, row, limits->max_rows, &memory_budget,
                          line, error);
        if (status != MILENA_OK) {
            free_fields(row, fields);
            goto fail;
        }
    }

    if (dataset_time_exceeded(started_ms, limits->max_elapsed_milliseconds)) {
        status = dataset_limit_error(error, line,
                                     "El CSV superó el límite de tiempo de carga");
        goto fail;
    }
    if (fclose(file) != 0) {
        file = NULL;
        milena_error_set(error, MILENA_ERR_IO, line, 0, 0,
                         "No se pudo cerrar el CSV después de leerlo");
        status = MILENA_ERR_IO;
        goto fail;
    }
    file = NULL;
    dataset_destroy(dataset);
    *dataset = tmp;
    return MILENA_OK;

fail:
    free(record);
    if (file) fclose(file);
    dataset_destroy(&tmp);
    return status;
}

MilenaStatus dataset_load_csv_with_limits(Dataset *dataset, const char *filename,
                                        char delimiter, const DatasetLimits *limits,
                                        MilenaError *error) {
    DatasetLimits defaults = dataset_default_limits();
    if (!limits) limits = &defaults;
    if (!limits->max_rows || !limits->max_columns || !limits->max_field_bytes) {
        if (error) {
            milena_error_clear(error);
            milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                             "Los límites históricos del CSV deben ser positivos");
        }
        return MILENA_ERR_ARGUMENT;
    }
    size_t max_record_bytes = 0;
    if (!dataset_record_limit_from_legacy(limits, &max_record_bytes) ||
        max_record_bytes == SIZE_MAX) {
        if (error) {
            milena_error_clear(error);
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "Desbordamiento calculando el límite histórico de registro");
        }
        return MILENA_ERR_OVERFLOW;
    }
    DatasetLoadLimits resource_limits = {
        limits->max_rows, limits->max_columns, 0, 0, 0,
        limits->max_field_bytes, 0.0
    };
    return dataset_load_csv_with_resource_limits(dataset, filename, delimiter,
                                                  &resource_limits, error);
}

MilenaStatus dataset_load_csv(Dataset *dataset, const char *filename,
                            char delimiter, MilenaError *error) {
    DatasetLimits limits = dataset_default_limits();
    return dataset_load_csv_with_limits(dataset, filename, delimiter,
                                        &limits, error);
}

int dataset_column_index(const Dataset *dataset, const char *name) {
    if (!dataset || !name) return -1;
    for (size_t i = 0; i < dataset->column_count; i++) {
        if (strcmp(dataset->headers[i], name) == 0) return (int)i;
    }
    return -1;
}

static MilenaStatus append_column(Dataset *dataset, const char *name,
                                char **values, MilenaError *error) {
    if (!dataset || !name || !values) return MILENA_ERR_ARGUMENT;
    char *copy = milena_strdup(name);
    if (!copy) return MILENA_ERR_MEMORY;

    size_t new_column_count = dataset->column_count + 1;
    char **new_headers = (char **)calloc(new_column_count, sizeof(*new_headers));
    char ***new_rows = dataset->row_count
        ? (char ***)calloc(dataset->row_count, sizeof(*new_rows)) : NULL;
    if (!new_headers || (dataset->row_count && !new_rows)) {
        free(new_headers);
        free(new_rows);
        free(copy);
        return MILENA_ERR_MEMORY;
    }
    for (size_t c = 0; c < dataset->column_count; c++) {
        new_headers[c] = dataset->headers[c];
    }
    new_headers[dataset->column_count] = copy;

    for (size_t r = 0; r < dataset->row_count; r++) {
        new_rows[r] = (char **)calloc(new_column_count, sizeof(*new_rows[r]));
        if (!new_rows[r]) {
            for (size_t j = 0; j < r; j++) free(new_rows[j]);
            free(new_rows);
            free(new_headers);
            free(copy);
            milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, r + 1,
                           "Sin memoria agregando columna");
            return MILENA_ERR_MEMORY;
        }
        memcpy(new_rows[r], dataset->rows[r],
               dataset->column_count * sizeof(*new_rows[r]));
        new_rows[r][dataset->column_count] = values[r];
    }

    for (size_t r = 0; r < dataset->row_count; r++) free(dataset->rows[r]);
    free(dataset->rows);
    free(dataset->headers);
    free(values);
    dataset->headers = new_headers;
    dataset->rows = new_rows;
    dataset->column_count = new_column_count;
    return MILENA_OK;
}

MilenaStatus dataset_add_product(Dataset *dataset, const char *left,
                               const char *right, const char *output,
                               MilenaError *error) {
    if (!dataset || !left || !right || !output) return MILENA_ERR_ARGUMENT;
    if (dataset_column_index(dataset, output) >= 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "La columna de salida ya existe");
        return MILENA_ERR_DATA;
    }
    int li = dataset_column_index(dataset, left);
    int ri = dataset_column_index(dataset, right);
    if (li < 0 || ri < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "Columna de producto inexistente");
        return MILENA_ERR_DATA;
    }
    char **values = (char **)calloc(dataset->row_count, sizeof(*values));
    if (dataset->row_count && !values) return MILENA_ERR_MEMORY;
    for (size_t r = 0; r < dataset->row_count; r++) {
        double a, b;
        if (milena_parse_double(dataset->rows[r][li], &a) != MILENA_OK ||
            milena_parse_double(dataset->rows[r][ri], &b) != MILENA_OK) {
            free_fields(values, r);
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Valor no numérico en producto");
            return MILENA_ERR_TYPE;
        }
        char buffer[96];
        int written = snprintf(buffer, sizeof(buffer), "%.10g", a * b);
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            free_fields(values, r);
            return MILENA_ERR_OVERFLOW;
        }
        values[r] = milena_strdup(buffer);
        if (!values[r]) {
            free_fields(values, r);
            return MILENA_ERR_MEMORY;
        }
    }
    return append_column(dataset, output, values, error);
}

MilenaStatus dataset_add_month(Dataset *dataset, const char *date_column,
                             const char *output, MilenaError *error) {
    if (!dataset || !date_column || !output) return MILENA_ERR_ARGUMENT;
    if (dataset_column_index(dataset, output) >= 0) return MILENA_ERR_DATA;
    int di = dataset_column_index(dataset, date_column);
    if (di < 0) return MILENA_ERR_DATA;
    char **values = (char **)calloc(dataset->row_count, sizeof(*values));
    if (dataset->row_count && !values) return MILENA_ERR_MEMORY;
    for (size_t r = 0; r < dataset->row_count; r++) {
        int year, month, day;
        if (sscanf(dataset->rows[r][di], "%d-%d-%d", &year, &month, &day) != 3 ||
            year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
            free_fields(values, r);
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Fecha inválida");
            return MILENA_ERR_TYPE;
        }
        char buffer[32];
        (void)snprintf(buffer, sizeof(buffer), "%04d-%02d", year, month);
        values[r] = milena_strdup(buffer);
        if (!values[r]) {
            free_fields(values, r);
            return MILENA_ERR_MEMORY;
        }
    }
    return append_column(dataset, output, values, error);
}

MilenaStatus dataset_filter_positive_product(Dataset *dataset,
                                           const char *left,
                                           const char *right,
                                           MilenaError *error) {
    if (!dataset || !left || !right) return MILENA_ERR_ARGUMENT;
    int li = dataset_column_index(dataset, left);
    int ri = dataset_column_index(dataset, right);
    if (li < 0 || ri < 0) return MILENA_ERR_DATA;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        double a, b;
        MilenaStatus sa = milena_parse_double(dataset->rows[r][li], &a);
        MilenaStatus sb = milena_parse_double(dataset->rows[r][ri], &b);
        if (sa != MILENA_OK || sb != MILENA_OK) {
            milena_error_set(error, MILENA_ERR_TYPE, 0, 0, r + 1,
                           "Valor no numérico en filtro");
            return MILENA_ERR_TYPE;
        }
        if (a * b > 0.0) {
            dataset->rows[write++] = dataset->rows[r];
        } else {
            free_fields(dataset->rows[r], dataset->column_count);
        }
    }
    dataset->row_count = write;
    return MILENA_OK;
}

MilenaStatus dataset_remove_null_rows(Dataset *dataset, MilenaError *error) {
    if (!dataset) return MILENA_ERR_ARGUMENT;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        bool has_null = false;
        for (size_t c = 0; c < dataset->column_count; c++) {
            if (!dataset->rows[r][c] || dataset->rows[r][c][0] == '\0') {
                has_null = true;
                break;
            }
        }
        if (has_null) free_fields(dataset->rows[r], dataset->column_count);
        else dataset->rows[write++] = dataset->rows[r];
    }
    dataset->row_count = write;
    (void)error;
    return MILENA_OK;
}

static bool rows_equal(const Dataset *dataset, size_t a, size_t b) {
    for (size_t c = 0; c < dataset->column_count; c++) {
        if (strcmp(dataset->rows[a][c], dataset->rows[b][c]) != 0) return false;
    }
    return true;
}

MilenaStatus dataset_remove_duplicates(Dataset *dataset, MilenaError *error) {
    if (!dataset) return MILENA_ERR_ARGUMENT;
    size_t write = 0;
    for (size_t r = 0; r < dataset->row_count; r++) {
        bool duplicate = false;
        for (size_t p = 0; p < write; p++) {
            if (rows_equal(dataset, r, p)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) free_fields(dataset->rows[r], dataset->column_count);
        else dataset->rows[write++] = dataset->rows[r];
    }
    dataset->row_count = write;
    (void)error;
    return MILENA_OK;
}

MilenaStatus dataset_save_json(const Dataset *dataset, const char *filename,
                             MilenaError *error) {
    if (!dataset || !filename) return MILENA_ERR_ARGUMENT;
    FILE *out = fopen(filename, "wb");
    if (!out) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo crear JSON");
        return MILENA_ERR_IO;
    }
    fprintf(out, "{\n  \"dataset\": ");
    milena_json_write_string(out, dataset->filename);
    fprintf(out, ",\n  \"columnas\": [");
    for (size_t c = 0; c < dataset->column_count; c++) {
        if (c) fputs(", ", out);
        milena_json_write_string(out, dataset->headers[c]);
    }
    fprintf(out, "],\n  \"filas_invalidas\": %zu,\n  \"filas\": [\n", dataset->invalid_rows);
    for (size_t r = 0; r < dataset->row_count; r++) {
        fprintf(out, "    [");
        for (size_t c = 0; c < dataset->column_count; c++) {
            if (c) fputs(", ", out);
            milena_json_write_string(out, dataset->rows[r][c]);
        }
        fprintf(out, "]%s\n", r + 1 < dataset->row_count ? "," : "");
    }
    fputs("  ]\n}\n", out);
    bool io_error = ferror(out) != 0;
    if (fclose(out) != 0) io_error = true;
    if (io_error) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "Error escribiendo JSON");
        return MILENA_ERR_IO;
    }
    return MILENA_OK;
}

void dataset_print(const Dataset *dataset, size_t max_rows, FILE *stream) {
    if (!dataset) return;
    if (!stream) stream = stdout;
    fprintf(stream, "Dataset: %s\nColumnas: %zu | Filas: %zu | Inválidas: %zu\n",
            dataset->filename ? dataset->filename : "", dataset->column_count,
            dataset->row_count, dataset->invalid_rows);
    for (size_t c = 0; c < dataset->column_count; c++) {
        fprintf(stream, "%s%s", dataset->headers[c], c + 1 < dataset->column_count ? " | " : "\n");
    }
    size_t limit = dataset->row_count < max_rows ? dataset->row_count : max_rows;
    for (size_t r = 0; r < limit; r++) {
        for (size_t c = 0; c < dataset->column_count; c++) {
            fprintf(stream, "%s%s", dataset->rows[r][c], c + 1 < dataset->column_count ? " | " : "\n");
        }
    }
}

bool dataset_cargar_csv(Dataset *dataset, const char *filename) {
    MilenaError error;
    return dataset_load_csv(dataset, filename, ',', &error) == MILENA_OK;
}

bool dataset_guardar_json(const Dataset *dataset, const char *filename) {
    MilenaError error;
    return dataset_save_json(dataset, filename, &error) == MILENA_OK;
}

void dataset_destruir(Dataset *dataset) { dataset_destroy(dataset); }
int dataset_indice_columna(const Dataset *dataset, const char *name) {
    return dataset_column_index(dataset, name);
}
