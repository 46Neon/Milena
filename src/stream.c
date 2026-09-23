#include "stream.h"
#include "grouped_aggregate.h"

#include <float.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define STREAM_MAX_COLUMNS 4096u
#define STREAM_MAX_METRICS 64u
#define STREAM_IO_BUFFER (64u * 1024u)
#define STREAM_INITIAL_RECORD 4096u
#define STREAM_DEFAULT_MAX_RECORD (64u * 1024u * 1024u)
#define STREAM_DEFAULT_MAX_COLUMNS STREAM_MAX_COLUMNS
#define STREAM_HARD_MAX_GROUPS 100000u
#define STREAM_DEFAULT_MAX_GROUPS 1000u
#define STREAM_MAX_GROUP_KEY_BYTES 4096u
#define STREAM_DEFAULT_SPILL_OUTPUT_BYTES (1024u * 1024u * 1024u)
#define STREAM_GROUP_STATE_BUDGET (64u * 1024u * 1024u)

typedef struct {
    double sum;
    double compensation;
    double mean;
    double m2;
    double minimum;
    double maximum;
    size_t count;
} StreamAccumulator;

static double stream_now_ms(void) {
    struct timespec value;
    if (timespec_get(&value, TIME_UTC) != TIME_UTC) return 0.0;
    return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
}

const char *milena_stream_operation_name(MilenaStreamOperation operation) {
    switch (operation) {
    case MILENA_STREAM_SUM: return "suma";
    case MILENA_STREAM_MEAN: return "media";
    case MILENA_STREAM_MIN: return "minimo";
    case MILENA_STREAM_MAX: return "maximo";
    case MILENA_STREAM_COUNT: return "conteo";
    case MILENA_STREAM_VARIANCE: return "varianza";
    case MILENA_STREAM_STDDEV: return "desviacion_estandar";
    default: return "desconocida";
    }
}

static MilenaStatus grow_record(char **record, size_t *capacity,
                                size_t needed, size_t max_record_bytes,
                                MilenaError *error) {
    if (needed <= *capacity) return MILENA_OK;
    if (needed > max_record_bytes) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El registro CSV supera el límite configurado del flujo");
        return MILENA_ERR_OVERFLOW;
    }
    size_t next = *capacity ? *capacity : STREAM_INITIAL_RECORD;
    while (next < needed) {
        if (next > max_record_bytes / 2) {
            next = max_record_bytes;
            break;
        }
        next *= 2;
    }
    char *grown = (char *)realloc(*record, next);
    if (!grown) {
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                         "Memoria insuficiente para el registro CSV");
        return MILENA_ERR_MEMORY;
    }
    *record = grown;
    *capacity = next;
    return MILENA_OK;
}

/* Reads one complete CSV record and reuses the same bounded buffer. */
static MilenaStatus stream_read_record(FILE *file, char **record,
                                       size_t *capacity, size_t max_record_bytes,
                                       size_t *record_length,
                                       MilenaError *error) {
    if (!file || !record || !capacity || !record_length) return MILENA_ERR_ARGUMENT;
    size_t length = 0;
    bool in_quotes = false;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '"') {
            MilenaStatus status = grow_record(record, capacity, length + 2, max_record_bytes, error);
            if (status != MILENA_OK) return status;
            (*record)[length++] = (char)ch;
            if (in_quotes) {
                int next = fgetc(file);
                if (next == '"') {
                    status = grow_record(record, capacity, length + 2, max_record_bytes, error);
                    if (status != MILENA_OK) return status;
                    (*record)[length++] = (char)next;
                } else {
                    if (next != EOF) (void)ungetc(next, file);
                    in_quotes = false;
                }
            } else {
                in_quotes = true;
            }
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r') {
                int next = fgetc(file);
                if (next != '\n' && next != EOF) (void)ungetc(next, file);
            }
            if (!in_quotes) break;
            MilenaStatus status = grow_record(record, capacity, length + 2, max_record_bytes, error);
            if (status != MILENA_OK) return status;
            (*record)[length++] = '\n';
        } else {
            MilenaStatus status = grow_record(record, capacity, length + 2, max_record_bytes, error);
            if (status != MILENA_OK) return status;
            (*record)[length++] = (char)ch;
        }
    }
    if (ch == EOF && length == 0) return MILENA_ERR_IO;
    if (in_quotes) {
        milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                         "CSV con comillas sin cerrar en modo flujo");
        return MILENA_ERR_PARSE;
    }
    (*record)[length] = '\0';
    *record_length = length;
    return MILENA_OK;
}

/* Splits in place; field pointers remain valid until the next record. */
static MilenaStatus stream_split(char *record, char delimiter, char **fields,
                                 size_t field_capacity, size_t *field_count,
                                 MilenaError *error) {
    if (!record || !fields || !field_count) return MILENA_ERR_ARGUMENT;
    if (*record == '\0') {
        if (field_capacity == 0) return MILENA_ERR_OVERFLOW;
        fields[0] = record;
        *field_count = 1;
        return MILENA_OK;
    }
    char *read = record;
    char *write = record;
    size_t count = 0;
    while (true) {
        if (count >= field_capacity) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El CSV supera el máximo de 4096 columnas");
            return MILENA_ERR_OVERFLOW;
        }
        fields[count++] = write;
        bool quoted = *read == '"';
        if (quoted) read++;
        bool closed = false;
        bool delimiter_found = false;
        while (*read) {
            char ch = *read++;
            if (quoted) {
                if (ch == '"') {
                    if (*read == '"') {
                        *write++ = '"';
                        read++;
                    } else {
                        quoted = false;
                        closed = true;
                    }
                } else {
                    *write++ = ch;
                }
            } else if (ch == delimiter) {
                *write++ = '\0';
                delimiter_found = true;
                break;
            } else {
                if (closed && ch != ' ' && ch != '\t') {
                    milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                                     "Caracteres después de comilla CSV");
                    return MILENA_ERR_PARSE;
                }
                *write++ = ch;
            }
        }
        if (quoted) {
            milena_error_set(error, MILENA_ERR_PARSE, 0, 0, 0,
                             "Campo CSV con comillas sin cerrar");
            return MILENA_ERR_PARSE;
        }
        if (!delimiter_found) {
            *write = '\0';
            break;
        }
        if (*read == '\0') {
            if (count >= field_capacity) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "El CSV supera el máximo de 4096 columnas");
                return MILENA_ERR_OVERFLOW;
            }
            fields[count++] = write;
            *write = '\0';
            break;
        }
    }
    *field_count = count;
    return MILENA_OK;
}

static int stream_column_index(char **headers, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(headers[i], name) == 0) return (int)i;
    }
    return -1;
}

static bool stream_has_duplicate_header(char **headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(headers[i], headers[j]) == 0) return true;
        }
    }
    return false;
}

static bool stream_parse_number(const char *text, double *value) {
    if (!text || !value) return false;
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return false;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(parsed)) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    *value = parsed;
    return true;
}

static bool stream_filter_options_valid(const MilenaStreamOptions *options) {
    if (!options->filter_column)
        return options->filter_kind == MILENA_STREAM_FILTER_NONE &&
               options->filter_value == NULL;
    if (!options->filter_column[0]) return false;
    if (options->filter_kind == MILENA_STREAM_FILTER_TEXT_EQUAL ||
        options->filter_kind == MILENA_STREAM_FILTER_NONE)
        /* NONE plus the legacy filter_value pair preserves pre-typed API callers. */
        return options->filter_value != NULL;
    if (options->filter_kind == MILENA_STREAM_FILTER_NUMERIC_GREATER)
        return options->filter_value == NULL && isfinite(options->filter_number);
    return false;
}

static bool stream_filter_matches(const MilenaStreamOptions *options,
                                  const char *field) {
    if (!options->filter_column) return true;
    if (options->filter_kind == MILENA_STREAM_FILTER_TEXT_EQUAL ||
        options->filter_kind == MILENA_STREAM_FILTER_NONE)
        return strcmp(field, options->filter_value) == 0;
    if (options->filter_kind == MILENA_STREAM_FILTER_NUMERIC_GREATER) {
        double value = 0.0;
        return stream_parse_number(field, &value) && value > options->filter_number;
    }
    return false;
}

static void stream_accumulate(StreamAccumulator *accumulator, double value) {
    double corrected = value - accumulator->compensation;
    double next_sum = accumulator->sum + corrected;
    accumulator->compensation = (next_sum - accumulator->sum) - corrected;
    accumulator->sum = next_sum;
    accumulator->count++;
    double delta = value - accumulator->mean;
    accumulator->mean += delta / (double)accumulator->count;
    double delta2 = value - accumulator->mean;
    accumulator->m2 += delta * delta2;
    if (accumulator->count == 1 || value < accumulator->minimum) accumulator->minimum = value;
    if (accumulator->count == 1 || value > accumulator->maximum) accumulator->maximum = value;
}

static double stream_value(const StreamAccumulator *accumulator,
                           MilenaStreamOperation operation) {
    if (accumulator->count == 0) return NAN;
    switch (operation) {
    case MILENA_STREAM_SUM: return accumulator->sum;
    case MILENA_STREAM_MEAN: return accumulator->mean;
    case MILENA_STREAM_MIN: return accumulator->minimum;
    case MILENA_STREAM_MAX: return accumulator->maximum;
    case MILENA_STREAM_COUNT: return (double)accumulator->count;
    case MILENA_STREAM_VARIANCE: return accumulator->count > 1 ? accumulator->m2 / (double)(accumulator->count - 1) : 0.0;
    case MILENA_STREAM_STDDEV: return accumulator->count > 1 ? sqrt(accumulator->m2 / (double)(accumulator->count - 1)) : 0.0;
    default: return NAN;
    }
}

static void stream_free_headers(char **headers, size_t count) {
    if (!headers) return;
    for (size_t i = 0; i < count; i++) free(headers[i]);
    free(headers);
}

MilenaStreamOptions milena_stream_options_default(void) {
    MilenaStreamOptions options = {
        .chunk_rows = 4096u,
        .max_record_bytes = STREAM_DEFAULT_MAX_RECORD,
        .max_columns = STREAM_DEFAULT_MAX_COLUMNS,
        .max_rows = 0u,
        .max_elapsed_milliseconds = 0.0,
        .max_groups = STREAM_DEFAULT_MAX_GROUPS,
        .filter_column = NULL,
        .filter_value = NULL,
        .filter_kind = MILENA_STREAM_FILTER_NONE,
        .filter_number = 0.0
    };
    return options;
}

MilenaStatus milena_stream_csv_summary_with_options(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       const MilenaStreamOptions *requested,
                                       MilenaStreamReport *report,
                                       MilenaError *error) {
    if (report) memset(report, 0, sizeof(*report));
    MilenaStreamOptions defaults = milena_stream_options_default();
    const MilenaStreamOptions *options = requested ? requested : &defaults;
    if (!input_path || !output_path || !metrics || metric_count == 0 ||
        metric_count > STREAM_MAX_METRICS || options->chunk_rows == 0 ||
        options->max_record_bytes < STREAM_INITIAL_RECORD ||
        options->max_record_bytes > STREAM_DEFAULT_MAX_RECORD ||
        options->max_columns == 0 || options->max_columns > STREAM_MAX_COLUMNS ||
        options->max_elapsed_milliseconds < 0.0 ||
        !isfinite(options->max_elapsed_milliseconds) ||
        options->max_groups > STREAM_HARD_MAX_GROUPS ||
        !stream_filter_options_valid(options))
        return MILENA_ERR_ARGUMENT;
    if (error) milena_error_clear(error);
    double started = stream_now_ms();
    FILE *input = fopen(input_path, "rb");
    if (!input) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo abrir el CSV para lectura en flujo");
        return MILENA_ERR_IO;
    }
    (void)setvbuf(input, NULL, _IOFBF, STREAM_IO_BUFFER);
    char *record = NULL;
    size_t record_capacity = 0;
    char **fields = NULL;
    char **headers = NULL;
    size_t column_count = 0;
    StreamAccumulator accumulators[STREAM_MAX_METRICS] = {{0}};
    size_t invalid_values[STREAM_MAX_METRICS] = {0};
    size_t rows_read = 0, rows_valid = 0, malformed = 0;
    size_t record_length = 0, observed_record_bytes = 0;
    size_t input_bytes = 0, bytes_read = 0;
    bool resource_limit_reached = false;
    MilenaStatus status = stream_read_record(input, &record, &record_capacity,
        options->max_record_bytes, &record_length, error);
    if (status != MILENA_OK) {
        if (status == MILENA_ERR_IO && feof(input)) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "El CSV no tiene cabecera");
            status = MILENA_ERR_DATA;
        }
        goto finish;
    }
    fields = (char **)calloc(options->max_columns, sizeof(*fields));
    if (!fields) { status = MILENA_ERR_MEMORY; goto finish; }
    if (record_length > observed_record_bytes) observed_record_bytes = record_length;
    status = stream_split(record, ',', fields, options->max_columns, &column_count, error);
    if (status != MILENA_OK || column_count == 0) goto finish;
    headers = (char **)calloc(column_count, sizeof(*headers));
    if (!headers) { status = MILENA_ERR_MEMORY; goto finish; }
    for (size_t i = 0; i < column_count; i++) {
        headers[i] = milena_strdup(fields[i]);
        if (!headers[i]) { status = MILENA_ERR_MEMORY; goto finish; }
    }
    if (stream_has_duplicate_header(headers, column_count)) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La cabecera CSV contiene columnas duplicadas");
        status = MILENA_ERR_DATA;
        goto finish;
    }
    int indexes[STREAM_MAX_METRICS];
    for (size_t i = 0; i < metric_count; i++) {
        if (!metrics[i].column || !metrics[i].column[0]) {
            milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0, "Métrica de flujo sin columna");
            status = MILENA_ERR_ARGUMENT; goto finish;
        }
        indexes[i] = stream_column_index(headers, column_count, metrics[i].column);
        if (indexes[i] < 0) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0, "La columna de flujo no existe en el CSV");
            status = MILENA_ERR_DATA; goto finish;
        }
    }
    int filter_index = options->filter_column
        ? stream_column_index(headers, column_count, options->filter_column) : -1;
    if (options->filter_column && filter_index < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La columna del filtro no existe en el CSV");
        status = MILENA_ERR_DATA; goto finish;
    }
    while ((status = stream_read_record(input, &record, &record_capacity,
            options->max_record_bytes, &record_length, error)) == MILENA_OK) {
        long position = ftell(input);
        if (position >= 0) bytes_read = (size_t)position;
        if (options->max_rows != 0 && rows_read >= options->max_rows) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El flujo alcanzó el máximo de filas configurado");
            resource_limit_reached = true;
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        if (options->max_elapsed_milliseconds > 0.0 &&
            stream_now_ms() - started >= options->max_elapsed_milliseconds) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El flujo alcanzó el presupuesto de tiempo configurado");
            resource_limit_reached = true;
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        if (rows_read == SIZE_MAX) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El contador de filas del flujo se desbordó");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        rows_read++;
        if (record_length > observed_record_bytes) observed_record_bytes = record_length;
        size_t field_count = 0;
        status = stream_split(record, ',', fields, options->max_columns, &field_count, error);
        if (status != MILENA_OK) break;
        if (field_count != column_count) { malformed++; continue; }
        if (filter_index >= 0 && !stream_filter_matches(options, fields[filter_index]))
            continue;
        bool row_valid = true;
        for (size_t i = 0; i < metric_count; i++) {
            double value = 0.0;
            if (!stream_parse_number(fields[indexes[i]], &value)) {
                row_valid = false;
                invalid_values[i]++;
                continue;
            }
            stream_accumulate(&accumulators[i], value);
        }
        if (row_valid) rows_valid++;
        else malformed++;
    }
    if (status == MILENA_ERR_IO && feof(input)) status = MILENA_OK;
    long measured_bytes = ftell(input);
    if (measured_bytes >= 0) {
        input_bytes = (size_t)measured_bytes;
        bytes_read = (size_t)measured_bytes;
    }
    if (status == MILENA_OK && options->max_elapsed_milliseconds > 0.0 &&
        stream_now_ms() - started >= options->max_elapsed_milliseconds) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El flujo superó el presupuesto de tiempo configurado");
        resource_limit_reached = true;
        status = MILENA_ERR_OVERFLOW;
    }
    if (status != MILENA_OK) goto finish;

    {
        FILE *output = fopen(output_path, "wb");
        if (!output) {
            milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo crear el reporte de flujo");
            status = MILENA_ERR_IO;
            goto finish;
        }
        double elapsed = stream_now_ms() - started;
        double seconds = elapsed > 0.0 ? elapsed / 1000.0 : 0.0;
        double rows_per_second = seconds > 0.0 ? (double)rows_read / seconds : 0.0;
        double megabytes_per_second = seconds > 0.0 ?
            ((double)input_bytes / 1000000.0) / seconds : 0.0;
        fprintf(output, "{\"modo\":\"flujo\",\"filas\":%zu,\"filas_validas\":%zu,\"filas_malformadas\":%zu,\"bytes_entrada\":%zu,\"bytes_leidos\":%zu,\"filas_por_segundo\":%.6f,\"megabytes_por_segundo\":%.6f,\"tamano_lote\":%zu,\"limite_registro_bytes\":%zu,\"limite_columnas\":%zu,\"limite_filas\":%zu,\"presupuesto_tiempo_ms\":%.3f,\"pico_registro_bytes\":%zu,\"capacidad_buffer_registro_bytes\":%zu,\"tiempo_ms\":%.3f,\"resultados\":[",
            rows_read, rows_valid, malformed, input_bytes, bytes_read,
            rows_per_second, megabytes_per_second, options->chunk_rows,
            options->max_record_bytes, options->max_columns, options->max_rows,
            options->max_elapsed_milliseconds, observed_record_bytes,
            record_capacity, elapsed);
        for (size_t i = 0; i < metric_count; i++) {
            if (i) fputc(',', output);
            char generated_name[256];
            const char *name = metrics[i].name;
            if (!name || !name[0]) {
                (void)snprintf(generated_name, sizeof(generated_name), "%s_%s", metrics[i].column, milena_stream_operation_name(metrics[i].operation));
                name = generated_name;
            }
            fprintf(output, "{\"columna\":");
            milena_json_write_string(output, metrics[i].column);
            fprintf(output, ",\"operacion\":");
            milena_json_write_string(output, milena_stream_operation_name(metrics[i].operation));
            fprintf(output, ",\"nombre\":");
            milena_json_write_string(output, name);
            fprintf(output, ",\"valores_validos\":%zu,\"valores_invalidos\":%zu,\"valor\":", accumulators[i].count, invalid_values[i]);
            double value = stream_value(&accumulators[i], metrics[i].operation);
            if (isnan(value)) fputs("null", output); else fprintf(output, "%.17g", value);
            fputc('}', output);
        }
        fputs("]}\n", output);
        if (fclose(output) != 0) {
            milena_error_set(error, MILENA_ERR_IO, 0, 0, 0, "No se pudo cerrar el reporte de flujo");
            status = MILENA_ERR_IO;
            goto finish;
        }
        if (report) {
            report->rows_read = rows_read;
            report->rows_with_valid_values = rows_valid;
            report->malformed_rows = malformed;
            report->input_bytes = input_bytes;
            report->chunk_rows = options->chunk_rows;
            report->elapsed_milliseconds = elapsed;
            report->observed_record_bytes = observed_record_bytes;
            report->peak_record_bytes = record_capacity;
            report->header_columns = column_count;
            report->max_record_bytes = options->max_record_bytes;
            report->max_columns = options->max_columns;
            report->max_rows = options->max_rows;
            report->max_elapsed_milliseconds = options->max_elapsed_milliseconds;
            report->resource_limit_reached = resource_limit_reached;
            report->bytes_read = bytes_read;
        }
    }

finish:
    if (fclose(input) != 0 && status == MILENA_OK) status = MILENA_ERR_IO;
    if (report && status != MILENA_OK) {
        report->rows_read = rows_read;
        report->rows_with_valid_values = rows_valid;
        report->malformed_rows = malformed;
        report->input_bytes = input_bytes;
        report->chunk_rows = options->chunk_rows;
        report->elapsed_milliseconds = stream_now_ms() - started;
        report->observed_record_bytes = observed_record_bytes;
        report->peak_record_bytes = record_capacity;
        report->header_columns = column_count;
        report->max_record_bytes = options->max_record_bytes;
        report->max_columns = options->max_columns;
        report->max_rows = options->max_rows;
        report->max_elapsed_milliseconds = options->max_elapsed_milliseconds;
        report->resource_limit_reached = resource_limit_reached;
        report->bytes_read = bytes_read;
    }
    free(record);
    free(fields);
    stream_free_headers(headers, column_count);
    return status;
}


MilenaStatus milena_stream_csv_summary(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       size_t chunk_rows,
                                       MilenaStreamReport *report,
                                       MilenaError *error) {
    MilenaStreamOptions options = milena_stream_options_default();
    options.chunk_rows = chunk_rows;
    return milena_stream_csv_summary_with_options(input_path, output_path,
                                                   metrics, metric_count,
                                                   &options, report, error);
}



typedef struct {
    char *key;
    StreamAccumulator *accumulators;
    size_t *null_values;
    size_t *invalid_values;
} StreamGroup;

static int stream_group_compare(const void *left, const void *right) {
    const StreamGroup *a = (const StreamGroup *)left;
    const StreamGroup *b = (const StreamGroup *)right;
    return strcmp(a->key, b->key);
}

static uint64_t stream_group_hash(const char *key) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        hash = (hash ^ (uint64_t)*p) * UINT64_C(1099511628211);
    return hash;
}

static bool stream_field_nonempty(const char *text) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    return *text != '\0';
}

static void stream_groups_release(StreamGroup *groups, size_t count) {
    if (!groups) return;
    for (size_t i = 0; i < count; i++) {
        free(groups[i].key);
        free(groups[i].accumulators);
        free(groups[i].null_values);
        free(groups[i].invalid_values);
    }
    free(groups);
}

static bool stream_group_emit(FILE *output, const char *group_column,
                              const MilenaStreamMetric *metrics,
                              size_t metric_count, const StreamGroup *groups,
                              size_t group_count, size_t group_limit,
                              size_t rows_read, size_t rows_valid,
                              size_t malformed, size_t input_bytes,
                              size_t observed_record_bytes,
                              size_t record_capacity, double elapsed_ms,
                              size_t max_rows, double max_elapsed_ms) {
    double seconds = elapsed_ms > 0.0 ? elapsed_ms / 1000.0 : 0.0;
    double rows_per_second = seconds > 0.0 ? (double)rows_read / seconds : 0.0;
    double megabytes_per_second = seconds > 0.0 ?
        ((double)input_bytes / 1000000.0) / seconds : 0.0;
    if (fprintf(output,
        "{\"modo\":\"flujo_agrupado\",\"grupo\":") < 0) return false;
    milena_json_write_string(output, group_column);
    if (fprintf(output,
        ",\"filas\":%zu,\"filas_validas\":%zu,\"filas_malformadas\":%zu,"
        "\"grupos\":%zu,\"limite_grupos\":%zu,\"bytes_entrada\":%zu,"
        "\"filas_por_segundo\":%.6f,\"megabytes_por_segundo\":%.6f,"
        "\"limite_filas\":%zu,\"presupuesto_tiempo_ms\":%.3f,"
        "\"pico_registro_bytes\":%zu,\"capacidad_buffer_registro_bytes\":%zu,"
        "\"tiempo_ms\":%.3f,\"resultados\":[",
        rows_read, rows_valid, malformed, group_count, group_limit,
        input_bytes, rows_per_second, megabytes_per_second, max_rows,
        max_elapsed_ms, observed_record_bytes, record_capacity, elapsed_ms) < 0) return false;
    for (size_t g = 0; g < group_count; g++) {
        if (g && fputc(',', output) == EOF) return false;
        if (fputs("{\"clave\":", output) == EOF) return false;
        milena_json_write_string(output, groups[g].key);
        if (fputs(",\"metricas\":[", output) == EOF) return false;
        for (size_t i = 0; i < metric_count; i++) {
            if (i && fputc(',', output) == EOF) return false;
            char generated_name[256];
            const char *name = metrics[i].name;
            if (!name || !name[0]) {
                (void)snprintf(generated_name, sizeof(generated_name),
                    "%s_%s", metrics[i].column,
                    milena_stream_operation_name(metrics[i].operation));
                name = generated_name;
            }
            double value = stream_value(&groups[g].accumulators[i],
                                        metrics[i].operation);
            if (fputs("{\"columna\":", output) == EOF) return false;
            milena_json_write_string(output, metrics[i].column);
            if (fputs(",\"operacion\":", output) == EOF) return false;
            milena_json_write_string(output,
                milena_stream_operation_name(metrics[i].operation));
            if (fputs(",\"nombre\":", output) == EOF) return false;
            milena_json_write_string(output, name);
            if (fprintf(output,
                ",\"valores_validos\":%zu,\"valores_nulos\":%zu,\"valores_invalidos\":%zu,\"valor\":",
                groups[g].accumulators[i].count, groups[g].null_values[i],
                groups[g].invalid_values[i]) < 0) return false;
            if (metrics[i].operation == MILENA_STREAM_COUNT) {
                if (fprintf(output, "%zu", groups[g].accumulators[i].count) < 0)
                    return false;
            } else if (isnan(value)) {
                if (fputs("null", output) == EOF) return false;
            } else if (fprintf(output, "%.17g", value) < 0) return false;
            if (fputc('}', output) == EOF) return false;
        }
        if (fputs("]}", output) == EOF) return false;
    }
    return fputs("]}\n", output) != EOF;
}

MilenaStatus milena_stream_csv_grouped_with_options(
    const char *input_path, const char *output_path, const char *group_column,
    const MilenaStreamMetric *metrics, size_t metric_count,
    const MilenaStreamOptions *requested, MilenaStreamReport *report,
    MilenaError *error) {
    MilenaStreamOptions defaults = milena_stream_options_default();
    const MilenaStreamOptions *options = requested ? requested : &defaults;
    size_t group_limit = options->max_groups ? options->max_groups : defaults.max_groups;
    if (!input_path || !output_path || !group_column || !group_column[0] ||
        !metrics || metric_count == 0 || metric_count > STREAM_MAX_METRICS ||
        options->chunk_rows == 0 ||
        options->max_record_bytes < STREAM_INITIAL_RECORD ||
        options->max_record_bytes > STREAM_DEFAULT_MAX_RECORD ||
        options->max_columns == 0 || options->max_columns > STREAM_MAX_COLUMNS ||
        options->max_elapsed_milliseconds < 0.0 ||
        !isfinite(options->max_elapsed_milliseconds) ||
        group_limit > STREAM_HARD_MAX_GROUPS ||
        !stream_filter_options_valid(options)) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "Opciones inválidas para agrupación CSV en flujo");
        return MILENA_ERR_ARGUMENT;
    }
    if (report) memset(report, 0, sizeof(*report));
    if (error) milena_error_clear(error);

    double started = stream_now_ms();
    FILE *input = fopen(input_path, "rb");
    if (!input) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo abrir el CSV agrupado");
        return MILENA_ERR_IO;
    }
    (void)setvbuf(input, NULL, _IOFBF, STREAM_IO_BUFFER);
    char *record = NULL;
    char **fields = NULL;
    char **headers = NULL;
    StreamGroup *groups = NULL;
    size_t *group_slots = NULL;
    size_t group_slot_capacity = 0;
    size_t record_capacity = 0, record_length = 0, column_count = 0;
    size_t group_count = 0, rows_read = 0, rows_valid = 0, malformed = 0;
    size_t input_bytes = 0, bytes_read = 0, observed_record_bytes = 0;
    size_t group_state_bytes = 0;
    bool resource_limit_reached = false;
    MilenaStatus status = MILENA_OK;

    status = stream_read_record(input, &record, &record_capacity,
                                options->max_record_bytes, &record_length, error);
    if (status != MILENA_OK) {
        if (status == MILENA_ERR_IO && feof(input)) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                             "El CSV agrupado no tiene cabecera");
            status = MILENA_ERR_DATA;
        }
        goto grouped_finish;
    }
    observed_record_bytes = record_length;
    fields = (char **)calloc(options->max_columns, sizeof(*fields));
    if (!fields) { status = MILENA_ERR_MEMORY; goto grouped_finish; }
    status = stream_split(record, ',', fields, options->max_columns,
                          &column_count, error);
    if (status != MILENA_OK || column_count == 0) goto grouped_finish;
    headers = (char **)calloc(column_count, sizeof(*headers));
    if (!headers) { status = MILENA_ERR_MEMORY; goto grouped_finish; }
    for (size_t i = 0; i < column_count; i++) {
        headers[i] = milena_strdup(fields[i]);
        if (!headers[i]) { status = MILENA_ERR_MEMORY; goto grouped_finish; }
    }
    if (stream_has_duplicate_header(headers, column_count)) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La cabecera CSV contiene columnas duplicadas");
        status = MILENA_ERR_DATA;
        goto grouped_finish;
    }
    int group_index = stream_column_index(headers, column_count, group_column);
    if (group_index < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La columna de agrupación no existe");
        status = MILENA_ERR_DATA;
        goto grouped_finish;
    }
    int metric_indexes[STREAM_MAX_METRICS];
    for (size_t i = 0; i < metric_count; i++) {
        if (!metrics[i].column || !metrics[i].column[0] ||
            metrics[i].operation < MILENA_STREAM_SUM ||
            metrics[i].operation > MILENA_STREAM_STDDEV) {
            milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                             "Métrica de agrupación inválida");
            status = MILENA_ERR_ARGUMENT;
            goto grouped_finish;
        }
        metric_indexes[i] = stream_column_index(headers, column_count,
                                                metrics[i].column);
        if (metric_indexes[i] < 0) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                             "La columna métrica de agrupación no existe");
            status = MILENA_ERR_DATA;
            goto grouped_finish;
        }
    }
    int filter_index = options->filter_column
        ? stream_column_index(headers, column_count, options->filter_column) : -1;
    if (options->filter_column && filter_index < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La columna del filtro no existe en el CSV");
        status = MILENA_ERR_DATA;
        goto grouped_finish;
    }
    if (group_limit > SIZE_MAX / sizeof(*groups)) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El límite de grupos desborda el estado de flujo");
        status = MILENA_ERR_OVERFLOW;
        goto grouped_finish;
    }
    size_t group_array_bytes = group_limit * sizeof(*groups);
    if (group_limit > SIZE_MAX / 2) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "La tabla hash de grupos desborda el límite configurado");
        status = MILENA_ERR_OVERFLOW;
        goto grouped_finish;
    }
    group_slot_capacity = 1;
    size_t desired_slots = group_limit * 2;
    while (group_slot_capacity < desired_slots) {
        if (group_slot_capacity > SIZE_MAX / 2) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "La tabla hash de grupos desborda el límite configurado");
            status = MILENA_ERR_OVERFLOW;
            goto grouped_finish;
        }
        group_slot_capacity *= 2;
    }
    if (group_slot_capacity > SIZE_MAX / sizeof(*group_slots)) {
        status = MILENA_ERR_OVERFLOW;
        goto grouped_finish;
    }
    size_t slot_bytes = group_slot_capacity * sizeof(*group_slots);
    if (slot_bytes > STREAM_GROUP_STATE_BUDGET ||
        group_array_bytes > STREAM_GROUP_STATE_BUDGET - slot_bytes) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El límite de grupos supera el presupuesto de estado");
        status = MILENA_ERR_OVERFLOW;
        goto grouped_finish;
    }
    groups = (StreamGroup *)calloc(group_limit, sizeof(*groups));
    group_slots = (size_t *)calloc(group_slot_capacity, sizeof(*group_slots));
    if (!groups || !group_slots) { status = MILENA_ERR_MEMORY; goto grouped_finish; }
    group_state_bytes = group_array_bytes + slot_bytes;

    while ((status = stream_read_record(input, &record, &record_capacity,
            options->max_record_bytes, &record_length, error)) == MILENA_OK) {
        long position = ftell(input);
        if (position >= 0) bytes_read = (size_t)position;
        if ((options->max_rows != 0 && rows_read >= options->max_rows) ||
            (options->max_elapsed_milliseconds > 0.0 &&
             stream_now_ms() - started >= options->max_elapsed_milliseconds)) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                options->max_rows != 0 && rows_read >= options->max_rows
                ? "La agrupación alcanzó el máximo de filas configurado"
                : "La agrupación alcanzó el presupuesto de tiempo configurado");
            resource_limit_reached = true;
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        if (rows_read == SIZE_MAX) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El contador de filas agrupadas se desbordó");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        rows_read++;
        if (record_length > observed_record_bytes) observed_record_bytes = record_length;
        size_t field_count = 0;
        status = stream_split(record, ',', fields, options->max_columns,
                              &field_count, error);
        if (status != MILENA_OK) break;
        if (field_count != column_count) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                             "Fila CSV agrupada con cantidad incorrecta de columnas");
            status = MILENA_ERR_DATA;
            break;
        }
        if (filter_index >= 0 && !stream_filter_matches(options, fields[filter_index]))
            continue;

        size_t slot = (size_t)stream_group_hash(fields[group_index]) &
                      (group_slot_capacity - 1);
        while (group_slots[slot] != 0 &&
               strcmp(groups[group_slots[slot] - 1].key, fields[group_index]) != 0)
            slot = (slot + 1) & (group_slot_capacity - 1);
        size_t group_index_found = group_slots[slot] == 0
            ? group_count : group_slots[slot] - 1;
        if (group_index_found == group_count) {
            size_t key_length = strlen(fields[group_index]);
            size_t accumulator_bytes = metric_count * sizeof(StreamAccumulator);
            size_t counter_bytes = metric_count * sizeof(size_t);
            if (counter_bytes > SIZE_MAX / 2u) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "Presupuesto de contadores agrupados desbordado");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            size_t missing_bytes = counter_bytes * 2u;
            if (key_length > STREAM_MAX_GROUP_KEY_BYTES ||
                accumulator_bytes > SIZE_MAX - missing_bytes ||
                key_length + 1 > SIZE_MAX - accumulator_bytes - missing_bytes ||
                group_state_bytes > STREAM_GROUP_STATE_BUDGET -
                    (key_length + 1 + accumulator_bytes + missing_bytes)) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "La agrupación alcanzó el presupuesto de estado acotado");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            if (group_count >= group_limit) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "Se superó el límite máximo de grupos del flujo");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            StreamGroup *group = &groups[group_count];
            group->key = milena_strdup(fields[group_index]);
            group->accumulators = (StreamAccumulator *)calloc(
                metric_count, sizeof(*group->accumulators));
            group->null_values = (size_t *)calloc(
                metric_count, sizeof(*group->null_values));
            group->invalid_values = (size_t *)calloc(
                metric_count, sizeof(*group->invalid_values));
            if (!group->key || !group->accumulators || !group->null_values ||
                !group->invalid_values) {
                free(group->key); free(group->accumulators);
                free(group->null_values); free(group->invalid_values);
                memset(group, 0, sizeof(*group));
                status = MILENA_ERR_MEMORY;
                break;
            }
            group_state_bytes += key_length + 1 + accumulator_bytes + missing_bytes;
            group_index_found = group_count++;
            group_slots[slot] = group_index_found + 1;
        }
        StreamGroup *group = &groups[group_index_found];
        bool row_valid = false;
        bool row_malformed = false;
        for (size_t i = 0; i < metric_count; i++) {
            const char *field = fields[metric_indexes[i]];
            double value = 1.0;
            bool is_null = !stream_field_nonempty(field);
            bool valid = metrics[i].operation == MILENA_STREAM_COUNT
                ? !is_null : stream_parse_number(field, &value);
            if (!valid) {
                size_t *counter = is_null ? &group->null_values[i] :
                                            &group->invalid_values[i];
                if (*counter == SIZE_MAX) {
                    milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                     "El contador de valores ausentes agrupados se desbordó");
                    status = MILENA_ERR_OVERFLOW;
                    break;
                }
                (*counter)++;
                row_malformed = true;
                continue;
            }
            if (group->accumulators[i].count == SIZE_MAX) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "El contador de métricas agrupadas se desbordó");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            stream_accumulate(&group->accumulators[i], value);
            row_valid = true;
        }
        if (status != MILENA_OK) break;
        if (row_valid) rows_valid++;
        if (row_malformed) malformed++;
    }
    if (status == MILENA_ERR_IO && feof(input)) status = MILENA_OK;
    {
        long position = ftell(input);
        if (position >= 0) input_bytes = bytes_read = (size_t)position;
    }
    if (status == MILENA_OK && options->max_elapsed_milliseconds > 0.0 &&
        stream_now_ms() - started >= options->max_elapsed_milliseconds) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "La agrupación superó el presupuesto de tiempo configurado");
        resource_limit_reached = true;
        status = MILENA_ERR_OVERFLOW;
    }
    if (status != MILENA_OK) goto grouped_finish;

    qsort(groups, group_count, sizeof(*groups), stream_group_compare);
    FILE *output = fopen(output_path, "wb");
    if (!output) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo crear el reporte agrupado");
        status = MILENA_ERR_IO;
        goto grouped_finish;
    }
    double elapsed = stream_now_ms() - started;
    bool emitted = stream_group_emit(output, group_column, metrics, metric_count,
        groups, group_count, group_limit, rows_read, rows_valid, malformed,
        input_bytes, observed_record_bytes, record_capacity, elapsed,
        options->max_rows, options->max_elapsed_milliseconds);
    if (fclose(output) != 0) emitted = false;
    if (!emitted) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo escribir el reporte agrupado");
        status = MILENA_ERR_IO;
        goto grouped_finish;
    }

grouped_finish:
    if (input) {
        long position = ftell(input);
        if (position >= 0) input_bytes = bytes_read = (size_t)position;
        if (fclose(input) != 0 && status == MILENA_OK) status = MILENA_ERR_IO;
    }
    if (report) {
        report->rows_read = rows_read;
        report->rows_with_valid_values = rows_valid;
        report->malformed_rows = malformed;
        report->input_bytes = input_bytes;
        report->chunk_rows = options->chunk_rows;
        report->elapsed_milliseconds = stream_now_ms() - started;
        report->observed_record_bytes = observed_record_bytes;
        report->peak_record_bytes = record_capacity;
        report->header_columns = column_count;
        report->max_record_bytes = options->max_record_bytes;
        report->max_columns = options->max_columns;
        report->max_rows = options->max_rows;
        report->max_elapsed_milliseconds = options->max_elapsed_milliseconds;
        report->resource_limit_reached = resource_limit_reached;
        report->bytes_read = bytes_read;
        report->groups = group_count;
        report->max_groups = group_limit;
    }
    free(record);
    free(fields);
    free(group_slots);
    stream_free_headers(headers, column_count);
    stream_groups_release(groups, group_count);
    return status;
}

/* Spill output is emitted one reducer callback at a time. It is written to an
 * exclusive sibling staging file and published only after parsing, reduction,
 * callback output, close and quota checks have all succeeded. */
typedef struct {
    FILE *output;
    const MilenaStreamMetric *metric;
    size_t max_groups;
    size_t groups;
    size_t max_output_bytes;
    bool first;
    double started_ms;
    double max_elapsed_ms;
} StreamSpillEmitter;

static bool stream_spill_size_add(size_t *total, size_t value) {
    if (value > SIZE_MAX - *total) return false;
    *total += value;
    return true;
}

static bool stream_spill_json_string_size(const char *text, size_t *size) {
    size_t total = 2u;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p; ++p) {
        size_t amount = (*p == '"' || *p == '\\' || *p == '\n' ||
                         *p == '\r' || *p == '\t') ? 2u : (*p < 0x20 ? 6u : 1u);
        if (!stream_spill_size_add(&total, amount)) return false;
    }
    *size = total;
    return true;
}

static MilenaStatus stream_spill_emit_group(
    const MilenaGroupedAggregateResult *result, void *opaque,
    MilenaError *error) {
    StreamSpillEmitter *emitter = (StreamSpillEmitter *)opaque;
    if (!result || !result->key || result->key_length < 1 ||
        result->key[0] != 1 || !emitter || !emitter->output) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "Clave no textual en el reductor agrupado de flujo");
        return MILENA_ERR_DATA;
    }
    if (emitter->groups >= emitter->max_groups) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "Se agotó el límite AST de grupos de salida spill");
        return MILENA_ERR_OVERFLOW;
    }
    if (emitter->max_elapsed_ms > 0.0 &&
        stream_now_ms() - emitter->started_ms >= emitter->max_elapsed_ms) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "La reducción agrupada superó el presupuesto de tiempo");
        return MILENA_ERR_OVERFLOW;
    }

    size_t key_length = result->key_length - 1u;
    char *key = (char *)malloc(key_length + 1u);
    if (!key) {
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                         "Sin memoria para escribir clave agrupada");
        return MILENA_ERR_MEMORY;
    }
    if (key_length) memcpy(key, result->key + 1u, key_length);
    key[key_length] = '\0';

    char default_name[256];
    const char *name = emitter->metric->name;
    if (!name || !name[0]) {
        (void)snprintf(default_name, sizeof(default_name), "%s_%s",
            emitter->metric->column,
            milena_stream_operation_name(emitter->metric->operation));
        name = default_name;
    }
    const char *operation = milena_stream_operation_name(emitter->metric->operation);
    uint64_t valid = result->aggregate.count;
    double value = NAN;
    if (emitter->metric->operation == MILENA_STREAM_COUNT) {
        value = (double)valid;
    } else if (result->aggregate.has_values) {
        switch (emitter->metric->operation) {
        case MILENA_STREAM_SUM: value = result->aggregate.sum; break;
        case MILENA_STREAM_MEAN: value = result->aggregate.mean; break;
        case MILENA_STREAM_MIN: value = result->aggregate.min; break;
        case MILENA_STREAM_MAX: value = result->aggregate.max; break;
        default: break;
        }
    }
    char valid_text[32], null_text[32], invalid_text[32], value_text[64];
    int valid_chars = snprintf(valid_text, sizeof(valid_text), "%llu",
                               (unsigned long long)valid);
    int null_chars = snprintf(null_text, sizeof(null_text), "%llu",
                              (unsigned long long)result->aggregate.null_count);
    int invalid_chars = snprintf(invalid_text, sizeof(invalid_text), "%llu",
                                 (unsigned long long)result->aggregate.invalid_count);
    bool exact_count = emitter->metric->operation == MILENA_STREAM_COUNT;
    int value_chars = exact_count ? valid_chars : (isnan(value) ? 4 :
        snprintf(value_text, sizeof(value_text), "%.17g", value));
    if (valid_chars < 0 || (size_t)valid_chars >= sizeof(valid_text) ||
        null_chars < 0 || (size_t)null_chars >= sizeof(null_text) ||
        invalid_chars < 0 || (size_t)invalid_chars >= sizeof(invalid_text) ||
        value_chars < 0 || (!isnan(value) && (size_t)value_chars >= sizeof(value_text))) {
        free(key);
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "No se pudo dimensionar el resultado spill");
        return MILENA_ERR_OVERFLOW;
    }
    if (exact_count) memcpy(value_text, valid_text, (size_t)valid_chars + 1u);
    else if (isnan(value)) memcpy(value_text, "null", 5u);

    size_t encoded = 0, part = 0;
#define ADD_SPILL_LITERAL(literal) \
    do { if (!stream_spill_size_add(&encoded, sizeof(literal) - 1u)) goto size_error; } while (0)
    ADD_SPILL_LITERAL("{\"clave\":");
    if (!stream_spill_json_string_size(key, &part) ||
        !stream_spill_size_add(&encoded, part)) goto size_error;
    ADD_SPILL_LITERAL(",\"metricas\":[{\"columna\":");
    if (!stream_spill_json_string_size(emitter->metric->column, &part) ||
        !stream_spill_size_add(&encoded, part)) goto size_error;
    ADD_SPILL_LITERAL(",\"operacion\":");
    if (!stream_spill_json_string_size(operation, &part) ||
        !stream_spill_size_add(&encoded, part)) goto size_error;
    ADD_SPILL_LITERAL(",\"nombre\":");
    if (!stream_spill_json_string_size(name, &part) ||
        !stream_spill_size_add(&encoded, part)) goto size_error;
    ADD_SPILL_LITERAL(",\"valores_validos\":");
    if (!stream_spill_size_add(&encoded, (size_t)valid_chars)) goto size_error;
    ADD_SPILL_LITERAL(",\"valores_nulos\":");
    if (!stream_spill_size_add(&encoded, (size_t)null_chars)) goto size_error;
    ADD_SPILL_LITERAL(",\"valores_invalidos\":");
    if (!stream_spill_size_add(&encoded, (size_t)invalid_chars)) goto size_error;
    ADD_SPILL_LITERAL(",\"valor\":");
    if (!stream_spill_size_add(&encoded, (size_t)value_chars)) goto size_error;
    ADD_SPILL_LITERAL("}]}");
#undef ADD_SPILL_LITERAL

    long position = ftell(emitter->output);
    size_t comma = emitter->first ? 0u : 1u;
    if (position < 0 || (size_t)position > emitter->max_output_bytes ||
        comma > emitter->max_output_bytes - (size_t)position ||
        encoded > emitter->max_output_bytes - (size_t)position - comma) {
        free(key);
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El reporte spill superaría el límite de bytes de salida");
        return MILENA_ERR_OVERFLOW;
    }

    FILE *output = emitter->output;
    if (!emitter->first && fputc(',', output) == EOF) goto write_error;
    emitter->first = false;
    if (fputs("{\"clave\":", output) == EOF) goto write_error;
    milena_json_write_string(output, key);
    if (fputs(",\"metricas\":[{\"columna\":", output) == EOF) goto write_error;
    milena_json_write_string(output, emitter->metric->column);
    if (fputs(",\"operacion\":", output) == EOF) goto write_error;
    milena_json_write_string(output, operation);
    if (fputs(",\"nombre\":", output) == EOF) goto write_error;
    milena_json_write_string(output, name);
    if (fprintf(output, ",\"valores_validos\":%s,\"valores_nulos\":%s,\"valores_invalidos\":%s,\"valor\":%s}]}",
                valid_text, null_text, invalid_text, value_text) < 0)
        goto write_error;
    if (ferror(output)) goto write_error;
    free(key);
    emitter->groups++;
    return MILENA_OK;

size_error:
#undef ADD_SPILL_LITERAL
    free(key);
    milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                     "Tamaño del resultado spill desbordado");
    return MILENA_ERR_OVERFLOW;
write_error:
    free(key);
    milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                     "No se pudo escribir el resultado spill agrupado");
    return MILENA_ERR_IO;
}

static bool stream_publish_staged(const char *staging_path,
                                  const char *output_path) {
#ifdef _WIN32
    return MoveFileExA(staging_path, output_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(staging_path, output_path) == 0;
#endif
}

static FILE *stream_open_staging(const char *output_path, char **staging_path,
                                 MilenaError *error) {
    size_t path_length = strlen(output_path);
    if (path_length > SIZE_MAX - 64u) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "Ruta del reporte demasiado larga");
        return NULL;
    }
    char *candidate = (char *)malloc(path_length + 64u);
    if (!candidate) {
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                         "Sin memoria para crear reporte temporal");
        return NULL;
    }
    unsigned long long stamp = (unsigned long long)(stream_now_ms() * 1000.0);
    for (unsigned int attempt = 0; attempt < 64u; ++attempt) {
        (void)snprintf(candidate, path_length + 64u, "%s.part.%llu.%u",
                       output_path, stamp, attempt);
        FILE *file = fopen(candidate, "wbx");
        if (file) {
            *staging_path = candidate;
            return file;
        }
        if (errno != EEXIST) break;
    }
    free(candidate);
    milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                     "No se pudo crear archivo temporal exclusivo para el reporte");
    return NULL;
}

static bool stream_spill_supported_operation(MilenaStreamOperation operation) {
    return operation == MILENA_STREAM_SUM || operation == MILENA_STREAM_MEAN ||
           operation == MILENA_STREAM_MIN || operation == MILENA_STREAM_MAX ||
           operation == MILENA_STREAM_COUNT;
}

MilenaStatus milena_stream_csv_grouped_spill_with_options(
    const char *input_path, const char *output_path, const char *group_column,
    const MilenaStreamMetric *metric, const MilenaStreamOptions *requested,
    const MilenaStreamSpillPolicy *policy, MilenaStreamReport *report,
    MilenaError *error) {
    if (report) memset(report, 0, sizeof(*report));
    MilenaStreamOptions defaults = milena_stream_options_default();
    const MilenaStreamOptions *options = requested ? requested : &defaults;
    if (!input_path || !output_path || !group_column || !group_column[0] ||
        !metric || !metric->column || !metric->column[0] || !policy ||
        !policy->scratch_path || !policy->scratch_path[0] ||
        strlen(policy->scratch_path) > 220u ||
        strcmp(policy->scratch_path, output_path) == 0 ||
        !stream_spill_supported_operation(metric->operation) ||
        options->chunk_rows == 0 ||
        options->max_record_bytes < STREAM_INITIAL_RECORD ||
        options->max_record_bytes > STREAM_DEFAULT_MAX_RECORD ||
        options->max_columns == 0 || options->max_columns > STREAM_MAX_COLUMNS ||
        options->max_elapsed_milliseconds < 0.0 ||
        !isfinite(options->max_elapsed_milliseconds) ||
        !stream_filter_options_valid(options) ||
        policy->memory_budget_bytes < 4096u ||
        policy->memory_budget_bytes > 536870912u ||
        policy->spill_quota_bytes == 0 ||
        policy->spill_quota_bytes > 4294967296u ||
        policy->max_key_bytes < 2u || policy->max_key_bytes > 1048576u ||
        policy->max_output_groups == 0 ||
        policy->max_output_groups > 1000000u ||
        policy->max_output_bytes > STREAM_DEFAULT_SPILL_OUTPUT_BYTES ||
        policy->max_runs == 0 ||
        policy->max_runs > MILENA_GROUPED_HARD_MAX_RUNS) {
        milena_error_set(error, MILENA_ERR_ARGUMENT, 0, 0, 0,
                         "Política u operación inválida para spill agrupado CSV");
        return MILENA_ERR_ARGUMENT;
    }
    size_t output_byte_limit = policy->max_output_bytes ?
        policy->max_output_bytes : STREAM_DEFAULT_SPILL_OUTPUT_BYTES;
    if (error) milena_error_clear(error);
    double started = stream_now_ms();
    FILE *input = NULL, *staged = NULL;
    char *record = NULL, *staging_path = NULL;
    char **fields = NULL, **headers = NULL;
    size_t record_capacity = 0, record_length = 0, column_count = 0;
    size_t rows_read = 0, rows_valid = 0, malformed = 0;
    size_t input_bytes = 0, bytes_read = 0, observed_record_bytes = 0;
    int group_index = -1, metric_index = -1;
    bool reducer_open = false, scratch_owned = false, published = false;
    bool resource_limit_reached = false;
    size_t spill_bytes = 0, spill_records = 0, spill_runs = 0;
    MilenaGroupedAggregate reducer = {0};
    MilenaStatus status = MILENA_OK;

    input = fopen(input_path, "rb");
    if (!input) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo abrir el CSV para spill agrupado");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    (void)setvbuf(input, NULL, _IOFBF, STREAM_IO_BUFFER);
    status = stream_read_record(input, &record, &record_capacity,
                                options->max_record_bytes, &record_length, error);
    if (status != MILENA_OK) {
        if (status == MILENA_ERR_IO && feof(input)) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                             "El CSV agrupado no tiene cabecera");
            status = MILENA_ERR_DATA;
        }
        goto spill_finish;
    }
    observed_record_bytes = record_length;
    fields = (char **)calloc(options->max_columns, sizeof(*fields));
    if (!fields) { status = MILENA_ERR_MEMORY; goto spill_finish; }
    status = stream_split(record, ',', fields, options->max_columns,
                          &column_count, error);
    if (status != MILENA_OK || column_count == 0) goto spill_finish;
    headers = (char **)calloc(column_count, sizeof(*headers));
    if (!headers) { status = MILENA_ERR_MEMORY; goto spill_finish; }
    for (size_t i = 0; i < column_count; ++i) {
        headers[i] = milena_strdup(fields[i]);
        if (!headers[i]) { status = MILENA_ERR_MEMORY; goto spill_finish; }
    }
    if (stream_has_duplicate_header(headers, column_count)) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La cabecera CSV contiene columnas duplicadas");
        status = MILENA_ERR_DATA; goto spill_finish;
    }
    group_index = stream_column_index(headers, column_count, group_column);
    metric_index = stream_column_index(headers, column_count, metric->column);
    if (group_index < 0 || metric_index < 0 ||
        (group_index == metric_index && metric->operation != MILENA_STREAM_COUNT)) {
        milena_error_set(error, MILENA_ERR_UNSUPPORTED, 0, 0, 0,
            group_index < 0 || metric_index < 0
            ? "La clave o métrica spill no existe en la cabecera CSV"
            : "La clave textual no puede reutilizarse como métrica numérica en #spill");
        status = MILENA_ERR_UNSUPPORTED; goto spill_finish;
    }
    int filter_index = options->filter_column
        ? stream_column_index(headers, column_count, options->filter_column) : -1;
    if (options->filter_column && filter_index < 0) {
        milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                         "La columna del filtro no existe en el CSV");
        status = MILENA_ERR_DATA; goto spill_finish;
    }
    status = milena_grouped_aggregate_open_with_max_runs(policy->scratch_path,
        policy->memory_budget_bytes, policy->max_key_bytes,
        policy->spill_quota_bytes, policy->max_runs, &reducer, error);
    if (status != MILENA_OK) goto spill_finish;
    reducer_open = true;
    scratch_owned = true;

    while ((status = stream_read_record(input, &record, &record_capacity,
            options->max_record_bytes, &record_length, error)) == MILENA_OK) {
        long position = ftell(input);
        if (position >= 0) bytes_read = (size_t)position;
        if ((options->max_rows && rows_read >= options->max_rows) ||
            (options->max_elapsed_milliseconds > 0.0 &&
             stream_now_ms() - started >= options->max_elapsed_milliseconds)) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                options->max_rows && rows_read >= options->max_rows
                ? "La agrupación spill alcanzó el límite de filas"
                : "La agrupación spill alcanzó el presupuesto de tiempo");
            resource_limit_reached = true;
            status = MILENA_ERR_OVERFLOW; break;
        }
        if (rows_read == SIZE_MAX) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "El contador de filas spill se desbordó");
            status = MILENA_ERR_OVERFLOW;
            break;
        }
        rows_read++;
        if (record_length > observed_record_bytes)
            observed_record_bytes = record_length;
        size_t field_count = 0;
        status = stream_split(record, ',', fields, options->max_columns,
                              &field_count, error);
        if (status != MILENA_OK) break;
        if (field_count != column_count) {
            milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                "Fila CSV agrupada con cantidad incorrecta de columnas");
            status = MILENA_ERR_DATA; break;
        }
        if (filter_index >= 0 && !stream_filter_matches(options, fields[filter_index]))
            continue;
        size_t key_length = strlen(fields[group_index]);
        if (key_length >= policy->max_key_bytes) {
            milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                             "Clave textual supera el límite AST de #spill");
            status = MILENA_ERR_OVERFLOW; break;
        }
        unsigned char *key = (unsigned char *)malloc(key_length + 1u);
        if (!key) { status = MILENA_ERR_MEMORY; break; }
        key[0] = 1u;
        if (key_length) memcpy(key + 1u, fields[group_index], key_length);
        const char *raw = fields[metric_index];
        bool is_null = !stream_field_nonempty(raw);
        bool valid = false;
        double value = 1.0;
        if (metric->operation == MILENA_STREAM_COUNT) {
            valid = !is_null;
        } else {
            valid = stream_parse_number(raw, &value);
        }
        if (!valid) {
            status = is_null ?
                milena_grouped_aggregate_add_null(&reducer, key,
                                                  key_length + 1u, error) :
                milena_grouped_aggregate_add_invalid(&reducer, key,
                                                     key_length + 1u, error);
            malformed++;
        } else {
            if (metric->operation == MILENA_STREAM_COUNT) value = 1.0;
            status = milena_grouped_aggregate_add(&reducer, key,
                                                   key_length + 1u, value, error);
            rows_valid++;
        }
        free(key);
        if (status != MILENA_OK) break;
    }
    if (status == MILENA_ERR_IO && feof(input)) status = MILENA_OK;
    {
        long position = ftell(input);
        if (position >= 0) input_bytes = bytes_read = (size_t)position;
    }
    if (input) {
        if (fclose(input) != 0) {
            input = NULL;
            milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                             "No se pudo cerrar el CSV agrupado spill");
            status = MILENA_ERR_IO;
        } else input = NULL;
    }
    if (status == MILENA_OK && options->max_elapsed_milliseconds > 0.0 &&
        stream_now_ms() - started >= options->max_elapsed_milliseconds) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "La agrupación spill superó el presupuesto de tiempo");
        resource_limit_reached = true;
        status = MILENA_ERR_OVERFLOW;
    }
    if (status != MILENA_OK) goto spill_finish;

    staged = stream_open_staging(output_path, &staging_path, error);
    if (!staged) { status = error ? error->code : MILENA_ERR_IO; goto spill_finish; }
    double elapsed = stream_now_ms() - started;
    double seconds = elapsed > 0.0 ? elapsed / 1000.0 : 0.0;
    size_t output_group_limit = policy->max_output_groups;
    if (options->max_groups && options->max_groups < output_group_limit)
        output_group_limit = options->max_groups;
    if (fprintf(staged,
        "{\"modo\":\"flujo_agrupado_spill\",\"grupo\":") < 0) {
        status = MILENA_ERR_IO; goto spill_finish;
    }
    milena_json_write_string(staged, group_column);
    if (fprintf(staged,
        ",\"filas\":%zu,\"filas_validas\":%zu,\"filas_malformadas\":%zu,"
        "\"bytes_entrada\":%zu,\"limite_filas\":%zu,\"presupuesto_tiempo_ms\":%.3f,"
        "\"pico_registro_bytes\":%zu,\"capacidad_buffer_registro_bytes\":%zu,"
        "\"limite_registro_bytes\":%zu,\"limite_columnas\":%zu,"
        "\"memoria_reductor_bytes\":%zu,\"cuota_spill_bytes\":%zu,"
        "\"limite_salida_bytes\":%zu,\"grupos\":", rows_read, rows_valid, malformed,
        input_bytes, options->max_rows, options->max_elapsed_milliseconds,
        observed_record_bytes, record_capacity, options->max_record_bytes,
        options->max_columns, policy->memory_budget_bytes,
        policy->spill_quota_bytes, output_byte_limit) < 0) {
        status = MILENA_ERR_IO; goto spill_finish;
    }
    long groups_position = ftell(staged);
    if (groups_position < 0 || fprintf(staged,
        "%20s,\"limite_grupos\":%zu,\"filas_por_segundo\":%.6f,"
        "\"megabytes_por_segundo\":%.6f,\"tiempo_ms\":%.3f,"
        "\"resultados\":[", "", output_group_limit,
        seconds > 0.0 ? (double)rows_read / seconds : 0.0,
        seconds > 0.0 ? ((double)input_bytes / 1000000.0) / seconds : 0.0,
        elapsed) < 0) {
        status = MILENA_ERR_IO; goto spill_finish;
    }
    long prefix_end = ftell(staged);
    if (prefix_end < 0 || (size_t)prefix_end > output_byte_limit) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El encabezado supera el límite de bytes del reporte spill");
        status = MILENA_ERR_OVERFLOW; goto spill_finish;
    }
    StreamSpillEmitter emitter = {staged, metric, output_group_limit,
        0, output_byte_limit, true, started, options->max_elapsed_milliseconds};
    size_t groups_emitted = 0;
    status = milena_grouped_aggregate_finalize(&reducer,
        stream_spill_emit_group, &emitter, &groups_emitted, error);
    if (status != MILENA_OK) goto spill_finish;
    /* Snapshot counters from the real reducer before close releases its state. */
    spill_bytes = reducer.spill.bytes_used;
    spill_records = reducer.spill.record_count;
    spill_runs = reducer.sorted_runs;
    if (options->max_elapsed_milliseconds > 0.0 &&
        stream_now_ms() - started >= options->max_elapsed_milliseconds) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "La agrupación spill superó el presupuesto de tiempo");
        resource_limit_reached = true;
        status = MILENA_ERR_OVERFLOW; goto spill_finish;
    }
    long report_end = ftell(staged);
    if (report_end < 0 || fseek(staged, groups_position, SEEK_SET) != 0 ||
        fprintf(staged, "%20zu", groups_emitted) < 0 ||
        fseek(staged, report_end, SEEK_SET) != 0) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo completar el conteo de grupos del reporte");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    char telemetry_suffix[192];
    int telemetry_length = snprintf(telemetry_suffix, sizeof(telemetry_suffix),
        "],\"bytes_spill\":%zu,\"registros_spill\":%zu,\"runs_spill\":%zu}\n",
        spill_bytes, spill_records, spill_runs);
    if (report_end < 0 || (size_t)report_end > output_byte_limit ||
        telemetry_length < 0 || (size_t)telemetry_length >= sizeof(telemetry_suffix) ||
        (size_t)telemetry_length > output_byte_limit - (size_t)report_end) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El reporte spill superaría el límite de bytes de salida");
        status = MILENA_ERR_OVERFLOW; goto spill_finish;
    }
    if (fwrite(telemetry_suffix, 1, (size_t)telemetry_length, staged) !=
            (size_t)telemetry_length || fflush(staged) != 0) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo completar el reporte spill agrupado");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    if (fclose(staged) != 0) {
        staged = NULL;
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo cerrar el reporte spill agrupado");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    staged = NULL;
    status = milena_grouped_aggregate_close(&reducer, error);
    reducer_open = false;
    if (status != MILENA_OK) goto spill_finish;
    if (remove(policy->scratch_path) != 0 && errno != ENOENT) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo limpiar el scratch spill antes de publicar");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    scratch_owned = false;
    if (!stream_publish_staged(staging_path, output_path)) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo publicar atómicamente el reporte spill");
        status = MILENA_ERR_IO; goto spill_finish;
    }
    published = true;
    if (report) {
        report->rows_read = rows_read;
        report->rows_with_valid_values = rows_valid;
        report->malformed_rows = malformed;
        report->input_bytes = input_bytes;
        report->bytes_read = bytes_read;
        report->chunk_rows = options->chunk_rows;
        report->elapsed_milliseconds = stream_now_ms() - started;
        report->observed_record_bytes = observed_record_bytes;
        report->peak_record_bytes = record_capacity;
        report->header_columns = column_count;
        report->max_record_bytes = options->max_record_bytes;
        report->max_columns = options->max_columns;
        report->max_rows = options->max_rows;
        report->max_elapsed_milliseconds = options->max_elapsed_milliseconds;
        report->resource_limit_reached = resource_limit_reached;
        report->groups = groups_emitted;
        report->max_groups = output_group_limit;
        report->spill_bytes = spill_bytes;
        report->spill_records = spill_records;
        report->spill_runs = spill_runs;
    }
spill_finish:
    if (input) {
        long position = ftell(input);
        if (position >= 0) input_bytes = bytes_read = (size_t)position;
        if (fclose(input) != 0 && status == MILENA_OK) status = MILENA_ERR_IO;
    }
    if (staged) fclose(staged);
    if (reducer_open) {
        MilenaStatus close_status = milena_grouped_aggregate_close(&reducer,
            status == MILENA_OK ? error : NULL);
        if (status == MILENA_OK) status = close_status;
    }
    if (scratch_owned) (void)remove(policy->scratch_path);
    if (staging_path) {
        if (!published) (void)remove(staging_path);
        free(staging_path);
    }
    free(record);
    free(fields);
    stream_free_headers(headers, column_count);
    if (report && status != MILENA_OK) {
        memset(report, 0, sizeof(*report));
        report->resource_limit_reached = resource_limit_reached;
        report->rows_read = rows_read;
        report->bytes_read = bytes_read;
        report->max_record_bytes = options->max_record_bytes;
        report->max_columns = options->max_columns;
        report->max_rows = options->max_rows;
        report->max_elapsed_milliseconds = options->max_elapsed_milliseconds;
        report->max_groups = policy->max_output_groups;
        report->peak_record_bytes = record_capacity;
    }
    return status;
}
