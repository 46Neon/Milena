#include "stream.h"
#include "spill.h"

#include <float.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define STREAM_MAX_COLUMNS 4096u
#define STREAM_MAX_METRICS 64u
#define STREAM_IO_BUFFER (64u * 1024u)
#define STREAM_INITIAL_RECORD 4096u
#define STREAM_DEFAULT_MAX_RECORD (64u * 1024u * 1024u)
#define STREAM_DEFAULT_MAX_COLUMNS STREAM_MAX_COLUMNS
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
    MilenaStreamOptions options = {4096u, STREAM_DEFAULT_MAX_RECORD, STREAM_DEFAULT_MAX_COLUMNS, 1000u, NULL, 0, 0, 0};
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



typedef struct { char *key; StreamAccumulator *acc; size_t *invalid; } StreamGroup;
static uint64_t spill_hash(uint64_t h, const void *p, size_t n) { const unsigned char *b=p; while(n--) h=(h^*b++)*UINT64_C(1099511628211); return h; }
static void spill_mix(uint64_t *h, const void *p, size_t n) { *h=spill_hash(*h,p,n); }
static int stream_group_compare(const void *a, const void *b) {
    const StreamGroup *ga = (const StreamGroup *)a;
    const StreamGroup *gb = (const StreamGroup *)b;
    return strcmp(ga->key, gb->key);
}

static MilenaStatus grouped_emit(FILE *out,const char *group_column,const MilenaStreamMetric *m,size_t nm,StreamGroup *g,size_t ng,size_t maxg,size_t rows,size_t spilled) {
    if (fprintf(out,"{\"modo\":\"flujo_agrupado\",\"grupo\":")<0) return MILENA_ERR_IO; milena_json_write_string(out,group_column);
    if(fprintf(out,",\"filas\":%zu,\"grupos\":%zu,\"limite_grupos\":%zu,\"derramado\":%s,\"resultados\":[",rows,ng,maxg,spilled?"true":"false")<0)return MILENA_ERR_IO;
    for(size_t k=0;k<ng;k++){if(k&&fputc(',',out)==EOF)return MILENA_ERR_IO;fprintf(out,"{\"clave\":");milena_json_write_string(out,g[k].key);fputs(",\"metricas\":[",out);for(size_t i=0;i<nm;i++){if(i)fputc(',',out);char gen[256];const char*n=m[i].name;if(!n||!n[0]){snprintf(gen,sizeof(gen),"%s_%s",m[i].column,milena_stream_operation_name(m[i].operation));n=gen;}double v=stream_value(&g[k].acc[i],m[i].operation);fprintf(out,"{\"nombre\":");milena_json_write_string(out,n);fprintf(out,",\"operacion\":");milena_json_write_string(out,milena_stream_operation_name(m[i].operation));fprintf(out,",\"valores_invalidos\":%zu,\"valor\":",g[k].invalid[i]);if(isnan(v))fputs("null",out);else fprintf(out,"%.17g",v);fputc('}',out);}fputs("]}",out);}return fputs("]}\n",out)==EOF?MILENA_ERR_IO:MILENA_OK;
}
MilenaStatus milena_stream_csv_grouped_with_options(const char *input_path,const char *output_path,const char *group_column,const MilenaStreamMetric *metrics,size_t metric_count,const MilenaStreamOptions *requested,MilenaStreamReport *report,MilenaError *error) {
 MilenaStreamOptions d=milena_stream_options_default(),o=requested?*requested:d; size_t maxg=o.max_groups?o.max_groups:d.max_groups;
 if(!input_path||!output_path||!group_column||!group_column[0]||!metrics||!metric_count||metric_count>STREAM_MAX_METRICS||maxg>STREAM_HARD_MAX_GROUPS)return MILENA_ERR_ARGUMENT;
 bool spill=o.spill_directory&&o.spill_directory[0]&&o.spill_partitions>0; size_t np=spill?o.spill_partitions:0; if(spill&&(np>64||o.spill_max_bytes==0||o.spill_max_records==0))return MILENA_ERR_ARGUMENT;
 size_t group_cap=maxg;if(spill&&np>SIZE_MAX/maxg)return MILENA_ERR_OVERFLOW;if(spill)group_cap=maxg*np; FILE *in=fopen(input_path,"rb");if(!in){milena_error_set(error,MILENA_ERR_IO,0,0,0,"No se pudo abrir el CSV agrupado");return MILENA_ERR_IO;} char *record=NULL,**fields=NULL,**headers=NULL;size_t cap=0,len=0,cols=0,groups=0,rows=0,badrows=0,obs=0;MilenaStatus st; StreamGroup *gs=NULL; FILE **pf=NULL;uint64_t *ph=NULL;size_t *pb=NULL,*pr=NULL;int gi=-1,ix[STREAM_MAX_METRICS];
 st=stream_read_record(in,&record,&cap,o.max_record_bytes,&len,error);if(st!=MILENA_OK)goto done;fields=calloc(o.max_columns,sizeof(*fields));if(!fields){st=MILENA_ERR_MEMORY;goto done;}st=stream_split(record,',',fields,o.max_columns,&cols,error);if(st!=MILENA_OK)goto done;headers=calloc(cols,sizeof(*headers));if(!headers){st=MILENA_ERR_MEMORY;goto done;}for(size_t i=0;i<cols;i++){headers[i]=milena_strdup(fields[i]);if(!headers[i]){st=MILENA_ERR_MEMORY;goto done;}}gi=stream_column_index(headers,cols,group_column);if(gi<0){milena_error_set(error,MILENA_ERR_DATA,0,0,0,"La columna de agrupación no existe");st=MILENA_ERR_DATA;goto done;}for(size_t i=0;i<metric_count;i++){ix[i]=stream_column_index(headers,cols,metrics[i].column);if(ix[i]<0){milena_error_set(error,MILENA_ERR_DATA,0,0,0,"La columna métrica no existe");st=MILENA_ERR_DATA;goto done;}}
 if(spill){pf=calloc(np,sizeof(*pf));ph=calloc(np,sizeof(*ph));pb=calloc(np,sizeof(*pb));pr=calloc(np,sizeof(*pr));if(!pf||!ph||!pb||!pr){st=MILENA_ERR_MEMORY;goto done;}for(size_t j=0;j<np;j++){char path[512];if(snprintf(path,sizeof(path),"%s/milena-spill-%ld-%zu.bin",o.spill_directory,(long)getpid(),j)>= (int)sizeof(path)){st=MILENA_ERR_OVERFLOW;goto done;}pf[j]=fopen(path,"wb+");if(!pf[j]){milena_error_set(error,MILENA_ERR_IO,0,0,0,"No se pudo crear archivo temporal de spill");st=MILENA_ERR_IO;goto done;}fputs("MLSP",pf[j]);ph[j]=UINT64_C(1469598103934665603);pb[j]=4;}}
 gs=calloc(group_cap,sizeof(*gs));if(!gs){st=MILENA_ERR_MEMORY;goto done;}
 while((st=stream_read_record(in,&record,&cap,o.max_record_bytes,&len,error))==MILENA_OK){rows++;if(len>obs)obs=len;size_t fc=0;st=stream_split(record,',',fields,o.max_columns,&fc,error);if(st!=MILENA_OK)break;if(fc!=cols){badrows++;continue;}size_t k=0;while(k<groups&&strcmp(gs[k].key,fields[gi]))k++;
  if(!spill&&k==groups&&groups>=maxg){milena_error_set(error,MILENA_ERR_OVERFLOW,0,0,0,"Se superó el límite máximo de grupos del flujo");st=MILENA_ERR_OVERFLOW;break;}
  if(spill){uint64_t hv=spill_hash(UINT64_C(1469598103934665603),fields[gi],strlen(fields[gi]));size_t q=(size_t)(hv%np);uint32_t kl=(uint32_t)strlen(fields[gi]);size_t need=sizeof kl+kl+metric_count*(sizeof(double)+1);if(kl==UINT32_MAX||pr[q]>=o.spill_max_records||need > o.spill_max_bytes || sizeof(uint64_t) > o.spill_max_bytes - need || pb[q] > o.spill_max_bytes - need - sizeof(uint64_t)){milena_error_set(error,MILENA_ERR_OVERFLOW,0,0,0,"El spill supera los límites configurados");st=MILENA_ERR_OVERFLOW;break;}if(fwrite(&kl,1,sizeof kl,pf[q])!=sizeof kl||fwrite(fields[gi],1,kl,pf[q])!=kl){st=MILENA_ERR_IO;break;}spill_mix(&ph[q],&kl,sizeof kl);spill_mix(&ph[q],fields[gi],kl);for(size_t i=0;i<metric_count;i++){double v;unsigned char valid=stream_parse_number(fields[ix[i]],&v)?1:0;if(!valid){badrows++;v=0;}if(fwrite(&valid,1,1,pf[q])!=1||fwrite(&v,1,sizeof v,pf[q])!=sizeof v){st=MILENA_ERR_IO;break;}spill_mix(&ph[q],&valid,1);spill_mix(&ph[q],&v,sizeof v);}if(st!=MILENA_OK)break;pb[q]+=need;pr[q]++;continue;}
  if(k==groups){gs[k].key=milena_strdup(fields[gi]);gs[k].acc=calloc(metric_count,sizeof(StreamAccumulator));gs[k].invalid=calloc(metric_count,sizeof(size_t));if(!gs[k].key||!gs[k].acc||!gs[k].invalid){st=MILENA_ERR_MEMORY;break;}groups++;}bool bad=false;for(size_t i=0;i<metric_count;i++){double v;if(!stream_parse_number(fields[ix[i]],&v)){gs[k].invalid[i]++;bad=true;continue;}stream_accumulate(&gs[k].acc[i],v);}if(bad)badrows++;
 }
 if(st==MILENA_ERR_IO&&feof(in))st=MILENA_OK;if(st!=MILENA_OK)goto done;
 if(spill){for(size_t q=0;q<np;q++){if(!pf[q])continue;fseek(pf[q],0,SEEK_END);uint64_t h=ph[q];if(fwrite(&h,1,sizeof h,pf[q])!=sizeof h||fclose(pf[q])!=0){st=MILENA_ERR_IO;goto done;}pf[q]=NULL;/* reopen and aggregate records */char path[512];snprintf(path,sizeof(path),"%s/milena-spill-%ld-%zu.bin",o.spill_directory,(long)getpid(),q);FILE*f=fopen(path,"rb");if(!f){st=MILENA_ERR_IO;goto done;}char magic[4];if(fread(magic,1,4,f)!=4||memcmp(magic,"MLSP",4)){fclose(f);st=MILENA_ERR_DATA;goto done;}uint64_t hh=UINT64_C(1469598103934665603);while(true){uint32_t kl;if(fread(&kl,1,4,f)!=4)break;char*key=malloc((size_t)kl+1);if(!key||fread(key,1,kl,f)!=kl){free(key);fclose(f);st=MILENA_ERR_DATA;goto done;}key[kl]=0;spill_mix(&hh,&kl,4);spill_mix(&hh,key,kl);size_t k=0;while(k<groups&&strcmp(gs[k].key,key))k++;if(k==groups){if(groups>=group_cap){free(key);fclose(f);milena_error_set(error,MILENA_ERR_OVERFLOW,0,0,0,"Una partición supera el límite de grupos");st=MILENA_ERR_OVERFLOW;goto done;}gs[k].key=key;gs[k].acc=calloc(metric_count,sizeof(StreamAccumulator));gs[k].invalid=calloc(metric_count,sizeof(size_t));if(!gs[k].acc||!gs[k].invalid){st=MILENA_ERR_MEMORY;goto done;}groups++;}else free(key);for(size_t i=0;i<metric_count;i++){unsigned char valid;double v;if(fread(&valid,1,1,f)!=1||fread(&v,1,sizeof v,f)!=sizeof v){fclose(f);st=MILENA_ERR_DATA;goto done;}spill_mix(&hh,&valid,1);spill_mix(&hh,&v,sizeof v);if(valid)stream_accumulate(&gs[k].acc[i],v);else gs[k].invalid[i]++;}}uint64_t stored;if(fread(&stored,1,sizeof stored,f)!=sizeof stored||stored!=hh||stored!=ph[q]){fclose(f);milena_error_set(error,MILENA_ERR_DATA,0,0,0,"Checksum de spill inválido");st=MILENA_ERR_DATA;goto done;}fclose(f);unlink(path);}}
 {qsort(gs,groups,sizeof(*gs),stream_group_compare);FILE*out=fopen(output_path,"wb");if(!out){st=MILENA_ERR_IO;goto done;}st=grouped_emit(out,group_column,metrics,metric_count,gs,groups,maxg,rows,spill);if(fclose(out)!=0&&st==MILENA_OK)st=MILENA_ERR_IO;}
 done: if(pf){for(size_t j=0;j<np;j++){if(pf[j])fclose(pf[j]);char path[512];snprintf(path,sizeof(path),"%s/milena-spill-%ld-%zu.bin",o.spill_directory?o.spill_directory:"",(long)getpid(),j);unlink(path);}}if(report){memset(report,0,sizeof(*report));report->rows_read=rows;report->malformed_rows=badrows;report->observed_record_bytes=obs;report->peak_record_bytes=cap;report->header_columns=cols;report->spilled_bytes=0;if(spill&&pb)for(size_t j=0;j<np;j++)report->spilled_bytes+=pb[j];report->spill_partitions=spill?np:0;report->spill_temp_files=spill?np:0;report->spill_rows=spill?rows:0;}for(size_t k=0;k<groups;k++){free(gs[k].key);free(gs[k].acc);free(gs[k].invalid);}free(gs);free(pf);free(ph);free(pb);free(pr);if(in)fclose(in);free(record);free(fields);stream_free_headers(headers,cols);return st;
}

