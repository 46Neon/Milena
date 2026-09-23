#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "source_reader.h"
#include "file_position.h"

#define SOURCE_READER_IO_BUFFER (64u * 1024u)
#define SOURCE_READER_INITIAL_RECORD 4096u

typedef struct {
    FILE *file;
} LocalCsvReader;

static MilenaStatus local_csv_grow_record(char **record, size_t *capacity,
                                           size_t needed,
                                           size_t max_record_bytes,
                                           MilenaError *error) {
    if (needed <= *capacity) return MILENA_OK;
    if (needed > max_record_bytes) {
        milena_error_set(error, MILENA_ERR_OVERFLOW, 0, 0, 0,
                         "El registro CSV supera el límite configurado del flujo");
        return MILENA_ERR_OVERFLOW;
    }
    size_t next = *capacity ? *capacity : SOURCE_READER_INITIAL_RECORD;
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

static MilenaStatus local_csv_read_record(void *opaque, char **record,
                                           size_t *capacity,
                                           size_t max_record_bytes,
                                           size_t *record_length,
                                           MilenaError *error) {
    LocalCsvReader *reader = (LocalCsvReader *)opaque;
    if (!reader || !reader->file || !record || !capacity || !record_length)
        return MILENA_ERR_ARGUMENT;
    size_t length = 0;
    bool in_quotes = false;
    int ch;
    while ((ch = fgetc(reader->file)) != EOF) {
        if (ch == '"') {
            MilenaStatus status = local_csv_grow_record(record, capacity,
                length + 2, max_record_bytes, error);
            if (status != MILENA_OK) return status;
            (*record)[length++] = (char)ch;
            if (in_quotes) {
                int next = fgetc(reader->file);
                if (next == '"') {
                    status = local_csv_grow_record(record, capacity,
                        length + 2, max_record_bytes, error);
                    if (status != MILENA_OK) return status;
                    (*record)[length++] = (char)next;
                } else {
                    if (next != EOF) (void)ungetc(next, reader->file);
                    in_quotes = false;
                }
            } else {
                in_quotes = true;
            }
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r') {
                int next = fgetc(reader->file);
                if (next != '\n' && next != EOF) (void)ungetc(next, reader->file);
            }
            if (!in_quotes) break;
            MilenaStatus status = local_csv_grow_record(record, capacity,
                length + 2, max_record_bytes, error);
            if (status != MILENA_OK) return status;
            (*record)[length++] = '\n';
        } else {
            MilenaStatus status = local_csv_grow_record(record, capacity,
                length + 2, max_record_bytes, error);
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

static bool local_csv_at_end(void *opaque) {
    LocalCsvReader *reader = (LocalCsvReader *)opaque;
    return reader && reader->file && feof(reader->file) != 0;
}

static bool local_csv_position_bytes(void *opaque, size_t *position) {
    LocalCsvReader *reader = (LocalCsvReader *)opaque;
    if (!reader || !reader->file || !position) return false;
    return milena_file_position_size(reader->file, position);
}

static MilenaStatus local_csv_close(void *opaque) {
    LocalCsvReader *reader = (LocalCsvReader *)opaque;
    if (!reader) return MILENA_OK;
    MilenaStatus status = MILENA_OK;
    if (reader->file && fclose(reader->file) != 0) status = MILENA_ERR_IO;
    free(reader);
    return status;
}

static const MilenaSourceReaderOps LOCAL_CSV_OPS = {
    local_csv_read_record,
    local_csv_at_end,
    local_csv_position_bytes,
    local_csv_close
};

MilenaStatus milena_source_reader_open_local_csv(MilenaSourceReader *reader,
                                                  const char *path,
                                                  MilenaError *error) {
    if (!reader || !path || !path[0]) return MILENA_ERR_ARGUMENT;
    reader->ops = NULL;
    reader->context = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) {
        milena_error_set(error, MILENA_ERR_IO, 0, 0, 0,
                         "No se pudo abrir el CSV para lectura en flujo");
        return MILENA_ERR_IO;
    }
    (void)setvbuf(file, NULL, _IOFBF, SOURCE_READER_IO_BUFFER);
    LocalCsvReader *context = (LocalCsvReader *)malloc(sizeof(*context));
    if (!context) {
        (void)fclose(file);
        milena_error_set(error, MILENA_ERR_MEMORY, 0, 0, 0,
                         "Memoria insuficiente para abrir la fuente CSV");
        return MILENA_ERR_MEMORY;
    }
    context->file = file;
    reader->ops = &LOCAL_CSV_OPS;
    reader->context = context;
    return MILENA_OK;
}

MilenaStatus milena_source_reader_read_record(MilenaSourceReader *reader,
                                               char **record, size_t *capacity,
                                               size_t max_record_bytes,
                                               size_t *record_length,
                                               MilenaError *error) {
    if (!reader || !reader->ops || !reader->ops->read_record)
        return MILENA_ERR_ARGUMENT;
    return reader->ops->read_record(reader->context, record, capacity,
                                    max_record_bytes, record_length, error);
}

bool milena_source_reader_at_end(const MilenaSourceReader *reader) {
    return reader && reader->ops && reader->ops->at_end &&
           reader->ops->at_end(reader->context);
}

bool milena_source_reader_position_bytes(const MilenaSourceReader *reader,
                                          size_t *position) {
    return reader && reader->ops && reader->ops->position_bytes &&
           reader->ops->position_bytes(reader->context, position);
}

MilenaStatus milena_source_reader_close(MilenaSourceReader *reader) {
    if (!reader) return MILENA_ERR_ARGUMENT;
    MilenaStatus status = MILENA_OK;
    if (reader->ops && reader->ops->close)
        status = reader->ops->close(reader->context);
    reader->ops = NULL;
    reader->context = NULL;
    return status;
}
