#include "stream.h"

#include <float.h>
#include <time.h>

#define STREAM_MAX_COLUMNS 4096u
#define STREAM_MAX_METRICS 64u
#define STREAM_IO_BUFFER (64u * 1024u)
#define STREAM_INITIAL_RECORD 4096u
#define STREAM_DEFAULT_MAX_RECORD (64u * 1024u * 1024u)
#define STREAM_DEFAULT_MAX_COLUMNS STREAM_MAX_COLUMNS
#define STREAM_HARD_MAX_GROUPS 100000u
#define STREAM_DEFAULT_MAX_GROUPS 1000u
#define STREAM_MAX_GROUP_KEY_BYTES 4096u
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
    MilenaStreamOptions options = {4096u, STREAM_DEFAULT_MAX_RECORD,
                                   STREAM_DEFAULT_MAX_COLUMNS, 0u, 0.0,
                                   STREAM_DEFAULT_MAX_GROUPS};
    return options;
}

MilenaStatus milena_stream_csv_summary_with_options(const char *input_path,
                                       const char *output_path,
                                       const MilenaStreamMetric *metrics,
                                       size_t metric_count,
                                       const MilenaStreamOptions *requested,
                                       MilenaStreamReport *report,
                                       MilenaError *error) {
    MilenaStreamOptions defaults = milena_stream_options_default();
    const MilenaStreamOptions *options = requested ? requested : &defaults;
    if (!input_path || !output_path || !metrics || metric_count == 0 ||
        metric_count > STREAM_MAX_METRICS || options->chunk_rows == 0 ||
        options->max_record_bytes < STREAM_INITIAL_RECORD ||
        options->max_record_bytes > STREAM_DEFAULT_MAX_RECORD ||
        options->max_columns == 0 || options->max_columns > STREAM_MAX_COLUMNS ||
        options->max_elapsed_milliseconds < 0.0 ||
        !isfinite(options->max_elapsed_milliseconds) ||
        options->max_groups > STREAM_HARD_MAX_GROUPS)
        return MILENA_ERR_ARGUMENT;
    if (report) memset(report, 0, sizeof(*report));
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
        rows_read++;
        if (record_length > observed_record_bytes) observed_record_bytes = record_length;
        size_t field_count = 0;
        status = stream_split(record, ',', fields, options->max_columns, &field_count, error);
        if (status != MILENA_OK) break;
        if (field_count != column_count) { malformed++; continue; }
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
                ",\"valores_validos\":%zu,\"valores_invalidos\":%zu,\"valor\":",
                groups[g].accumulators[i].count,
                groups[g].invalid_values[i]) < 0) return false;
            if (isnan(value)) {
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
        group_limit > STREAM_HARD_MAX_GROUPS) {
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
            size_t invalid_bytes = metric_count * sizeof(size_t);
            if (key_length > STREAM_MAX_GROUP_KEY_BYTES ||
                accumulator_bytes > SIZE_MAX - invalid_bytes ||
                key_length + 1 > SIZE_MAX - accumulator_bytes - invalid_bytes ||
                group_state_bytes > STREAM_GROUP_STATE_BUDGET -
                    (key_length + 1 + accumulator_bytes + invalid_bytes)) {
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
            group->invalid_values = (size_t *)calloc(
                metric_count, sizeof(*group->invalid_values));
            if (!group->key || !group->accumulators || !group->invalid_values) {
                free(group->key); free(group->accumulators); free(group->invalid_values);
                memset(group, 0, sizeof(*group));
                status = MILENA_ERR_MEMORY;
                break;
            }
            group_state_bytes += key_length + 1 + accumulator_bytes + invalid_bytes;
            group_index_found = group_count++;
            group_slots[slot] = group_index_found + 1;
        }
        StreamGroup *group = &groups[group_index_found];
        bool row_valid = false;
        bool row_malformed = false;
        for (size_t i = 0; i < metric_count; i++) {
            const char *field = fields[metric_indexes[i]];
            double value = 1.0;
            bool valid = metrics[i].operation == MILENA_STREAM_COUNT
                ? stream_field_nonempty(field)
                : stream_parse_number(field, &value);
            if (!valid) {
                group->invalid_values[i]++;
                row_malformed = true;
                continue;
            }
            stream_accumulate(&group->accumulators[i], value);
            row_valid = true;
        }
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
