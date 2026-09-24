#ifndef MILENA_SOURCE_READER_H
#define MILENA_SOURCE_READER_H

#include "common.h"

typedef struct MilenaSourceReaderOps MilenaSourceReaderOps;

typedef struct {
    const MilenaSourceReaderOps *ops;
    void *context;
} MilenaSourceReader;

struct MilenaSourceReaderOps {
    MilenaStatus (*read_record)(void *context, char **record, size_t *capacity,
                                size_t max_record_bytes, size_t *record_length,
                                MilenaError *error);
    bool (*at_end)(void *context);
    bool (*position_bytes)(void *context, size_t *position);
    MilenaStatus (*close)(void *context);
};

/* This phase exposes the existing local CSV record stream through a narrow
 * reader interface. It does not add remote, cloud, or columnar backends. */
MilenaStatus milena_source_reader_open_local_csv(MilenaSourceReader *reader,
                                                  const char *path,
                                                  MilenaError *error);
MilenaStatus milena_source_reader_read_record(MilenaSourceReader *reader,
                                               char **record, size_t *capacity,
                                               size_t max_record_bytes,
                                               size_t *record_length,
                                               MilenaError *error);
bool milena_source_reader_at_end(const MilenaSourceReader *reader);
bool milena_source_reader_position_bytes(const MilenaSourceReader *reader,
                                          size_t *position);
MilenaStatus milena_source_reader_close(MilenaSourceReader *reader);

#endif
