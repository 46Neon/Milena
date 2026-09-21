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
#define STREAM_HARD_MAX_GROUPS 100000u

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
        if (read[-1] != delimiter && *read == '\0') {
            *write = '\0';
            break;
        }
        if (*read == '\0' && read[-1] == delimiter) {
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
                                   STREAM_DEFAULT_MAX_COLUMNS, 1000u};
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
        (options->max_groups != 0 && options->max_groups > STREAM_HARD_MAX_GROUPS))
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
    size_t input_bytes = 0;
    MilenaStatus status = stream_read_record(input, &record, &record_capacity, options->max_record_bytes,
                       &record_length, error);
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
    while ((status = stream_read_record(input, &record, &record_capacity, options->max_record_bytes,
                                       &record_length, error)) == MILENA_OK) {
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
    if (measured_bytes >= 0) input_bytes = (size_t)measured_bytes;
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
        double megabytes_per_second = seconds > 0.0 ? ((double)input_bytes / (1024.0 * 1024.0)) / seconds : 0.0;
        fprintf(output, "{\"modo\":\"flujo\",\"filas\":%zu,\"filas_validas\":%zu,\"filas_malformadas\":%zu,\"bytes_entrada\":%zu,\"filas_por_segundo\":%.6f,\"megabytes_por_segundo\":%.6f,\"tamano_lote\":%zu,\"limite_registro_bytes\":%zu,\"limite_columnas\":%zu,\"pico_registro_bytes\":%zu,\"capacidad_buffer_registro_bytes\":%zu,\"tiempo_ms\":%.3f,\"resultados\":[", rows_read, rows_valid, malformed, input_bytes, rows_per_second, megabytes_per_second, options->chunk_rows, options->max_record_bytes, options->max_columns, observed_record_bytes, record_capacity, elapsed);
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
        }
    }

finish:
    if (fclose(input) != 0 && status == MILENA_OK) status = MILENA_ERR_IO;
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



typedef struct { char *key; StreamAccumulator *acc; size_t invalid; } StreamGroup;

MilenaStatus milena_stream_csv_grouped_with_options(const char *input_path,
    const char *output_path, const char *group_column,
    const MilenaStreamMetric *metrics, size_t metric_count,
    const MilenaStreamOptions *requested, MilenaStreamReport *report,
    MilenaError *error) {
    MilenaStreamOptions defaults = milena_stream_options_default();
    const MilenaStreamOptions *o = requested ? requested : &defaults;
    size_t max_groups = o->max_groups ? o->max_groups : defaults.max_groups;
    if (!input_path || !output_path || !group_column || !group_column[0] ||
        !metrics || !metric_count || metric_count > STREAM_MAX_METRICS ||
        max_groups > STREAM_HARD_MAX_GROUPS) return MILENA_ERR_ARGUMENT;
    FILE *in=fopen(input_path,"rb"); if(!in){milena_error_set(error,MILENA_ERR_IO,0,0,0,"No se pudo abrir el CSV agrupado");return MILENA_ERR_IO;}
    char *record=NULL, **fields=NULL, **headers=NULL; size_t cap=0,len=0,cols=0,groups=0,rows=0,malformed=0,observed=0; MilenaStatus st;
    st=stream_read_record(in,&record,&cap,o->max_record_bytes,&len,error); if(st!=MILENA_OK) goto done;
    fields=calloc(o->max_columns,sizeof(*fields)); if(!fields){st=MILENA_ERR_MEMORY;goto done;}
    st=stream_split(record,',',fields,o->max_columns,&cols,error); if(st!=MILENA_OK)goto done;
    headers=calloc(cols,sizeof(*headers)); if(!headers){st=MILENA_ERR_MEMORY;goto done;}
    for(size_t i=0;i<cols;i++){headers[i]=milena_strdup(fields[i]);if(!headers[i]){st=MILENA_ERR_MEMORY;goto done;}}
    int gi=stream_column_index(headers,cols,group_column), ix[STREAM_MAX_METRICS];
    if(gi<0){milena_error_set(error,MILENA_ERR_DATA,0,0,0,"La columna de agrupación no existe");st=MILENA_ERR_DATA;goto done;}
    for(size_t i=0;i<metric_count;i++){ix[i]=stream_column_index(headers,cols,metrics[i].column);if(ix[i]<0){milena_error_set(error,MILENA_ERR_DATA,0,0,0,"La columna métrica no existe");st=MILENA_ERR_DATA;goto done;}}
    StreamGroup *gs=calloc(max_groups,sizeof(*gs)); if(!gs){st=MILENA_ERR_MEMORY;goto done;}
    while((st=stream_read_record(in,&record,&cap,o->max_record_bytes,&len,error))==MILENA_OK){
      rows++; if(len>observed)observed=len; size_t fc=0; st=stream_split(record,',',fields,o->max_columns,&fc,error); if(st!=MILENA_OK)break; if(fc!=cols){malformed++;continue;}
      size_t k=0; while(k<groups && strcmp(gs[k].key,fields[gi]))k++;
      if(k==groups){if(groups>=max_groups){milena_error_set(error,MILENA_ERR_OVERFLOW,0,0,0,"Se superó el límite máximo de grupos del flujo");st=MILENA_ERR_OVERFLOW;break;} gs[k].key=milena_strdup(fields[gi]);gs[k].acc=calloc(metric_count,sizeof(StreamAccumulator));if(!gs[k].key||!gs[k].acc){st=MILENA_ERR_MEMORY;break;}groups++;}
      bool bad=false; for(size_t i=0;i<metric_count;i++){double v;if(!stream_parse_number(fields[ix[i]],&v)){gs[k].invalid++;bad=true;continue;}stream_accumulate(&gs[k].acc[i],v);} if(bad)malformed++;
    }
    if(st==MILENA_ERR_IO&&feof(in))st=MILENA_OK; if(st!=MILENA_OK)goto grouped_done;
    {FILE*out=fopen(output_path,"wb");if(!out){st=MILENA_ERR_IO;goto grouped_done;} fprintf(out,"{\"modo\":\"flujo_agrupado\",\"grupo\":");milena_json_write_string(out,group_column);fprintf(out,",\"filas\":%zu,\"grupos\":%zu,\"limite_grupos\":%zu,\"resultados\":[",rows,groups,max_groups);
      for(size_t k=0;k<groups;k++){if(k)fputc(',',out);fprintf(out,"{\"clave\":");milena_json_write_string(out,gs[k].key);fputs(",\"metricas\":[",out);for(size_t i=0;i<metric_count;i++){if(i)fputc(',',out);const char*n=metrics[i].name;char gen[256];if(!n||!n[0]){snprintf(gen,sizeof(gen),"%s_%s",metrics[i].column,milena_stream_operation_name(metrics[i].operation));n=gen;}double v=stream_value(&gs[k].acc[i],metrics[i].operation);fprintf(out,"{\"nombre\":");milena_json_write_string(out,n);fprintf(out,",\"operacion\":");milena_json_write_string(out,milena_stream_operation_name(metrics[i].operation));fprintf(out,",\"valores_invalidos\":%zu,\"valor\":",gs[k].invalid);if(isnan(v))fputs("null",out);else fprintf(out,"%.17g",v);fputc('}',out);}fputs("]}",out);}fputs("]}\n",out);if(fclose(out)!=0)st=MILENA_ERR_IO;}
 grouped_done: for(size_t k=0;k<groups;k++){free(gs[k].key);free(gs[k].acc);}free(gs);
 done: if(in)fclose(in);free(record);free(fields);stream_free_headers(headers,cols);return st;
}




