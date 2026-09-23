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
#define STREAM_DEFAULT_SPILL_BYTES (64u * 1024u * 1024u)
#define STREAM_DEFAULT_SPILL_RECORDS 1000000u
#define STREAM_DEFAULT_SPILL_FILES 64u
#define STREAM_HARD_SPILL_BYTES (1024u * 1024u * 1024u)
#define STREAM_HARD_SPILL_RECORDS 10000000u
#define STREAM_HARD_SPILL_FILES 256u
#define STREAM_SPILL_HEADER_BYTES 16u

_Static_assert(sizeof(double) == sizeof(uint64_t),
               "grouped spill format requires 64-bit doubles");

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
    MilenaStreamOptions options = {0};
    options.chunk_rows = 4096u;
    options.max_record_bytes = STREAM_DEFAULT_MAX_RECORD;
    options.max_columns = STREAM_DEFAULT_MAX_COLUMNS;
    options.max_groups = STREAM_DEFAULT_MAX_GROUPS;
    options.max_spill_bytes = STREAM_DEFAULT_SPILL_BYTES;
    options.max_spill_records = STREAM_DEFAULT_SPILL_RECORDS;
    options.max_spill_files = STREAM_DEFAULT_SPILL_FILES;
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

static void stream_groups_clear(StreamGroup *groups, size_t count) {
    if (!groups) return;
    for (size_t i = 0; i < count; i++) {
        free(groups[i].key);
        free(groups[i].accumulators);
        free(groups[i].invalid_values);
        memset(&groups[i], 0, sizeof(groups[i]));
    }
}

static void stream_groups_release(StreamGroup *groups, size_t count) {
    stream_groups_clear(groups, count);
    free(groups);
}

typedef struct {
    FILE *file;
    StreamGroup current;
    bool active;
} StreamSpillRun;

static uint32_t spill_hash32(const unsigned char *bytes, size_t length) {
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static uint64_t spill_hash64(const unsigned char *bytes, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static void spill_put_u32(unsigned char *out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}
static uint32_t spill_get_u32(const unsigned char *in) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t)in[i] << (i * 8);
    return value;
}
static void spill_put_u64(unsigned char *out, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}
static uint64_t spill_get_u64(const unsigned char *in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)in[i] << (i * 8);
    return value;
}
static void spill_put_double(unsigned char *out, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    spill_put_u64(out, bits);
}
static double spill_get_double(const unsigned char *in) {
    uint64_t bits = spill_get_u64(in);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool spill_run_header_write(FILE *file, size_t metric_count) {
    unsigned char header[STREAM_SPILL_HEADER_BYTES] = {'M','L','S','R',1,0,0,0,0,0,0,0,0,0,0,0};
    spill_put_u32(header + 8, (uint32_t)metric_count);
    spill_put_u32(header + 12, spill_hash32(header, 12));
    return fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

static bool spill_run_header_read(FILE *file, size_t metric_count) {
    unsigned char header[STREAM_SPILL_HEADER_BYTES];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) return false;
    return memcmp(header, "MLSR", 4) == 0 && header[4] == 1 &&
        header[5] == 0 && header[6] == 0 && header[7] == 0 &&
        spill_get_u32(header + 8) == metric_count &&
        spill_get_u32(header + 12) == spill_hash32(header, 12);
}

static bool spill_record_write(FILE *file, const StreamGroup *group,
                               size_t metric_count, size_t *written_bytes) {
    size_t key_length = strlen(group->key);
    if (key_length > STREAM_MAX_GROUP_KEY_BYTES ||
        metric_count > (SIZE_MAX - 4u - key_length) / 64u) return false;
    size_t payload_length = 4u + key_length + metric_count * 64u;
    unsigned char *payload = (unsigned char *)malloc(payload_length ? payload_length : 1u);
    if (!payload) return false;
    spill_put_u32(payload, (uint32_t)key_length);
    memcpy(payload + 4, group->key, key_length);
    size_t offset = 4u + key_length;
    for (size_t i = 0; i < metric_count; i++) {
        const StreamAccumulator *a = &group->accumulators[i];
        spill_put_double(payload + offset, a->sum); offset += 8;
        spill_put_double(payload + offset, a->compensation); offset += 8;
        spill_put_double(payload + offset, a->mean); offset += 8;
        spill_put_double(payload + offset, a->m2); offset += 8;
        spill_put_double(payload + offset, a->minimum); offset += 8;
        spill_put_double(payload + offset, a->maximum); offset += 8;
        spill_put_u64(payload + offset, (uint64_t)a->count); offset += 8;
        spill_put_u64(payload + offset, (uint64_t)group->invalid_values[i]); offset += 8;
    }
    unsigned char frame[12];
    spill_put_u32(frame, (uint32_t)payload_length);
    spill_put_u64(frame + 4, spill_hash64(payload, payload_length));
    bool ok = fwrite(frame, 1, 4, file) == 4 &&
              fwrite(payload, 1, payload_length, file) == payload_length &&
              fwrite(frame + 4, 1, 8, file) == 8;
    free(payload);
    if (ok) *written_bytes += 12u + payload_length;
    return ok;
}

static void spill_cursor_clear(StreamSpillRun *run) {
    if (!run) return;
    free(run->current.key);
    free(run->current.accumulators);
    free(run->current.invalid_values);
    memset(&run->current, 0, sizeof(run->current));
    run->active = false;
}

static MilenaStatus spill_cursor_next(StreamSpillRun *run, size_t metric_count,
                                      MilenaError *error) {
    spill_cursor_clear(run);
    unsigned char length_bytes[4];
    size_t got = fread(length_bytes, 1, 4, run->file);
    if (got == 0 && feof(run->file)) return MILENA_OK;
    if (got != 4) goto corrupt;
    uint32_t payload_length = spill_get_u32(length_bytes);
    size_t minimum = 4u + metric_count * 64u;
    if (payload_length < minimum || payload_length > 4u + STREAM_MAX_GROUP_KEY_BYTES + STREAM_MAX_METRICS * 64u)
        goto corrupt;
    unsigned char *payload = (unsigned char *)malloc(payload_length);
    if (!payload) return MILENA_ERR_MEMORY;
    unsigned char checksum_bytes[8];
    if (fread(payload, 1, payload_length, run->file) != payload_length ||
        fread(checksum_bytes, 1, sizeof(checksum_bytes), run->file) != sizeof(checksum_bytes) ||
        spill_get_u64(checksum_bytes) != spill_hash64(payload, payload_length)) {
        free(payload);
        goto corrupt;
    }
    uint32_t key_length = spill_get_u32(payload);
    if (key_length > STREAM_MAX_GROUP_KEY_BYTES ||
        payload_length != 4u + (size_t)key_length + metric_count * 64u ||
        memchr(payload + 4, '\0', key_length) != NULL) {
        free(payload);
        goto corrupt;
    }
    run->current.key = (char *)malloc((size_t)key_length + 1u);
    run->current.accumulators = (StreamAccumulator *)calloc(metric_count, sizeof(StreamAccumulator));
    run->current.invalid_values = (size_t *)calloc(metric_count, sizeof(size_t));
    if (!run->current.key || !run->current.accumulators || !run->current.invalid_values) {
        free(payload); spill_cursor_clear(run); return MILENA_ERR_MEMORY;
    }
    memcpy(run->current.key, payload + 4, key_length);
    run->current.key[key_length] = '\0';
    size_t offset = 4u + key_length;
    for (size_t i = 0; i < metric_count; i++) {
        StreamAccumulator *a = &run->current.accumulators[i];
        a->sum = spill_get_double(payload + offset); offset += 8;
        a->compensation = spill_get_double(payload + offset); offset += 8;
        a->mean = spill_get_double(payload + offset); offset += 8;
        a->m2 = spill_get_double(payload + offset); offset += 8;
        a->minimum = spill_get_double(payload + offset); offset += 8;
        a->maximum = spill_get_double(payload + offset); offset += 8;
        uint64_t count = spill_get_u64(payload + offset); offset += 8;
        uint64_t invalid = spill_get_u64(payload + offset); offset += 8;
#if SIZE_MAX < UINT64_MAX
        if (count > (uint64_t)SIZE_MAX || invalid > (uint64_t)SIZE_MAX) {
            free(payload); spill_cursor_clear(run); goto corrupt;
        }
#endif
        a->count = (size_t)count;
        run->current.invalid_values[i] = (size_t)invalid;
    }
    free(payload);
    run->active = true;
    return MILENA_OK;
corrupt:
    milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                     "Corrida temporal de agrupación truncada o con checksum inválido");
    return MILENA_ERR_DATA;
}

static bool stream_accumulator_merge(StreamAccumulator *a, const StreamAccumulator *b) {
    if (b->count == 0) return true;
    if (a->count == 0) { *a = *b; return true; }
    if (b->count > SIZE_MAX - a->count) return false;
    size_t total = a->count + b->count;
    double corrected = b->sum - a->compensation;
    double next_sum = a->sum + corrected;
    a->compensation = (next_sum - a->sum) - corrected;
    a->sum = next_sum;
    corrected = -b->compensation - a->compensation;
    next_sum = a->sum + corrected;
    a->compensation = (next_sum - a->sum) - corrected;
    a->sum = next_sum;
    double delta = b->mean - a->mean;
    a->mean += delta * ((double)b->count / (double)total);
    a->m2 += b->m2 + delta * delta * ((double)a->count * (double)b->count / (double)total);
    a->count = total;
    if (b->minimum < a->minimum) a->minimum = b->minimum;
    if (b->maximum > a->maximum) a->maximum = b->maximum;
    return true;
}

static MilenaStatus stream_spill_merge_next(StreamSpillRun *runs, size_t run_count,
                                            size_t metric_count, StreamGroup *merged,
                                            MilenaError *error) {
    memset(merged, 0, sizeof(*merged));
    size_t selected = run_count;
    for (size_t r = 0; r < run_count; r++) {
        if (runs[r].active && (selected == run_count ||
            strcmp(runs[r].current.key, runs[selected].current.key) < 0)) selected = r;
    }
    if (selected == run_count) return MILENA_OK;
    merged->key = milena_strdup(runs[selected].current.key);
    merged->accumulators = (StreamAccumulator *)calloc(metric_count, sizeof(*merged->accumulators));
    merged->invalid_values = (size_t *)calloc(metric_count, sizeof(*merged->invalid_values));
    if (!merged->key || !merged->accumulators || !merged->invalid_values) {
        free(merged->key); free(merged->accumulators); free(merged->invalid_values);
        memset(merged, 0, sizeof(*merged)); return MILENA_ERR_MEMORY;
    }
    const char *key = merged->key;
    for (size_t r = 0; r < run_count; r++) {
        while (runs[r].active && strcmp(runs[r].current.key, key) == 0) {
            for (size_t m = 0; m < metric_count; m++) {
                if (!stream_accumulator_merge(&merged->accumulators[m], &runs[r].current.accumulators[m]) ||
                    runs[r].current.invalid_values[m] > SIZE_MAX - merged->invalid_values[m]) {
                    free(merged->key); free(merged->accumulators); free(merged->invalid_values);
                    memset(merged, 0, sizeof(*merged));
                    milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                     "Desbordamiento al fusionar corridas de agrupación");
                    return MILENA_ERR_OVERFLOW;
                }
                merged->invalid_values[m] += runs[r].current.invalid_values[m];
            }
            MilenaStatus status = spill_cursor_next(&runs[r], metric_count, error);
            if (status != MILENA_OK) {
                free(merged->key); free(merged->accumulators); free(merged->invalid_values);
                memset(merged, 0, sizeof(*merged)); return status;
            }
        }
    }
    return MILENA_OK;
}

static MilenaStatus stream_spill_write_run(StreamGroup *groups, size_t group_count,
        size_t metric_count, StreamSpillRun *runs, size_t *run_count,
        size_t max_files, size_t max_bytes, size_t max_records,
        size_t *spill_bytes, size_t *spill_records, MilenaError *error) {
    if (*run_count >= max_files || group_count > max_records - *spill_records) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "Se alcanzó el límite de corridas o registros temporales");
        return MILENA_ERR_OVERFLOW;
    }
    qsort(groups, group_count, sizeof(*groups), stream_group_compare);
    FILE *file = tmpfile();
    if (!file) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo crear una corrida temporal local para agrupación");
        return MILENA_ERR_IO;
    }
    size_t run_bytes = STREAM_SPILL_HEADER_BYTES;
    bool byte_limit = run_bytes > max_bytes - *spill_bytes;
    if (byte_limit) goto limit_or_io;
    if (!spill_run_header_write(file, metric_count)) goto io_error;
    for (size_t i = 0; i < group_count; i++) {
        size_t key_length = strlen(groups[i].key);
        size_t record_bytes = 12u + 4u + key_length + metric_count * 64u;
        if (run_bytes > max_bytes - *spill_bytes ||
            record_bytes > max_bytes - *spill_bytes - run_bytes) {
            byte_limit = true;
            goto limit_or_io;
        }
        if (!spill_record_write(file, &groups[i], metric_count, &run_bytes)) goto io_error;
    }
    if (fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0 ||
        !spill_run_header_read(file, metric_count)) goto io_error;
    runs[*run_count].file = file;
    runs[*run_count].active = false;
    (*run_count)++;
    *spill_bytes += run_bytes;
    *spill_records += group_count;
    return MILENA_OK;
limit_or_io:
    fclose(file);
    milena_error_set(error, byte_limit ? MILENA_ERR_OVERFLOW : MILENA_ERR_IO,
                     0, 0, 0, byte_limit
                     ? "Se alcanzó el límite de bytes temporales de agrupación"
                     : "No se pudo serializar una corrida temporal de agrupación");
    return byte_limit ? MILENA_ERR_OVERFLOW : MILENA_ERR_IO;
io_error:
    fclose(file);
    milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                     "No se pudo validar la cabecera de una corrida temporal");
    return MILENA_ERR_IO;
}

static bool stream_group_emit_start(FILE *output, const char *group_column,
        size_t group_count, size_t group_limit, size_t rows_read, size_t rows_valid,
        size_t malformed, size_t input_bytes, size_t observed_record_bytes,
        size_t record_capacity, double elapsed_ms, bool spilled,
        size_t spill_runs, size_t spill_bytes, size_t max_rows,
        double max_elapsed_ms) {
    double seconds = elapsed_ms > 0.0 ? elapsed_ms / 1000.0 : 0.0;
    double rows_per_second = seconds > 0.0 ? (double)rows_read / seconds : 0.0;
    double megabytes_per_second = seconds > 0.0 ?
        ((double)input_bytes / 1000000.0) / seconds : 0.0;
    if (fprintf(output,
        "{\"modo\":\"flujo_agrupado\",\"grupo\":") < 0) return false;
    milena_json_write_string(output, group_column);
    return fprintf(output,
        ",\"filas\":%zu,\"filas_validas\":%zu,\"filas_malformadas\":%zu,"
        "\"grupos\":%zu,\"limite_grupos\":%zu,\"spill\":%s,"
        "\"corridas_spill\":%zu,\"bytes_spill\":%zu,\"bytes_entrada\":%zu,"
        "\"filas_por_segundo\":%.6f,\"megabytes_por_segundo\":%.6f,"
        "\"limite_filas\":%zu,\"presupuesto_tiempo_ms\":%.3f,"
        "\"pico_registro_bytes\":%zu,\"capacidad_buffer_registro_bytes\":%zu,"
        "\"tiempo_ms\":%.3f,\"resultados\":[",
        rows_read, rows_valid, malformed, group_count, group_limit,
        spilled ? "true" : "false", spill_runs, spill_bytes, input_bytes,
        rows_per_second, megabytes_per_second, max_rows, max_elapsed_ms,
        observed_record_bytes, record_capacity, elapsed_ms) >= 0;
}

static bool stream_group_emit_one(FILE *output, const MilenaStreamMetric *metrics,
                                  size_t metric_count, const StreamGroup *group,
                                  bool first) {
    if (!first && fputc(',', output) == EOF) return false;
    if (fputs("{\"clave\":", output) == EOF) return false;
    milena_json_write_string(output, group->key);
    if (fputs(",\"metricas\":[", output) == EOF) return false;
    for (size_t i = 0; i < metric_count; i++) {
        if (i && fputc(',', output) == EOF) return false;
        char generated_name[256];
        const char *name = metrics[i].name;
        if (!name || !name[0]) {
            (void)snprintf(generated_name, sizeof(generated_name), "%s_%s",
                metrics[i].column, milena_stream_operation_name(metrics[i].operation));
            name = generated_name;
        }
        double value = stream_value(&group->accumulators[i], metrics[i].operation);
        if (fputs("{\"columna\":", output) == EOF) return false;
        milena_json_write_string(output, metrics[i].column);
        if (fputs(",\"operacion\":", output) == EOF) return false;
        milena_json_write_string(output, milena_stream_operation_name(metrics[i].operation));
        if (fputs(",\"nombre\":", output) == EOF) return false;
        milena_json_write_string(output, name);
        if (fprintf(output, ",\"valores_validos\":%zu,\"valores_invalidos\":%zu,\"valor\":",
            group->accumulators[i].count, group->invalid_values[i]) < 0) return false;
        if (isnan(value)) {
            if (fputs("null", output) == EOF) return false;
        } else if (fprintf(output, "%.17g", value) < 0) return false;
        if (fputc('}', output) == EOF) return false;
    }
    return fputs("]}", output) != EOF;
}

static bool stream_group_emit(FILE *output, const char *group_column,
        const MilenaStreamMetric *metrics, size_t metric_count,
        const StreamGroup *groups, size_t group_count, size_t group_limit,
        size_t rows_read, size_t rows_valid, size_t malformed, size_t input_bytes,
        size_t observed_record_bytes, size_t record_capacity, double elapsed_ms,
        bool spilled, size_t spill_runs, size_t spill_bytes, size_t max_rows,
        double max_elapsed_ms) {
    if (!stream_group_emit_start(output, group_column, group_count, group_limit,
            rows_read, rows_valid, malformed, input_bytes, observed_record_bytes,
            record_capacity, elapsed_ms, spilled, spill_runs, spill_bytes,
            max_rows, max_elapsed_ms)) return false;
    for (size_t g = 0; g < group_count; g++)
        if (!stream_group_emit_one(output, metrics, metric_count, &groups[g], g == 0)) return false;
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
    size_t spill_byte_limit = options->max_spill_bytes ? options->max_spill_bytes : defaults.max_spill_bytes;
    size_t spill_record_limit = options->max_spill_records ? options->max_spill_records : defaults.max_spill_records;
    size_t spill_file_limit = options->max_spill_files ? options->max_spill_files : defaults.max_spill_files;
    if (!input_path || !output_path || !group_column || !group_column[0] ||
        !metrics || metric_count == 0 || metric_count > STREAM_MAX_METRICS ||
        options->chunk_rows == 0 ||
        options->max_record_bytes < STREAM_INITIAL_RECORD ||
        options->max_record_bytes > STREAM_DEFAULT_MAX_RECORD ||
        options->max_columns == 0 || options->max_columns > STREAM_MAX_COLUMNS ||
        options->max_elapsed_milliseconds < 0.0 ||
        !isfinite(options->max_elapsed_milliseconds) ||
        group_limit > STREAM_HARD_MAX_GROUPS ||
        spill_byte_limit > STREAM_HARD_SPILL_BYTES ||
        spill_record_limit > STREAM_HARD_SPILL_RECORDS ||
        spill_file_limit > STREAM_HARD_SPILL_FILES ||
        (options->spill_enabled && (spill_byte_limit < STREAM_SPILL_HEADER_BYTES ||
         spill_record_limit == 0 || spill_file_limit == 0))) {
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
    StreamSpillRun *spill_runs = NULL;
    size_t spill_run_count = 0, spill_bytes = 0, spill_records = 0;
    size_t total_group_count = 0;
    size_t group_slot_capacity = 0;
    size_t record_capacity = 0, record_length = 0, column_count = 0;
    size_t group_count = 0, rows_read = 0, rows_valid = 0, malformed = 0;
    size_t input_bytes = 0, bytes_read = 0, observed_record_bytes = 0;
    size_t group_state_bytes = 0;
    size_t group_base_state_bytes = 0;
    bool resource_limit_reached = false;
    bool spilled = false;
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
    if (options->spill_enabled) {
        spill_runs = (StreamSpillRun *)calloc(spill_file_limit, sizeof(*spill_runs));
        if (!spill_runs) { status = MILENA_ERR_MEMORY; goto grouped_finish; }
    }
    group_base_state_bytes = group_array_bytes + slot_bytes;
    group_state_bytes = group_base_state_bytes;

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
            if (key_length > STREAM_MAX_GROUP_KEY_BYTES) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "La clave de agrupación supera los 4096 bytes");
                status = MILENA_ERR_OVERFLOW;
                break;
            }
            if (group_count >= group_limit) {
                if (!options->spill_enabled) {
                    milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                     "Se superó el límite máximo de grupos del flujo");
                    status = MILENA_ERR_OVERFLOW;
                    break;
                }
                status = stream_spill_write_run(groups, group_count, metric_count,
                    spill_runs, &spill_run_count, spill_file_limit,
                    spill_byte_limit, spill_record_limit, &spill_bytes,
                    &spill_records, error);
                if (status != MILENA_OK) { resource_limit_reached = status == MILENA_ERR_OVERFLOW; break; }
                spilled = true;
                stream_groups_clear(groups, group_count);
                group_count = 0;
                memset(group_slots, 0, group_slot_capacity * sizeof(*group_slots));
                group_state_bytes = group_base_state_bytes;
                slot = (size_t)stream_group_hash(fields[group_index]) & (group_slot_capacity - 1);
                group_index_found = 0;
            }
            size_t accumulator_bytes = metric_count * sizeof(StreamAccumulator);
            size_t invalid_bytes = metric_count * sizeof(size_t);
            if (accumulator_bytes > SIZE_MAX - invalid_bytes ||
                key_length + 1 > SIZE_MAX - accumulator_bytes - invalid_bytes ||
                group_state_bytes > STREAM_GROUP_STATE_BUDGET -
                    (key_length + 1 + accumulator_bytes + invalid_bytes)) {
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "La agrupación alcanzó el presupuesto de estado acotado");
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

    if (spilled && group_count > 0) {
        status = stream_spill_write_run(groups, group_count, metric_count,
            spill_runs, &spill_run_count, spill_file_limit, spill_byte_limit,
            spill_record_limit, &spill_bytes, &spill_records, error);
        if (status != MILENA_OK) { resource_limit_reached = status == MILENA_ERR_OVERFLOW; goto grouped_finish; }
        stream_groups_clear(groups, group_count);
        group_count = 0;
    }
    FILE *output = NULL;
    double elapsed = stream_now_ms() - started;
    bool emitted = true;
    if (!spilled) {
        qsort(groups, group_count, sizeof(*groups), stream_group_compare);
        total_group_count = group_count;
        output = fopen(output_path, "wb");
        if (!output) {
            milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                             "No se pudo crear el reporte agrupado");
            status = MILENA_ERR_IO;
            goto grouped_finish;
        }
        emitted = stream_group_emit(output, group_column, metrics, metric_count,
            groups, group_count, group_limit, rows_read, rows_valid, malformed,
            input_bytes, observed_record_bytes, record_capacity, elapsed,
            false, 0, 0, options->max_rows, options->max_elapsed_milliseconds);
    } else {
        for (size_t r = 0; r < spill_run_count; r++) {
            if (fseek(spill_runs[r].file, 0, SEEK_SET) != 0 ||
                !spill_run_header_read(spill_runs[r].file, metric_count)) {
                milena_error_set(error, MILENA_ERR_DATA, 0, 0, 0,
                                 "Corrida temporal con cabecera inválida");
                status = MILENA_ERR_DATA; emitted = false; break;
            }
            status = spill_cursor_next(&spill_runs[r], metric_count, error);
            if (status != MILENA_OK) { emitted = false; break; }
        }
        /* First merge pass validates every checksum and counts distinct groups. */
        while (emitted) {
            StreamGroup merged = {0};
            status = stream_spill_merge_next(spill_runs, spill_run_count,
                                             metric_count, &merged, error);
            if (status != MILENA_OK) { emitted = false; break; }
            if (!merged.key) break;
            if (total_group_count == SIZE_MAX) {
                stream_groups_clear(&merged, 1);
                milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                                 "Desbordamiento al contar grupos fusionados");
                status = MILENA_ERR_OVERFLOW; emitted = false; break;
            }
            total_group_count++;
            free(merged.key); free(merged.accumulators); free(merged.invalid_values);
        }
        /* Reset readers for the output pass only after complete validation. */
        for (size_t r = 0; emitted && r < spill_run_count; r++) {
            spill_cursor_clear(&spill_runs[r]);
            if (fseek(spill_runs[r].file, 0, SEEK_SET) != 0 ||
                !spill_run_header_read(spill_runs[r].file, metric_count)) {
                status = MILENA_ERR_DATA; emitted = false; break;
            }
            status = spill_cursor_next(&spill_runs[r], metric_count, error);
            if (status != MILENA_OK) emitted = false;
        }
        if (emitted) {
            output = fopen(output_path, "wb");
            if (!output) {
                milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                                 "No se pudo crear el reporte agrupado");
                status = MILENA_ERR_IO;
                emitted = false;
            }
        }
        if (emitted && !stream_group_emit_start(output, group_column,
                total_group_count, group_limit, rows_read, rows_valid, malformed,
                input_bytes, observed_record_bytes, record_capacity, elapsed,
                true, spill_run_count, spill_bytes, options->max_rows,
                options->max_elapsed_milliseconds)) emitted = false;
        bool first_group = true;
        while (emitted) {
            StreamGroup merged = {0};
            status = stream_spill_merge_next(spill_runs, spill_run_count,
                                             metric_count, &merged, error);
            if (status != MILENA_OK) { emitted = false; break; }
            if (!merged.key) break;
            emitted = stream_group_emit_one(output, metrics, metric_count,
                                             &merged, first_group);
            first_group = false;
            free(merged.key); free(merged.accumulators); free(merged.invalid_values);
        }
        if (emitted && fputs("]}\n", output) == EOF) emitted = false;
    }
    if (output && fclose(output) != 0) emitted = false;
    if (!emitted) {
        if (status == MILENA_OK) {
            milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                             "No se pudo escribir el reporte agrupado");
            status = MILENA_ERR_IO;
        }
        if (output) remove(output_path);
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
        report->groups = spilled ? total_group_count : group_count;
        report->max_groups = group_limit;
        report->spilled = spilled;
        report->spill_runs = spill_run_count;
        report->spill_bytes = spill_bytes;
        report->spill_records = spill_records;
    }
    if (spill_runs) {
        for (size_t r = 0; r < spill_run_count; r++) {
            spill_cursor_clear(&spill_runs[r]);
            if (spill_runs[r].file) fclose(spill_runs[r].file);
        }
        free(spill_runs);
    }
    free(record);
    free(fields);
    free(group_slots);
    stream_free_headers(headers, column_count);
    stream_groups_release(groups, group_count);
    return status;
}
